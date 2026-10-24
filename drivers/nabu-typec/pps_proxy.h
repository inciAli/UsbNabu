#pragma once

namespace nabu::pps_proxy {

constexpr unsigned MsgType(unsigned h) { return h & 0x1F; }
constexpr unsigned SpecRev(unsigned h) { return (h >> 6) & 3; }
constexpr unsigned MsgId(unsigned h) { return (h >> 9) & 7; }
constexpr unsigned Objects(unsigned h) { return (h >> 12) & 7; }
constexpr bool Extended(unsigned h) { return (h >> 15) & 1; }
constexpr unsigned WithId(unsigned h, unsigned id) { return (h & ~(7u << 9)) | ((id & 7) << 9); }
constexpr bool IsControl(unsigned h, unsigned type) { return !Extended(h) && Objects(h) == 0 && MsgType(h) == type; }
constexpr bool IsData(unsigned h, unsigned type) { return !Extended(h) && Objects(h) != 0 && MsgType(h) == type; }

constexpr unsigned kAccept = 0x03, kReject = 0x04, kPsRdy = 0x06, kPrSwap = 0x0A, kWait = 0x0C, kSoftReset = 0x0D;
constexpr unsigned kSourceCapabilities = 0x01, kRequest = 0x02;
constexpr unsigned kRev30 = 2;

constexpr unsigned kExitHoldMs = 15;
constexpr unsigned kAcceptTimeoutMs = 100;
constexpr unsigned kRequestTimeoutMs = 600;
constexpr unsigned kWaitRetryMs = 100;
constexpr unsigned kMaxWaits = 3;
constexpr unsigned kKeepaliveMs = 5000;
constexpr unsigned kQuietMs = 500;
constexpr unsigned kNoDeadline = 0xFFFFFFFFu;

constexpr bool IsPpsApdo(unsigned pdo) { return (pdo >> 30) == 3 && ((pdo >> 28) & 3) == 0; }
constexpr unsigned ApdoMaxMv(unsigned pdo) { return ((pdo >> 17) & 0xFF) * 100; }
constexpr unsigned ApdoMinMv(unsigned pdo) { return ((pdo >> 8) & 0xFF) * 100; }
constexpr unsigned ApdoMaxMa(unsigned pdo) { return (pdo & 0x7F) * 50; }
constexpr bool ApdoPowerLimited(unsigned pdo) { return (pdo >> 27) & 1; }

constexpr unsigned kRdoFlagMask = 3u << 24;
constexpr unsigned EncodePpsRdo(unsigned pos, unsigned mv, unsigned ma, unsigned flags) {
    return ((pos & 7) << 28) | (flags & kRdoFlagMask) | (((mv / 20) & 0xFFF) << 9) | ((ma / 50) & 0x7F);
}

inline unsigned FixedPowerMw(const unsigned* pdos, unsigned n) {
    unsigned best = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (pdos[i] >> 30) continue;
        const unsigned mw = ((pdos[i] >> 10) & 0x3FF) * 50 * ((pdos[i] & 0x3FF) * 10) / 1000;
        if (mw > best) best = mw;
    }
    return best;
}

enum class Status : unsigned { None = 0, InFlight = 1, Complete = 2, Rejected = 3, TimedOut = 4, Ended = 5 };
enum class Reason : unsigned {
    None = 0, FrameworkRequest = 1, HardReset = 2, Detach = 3, RoleChange = 4, RequestFailed = 5,
    Released = 6, Disabled = 7, SoftReset = 8,
};
enum class CommandKind : unsigned { Request = 1, Release = 2, StandbyAck = 3 };
enum class CommandResult : unsigned { Queued = 0, NotEnabled = 1, NoContract = 2, Busy = 3, Invalid = 4 };

enum class Ams : unsigned {
    Idle = 0,
    Queued,
    Sending,
    WaitAccept,
    WaitPsRdy,
    WaitRetry,
};
enum class Purpose : unsigned { Pps = 0, Keepalive, Release };

