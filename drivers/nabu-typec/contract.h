#pragma once

namespace nabu::contract {

constexpr unsigned MsgType(unsigned header) { return header & 0x1F; }
constexpr unsigned Objects(unsigned header) { return (header >> 12) & 7; }
constexpr bool Extended(unsigned header) { return (header >> 15) & 1; }
constexpr bool FromSource(unsigned header) { return (header >> 8) & 1; }

constexpr unsigned kAccept = 0x03, kReject = 0x04, kPsRdy = 0x06, kPrSwap = 0x0A, kWait = 0x0C, kSoftReset = 0x0D;
constexpr unsigned kSourceCapabilities = 0x01, kRequest = 0x02;

constexpr unsigned kMaxMa = 5000;

struct Contract {
    bool Active;
    unsigned Millivolts;
    unsigned Milliamps;
};

enum class Phase : unsigned { Idle = 0, RequestSent = 1, Accepted = 2 };

struct Tracker {
    unsigned Caps[7];
    unsigned CapCount;
    Phase P;
    unsigned PendingRdo;
    Contract Now;
};

inline Tracker Initial() { return Tracker{}; }

constexpr unsigned Le32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<unsigned>(p[3]) << 24);
}

inline Contract Decode(unsigned rdo, const Tracker& t) {
    Contract c{};
    const unsigned pos = (rdo >> 28) & 7;
    if (pos == 0 || pos > t.CapCount) return c;
    const unsigned pdo = t.Caps[pos - 1];
    switch (pdo >> 30) {
    case 0:
        c.Millivolts = ((pdo >> 10) & 0x3FF) * 50;
        c.Milliamps = ((rdo >> 10) & 0x3FF) * 10;
        break;
    case 2:
        c.Millivolts = ((pdo >> 10) & 0x3FF) * 50;
        c.Milliamps = ((rdo >> 10) & 0x3FF) * 10;
        break;
    case 3:
        if (((pdo >> 28) & 3) != 0) return c;
        c.Millivolts = ((rdo >> 9) & 0x7FF) * 20;
        c.Milliamps = (rdo & 0x7F) * 50;
        break;
    default:
        return c;
    }
    if (c.Milliamps == 0 || c.Milliamps > kMaxMa || c.Millivolts == 0) return Contract{};
    c.Active = true;
    return c;
}

constexpr bool Same(const Contract& a, const Contract& b) {
    return a.Active == b.Active && a.Millivolts == b.Millivolts && a.Milliamps == b.Milliamps;
}

inline bool Reset(Tracker& t) {
    const bool changed = t.Now.Active;
    t = Tracker{};
    return changed;
}

inline bool OnSent(Tracker& t, unsigned header, const unsigned char* data, unsigned dataBytes) {
    if (Extended(header)) return false;
    const unsigned n = Objects(header);
    const unsigned type = MsgType(header);
    if (n == 0) {
        if (type == kSoftReset) return Reset(t);
        if (type == kPrSwap) { const bool changed = t.Now.Active; t.Now = Contract{}; t.P = Phase::Idle; return changed; }
        return false;
    }
    if (type == kRequest && n == 1 && !FromSource(header) && data && dataBytes >= 4) {
        t.PendingRdo = Le32(data);
        t.P = Phase::RequestSent;
    }
    return false;
}

inline bool OnReceived(Tracker& t, unsigned header, const unsigned char* data, unsigned dataBytes) {
    if (Extended(header)) return false;
    const unsigned n = Objects(header);
    const unsigned type = MsgType(header);
    const Contract before = t.Now;
    if (n == 0) {
        switch (type) {
        case kSoftReset: return Reset(t);
        case kPrSwap: t.Now = Contract{}; t.P = Phase::Idle; break;
        case kAccept:
            if (t.P == Phase::RequestSent && FromSource(header)) {
                t.P = Phase::Accepted;
                const Contract next = Decode(t.PendingRdo, t);
                if (t.Now.Active && (!next.Active || next.Milliamps < t.Now.Milliamps))
                    t.Now.Milliamps = next.Active ? next.Milliamps : 0;
                if (t.Now.Active && t.Now.Milliamps == 0) t.Now = Contract{};
            }
            break;
        case kReject: case kWait:
            if (t.P == Phase::RequestSent && FromSource(header)) t.P = Phase::Idle;
            break;
        case kPsRdy:
            if (t.P == Phase::Accepted && FromSource(header)) {
                t.Now = Decode(t.PendingRdo, t);
                t.P = Phase::Idle;
            }
            break;
        default: break;
        }
        return !Same(before, t.Now);
    }
    if (type == kSourceCapabilities && FromSource(header) && data && dataBytes >= 4 * n) {
        for (unsigned i = 0; i < n; ++i) t.Caps[i] = Le32(data + 4 * i);
        t.CapCount = n;
    }
    return false;
}

}
