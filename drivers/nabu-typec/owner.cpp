#include "nabutypec.h"
#include "pps_proxy.h"
#include "../shared/pps_link.h"

namespace nabutypec {

namespace {

namespace r = nabu::pm8150b;
namespace tc = nabu::tcpc;
namespace pd = nabu::pdphy;
namespace ct = nabu::contract;
namespace lk = nabu::link;
namespace pp = nabu::pps_proxy;
namespace pl = nabu::pps;

constexpr ULONG kPollMs = 5;
constexpr LONG kPendingPoll = 1L << 16;
constexpr LONG kPendingProxy = 1L << 17;

constexpr ULONG kVbusWatchMs = 2500;
constexpr ULONG kVbusPollMs = 10;

struct Owner {
    WDFDEVICE Device;
    WDFIOTARGET Pmic;
    WDFWAITLOCK Lock;
    WDFWORKITEM Work;
    WDFTIMER PollTimer;
    WDFINTERRUPT Irq[kIrqCount];
    volatile LONG Pending;
    volatile LONG Ready;
    tc::Config Config;
    tc::State Tc;
    unsigned TcpcControl, FaultControl, PowerControl;
    unsigned FrameFilter;
    bool HardResetPending;
    pd::TxBuffer Tx;
    PortCallbacks Port;
    LARGE_INTEGER LastEvaluate;
    ct::Tracker Contract;
    LONGLONG VbusWatchUntil;
    bool PpsEnabled;
    WDFTIMER ProxyTimer;
    KSPIN_LOCK ProxyLock;
    pp::State Proxy;
    unsigned ProxyPublished;
};

Owner* g_owner = nullptr;

KSPIN_LOCK g_publishedLock = 0;
lk::ContractInfo g_published = {sizeof(lk::ContractInfo), lk::kContractVersion, 0, 0, 0, 0};
PCALLBACK_OBJECT g_contractCallback = nullptr;
PVOID g_contractRegistration = nullptr;

KSPIN_LOCK g_ppsLock = 0;
pl::PdSnapshot g_pps = {};
PCALLBACK_OBJECT g_snapshotCallback = nullptr, g_controlCallback = nullptr, g_pumpCallback = nullptr;
PVOID g_snapshotRegistration = nullptr, g_controlRegistration = nullptr, g_pumpRegistration = nullptr;

struct IrqContext { ULONG Index; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IrqContext, IrqContextOf)

void Log(PCSTR what, ULONG a, ULONG b) {
    TraceLoggingWrite(g_typecTrace, "Owner", TraceLoggingString(what, "What"),
                      TraceLoggingHexUInt32(a, "A"), TraceLoggingHexUInt32(b, "B"));
}

void ContractCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != lk::kContractQuery) return;
    auto* out = static_cast<lk::ContractInfo*>(argument2);
    if (!out || out->Size < sizeof(lk::ContractInfo)) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_publishedLock, &irql);
    *out = g_published;
    KeReleaseSpinLock(&g_publishedLock, irql);
}

void ContractLinkStart() {
    if (g_contractCallback) return;

    g_published.Sequence = static_cast<ULONG>(KeQueryInterruptTime() / 1000000);
    const NTSTATUS status = lk::OpenContractCallback(&g_contractCallback);
    if (!NT_SUCCESS(status)) g_contractCallback = nullptr;
    else g_contractRegistration = ExRegisterCallback(g_contractCallback, ContractCallback, nullptr);
    Log("ContractLink", static_cast<ULONG>(status), g_contractRegistration ? 1 : 0);
}

void ContractLinkStop() {
    if (g_contractRegistration) { ExUnregisterCallback(g_contractRegistration); g_contractRegistration = nullptr; }
    if (g_contractCallback) { ObDereferenceObject(g_contractCallback); g_contractCallback = nullptr; }
}

unsigned NowMs() { return static_cast<unsigned>(KeQueryInterruptTime() / 10000); }

void KickProxy(Owner* o) {
    InterlockedOr(&o->Pending, kPendingProxy);
    if (o->Work) WdfWorkItemEnqueue(o->Work);
}

void SnapshotCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != pl::kQuery) return;
    auto* out = static_cast<pl::PdSnapshot*>(argument2);
    if (!out || out->Size < sizeof(pl::PdSnapshot)) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_ppsLock, &irql);
    *out = g_pps;
    KeReleaseSpinLock(&g_ppsLock, irql);
}

void ControlCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != pl::kCommand) return;
    auto* c = static_cast<pl::PpsCommand*>(argument2);
    if (!c || c->Size < sizeof(pl::PpsCommand)) return;
    if (c->Version != pl::kPpsCommandVersion) { c->Result = pl::PpsCommandResult::BadVersion; return; }
    Owner* o = g_owner;
    if (!o || !o->PpsEnabled) { c->Result = pl::PpsCommandResult::NotEnabled; return; }
    if (!o->Ready) { c->Result = pl::PpsCommandResult::NoContract; return; }
    KIRQL irql;
    KeAcquireSpinLock(&o->ProxyLock, &irql);
    const pp::CommandResult r = pp::OnCommand(o->Proxy, static_cast<pp::CommandKind>(c->Kind), c->RequestId,
                                              c->ObjectPos, c->Millivolts, c->Milliamps);
    KeReleaseSpinLock(&o->ProxyLock, irql);
    c->Result = static_cast<pl::PpsCommandResult>(r);
    TraceLoggingWrite(g_typecTrace, "PpsCommand", TraceLoggingUInt32(static_cast<ULONG>(c->Kind), "Kind"),
                      TraceLoggingUInt32(c->RequestId, "RequestId"), TraceLoggingUInt32(c->ObjectPos, "ObjectPos"),
                      TraceLoggingUInt32(c->Millivolts, "Millivolts"), TraceLoggingUInt32(c->Milliamps, "Milliamps"),
                      TraceLoggingUInt32(static_cast<ULONG>(r), "Result"));
    if (r == pp::CommandResult::Queued) KickProxy(o);
}

