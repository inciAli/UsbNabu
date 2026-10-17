#include "nabutypec.h"

namespace nabutypec {

namespace {

namespace r = nabu::pm8150b;
namespace tc = nabu::tcpc;
namespace pd = nabu::pdphy;
namespace ct = nabu::contract;
namespace lk = nabu::link;

constexpr ULONG kPollMs = 5;
constexpr LONG kPendingPoll = 1L << 16;

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
};

Owner* g_owner = nullptr;

KSPIN_LOCK g_publishedLock = 0;
lk::ContractInfo g_published = {sizeof(lk::ContractInfo), lk::kContractVersion, 0, 0, 0, 0};
PCALLBACK_OBJECT g_contractCallback = nullptr;
PVOID g_contractRegistration = nullptr;

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
    if (irq != pd::Irq::MsgRx) Log("PdIrq", static_cast<ULONG>(irq), static_cast<ULONG>(a.Report));
    Exec(o, a.Writes);
    if (!a.ReadFrame) return a.Report;

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
            WdfWaitLockRelease(o->Lock);
            RaiseAlert(o, tc::Alerts{}, alert, haveRx ? &rx : nullptr);
        }
        const LONG typecBits = (1L << kIrqCc) | (1L << kIrqOtgOc) | (1L << kIrqSwitcherOk) | (1L << kIrqUsbinOv) | kPendingPoll;
        if (pending & typecBits) {
            if (pending & (1L << kIrqOtgOc)) Log("OtgOverCurrent", 0, 0);
            if (pending & (1L << kIrqUsbinOv)) Log("UsbinOverVoltage", 0, 0);
            tc::Alerts alerts{};
            WdfWaitLockAcquire(o->Lock, nullptr);
            EvaluateLocked(o, alerts);
            WdfWaitLockRelease(o->Lock);
            RaiseAlert(o, alerts, pd::Alert::None, nullptr);
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
    Owner* o = g_owner;
    g_owner = nullptr;
    if (o) ExFreePoolWithTag(o, 'cTbN');
}

NTSTATUS EvtReleaseHardware(WDFDEVICE, WDFCMRESLIST) {
    Owner* o = g_owner;
    InterlockedExchange(&o->Ready, 0);
    WdfTimerStop(o->PollTimer, TRUE);
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
        *result = pd::OnTransmit(transmit, o->Tx, rxAck, steps);
        if (*result == pd::TxResult::Started) {
            bool changed = false;
            if (pd::TxType(transmit) == pd::kTxHardReset) {
                o->HardResetPending = true;
                changed = ct::Reset(o->Contract);
                StartVbusWatchLocked(o, "Tx");
            } else {
                changed = ct::OnSent(o->Contract, o->Tx.Header[0] | (o->Tx.Header[1] << 8), o->Tx.Data,
                                     o->Tx.ByteCount >= 2 ? o->Tx.ByteCount - 2 : 0);
            }
            if (changed) PublishContractLocked(o, "Sent");
            status = Exec(o, steps);
        }
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

    for (ULONG i = 0; NT_SUCCESS(status) && i < kIrqCount; ++i) {
        WDF_INTERRUPT_CONFIG config;
        WDF_INTERRUPT_CONFIG_INIT(&config, EvtIsr, nullptr);
        config.PassiveHandling = TRUE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IrqContext);
        status = WdfInterruptCreate(device, &config, &attributes, &o->Irq[i]);
        if (NT_SUCCESS(status)) IrqContextOf(o->Irq[i])->Index = i;
    }
    if (!NT_SUCCESS(status)) return status;
    Log("OwnerAdded", o->Config.SourceAllowed ? 3 : 2, 0);
    return STATUS_SUCCESS;
}

}
