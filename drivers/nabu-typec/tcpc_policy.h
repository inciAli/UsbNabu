#pragma once

#include "pm8150b_regs.h"

namespace nabu::tcpc {

namespace r = nabu::pm8150b;

constexpr unsigned kCcOpen = 0;
constexpr unsigned kCcSrcRa = 1, kCcSrcRd = 2;
constexpr unsigned kCcSnkDefault = 1, kCcSnk1500 = 2, kCcSnk3000 = 3;
constexpr unsigned CcStatus(unsigned cc1, unsigned cc2, bool presentingRd, bool looking) {
    return (cc1 & 3) | ((cc2 & 3) << 2) | (presentingRd ? 1u << 4 : 0) | (looking ? 1u << 5 : 0);
}

constexpr unsigned kPsSinkingVbus = 1u << 0;
constexpr unsigned kPsVbusPresent = 1u << 2;
constexpr unsigned kPsVbusDetectEnabled = 1u << 3;
constexpr unsigned kPsSourcingVbus = 1u << 4;

constexpr unsigned kFsVbusOcp = 1u << 3;

constexpr unsigned kRcCcRa = 0, kRcCcRp = 1, kRcCcRd = 2, kRcCcOpen = 3;
constexpr unsigned RcCc1(unsigned rc) { return rc & 3; }
constexpr unsigned RcCc2(unsigned rc) { return (rc >> 2) & 3; }
constexpr unsigned RcRp(unsigned rc) { return (rc >> 4) & 3; }
constexpr bool RcDrp(unsigned rc) { return (rc >> 6) & 1; }

enum class Command : unsigned {
    EnterLowPower = 0x11, DisableVbusDetect = 0x22, EnableVbusDetect = 0x33,
    DisableSinkVbus = 0x44, EnableSinkVbus = 0x55, DisableSourceVbus = 0x66,
    SourceVbusDefaultVoltage = 0x77, SourceVbusHighVoltage = 0x88, Look4Connection = 0x99,
    RxOneMore = 0xAA, ExitLowPower = 0xFF,
};

struct Snapshot {
    bool Valid;
    unsigned Misc;
    unsigned Snk;
    unsigned Src;
    unsigned Sm;
    unsigned Mode;
    unsigned Otg;
    unsigned Usbin;
};

struct Config {
    bool SourceAllowed;
    unsigned Mode;