enum class TxOwner : unsigned { None = 0, Framework, Proxy, ProxyDone };

struct Held {
    bool Valid;
    unsigned Transmit;
    unsigned ByteCount;
    unsigned char Header[2];
    unsigned char Data[28];
    bool ExitHold;
    unsigned SinceMs;
};

struct State {
    bool Enabled;

    unsigned TxOffset, RxOffset;
    bool HaveLastRx;
    unsigned LastRxId;
    TxOwner Tx;
    unsigned LastFrameworkMs;

    bool Sink;
    bool SinkTxOk;
    unsigned PartnerRev;
    unsigned Pdos[7];
    unsigned PdoCount;
    unsigned FrameworkHeader;
    unsigned FrameworkTransmit;
    unsigned FwNextId;
    unsigned FixedRdo;
    bool FixedComplete;
    bool FixedPending;
    unsigned FixedMv, FixedMa;

    Ams A;
    Purpose P;
    unsigned AmsSinceMs;
    unsigned Waits;
    unsigned PendingRdo;
    unsigned PendingId;
    bool PpsActive;
    unsigned PpsRdo;
    unsigned PpsPos, PpsMv, PpsMa;
    unsigned LastPpsMs;
    bool ExitPending;
    bool StandbyAcked;
    bool PumpSwitching;
    Held H;

    unsigned LastRequestId;
    Status LastStatus;
    Reason Why;
    unsigned Changes;
};

inline State Initial(bool enabled) { State s{}; s.Enabled = enabled; s.PumpSwitching = true; return s; }

inline void FixedContract(const State& s, unsigned* mv, unsigned* ma) { *mv = s.FixedMv; *ma = s.FixedMa; }

struct Send {
    bool Valid;
    bool Injected;
    unsigned Transmit;
    unsigned ByteCount;
    unsigned char Header[2];
    unsigned char Data[28];
};

enum class RxVerdict : unsigned { Forward = 0, Consume = 1 };
enum class TxVerdict : unsigned { Send = 0, Hold = 1 };

namespace detail {

inline void Changed(State& s) { ++s.Changes; }

inline void Publish(State& s, Status st, Reason why) {
    s.LastStatus = st;
    s.Why = why;
    Changed(s);
}

inline void EndSession(State& s, Reason why) {
    const bool was = s.PpsActive || s.A != Ams::Idle || s.ExitPending;
    s.A = Ams::Idle;
    s.Waits = 0;
    s.PpsActive = false;
    s.PpsRdo = s.PpsPos = s.PpsMv = s.PpsMa = 0;
    s.ExitPending = false;
    s.StandbyAcked = false;
    if (s.H.Valid) s.H.ExitHold = false;
    if (was) Publish(s, Status::Ended, why);
}

inline void ResetWire(State& s) {
    s.TxOffset = s.RxOffset = 0;
    s.FwNextId = 0;
    s.HaveLastRx = false;
    s.Tx = TxOwner::None;
    s.FixedComplete = false;
    s.FixedPending = false;
    s.FixedMv = s.FixedMa = 0;
}

inline void FixedDone(State& s) {
    s.FixedPending = false;
    s.FixedComplete = true;
    const unsigned pos = (s.FixedRdo >> 28) & 7;
    s.FixedMv = s.FixedMa = 0;
    if (pos == 0 || pos > s.PdoCount || (s.Pdos[pos - 1] >> 30) != 0) return;
    s.FixedMv = ((s.Pdos[pos - 1] >> 10) & 0x3FF) * 50;
    s.FixedMa = ((s.FixedRdo >> 10) & 0x3FF) * 10;
}

inline unsigned Hdr(const unsigned char* b) { return b[0] | (b[1] << 8); }
inline void PutHdr(unsigned char* b, unsigned h) { b[0] = h & 0xFF; b[1] = (h >> 8) & 0xFF; }
inline unsigned Le32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<unsigned>(p[3]) << 24);
}

inline unsigned InjectedHeader(const State& s) {
    const unsigned keep = s.FrameworkHeader & ((1u << 5) | (3u << 6));
    return WithId(kRequest | keep | (1u << 12), s.FwNextId + s.TxOffset);
}