void ApplyPumpState(Owner* o, const pl::PumpState* p) {
    const bool switching = pl::Valid(p) && p->Valid && p->Mode == pl::PumpMode::Switching;
    KIRQL irql;
    KeAcquireSpinLock(&o->ProxyLock, &irql);
    const bool changed = o->Proxy.PumpSwitching != switching;
    pp::OnPump(o->Proxy, switching);
    KeReleaseSpinLock(&o->ProxyLock, irql);
    if (changed) {
        TraceLoggingWrite(g_typecTrace, "PumpState", TraceLoggingUInt32(pl::Valid(p) ? static_cast<ULONG>(p->Mode) : 0xFF, "Mode"),
                          TraceLoggingBoolean(switching, "Switching"));
        KickProxy(o);
    }
}

void PumpCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != pl::kPublish) return;
    Owner* o = g_owner;
    if (o && o->PpsEnabled) ApplyPumpState(o, static_cast<const pl::PumpState*>(argument2));
}

void PpsLinkStart(Owner* o) {
    KeInitializeSpinLock(&g_ppsLock);
    g_pps.Size = sizeof(pl::PdSnapshot);
    g_pps.Version = pl::kPdSnapshotVersion;
    g_pps.Sequence = static_cast<ULONG>(KeQueryInterruptTime() / 1000000);
    g_pps.ProxyEnabled = o->PpsEnabled ? 1 : 0;
    NTSTATUS a = pl::OpenCallback(pl::kPdSnapshotName, &g_snapshotCallback);
    if (NT_SUCCESS(a)) g_snapshotRegistration = ExRegisterCallback(g_snapshotCallback, SnapshotCallback, nullptr);
    else g_snapshotCallback = nullptr;
    if (!o->PpsEnabled) { Log("PpsLink", static_cast<ULONG>(a), 0); return; }
    NTSTATUS b = pl::OpenCallback(pl::kPpsControlName, &g_controlCallback);
    if (NT_SUCCESS(b)) g_controlRegistration = ExRegisterCallback(g_controlCallback, ControlCallback, nullptr);
    else g_controlCallback = nullptr;
    NTSTATUS c = pl::OpenCallback(pl::kPumpStateName, &g_pumpCallback);
    if (NT_SUCCESS(c)) g_pumpRegistration = ExRegisterCallback(g_pumpCallback, PumpCallback, nullptr);
    else g_pumpCallback = nullptr;
    if (g_pumpCallback) {
        pl::PumpState state = {};
        state.Size = sizeof(pl::PumpState);
        state.Version = pl::kPumpStateVersion;
        ExNotifyCallback(g_pumpCallback, reinterpret_cast<PVOID>(pl::kQuery), &state);
        if (state.Valid) ApplyPumpState(o, &state);
    }
    Log("PpsLink", static_cast<ULONG>(b), static_cast<ULONG>(c));
}

void PpsLinkStop() {
    PVOID* regs[] = {&g_controlRegistration, &g_pumpRegistration, &g_snapshotRegistration};
    PCALLBACK_OBJECT* objs[] = {&g_controlCallback, &g_pumpCallback, &g_snapshotCallback};
    for (int i = 0; i < 3; ++i) {
        if (*regs[i]) { ExUnregisterCallback(*regs[i]); *regs[i] = nullptr; }
        if (*objs[i]) { ObDereferenceObject(*objs[i]); *objs[i] = nullptr; }
    }
}