    unsigned ConfirmTimeoutMs;
};

constexpr Config kStage2{false, 0x00, 100};
constexpr Config kStage3{true, r::kModeEnTrySnk, 100};

struct Op { unsigned Addr; unsigned Value; };
struct Ops {
    static constexpr unsigned kMax = 8;
    Op Items[kMax];
    unsigned Count;
    bool Overflow;
    void Add(unsigned addr, unsigned value) {
        if (Count < kMax) Items[Count++] = Op{addr, value & 0xFF}; else Overflow = true;
    }
};

enum class Source : unsigned { Off = 0, Enabling = 1, On = 2 };

struct State {
    unsigned RoleControl;
    unsigned CurrsrcWritten;
    bool VbusDetectEnabled;
    bool SinkEnabled;
    bool Looking;
    Source Src;
    unsigned EnablingMs;
    unsigned Fault;
    unsigned ReportedCc;
    unsigned ReportedPower;
};

inline State InitialState() {
    State s{};
    s.CurrsrcWritten = 0xFF;
    s.ReportedCc = 0xFF;
    s.ReportedPower = 0xFF;
    return s;
}

constexpr bool Attached(const Snapshot& p) { return (p.Misc & r::kMiscCcAttached) != 0; }
constexpr bool AttachedAsSource(const Snapshot& p) { return Attached(p) && (p.Misc & r::kMiscSnkSrcMode); }
constexpr bool BoostEnabled(const Snapshot& p) { return (p.Otg & r::kOtgStatusMask) == r::kOtgStatusEnabled; }
constexpr bool Vsafe5v(const Snapshot& p) { return (p.Sm & r::kSmVbusVsafe5v) != 0; }

inline void BoostOn(Ops& o) {
    o.Add(r::kUsbinCmdIl, r::kUsbinSuspend);
    o.Add(r::kOtgFaultConditionCfg, 0x0F);
    o.Add(r::kDcdcFswSel, 0x0B);
    o.Add(r::kUsbCmdPulldown, 0x00);
    o.Add(r::kDcdcCmdOtg, r::kOtgEn);
}
inline void BoostOff(Ops& o) {
    o.Add(r::kUsbinCmdIl, 0x00);
    o.Add(r::kDcdcCmdOtg, 0x00);
}

constexpr unsigned DesiredMode(const Config& c) { return c.Mode; }

inline unsigned SnkLevel(unsigned snk) {
    if (snk & r::kSnkRp3p0) return kCcSnk3000;
    if (snk & r::kSnkRp1p5) return kCcSnk1500;
    return kCcSnkDefault;
}

inline unsigned BuildCcStatus(const Snapshot& p, const State& s) {
    if (!p.Valid || !Attached(p)) return CcStatus(kCcOpen, kCcOpen, true, s.Looking);
    const bool cc2Active = (p.Misc & r::kMiscCcOrientation) != 0;
    if (p.Misc & r::kMiscSnkSrcMode) {
        const unsigned t = p.Src & r::kSrcTypeMask;
        if (t == r::kSrcAudioRaRa) return CcStatus(kCcSrcRa, kCcSrcRa, false, false);
        const unsigned other = (t == r::kSrcRdRaVconn) ? kCcSrcRa : kCcOpen;
        return cc2Active ? CcStatus(other, kCcSrcRd, false, false) : CcStatus(kCcSrcRd, other, false, false);
    }
    const unsigned level = SnkLevel(p.Snk);
    return cc2Active ? CcStatus(kCcOpen, level, true, false) : CcStatus(level, kCcOpen, true, false);
}

inline unsigned BuildPowerStatus(const Snapshot& p, const State& s) {
    unsigned v = 0;
    const bool vbus = p.Valid && ((p.Usbin & r::kUsbinPluginRtSts) || s.Src == Source::On);
    if (vbus) v |= kPsVbusPresent;
    if (s.VbusDetectEnabled) v |= kPsVbusDetectEnabled;
    if (s.Src == Source::On) v |= kPsSourcingVbus;
    else if (s.SinkEnabled && vbus) v |= kPsSinkingVbus;
    return v;
}

inline Ops OnInit(const Config& c, const Snapshot& p, State& s) {
    Ops o{};
    if (!p.Valid) return o;
    if (!Attached(p) && p.Mode != DesiredMode(c)) o.Add(r::kTypecModeCfg, DesiredMode(c));
    if (BoostEnabled(p) && !(c.SourceAllowed && AttachedAsSource(p))) BoostOff(o);
    if (c.SourceAllowed && AttachedAsSource(p) && BoostEnabled(p)) s.Src = Source::On;
    return o;
}

constexpr unsigned kBootRecoveryMs = 100;
constexpr bool NeedsBootRecovery(const Snapshot& p) {
    return p.Valid && Attached(p) && !(p.Misc & r::kMiscSnkSrcMode);
}
inline Ops BootRecoveryDisable(const Config& c) {
    Ops o{};
    o.Add(r::kTypecModeCfg, DesiredMode(c) | r::kModeDisableCmd);
    return o;
}
inline Ops BootRecoveryEnable(const Config& c) {
    Ops o{};
    o.Add(r::kTypecModeCfg, DesiredMode(c));
    return o;
}

inline Ops OnSetRoleControl(const Config& c, unsigned rc, State& s) {
    Ops o{};
    s.RoleControl = rc & 0x7F;
    const bool advertisesRp = RcDrp(rc) || RcCc1(rc) == kRcCcRp || RcCc2(rc) == kRcCcRp;
    if (c.SourceAllowed && advertisesRp) {
        const unsigned rp = RcRp(rc);
        const unsigned sel = rp == 2 ? r::kRpSel330uA : rp == 1 ? r::kRpSel180uA : r::kRpSel80uA;
        if (s.CurrsrcWritten != sel) {
            o.Add(r::kTypecCurrsrcCfg, sel);
            s.CurrsrcWritten = sel;
        }
    }
    return o;
}

struct CommandResult { bool Supported; Ops Writes; };

inline CommandResult OnCommand(const Config& c, Command cmd, const Snapshot& p, State& s) {
    CommandResult res{true, {}};
    switch (cmd) {
    case Command::EnableVbusDetect: s.VbusDetectEnabled = true; break;
    case Command::DisableVbusDetect: s.VbusDetectEnabled = false; break;
    case Command::EnableSinkVbus: s.SinkEnabled = true; break;
    case Command::DisableSinkVbus: s.SinkEnabled = false; break;
    case Command::Look4Connection:
        s.Looking = true;
        if (p.Valid && !Attached(p) && p.Mode != DesiredMode(c)) res.Writes.Add(r::kTypecModeCfg, DesiredMode(c));
        break;
    case Command::SourceVbusDefaultVoltage:
        if (!c.SourceAllowed) { res.Supported = false; break; }
        if (s.Src == Source::Off) {
            BoostOn(res.Writes);
            s.Src = Source::Enabling;
            s.EnablingMs = 0;
        }
        break;
    case Command::DisableSourceVbus:
        if (s.Src != Source::Off) BoostOff(res.Writes);
        s.Src = Source::Off;
        break;
    case Command::SourceVbusHighVoltage: res.Supported = false; break;
    case Command::EnterLowPower: case Command::ExitLowPower: case Command::RxOneMore: break;
    default: res.Supported = false; break;
    }
    return res;
}

struct Alerts { bool Cc; bool Power; bool Fault; unsigned CcImage; unsigned PowerImage; unsigned FaultImage; };

inline Ops OnEvaluate(const Config& c, const Snapshot& p, unsigned elapsedMs, State& s, Alerts& alerts) {
    Ops o{};
    alerts = Alerts{};
    if (p.Valid) {
        if (Attached(p)) s.Looking = false;

        if (!Attached(p) && s.Src != Source::Off) {
            BoostOff(o);
            s.Src = Source::Off;
        }

        if (s.Src == Source::Enabling) {
            if (BoostEnabled(p) && Vsafe5v(p)) {
                s.Src = Source::On;
            } else {
                s.EnablingMs += elapsedMs;
                if (s.EnablingMs >= c.ConfirmTimeoutMs) {
                    BoostOff(o);
                    s.Src = Source::Off;
                    s.Fault |= kFsVbusOcp;
                    alerts.Fault = true;
                    alerts.FaultImage = s.Fault;
                }
            }
        }
    }
    const unsigned cc = BuildCcStatus(p, s);
    const unsigned power = BuildPowerStatus(p, s);
    if (cc != s.ReportedCc) { alerts.Cc = true; alerts.CcImage = cc; s.ReportedCc = cc; }
    if (power != s.ReportedPower) { alerts.Power = true; alerts.PowerImage = power; s.ReportedPower = power; }
    return o;
}

inline void OnClearFault(unsigned bits, State& s) { s.Fault &= ~bits; }

constexpr bool NeedsPoll(const State& s) { return s.Src == Source::Enabling; }

}
