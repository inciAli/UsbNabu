#pragma once

#include "pm8150b_regs.h"

namespace nabu::pdphy {

namespace r = nabu::pm8150b;

constexpr unsigned kRxDetectSop = 1u << 0;
constexpr unsigned kRxDetectHardReset = 1u << 5;
constexpr unsigned kTxSop = 0, kTxHardReset = 5, kTxCableReset = 6, kTxBistCarrier = 7;
constexpr unsigned TxType(unsigned transmit) { return transmit & 7; }
constexpr unsigned TxRetry(unsigned transmit) { return (transmit >> 4) & 3; }

constexpr unsigned PdHeaderObjects(unsigned header) { return (header >> 12) & 7; }
constexpr bool PdHeaderExtended(unsigned header) { return (header >> 15) & 1; }

enum class Kind : unsigned { Write = 0, Read = 1 };
struct Step { Kind K; unsigned Addr; unsigned Value; };
struct Steps {
    static constexpr unsigned kMax = 40;
    Step Items[kMax];
    unsigned Count;
    bool Overflow;
    void W(unsigned addr, unsigned value) { Add(Kind::Write, addr, value & 0xFF); }
    void R(unsigned addr) { Add(Kind::Read, addr, 0); }
    void Add(Kind k, unsigned addr, unsigned value) {
        if (Count < kMax) Items[Count++] = Step{k, addr, value}; else Overflow = true;
    }
};

inline void ClearTxControl(Steps& s) {
    s.W(r::kPdphyTxControl, 0);
    s.R(r::kPdphyTxControl);
}

inline Steps Enable() {
    Steps s{};
    s.W(r::kPdphyMsgConfig, 1);
    s.W(r::kPdphyEnControl, 0);
    s.W(r::kPdphyEnControl, r::kPdphyEnable);
    s.W(r::kPdphyFrameFilter, r::kFilterSop | r::kFilterHardReset);
    return s;
}

inline Steps Disable() {
    Steps s{};
    s.W(r::kPdphyTxControl, 0);
    s.W(r::kPdphyFrameFilter, 0);
    s.W(r::kPdphyEnControl, 0);
    return s;
}

inline unsigned MsgConfig(unsigned headerInfo) {
    const unsigned powerSource = headerInfo & 1;
    const unsigned rev = (headerInfo >> 1) & 3;
    const unsigned dataHost = (headerInfo >> 3) & 1;
    return (dataHost ? r::kMsgCfgDataRoleHost : 0) | (powerSource ? r::kMsgCfgPowerRoleSource : 0) | rev;
}
inline Steps OnSetMessageHeaderInfo(unsigned headerInfo) {
    Steps s{};
    s.W(r::kPdphyMsgConfig, MsgConfig(headerInfo));
    return s;
}

inline Steps OnSetReceiveDetect(unsigned receiveDetect) {
    Steps s{};
    unsigned filter = 0;
    if (receiveDetect & kRxDetectSop) filter |= r::kFilterSop;
    if (receiveDetect & kRxDetectHardReset) filter |= r::kFilterHardReset;
    s.W(r::kPdphyFrameFilter, filter);
    s.W(r::kPdphyRxAcknowledge, filter ? 0 : 1);
    return s;
}

enum class TxResult : unsigned {
    Started = 0,
    Discarded = 1,
    Invalid = 2,
    Unsupported = 3,
};

struct TxBuffer {
    unsigned ByteCount;
    unsigned char Header[2];
    unsigned char Data[28];
};

inline bool ValidTxBuffer(const TxBuffer& b) {
    if (b.ByteCount < 2 || b.ByteCount > r::kPdMaxMessageBytes) return false;
    if ((b.ByteCount - 2) % 4 != 0) return false;
    const unsigned header = b.Header[0] | (b.Header[1] << 8);
    if (PdHeaderExtended(header)) return false;
    return PdHeaderObjects(header) * 4 == b.ByteCount - 2;
}

inline TxResult OnTransmit(unsigned transmit, const TxBuffer& b, unsigned rxAckRegister, Steps& out) {
    out = Steps{};
    const unsigned type = TxType(transmit);
    const unsigned retry = r::TxRetryCount(TxRetry(transmit));
    if (type == kTxHardReset) {
        ClearTxControl(out);
        out.W(r::kPdphyTxControl, r::kTxSendSignal | retry | r::TxFrameType(1));
        return TxResult::Started;
    }
    if (type != kTxSop) return TxResult::Unsupported;
    if (!ValidTxBuffer(b)) return TxResult::Invalid;
    if (rxAckRegister & 1) return TxResult::Discarded;
    ClearTxControl(out);
    out.W(r::kPdphyTxBufferHdr, b.Header[0]);
    out.W(r::kPdphyTxBufferHdr + 1, b.Header[1]);
    for (unsigned i = 0; i + 2 < b.ByteCount; ++i) out.W(r::kPdphyTxBufferData + i, b.Data[i]);
    out.W(r::kPdphyTxSize, b.ByteCount - 1);
    ClearTxControl(out);
    out.W(r::kPdphyTxControl, r::TxFrameType(kTxSop) | r::kTxSendMsg | retry);
    return TxResult::Started;
}

struct RxBuffer {
    unsigned ByteCount;
    unsigned FrameType;
    unsigned char Header[2];
    unsigned char Data[28];
};

constexpr unsigned RxReadLength(unsigned rxSizeRegister) {
    return (rxSizeRegister >= 1 && rxSizeRegister <= r::kPdMaxMessageBytes - 1) ? rxSizeRegister + 1 : 0;
}

inline bool BuildRxBuffer(unsigned rxStatusRegister, const unsigned char* bytes, unsigned length, RxBuffer& out) {
    out = RxBuffer{};
    if (!bytes || length < 2 || length > r::kPdMaxMessageBytes || (length - 2) % 4 != 0) return false;
    const unsigned header = bytes[0] | (bytes[1] << 8);
    if (PdHeaderExtended(header) || PdHeaderObjects(header) * 4 != length - 2) return false;

    (void)rxStatusRegister;
    out.FrameType = 0;
    out.ByteCount = length + 1;
    out.Header[0] = bytes[0];
    out.Header[1] = bytes[1];
    for (unsigned i = 2; i < length; ++i) out.Data[i - 2] = bytes[i];
    return true;
}

inline Steps AcknowledgeRx() {
    Steps s{};
    s.W(r::kPdphyRxAcknowledge, 0);
    return s;
}

enum class Irq : unsigned { SigTx = 0, SigRx = 1, MsgTx = 2, MsgRx = 3, MsgTxFailed = 4, MsgTxDiscarded = 5, MsgRxDiscarded = 6 };

enum class Alert : unsigned {
    None = 0, TransmitSuccessful, TransmitFailed, TransmitDiscarded, ReceivedHardReset, ReceiveMessage, RxOverflow,
};

struct IrqAction {
    Alert Report;
    bool ReadFrame;
    Steps Writes;
};

inline IrqAction OnIrq(Irq irq, unsigned frameFilter, bool hardResetPending) {
    IrqAction a{};
    switch (irq) {
    case Irq::MsgTx: a.Report = Alert::TransmitSuccessful; break;
    case Irq::MsgTxFailed: a.Report = Alert::TransmitFailed; break;
    case Irq::MsgTxDiscarded: a.Report = Alert::TransmitDiscarded; break;
    case Irq::MsgRx: a.ReadFrame = true; break;
    case Irq::MsgRxDiscarded: a.Report = Alert::RxOverflow; break;
    case Irq::SigTx: a.Report = hardResetPending ? Alert::TransmitSuccessful : Alert::None; break;
    case Irq::SigRx:

        a.Writes.W(r::kPdphyTxControl, 0);
        a.Writes.W(r::kPdphyFrameFilter, 0);
        a.Writes.W(r::kPdphyFrameFilter, frameFilter);
        a.Report = Alert::ReceivedHardReset;
        break;
    }
    return a;
}

}