void PublishPpsLocked(Owner* o, bool force, bool valid = true) {
    pl::PdSnapshot snap;
    KIRQL irql;
    KeAcquireSpinLock(&o->ProxyLock, &irql);
    const pp::State& s = o->Proxy;
    if (!force && s.Changes == o->ProxyPublished) { KeReleaseSpinLock(&o->ProxyLock, irql); return; }
    o->ProxyPublished = s.Changes;
    snap = pl::PdSnapshot{};
    snap.Size = sizeof(pl::PdSnapshot);
    snap.Version = pl::kPdSnapshotVersion;
    snap.Valid = valid ? 1 : 0;
    snap.ProxyEnabled = o->PpsEnabled ? 1 : 0;
    snap.AttachedAsSink = s.Sink ? 1 : 0;
    snap.PdRevision = s.PartnerRev;
    unsigned mv = 0, ma = 0;
    pp::FixedContract(s, &mv, &ma);
    snap.FixedContractMv = mv;
    snap.FixedContractMa = ma;
    snap.PdoCount = s.PdoCount;
    for (unsigned i = 0; i < s.PdoCount && i < pl::kMaxPdos; ++i) snap.Pdos[i] = s.Pdos[i];
    snap.PpsActive = s.PpsActive ? 1 : 0;
    snap.PpsObjectPos = s.PpsPos;
    snap.PpsMillivolts = s.PpsMv;
    snap.PpsMilliamps = s.PpsMa;
    snap.LastRequestId = s.LastRequestId;
    snap.LastStatus = static_cast<pl::PpsStatus>(s.LastStatus);
    snap.ExitPending = s.ExitPending ? 1 : 0;
    snap.Reason = static_cast<pl::ExitReason>(s.Why);
    KeReleaseSpinLock(&o->ProxyLock, irql);
    KeAcquireSpinLock(&g_ppsLock, &irql);
    snap.Sequence = ++g_pps.Sequence;
    g_pps = snap;
    KeReleaseSpinLock(&g_ppsLock, irql);
    TraceLoggingWrite(g_typecTrace, "PdSnapshot", TraceLoggingUInt32(snap.Sequence, "Sequence"),
                      TraceLoggingUInt32(snap.AttachedAsSink, "Sink"), TraceLoggingUInt32(snap.PdoCount, "PdoCount"),
                      TraceLoggingUInt32(snap.PpsActive, "PpsActive"), TraceLoggingUInt32(snap.PpsMillivolts, "PpsMillivolts"),
                      TraceLoggingUInt32(snap.PpsMilliamps, "PpsMilliamps"), TraceLoggingUInt32(snap.LastRequestId, "LastRequestId"),
                      TraceLoggingUInt32(static_cast<ULONG>(snap.LastStatus), "LastStatus"),
                      TraceLoggingUInt32(snap.ExitPending, "ExitPending"), TraceLoggingUInt32(static_cast<ULONG>(snap.Reason), "Reason"),
                      TraceLoggingUInt32(o->Proxy.TxOffset, "TxOffset"), TraceLoggingUInt32(o->Proxy.RxOffset, "RxOffset"));
    if (g_snapshotCallback) ExNotifyCallback(g_snapshotCallback, reinterpret_cast<PVOID>(pl::kPublish), &snap);
}

void PublishContractLocked(Owner* o, PCSTR why) {
    lk::ContractInfo info;
    KIRQL irql;
    KeAcquireSpinLock(&g_publishedLock, &irql);
    g_published.Sequence++;
    g_published.Active = o->Contract.Now.Active ? 1 : 0;
    g_published.Millivolts = o->Contract.Now.Millivolts;
    g_published.Milliamps = o->Contract.Now.Milliamps;
    info = g_published;
    KeReleaseSpinLock(&g_publishedLock, irql);
    TraceLoggingWrite(g_typecTrace, "Contract", TraceLoggingString(why, "Why"),
                      TraceLoggingUInt32(info.Sequence, "Sequence"), TraceLoggingUInt32(info.Active, "Active"),
                      TraceLoggingUInt32(info.Millivolts, "Millivolts"), TraceLoggingUInt32(info.Milliamps, "Milliamps"));
    if (g_contractCallback) ExNotifyCallback(g_contractCallback, reinterpret_cast<PVOID>(lk::kContractPublish), &info);
}

LONGLONG Now() {
    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);
    return now.QuadPart;
}

void StartVbusWatchLocked(Owner* o, PCSTR why) {
    o->VbusWatchUntil = Now() + static_cast<LONGLONG>(kVbusWatchMs) * 10000;
    WdfTimerStart(o->PollTimer, WDF_REL_TIMEOUT_IN_MS(kVbusPollMs));
    Log("VbusWatch", kVbusWatchMs, why[0]);
}

void RearmPollLocked(Owner* o) {
    if (tc::NeedsPoll(o->Tc)) WdfTimerStart(o->PollTimer, WDF_REL_TIMEOUT_IN_MS(kPollMs));
    else if (Now() < o->VbusWatchUntil) WdfTimerStart(o->PollTimer, WDF_REL_TIMEOUT_IN_MS(kVbusPollMs));
}