inline bool ProxyOnPhy(const State& s) { return s.Tx == TxOwner::Proxy || s.Tx == TxOwner::ProxyDone; }
inline bool AmsBusy(const State& s) {
    return s.A == Ams::Sending || s.A == Ams::WaitAccept || s.A == Ams::WaitPsRdy || s.A == Ams::WaitRetry ||
           ProxyOnPhy(s);
}

inline bool FrameworkRequestLike(unsigned h) { return IsData(h, kRequest) || IsControl(h, kPrSwap); }

}

inline void OnPort(State& s, bool sink, bool sinkTxOk) {
    if (s.Sink && !sink) {
        detail::EndSession(s, Reason::Detach);
        detail::ResetWire(s);
        s.PdoCount = 0;
        s.PartnerRev = 0;
        s.H = Held{};
        detail::Changed(s);
    } else if (sink != s.Sink) {
        detail::Changed(s);
    }
    s.Sink = sink;
    s.SinkTxOk = sinkTxOk;
}

inline void OnHardReset(State& s) {
    detail::EndSession(s, Reason::HardReset);
    detail::ResetWire(s);
    s.H = Held{};
}

inline void OnPump(State& s, bool switching) {
    s.PumpSwitching = switching;
    if (!switching && s.ExitPending) s.StandbyAcked = true;
}

inline void Disable(State& s) {
    if (!s.Enabled) return;
    detail::EndSession(s, Reason::Disabled);
    s.Enabled = false;
}

inline TxVerdict OnFrameworkTransmit(State& s, unsigned transmit, unsigned byteCount, const unsigned char* header,
                                     const unsigned char* data, unsigned nowMs, unsigned* wireHeader) {
    const unsigned h = detail::Hdr(header);
    *wireHeader = h;
    if (!s.Enabled) return TxVerdict::Send;
    s.LastFrameworkMs = nowMs;
    if (IsControl(h, kSoftReset)) {
        detail::EndSession(s, Reason::SoftReset);
        detail::ResetWire(s);
    }
    if (s.H.Valid) return TxVerdict::Hold;
    const bool exitHold = detail::FrameworkRequestLike(h) && s.PpsActive && !s.StandbyAcked && s.PumpSwitching;
    if (exitHold || detail::AmsBusy(s)) {
        s.H.Valid = true;
        s.H.Transmit = transmit;
        s.H.ByteCount = byteCount > 30 ? 30 : byteCount;
        s.H.Header[0] = header[0];
        s.H.Header[1] = header[1];
        for (unsigned i = 0; i + 2 < s.H.ByteCount; ++i) s.H.Data[i] = data[i];
        s.H.SinceMs = nowMs;
        s.H.ExitHold = exitHold;
        if (exitHold && !s.ExitPending) {
            s.ExitPending = true;
            detail::Publish(s, s.LastStatus, Reason::FrameworkRequest);
        }
        return TxVerdict::Hold;
    }

    if (detail::FrameworkRequestLike(h) && s.PpsActive) {

        detail::EndSession(s, Reason::FrameworkRequest);
    }
    if (IsData(h, kRequest) && byteCount >= 6) {
        s.FixedRdo = detail::Le32(data);
        s.FixedPending = true;
        s.FixedComplete = false;
    }
    if (IsControl(h, kPrSwap)) detail::EndSession(s, Reason::RoleChange);
    s.FrameworkHeader = h;
    s.FrameworkTransmit = transmit;
    s.Tx = TxOwner::Framework;
    *wireHeader = WithId(h, MsgId(h) + s.TxOffset);
    return TxVerdict::Send;
}