NTSTATUS ReadSnapshot(Owner* o, tc::Snapshot* p) {
    *p = tc::Snapshot{};
    unsigned v[7] = {};
    const unsigned addr[7] = {r::kTypecMiscStatus, r::kTypecSnkStatus, r::kTypecSrcStatus, r::kTypecSmStatus,
                              r::kTypecModeCfg, r::kDcdcOtgStatus, r::kUsbinIntRtSts};
    for (int i = 0; i < 7; ++i) {
        NTSTATUS status = PmicRead(o->Pmic, addr[i], &v[i]);
        if (!NT_SUCCESS(status)) return status;
    }
    *p = tc::Snapshot{true, v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
    return STATUS_SUCCESS;
}

NTSTATUS Exec(Owner* o, const tc::Ops& ops) {
    if (ops.Overflow) { Log("OpsOverflow", ops.Count, 0); return STATUS_INTERNAL_ERROR; }
    for (unsigned i = 0; i < ops.Count; ++i) {
        NTSTATUS status = PmicWrite(o->Pmic, ops.Items[i].Addr, ops.Items[i].Value);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Exec(Owner* o, const pd::Steps& steps) {
    if (steps.Overflow) { Log("StepsOverflow", steps.Count, 0); return STATUS_INTERNAL_ERROR; }
    for (unsigned i = 0; i < steps.Count; ++i) {
        unsigned ignored = 0;
        NTSTATUS status = steps.Items[i].K == pd::Kind::Write
            ? PmicWrite(o->Pmic, steps.Items[i].Addr, steps.Items[i].Value)
            : PmicRead(o->Pmic, steps.Items[i].Addr, &ignored);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

pd::Alert ProxyPumpLocked(Owner* o) {
    if (!o->PpsEnabled || !o->Ready) return pd::Alert::None;
    pd::Alert late = pd::Alert::None;
    const unsigned now = NowMs();
    unsigned nextMs = pp::kNoDeadline;
    KIRQL irql;
    KeAcquireSpinLock(&o->ProxyLock, &irql);
    const pp::Send send = pp::Next(o->Proxy, now, !o->HardResetPending, &nextMs);
    KeReleaseSpinLock(&o->ProxyLock, irql);
    if (send.Valid) {
        pd::TxBuffer b{};
        b.ByteCount = send.ByteCount;
        b.Header[0] = send.Header[0];
        b.Header[1] = send.Header[1];
        RtlCopyMemory(b.Data, send.Data, sizeof(b.Data));
        unsigned rxAck = 0;
        pd::Steps steps;
        NTSTATUS status = PmicRead(o->Pmic, r::kPdphyRxAcknowledge, &rxAck);
        pd::TxResult res = NT_SUCCESS(status) ? pd::OnTransmit(send.Transmit, b, rxAck, steps) : pd::TxResult::Invalid;
        if (res == pd::TxResult::Started) {
            if (ct::OnSent(o->Contract, b.Header[0] | (b.Header[1] << 8), b.Data, b.ByteCount - 2))
                PublishContractLocked(o, "Sent");
            status = Exec(o, steps);
            if (!NT_SUCCESS(status)) res = pd::TxResult::Invalid;
        }
        if (res != pd::TxResult::Started) {

            KeAcquireSpinLock(&o->ProxyLock, &irql);
            pp::OnTransmitResult(o->Proxy, false, true, now);
            KeReleaseSpinLock(&o->ProxyLock, irql);
            if (!send.Injected) late = res == pd::TxResult::Discarded ? pd::Alert::TransmitDiscarded : pd::Alert::TransmitFailed;
        }
        Log(send.Injected ? "PpsInject" : "PpsRelease", b.Header[0] | (b.Header[1] << 8) | (static_cast<ULONG>(res) << 16),
            b.Data[0] | (b.Data[1] << 8) | (b.Data[2] << 16) | (static_cast<ULONG>(b.Data[3]) << 24));
    }
    if (nextMs != pp::kNoDeadline) {
        const int delta = static_cast<int>(nextMs - now);
        WdfTimerStart(o->ProxyTimer, WDF_REL_TIMEOUT_IN_MS(delta > 1 ? delta : 1));
    }
    PublishPpsLocked(o, false);
    return late;
}

ULONG ElapsedMs(Owner* o) {
    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);
    const LONGLONG ms = (now.QuadPart - o->LastEvaluate.QuadPart) / 10000;
    o->LastEvaluate = now;
    return ms < 0 ? 0 : ms > 1000 ? 1000 : static_cast<ULONG>(ms);
}

void EvaluateLocked(Owner* o, tc::Alerts& alerts) {
    tc::Snapshot p;
    const NTSTATUS readStatus = ReadSnapshot(o, &p);
    if (!NT_SUCCESS(readStatus)) {
        Log("SnapshotFailed", static_cast<ULONG>(readStatus), 0);
        RearmPollLocked(o);
        return;
    }

    if ((!tc::Attached(p) || tc::AttachedAsSource(p)) && ct::Reset(o->Contract))
        PublishContractLocked(o, tc::Attached(p) ? "SourceRole" : "Detach");
    if (o->PpsEnabled) {
        KIRQL irql;
        KeAcquireSpinLock(&o->ProxyLock, &irql);
        pp::OnPort(o->Proxy, tc::Attached(p) && !tc::AttachedAsSource(p), tc::SnkLevel(p.Snk) == tc::kCcSnk3000);
        KeReleaseSpinLock(&o->ProxyLock, irql);
    }
    const tc::Source before = o->Tc.Src;
    const tc::Ops ops = tc::OnEvaluate(o->Config, p, ElapsedMs(o), o->Tc, alerts);

    if (before == tc::Source::Enabling)
        Log(o->Tc.Src == tc::Source::On ? "BoostConfirmed" : o->Tc.Src == tc::Source::Off ? "BoostTimeout" : "BoostPoll",
            (p.Misc << 16) | (p.Sm << 8) | p.Otg, o->Tc.EnablingMs);
    const NTSTATUS status = Exec(o, ops);
    if (!NT_SUCCESS(status)) Log("EvaluateWriteFailed", static_cast<ULONG>(status), ops.Count);
    if (alerts.Cc || alerts.Power)
        Log("Evaluated", (alerts.CcImage << 8) | alerts.PowerImage, (p.Usbin << 16) | (p.Misc << 8) | p.Sm);
    if (alerts.Cc) o->VbusWatchUntil = Now() + static_cast<LONGLONG>(kVbusWatchMs) * 10000;
    RearmPollLocked(o);
}

pd::Alert PdIrqLocked(Owner* o, pd::Irq irq, pd::RxBuffer* rx, bool* haveRx) {
    const pd::IrqAction a = pd::OnIrq(irq, o->FrameFilter, o->HardResetPending);
    if (irq == pd::Irq::SigTx) o->HardResetPending = false;
    if (irq == pd::Irq::SigRx) {
        if (ct::Reset(o->Contract)) PublishContractLocked(o, "HardResetReceived");
        StartVbusWatchLocked(o, "Rx");
    }
    KIRQL irql;
    pd::Alert report = a.Report;
    if (o->PpsEnabled && (irq == pd::Irq::SigRx || irq == pd::Irq::MsgTx || irq == pd::Irq::MsgTxFailed ||
                          irq == pd::Irq::MsgTxDiscarded)) {
        KeAcquireSpinLock(&o->ProxyLock, &irql);
        if (irq == pd::Irq::SigRx) pp::OnHardReset(o->Proxy);
        else if (!pp::OnTransmitResult(o->Proxy, irq == pd::Irq::MsgTx, irq == pd::Irq::MsgTxDiscarded, NowMs()))
            report = pd::Alert::None;
        KeReleaseSpinLock(&o->ProxyLock, irql);
    }
    if (irq != pd::Irq::MsgRx) Log("PdIrq", static_cast<ULONG>(irq), static_cast<ULONG>(report));
    Exec(o, a.Writes);
    if (!a.ReadFrame) return report;

    unsigned size = 0, rxStatus = 0;
    unsigned char frame[32] = {};
    NTSTATUS status = PmicRead(o->Pmic, r::kPdphyRxSize, &size);
    if (NT_SUCCESS(status)) status = PmicRead(o->Pmic, r::kPdphyRxStatus, &rxStatus);
    const unsigned length = NT_SUCCESS(status) ? pd::RxReadLength(size) : 0;
    for (unsigned i = 0; NT_SUCCESS(status) && i < length; ++i) {
        unsigned v = 0;
        status = PmicRead(o->Pmic, r::kPdphyRxBuffer + i, &v);
        frame[i] = static_cast<unsigned char>(v);
    }
    if (!NT_SUCCESS(status)) Log("RxReadFailed", static_cast<ULONG>(status), size);
    else if (!length) Log("RxSizeInvalid", size, rxStatus);
    else {
        *haveRx = pd::BuildRxBuffer(rxStatus, frame, length, *rx);
        Log(*haveRx ? "RxFrame" : "RxFrameDropped", (size << 8) | rxStatus, frame[0] | (frame[1] << 8));
        if (*haveRx && ct::OnReceived(o->Contract, rx->Header[0] | (rx->Header[1] << 8), rx->Data, length - 2))
            PublishContractLocked(o, "Received");
        if (*haveRx && o->PpsEnabled) {
            KeAcquireSpinLock(&o->ProxyLock, &irql);
            const pp::RxVerdict v = pp::OnReceive(o->Proxy, rx->Header, rx->Data, length - 2, NowMs());
            KeReleaseSpinLock(&o->ProxyLock, irql);
            if (v == pp::RxVerdict::Consume) {
                Log("PpsConsumed", frame[0] | (frame[1] << 8), 0);
                *haveRx = false;
            }
        }
    }
    Exec(o, pd::AcknowledgeRx());
    return *haveRx ? pd::Alert::ReceiveMessage : pd::Alert::None;
}

void RaiseAlert(Owner* o, const tc::Alerts& alerts, pd::Alert pdAlert, const pd::RxBuffer* rx) {
    if (!o->Port.Alert) return;
    if (!alerts.Cc && !alerts.Power && !alerts.Fault && pdAlert == pd::Alert::None) return;
    o->Port.Alert(o->Port.Context, alerts, pdAlert, rx);
}

void EvtWork(WDFWORKITEM item) {
    Owner* o = g_owner;
    UNREFERENCED_PARAMETER(item);
    if (!o) return;
    for (LONG pending = InterlockedExchange(&o->Pending, 0); pending; pending = InterlockedExchange(&o->Pending, 0)) {
        if (!o->Ready) continue;

        static const pd::Irq kOrder[] = {pd::Irq::SigRx, pd::Irq::MsgRx, pd::Irq::MsgRxDiscarded, pd::Irq::SigTx,
                                         pd::Irq::MsgTx, pd::Irq::MsgTxFailed, pd::Irq::MsgTxDiscarded};
        for (pd::Irq irq : kOrder) {
            const ULONG resource = kIrqPdphyFirst + 6 - static_cast<ULONG>(irq);
            if (!(pending & (1L << resource))) continue;
            pd::RxBuffer rx{};
            bool haveRx = false;
            WdfWaitLockAcquire(o->Lock, nullptr);
            const pd::Alert alert = PdIrqLocked(o, irq, &rx, &haveRx);
            const pd::Alert late = ProxyPumpLocked(o);
            WdfWaitLockRelease(o->Lock);
            RaiseAlert(o, tc::Alerts{}, alert, haveRx ? &rx : nullptr);
            RaiseAlert(o, tc::Alerts{}, late, nullptr);
        }
        const LONG typecBits = (1L << kIrqCc) | (1L << kIrqOtgOc) | (1L << kIrqSwitcherOk) | (1L << kIrqUsbinOv) | kPendingPoll;
        if (pending & typecBits) {
            if (pending & (1L << kIrqOtgOc)) Log("OtgOverCurrent", 0, 0);
            if (pending & (1L << kIrqUsbinOv)) Log("UsbinOverVoltage", 0, 0);
            tc::Alerts alerts{};
            WdfWaitLockAcquire(o->Lock, nullptr);
            EvaluateLocked(o, alerts);
            const pd::Alert late = ProxyPumpLocked(o);
            WdfWaitLockRelease(o->Lock);
            RaiseAlert(o, alerts, late, nullptr);
        } else if (pending & kPendingProxy) {
            WdfWaitLockAcquire(o->Lock, nullptr);
            const pd::Alert late = ProxyPumpLocked(o);
            WdfWaitLockRelease(o->Lock);
            RaiseAlert(o, tc::Alerts{}, late, nullptr);
        }
    }
}

BOOLEAN EvtIsr(WDFINTERRUPT interrupt, ULONG) {
    Owner* o = g_owner;
    if (!o) return TRUE;
    InterlockedOr(&o->Pending, 1L << IrqContextOf(interrupt)->Index);
    WdfWorkItemEnqueue(o->Work);
    return TRUE;
}

void EvtPollTimer(WDFTIMER) {
    Owner* o = g_owner;
    if (!o) return;
    InterlockedOr(&o->Pending, kPendingPoll);
    WdfWorkItemEnqueue(o->Work);
}

void EvtProxyTimer(WDFTIMER) {
    Owner* o = g_owner;
    if (o) KickProxy(o);
}

NTSTATUS EvtPrepareHardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
    Owner* o = g_owner;
    NTSTATUS status = PmicOpen(device, &o->Pmic);
    if (!NT_SUCCESS(status)) return status;
    tc::Snapshot p;
    status = ReadSnapshot(o, &p);
    Log("StartSnapshot", static_cast<ULONG>(status), (p.Misc << 24) | (p.Mode << 16) | (p.Otg << 8) | p.Sm);
    if (!NT_SUCCESS(status)) return status;
    if (tc::NeedsBootRecovery(p)) {

        NTSTATUS rs = Exec(o, tc::BootRecoveryDisable(o->Config));
        LARGE_INTEGER delay;
        delay.QuadPart = -static_cast<LONGLONG>(tc::kBootRecoveryMs) * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        const NTSTATUS re = Exec(o, tc::BootRecoveryEnable(o->Config));
        if (NT_SUCCESS(rs)) rs = re;
        const NTSTATUS again = ReadSnapshot(o, &p);
        Log("BootRecovery", static_cast<ULONG>(rs), (p.Misc << 24) | (p.Mode << 16) | (p.Usbin << 8) | p.Sm);
        if (!NT_SUCCESS(again)) return again;
    }
    WdfWaitLockAcquire(o->Lock, nullptr);
    o->Tc = tc::InitialState();
    if (ct::Reset(o->Contract)) PublishContractLocked(o, "Started");
    {
        KIRQL irql;
        KeAcquireSpinLock(&o->ProxyLock, &irql);
        const bool switching = o->Proxy.PumpSwitching;
        o->Proxy = pp::Initial(o->PpsEnabled);
        pp::OnPump(o->Proxy, switching);
        KeReleaseSpinLock(&o->ProxyLock, irql);
    }
    PublishPpsLocked(o, true);
    status = Exec(o, tc::OnInit(o->Config, p, o->Tc));
    if (NT_SUCCESS(status)) status = Exec(o, pd::Enable());
    o->FrameFilter = r::kFilterSop | r::kFilterHardReset;
    KeQuerySystemTimePrecise(&o->LastEvaluate);
    WdfWaitLockRelease(o->Lock);
    Log("Started", static_cast<ULONG>(status), o->Config.SourceAllowed);
    if (NT_SUCCESS(status)) {
        InterlockedExchange(&o->Ready, 1);
        ClearBootAttempts();
    }
    return status;
}

void EvtOwnerCleanup(WDFOBJECT) {
    ContractLinkStop();
    PpsLinkStop();
    Owner* o = g_owner;
    g_owner = nullptr;
    if (o) ExFreePoolWithTag(o, 'cTbN');
}

NTSTATUS EvtReleaseHardware(WDFDEVICE, WDFCMRESLIST) {
    Owner* o = g_owner;
    InterlockedExchange(&o->Ready, 0);
    WdfTimerStop(o->PollTimer, TRUE);
    if (o->ProxyTimer) WdfTimerStop(o->ProxyTimer, TRUE);
    if (o->Pmic) {
        WdfWaitLockAcquire(o->Lock, nullptr);
        if (o->Tc.Src != tc::Source::Off) {
            tc::Ops off{};
            tc::BoostOff(off);
            Exec(o, off);
            o->Tc.Src = tc::Source::Off;
        }
        Exec(o, pd::Disable());
        if (ct::Reset(o->Contract)) PublishContractLocked(o, "Stopped");
        if (o->PpsEnabled) {
            KIRQL irql;
            KeAcquireSpinLock(&o->ProxyLock, &irql);
            pp::OnHardReset(o->Proxy);
            pp::OnPort(o->Proxy, false, false);
            KeReleaseSpinLock(&o->ProxyLock, irql);
        }
        PublishPpsLocked(o, true, false);
        WdfWaitLockRelease(o->Lock);
    }
    Log("Stopped", 0, 0);
    return STATUS_SUCCESS;
}

}

bool OwnerReady() { return g_owner && g_owner->Ready; }

NTSTATUS OwnerAttachPort(const PortCallbacks& callbacks) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    g_owner->Port = callbacks;

    g_owner->Tc.ReportedCc = 0xFF;
    g_owner->Tc.ReportedPower = 0xFF;
    WdfWaitLockRelease(g_owner->Lock);

    InterlockedOr(&g_owner->Pending, 1L << kIrqCc);
    WdfWorkItemEnqueue(g_owner->Work);
    return STATUS_SUCCESS;
}

void OwnerDetachPort() {
    if (!g_owner) return;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    g_owner->Port = PortCallbacks{};
    WdfWaitLockRelease(g_owner->Lock);
    WdfWorkItemFlush(g_owner->Work);
}

NTSTATUS OwnerGetStatus(unsigned* cc, unsigned* power, unsigned* fault) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    Owner* o = g_owner;
    tc::Snapshot p;
    WdfWaitLockAcquire(o->Lock, nullptr);
    const NTSTATUS readStatus = ReadSnapshot(o, &p);
    if (NT_SUCCESS(readStatus)) {
        *cc = tc::BuildCcStatus(p, o->Tc);
        *power = tc::BuildPowerStatus(p, o->Tc);
    } else {

        Log("StatusReadFailed", static_cast<ULONG>(readStatus), 0);
        *cc = o->Tc.ReportedCc != 0xFF ? o->Tc.ReportedCc : tc::BuildCcStatus(tc::Snapshot{}, o->Tc);
        *power = o->Tc.ReportedPower != 0xFF ? o->Tc.ReportedPower : tc::BuildPowerStatus(tc::Snapshot{}, o->Tc);
    }
    *fault = o->Tc.Fault;
    tc::OnClearFault(o->Tc.Fault, o->Tc);
    WdfWaitLockRelease(o->Lock);
    return STATUS_SUCCESS;
}

void OwnerGetControl(unsigned* tcpcControl, unsigned* roleControl, unsigned* faultControl, unsigned* powerControl) {
    Owner* o = g_owner;
    if (!o) { *tcpcControl = *roleControl = *faultControl = *powerControl = 0; return; }
    WdfWaitLockAcquire(o->Lock, nullptr);
    *tcpcControl = o->TcpcControl;
    *roleControl = o->Tc.RoleControl;
    *faultControl = o->FaultControl;
    *powerControl = o->PowerControl;
    WdfWaitLockRelease(o->Lock);
}

NTSTATUS OwnerSetRoleControl(unsigned roleControl) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    const NTSTATUS status = Exec(g_owner, tc::OnSetRoleControl(g_owner->Config, roleControl, g_owner->Tc));
    WdfWaitLockRelease(g_owner->Lock);
    return status;
}

NTSTATUS OwnerSetSimpleControl(ULONG controlType, unsigned value) {
    if (!g_owner) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    switch (controlType) {
    case 1: g_owner->TcpcControl = value; break;
    case 3: g_owner->FaultControl = value; break;
    case 4: g_owner->PowerControl = value; break;
    default: break;
    }
    WdfWaitLockRelease(g_owner->Lock);
    return STATUS_SUCCESS;
}

NTSTATUS OwnerCommand(unsigned command, bool* supported, tc::Alerts* alerts) {
    *alerts = tc::Alerts{};
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    Owner* o = g_owner;
    tc::Snapshot p;
    WdfWaitLockAcquire(o->Lock, nullptr);
    if (!NT_SUCCESS(ReadSnapshot(o, &p))) p = tc::Snapshot{};
    const tc::CommandResult res = tc::OnCommand(o->Config, static_cast<tc::Command>(command), p, o->Tc);
    *supported = res.Supported;
    NTSTATUS status = Exec(o, res.Writes);
    if (tc::NeedsPoll(o->Tc)) {
        KeQuerySystemTimePrecise(&o->LastEvaluate);
        WdfTimerStart(o->PollTimer, WDF_REL_TIMEOUT_IN_MS(kPollMs));
    }
    EvaluateLocked(o, *alerts);
    WdfWaitLockRelease(o->Lock);
    Log("Command", command, (res.Supported ? 0x100 : 0) | res.Writes.Count);
    return status;
}