inline bool OnTransmitResult(State& s, bool success, bool discarded, unsigned nowMs) {
    if (!s.Enabled) return true;
    const TxOwner owner = s.Tx;
    s.Tx = TxOwner::None;
    if (owner == TxOwner::ProxyDone) return false;
    if (owner != TxOwner::Proxy) {

        if (owner == TxOwner::Framework) s.FwNextId = (MsgId(s.FrameworkHeader) + (discarded ? 0 : 1)) & 7;
        return true;
    }
    if (discarded) {
        if (s.A == Ams::Sending) { s.A = Ams::Queued; }
        return false;
    }
    ++s.TxOffset;
    if (s.A != Ams::Sending) return false;
    if (success) {
        s.A = Ams::WaitAccept;
        s.AmsSinceMs = nowMs;
        return false;
    }

    const Purpose p = s.P;
    const bool wasActive = s.PpsActive;
    s.A = Ams::Idle;
    if (p == Purpose::Pps) detail::Publish(s, Status::TimedOut, Reason::RequestFailed);
    if (wasActive && p != Purpose::Release) {
        s.A = Ams::Queued;
        s.P = Purpose::Release;
        s.PendingRdo = s.FixedRdo;
    } else {
        detail::EndSession(s, Reason::RequestFailed);
    }
    return false;
}

inline RxVerdict OnReceive(State& s, unsigned char* header, const unsigned char* data, unsigned dataBytes,
                           unsigned nowMs) {
    if (!s.Enabled) return RxVerdict::Forward;
    const unsigned h = detail::Hdr(header);
    const unsigned id = MsgId(h);
    const bool softReset = IsControl(h, kSoftReset);
    if (!softReset && s.HaveLastRx && id == s.LastRxId) return RxVerdict::Consume;
    if (softReset) {
        detail::EndSession(s, Reason::SoftReset);
        detail::ResetWire(s);
        s.H.ExitHold = false;
    }
    s.HaveLastRx = true;
    s.LastRxId = id;
    s.PartnerRev = SpecRev(h);

    const bool fromSource = (h >> 8) & 1;
    const bool reply = IsControl(h, kAccept) || IsControl(h, kReject) || IsControl(h, kWait);
    if (fromSource && reply && s.A == Ams::Sending && s.Tx == TxOwner::Proxy) {
        s.Tx = TxOwner::ProxyDone;
        ++s.TxOffset;
        s.A = Ams::WaitAccept;
        s.AmsSinceMs = nowMs;
    }
    if (fromSource && (s.A == Ams::WaitAccept || s.A == Ams::WaitPsRdy)) {
        if (s.A == Ams::WaitAccept && IsControl(h, kAccept)) {
            s.A = Ams::WaitPsRdy;
            s.AmsSinceMs = nowMs;
            ++s.RxOffset;
            return RxVerdict::Consume;
        }
        if (s.A == Ams::WaitAccept && (IsControl(h, kReject) || IsControl(h, kWait))) {
            ++s.RxOffset;
            const bool wait = IsControl(h, kWait);
            if (wait && ++s.Waits < kMaxWaits) {
                s.A = Ams::WaitRetry;
                s.AmsSinceMs = nowMs;
                return RxVerdict::Consume;
            }
            const Purpose p = s.P;
            s.A = Ams::Idle;
            s.Waits = 0;
            if (p == Purpose::Pps) {
                detail::Publish(s, Status::Rejected, Reason::None);
            } else if (p == Purpose::Keepalive) {
                s.A = Ams::Queued;
                s.P = Purpose::Release;
                s.PendingRdo = s.FixedRdo;
                s.PpsActive = false;
                detail::Publish(s, Status::Ended, Reason::RequestFailed);
            } else {
                detail::EndSession(s, Reason::RequestFailed);
            }
            return RxVerdict::Consume;
        }
        if (s.A == Ams::WaitPsRdy && IsControl(h, kPsRdy)) {
            ++s.RxOffset;
            const Purpose p = s.P;
            s.A = Ams::Idle;
            s.Waits = 0;
            if (p == Purpose::Release) {
                detail::FixedDone(s);
                const bool was = s.PpsActive;
                s.PpsActive = false;
                s.PpsRdo = s.PpsPos = s.PpsMv = s.PpsMa = 0;
                if (was || s.LastStatus != Status::Ended) detail::Publish(s, Status::Ended, Reason::Released);
            } else {
                s.PpsActive = true;
                s.PpsRdo = s.PendingRdo;
                s.PpsPos = (s.PendingRdo >> 28) & 7;
                s.PpsMv = ((s.PendingRdo >> 9) & 0xFFF) * 20;
                s.PpsMa = (s.PendingRdo & 0x7F) * 50;
                s.LastPpsMs = nowMs;
                if (p == Purpose::Pps) detail::Publish(s, Status::Complete, Reason::None);
            }
            return RxVerdict::Consume;
        }

        if (s.P == Purpose::Pps) detail::Publish(s, Status::Ended, Reason::RequestFailed);
        s.A = Ams::Idle;
        s.Waits = 0;
    }

    s.LastFrameworkMs = nowMs;
    if (IsData(h, kSourceCapabilities) && fromSource && data && dataBytes >= 4 * Objects(h)) {
        s.PdoCount = Objects(h);
        for (unsigned i = 0; i < s.PdoCount; ++i) s.Pdos[i] = detail::Le32(data + 4 * i);
        s.FixedComplete = false;
        if (s.PpsActive && !s.ExitPending) {
            s.ExitPending = true;
            s.StandbyAcked = false;
            detail::Publish(s, s.LastStatus, Reason::FrameworkRequest);
        }
        detail::Changed(s);
    }
    if (fromSource && s.FixedPending && IsControl(h, kPsRdy)) detail::FixedDone(s);
    if (fromSource && s.FixedPending && (IsControl(h, kReject) || IsControl(h, kWait))) s.FixedPending = false;
    if (IsControl(h, kPrSwap)) detail::EndSession(s, Reason::RoleChange);
    detail::PutHdr(header, WithId(h, id - s.RxOffset));
    return RxVerdict::Forward;
}