NTSTATUS OwnerSetMessageHeaderInfo(unsigned info) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    const NTSTATUS status = Exec(g_owner, pd::OnSetMessageHeaderInfo(info));
    WdfWaitLockRelease(g_owner->Lock);
    return status;
}

NTSTATUS OwnerSetReceiveDetect(unsigned receiveDetect) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    const pd::Steps steps = pd::OnSetReceiveDetect(receiveDetect);
    g_owner->FrameFilter = steps.Items[0].Value;
    const NTSTATUS status = Exec(g_owner, steps);
    WdfWaitLockRelease(g_owner->Lock);
    return status;
}

NTSTATUS OwnerSetTransmitBuffer(const pd::TxBuffer& buffer) {
    if (!g_owner) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(g_owner->Lock, nullptr);
    g_owner->Tx = buffer;
    WdfWaitLockRelease(g_owner->Lock);
    return STATUS_SUCCESS;
}

NTSTATUS OwnerTransmit(unsigned transmit, pd::TxResult* result) {
    if (!OwnerReady()) return STATUS_DEVICE_NOT_READY;
    Owner* o = g_owner;
    unsigned rxAck = 0;
    pd::Steps steps;
    WdfWaitLockAcquire(o->Lock, nullptr);
    NTSTATUS status = PmicRead(o->Pmic, r::kPdphyRxAcknowledge, &rxAck);
    if (NT_SUCCESS(status)) {

        pd::TxBuffer wire = o->Tx;
        bool held = false, proxied = false;
        if (o->PpsEnabled && pd::TxType(transmit) == pd::kTxSop && pd::ValidTxBuffer(o->Tx)) {
            unsigned header = 0;
            KIRQL irql;
            KeAcquireSpinLock(&o->ProxyLock, &irql);
            held = pp::OnFrameworkTransmit(o->Proxy, transmit, o->Tx.ByteCount, o->Tx.Header, o->Tx.Data, NowMs(),
                                           &header) == pp::TxVerdict::Hold;
            KeReleaseSpinLock(&o->ProxyLock, irql);
            wire.Header[0] = header & 0xFF;
            wire.Header[1] = (header >> 8) & 0xFF;
            proxied = !held;
        }
        *result = held ? pd::TxResult::Started : pd::OnTransmit(transmit, wire, rxAck, steps);
        if (held) {
            Log("PpsHeld", o->Tx.Header[0] | (o->Tx.Header[1] << 8), transmit);
        } else if (*result == pd::TxResult::Started) {
            bool changed = false;
            if (pd::TxType(transmit) == pd::kTxHardReset) {
                o->HardResetPending = true;
                changed = ct::Reset(o->Contract);
                StartVbusWatchLocked(o, "Tx");
                if (o->PpsEnabled) {
                    KIRQL irql;
                    KeAcquireSpinLock(&o->ProxyLock, &irql);
                    pp::OnHardReset(o->Proxy);
                    KeReleaseSpinLock(&o->ProxyLock, irql);
                }
            } else {
                changed = ct::OnSent(o->Contract, wire.Header[0] | (wire.Header[1] << 8), wire.Data,
                                     wire.ByteCount >= 2 ? wire.ByteCount - 2 : 0);
            }
            if (changed) PublishContractLocked(o, "Sent");
            status = Exec(o, steps);
        }
        if (proxied && (*result != pd::TxResult::Started || !NT_SUCCESS(status))) {
            KIRQL irql;
            KeAcquireSpinLock(&o->ProxyLock, &irql);
            pp::OnTransmitResult(o->Proxy, false, true, NowMs());
            KeReleaseSpinLock(&o->ProxyLock, irql);
        }
        if (o->PpsEnabled) PublishPpsLocked(o, false);
    }
    WdfWaitLockRelease(o->Lock);
    if (!NT_SUCCESS(status)) *result = pd::TxResult::Invalid;
    return status;
}