inline CommandResult OnCommand(State& s, CommandKind kind, unsigned requestId, unsigned pos, unsigned mv, unsigned ma) {
    if (!s.Enabled) return CommandResult::NotEnabled;
    switch (kind) {
    case CommandKind::StandbyAck:
        if (s.ExitPending) s.StandbyAcked = true;
        return CommandResult::Queued;
    case CommandKind::Release:
        if (s.A != Ams::Idle && s.A != Ams::Queued) return CommandResult::Busy;
        s.LastRequestId = requestId;
        if (s.PpsActive && s.FixedRdo) {
            s.A = Ams::Queued;
            s.P = Purpose::Release;
            s.PendingRdo = s.FixedRdo;
            s.PendingId = requestId;
        } else {
            s.A = Ams::Idle;
        }
        s.PpsActive = false;
        s.ExitPending = false;
        detail::Publish(s, Status::Ended, Reason::Released);
        return CommandResult::Queued;
    case CommandKind::Request: break;
    default: return CommandResult::Invalid;
    }
    if (!s.Sink || !s.PdoCount || !s.FixedRdo) return CommandResult::NoContract;
    if (s.PartnerRev != kRev30 || SpecRev(s.FrameworkHeader) != kRev30) return CommandResult::Invalid;
    if (s.A != Ams::Idle || s.ExitPending || s.H.Valid) return CommandResult::Busy;
    if (pos < 1 || pos > s.PdoCount || requestId == 0) return CommandResult::Invalid;
    const unsigned pdo = s.Pdos[pos - 1];
    if (!IsPpsApdo(pdo)) return CommandResult::Invalid;
    if (mv % 20 || ma % 50 || ma == 0) return CommandResult::Invalid;
    if (mv < ApdoMinMv(pdo) || mv > ApdoMaxMv(pdo) || ma > ApdoMaxMa(pdo)) return CommandResult::Invalid;
    if (ApdoPowerLimited(pdo) && static_cast<unsigned long long>(mv) * ma > 1000ull * FixedPowerMw(s.Pdos, s.PdoCount))
        return CommandResult::Invalid;
    s.A = Ams::Queued;
    s.P = Purpose::Pps;
    s.PendingRdo = EncodePpsRdo(pos, mv, ma, s.FixedRdo);
    s.PendingId = requestId;
    s.Waits = 0;
    s.LastRequestId = requestId;
    detail::Publish(s, Status::InFlight, Reason::None);
    return CommandResult::Queued;
}

inline Send Next(State& s, unsigned nowMs, bool phyIdle, unsigned* nextMs) {
    Send out{};
    unsigned next = kNoDeadline;
    auto due = [&](unsigned since, unsigned span) {
        const unsigned at = since + span;
        if (static_cast<int>(nowMs - at) >= 0) return true;
        if (next == kNoDeadline || static_cast<int>(at - next) < 0) next = at;
        return false;
    };
    if (!s.Enabled) { *nextMs = next; return out; }

    if (s.A == Ams::WaitAccept && due(s.AmsSinceMs, kAcceptTimeoutMs)) {
        s.A = Ams::Idle;
        if (s.P == Purpose::Pps) detail::Publish(s, Status::TimedOut, Reason::RequestFailed);
        if (s.PpsActive && s.P != Purpose::Release) { s.A = Ams::Queued; s.P = Purpose::Release; s.PendingRdo = s.FixedRdo; }
        else detail::EndSession(s, Reason::RequestFailed);
    }
    if (s.A == Ams::WaitPsRdy && due(s.AmsSinceMs, kRequestTimeoutMs)) {

        s.A = Ams::Idle;
        if (s.P == Purpose::Pps) detail::Publish(s, Status::TimedOut, Reason::RequestFailed);
        detail::EndSession(s, Reason::RequestFailed);
    }
    if (s.A == Ams::WaitRetry && due(s.AmsSinceMs, kWaitRetryMs)) s.A = Ams::Queued;

    if (s.H.Valid && s.H.ExitHold && (s.StandbyAcked || due(s.H.SinceMs, kExitHoldMs))) {
        s.H.ExitHold = false;
        detail::EndSession(s, Reason::FrameworkRequest);
    }

    if (phyIdle && s.Tx == TxOwner::None && !detail::AmsBusy(s)) {
        if (s.H.Valid && !s.H.ExitHold) {

            Held h = s.H;
            s.H = Held{};
            unsigned wire = 0;
            if (OnFrameworkTransmit(s, h.Transmit, h.ByteCount, h.Header, h.Data, nowMs, &wire) == TxVerdict::Send) {
                out.Valid = true;
                out.Injected = false;
                out.Transmit = h.Transmit;
                out.ByteCount = h.ByteCount;
                detail::PutHdr(out.Header, wire);
                for (unsigned i = 0; i + 2 < h.ByteCount; ++i) out.Data[i] = h.Data[i];
            }
            *nextMs = next;
            return out;
        }
        if (!s.H.Valid) {

            if (s.PpsActive && s.A == Ams::Idle && due(s.LastPpsMs, kKeepaliveMs)) {
                s.A = Ams::Queued;
                s.P = Purpose::Keepalive;
                s.PendingRdo = s.PpsRdo;
            }
            const bool window = s.Sink && s.SinkTxOk && s.FrameworkHeader && !s.FixedPending &&
                                (s.FixedComplete || s.PpsActive) && !s.ExitPending;
            if (s.A == Ams::Queued && window && due(s.LastFrameworkMs, kQuietMs)) {
                s.A = Ams::Sending;
                s.Tx = TxOwner::Proxy;
                out.Valid = true;
                out.Injected = true;
                out.Transmit = s.FrameworkTransmit;
                out.ByteCount = 6;
                detail::PutHdr(out.Header, detail::InjectedHeader(s));
                out.Data[0] = s.PendingRdo & 0xFF;
                out.Data[1] = (s.PendingRdo >> 8) & 0xFF;
                out.Data[2] = (s.PendingRdo >> 16) & 0xFF;
                out.Data[3] = (s.PendingRdo >> 24) & 0xFF;
            }
        }
    }
    *nextMs = next;
    return out;
}

}