NTSTATUS OwnerDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    if (g_owner) return STATUS_DEVICE_ALREADY_ATTACHED;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    auto* o = static_cast<Owner*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Owner), 'cTbN'));
    if (!o) return STATUS_INSUFFICIENT_RESOURCES;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.EvtCleanupCallback = EvtOwnerCleanup;
    WDFDEVICE device = nullptr;
    NTSTATUS status = WdfDeviceCreate(&init, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(o, 'cTbN'); return status; }
    g_owner = o;
    o->Device = device;
    o->Config = ReadParam(driver, L"Stage", kStageDefault) == 2 ? tc::kStage2 : tc::kStage3;
    o->Tc = tc::InitialState();
    o->Contract = ct::Initial();
    KeInitializeSpinLock(&g_publishedLock);
    ContractLinkStart();
    o->PpsEnabled = ReadParam(driver, L"PpsEnable", 0) == 1;
    KeInitializeSpinLock(&o->ProxyLock);
    o->Proxy = pp::Initial(o->PpsEnabled);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &o->Lock);
    if (NT_SUCCESS(status)) {
        WDF_WORKITEM_CONFIG work;
        WDF_WORKITEM_CONFIG_INIT(&work, EvtWork);
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfWorkItemCreate(&work, &attributes, &o->Work);
    }
    if (NT_SUCCESS(status)) {
        WDF_TIMER_CONFIG timer;
        WDF_TIMER_CONFIG_INIT(&timer, EvtPollTimer);
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfTimerCreate(&timer, &attributes, &o->PollTimer);
    }
    if (NT_SUCCESS(status)) {
        WDF_TIMER_CONFIG timer;
        WDF_TIMER_CONFIG_INIT(&timer, EvtProxyTimer);
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfTimerCreate(&timer, &attributes, &o->ProxyTimer);
    }

    for (ULONG i = 0; NT_SUCCESS(status) && i < kIrqCount; ++i) {
        WDF_INTERRUPT_CONFIG config;
        WDF_INTERRUPT_CONFIG_INIT(&config, EvtIsr, nullptr);
        config.PassiveHandling = TRUE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IrqContext);
        status = WdfInterruptCreate(device, &config, &attributes, &o->Irq[i]);
        if (NT_SUCCESS(status)) IrqContextOf(o->Irq[i])->Index = i;
    }
    if (!NT_SUCCESS(status)) return status;
    PpsLinkStart(o);
    Log("OwnerAdded", o->Config.SourceAllowed ? 3 : 2, o->PpsEnabled ? 1 : 0);
    return STATUS_SUCCESS;
}

}
