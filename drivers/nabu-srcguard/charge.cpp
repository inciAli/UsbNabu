#include "charge.h"

#include <TraceLoggingProvider.h>

#include "charge_policy.h"
#include "pmic.h"
#include "../nabu-typec/contract_link.h"
#include "../shared/pps_link.h"

TRACELOGGING_DECLARE_PROVIDER(g_trace);

namespace srcguard {
namespace {

const GUID kBatteryClassGuid = {0x72631E54, 0x78A4, 0x11D0,
                                {0xBC, 0xF7, 0x00, 0xAA, 0x00, 0xB7, 0xB3, 0x2A}};

constexpr ULONG kIoctlBatteryQueryTag =
    CTL_CODE(FILE_DEVICE_BATTERY, 0x10, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr ULONG kIoctlBatteryQueryInfo =
    CTL_CODE(FILE_DEVICE_BATTERY, 0x11, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr ULONG kBatteryTemperatureLevel = 2;
constexpr ULONG kMinPlausibleDeciK = 2000;
constexpr ULONG kMaxPlausibleDeciK = 4000;
constexpr int kZeroCelsiusDeciK = 2731;

constexpr LONGLONG kMs = 10000;
constexpr LONGLONG kPollPeriod = 10000 * kMs;
constexpr LONGLONG kRetryPeriod = 2000 * kMs;
constexpr ULONG kNoPublisher = 0;

struct BatteryQueryInfo {
    ULONG BatteryTag;
    ULONG InformationLevel;
    ULONG AtRate;
};
static_assert(sizeof(BatteryQueryInfo) == 12, "BATTERY_QUERY_INFORMATION size");
static_assert(sizeof(unsigned) == sizeof(ULONG), "ULONG size");

struct SharedState {
    bool TempValid;
    int TempDeciC;
    bool SinkAttached;
    unsigned IclRequestMaxMa;
    bool HaveParams;
    unsigned LastParams[5];
    bool HaveMode;
    unsigned LastMode[4];
    bool HaveApplied;
    charge::Decision Applied;
    bool HaveContract;
    ULONG ContractSequence;
    bool ContractActive;
    unsigned ContractMv;
    unsigned ContractMa;
    bool HavePump;
    ULONG PumpSequence;
    bool PumpValid;
    bool PumpSwitching;
    unsigned PumpHoldMa;
    ULONGLONG PumpAt;
};

struct Worker {
    WDFDEVICE Device;
    WDFIOTARGET Extension;
    volatile LONG MutexReady;
    volatile LONG StopRequested;
    KEVENT Started;
    KEVENT Stopped;
    KEVENT Wake;
    PKTHREAD Thread;
    volatile LONG ExactPending;
    WDFIOTARGET Pmic;
    PCALLBACK_OBJECT ContractObject;
    PVOID ContractRegistration;
    PCALLBACK_OBJECT PumpObject;
    PVOID PumpRegistration;
    PCALLBACK_OBJECT BatteryObject;
    PVOID BatteryRegistration;
};

KSPIN_LOCK g_stateLock = 0;
KMUTEX g_sendMutex = {};
SharedState g_state = {};
Worker g_worker = {};
volatile LONG g_started = 0;
volatile LONG g_stopped = 0;

unsigned OwnerMode() { return GuardReadParam(L"ChargeOwner", charge::kOwnerDefault); }

ULONG PumpAgeMsLocked() {
    const ULONGLONG ms = (KeQueryInterruptTime() - g_state.PumpAt) / 10000;
    return ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<ULONG>(ms);
}

charge::Inputs InputsLocked(unsigned mode) {
    const bool hold = g_state.HavePump &&
        charge::PumpHoldWanted(g_state.PumpValid, g_state.PumpSwitching, g_state.PumpHoldMa, PumpAgeMsLocked());
    return charge::Inputs{
        g_state.TempValid,
        g_state.TempDeciC,
        g_state.SinkAttached,
        g_state.IclRequestMaxMa,
        g_state.ContractActive,
        g_state.ContractMa,
        mode,
        hold,
        hold ? g_state.PumpHoldMa : 0,
        g_state.ContractMv,
    };
}

void LogCharge(PCSTR stage, ULONG status, const charge::Inputs& in,
               const unsigned orig[3], const charge::Decision& d) {
    TraceLoggingWrite(g_trace, "Charge",
        TraceLoggingString(stage, "Stage"),
        TraceLoggingHexUInt32(status, "Status"),
        TraceLoggingInt32(in.TempDeciC, "TempDeciC"),
        TraceLoggingUInt32(in.IclRequestMaxMa, "IclMaxMa"),
        TraceLoggingHexUInt32(orig[0], "Orig1"),
        TraceLoggingHexUInt32(orig[1], "Orig2"),
        TraceLoggingHexUInt32(orig[2], "Orig3"),
        TraceLoggingBoolean(d.Active, "Active"),
        TraceLoggingBoolean(d.Stop, "Stop"),
        TraceLoggingHexUInt32(d.Fcc, "Fcc"),
        TraceLoggingHexUInt32(d.Float, "Float"),
        TraceLoggingHexUInt32(d.Icl, "Icl"),
        TraceLoggingUInt32(in.OwnerMode, "Owner"),
        TraceLoggingUInt32(in.ContractActive ? in.ContractMa : 0, "ContractMa"),
        TraceLoggingBoolean(d.IclFromContract, "FromContract"),
        TraceLoggingHexUInt32(d.FccExact, "FccExact"),
        TraceLoggingHexUInt32(d.FloatExact, "FloatExact"),
        TraceLoggingHexUInt32(d.IclExact, "IclExact"),
        TraceLoggingBoolean(d.PumpHold, "PumpHold"),
        TraceLoggingUInt32(in.PumpHoldMa, "PumpHoldMa"),
        TraceLoggingUInt32(in.ContractMv, "ContractMv"));
}

void NoteContract(const nabu::link::ContractInfo* info, PCSTR source) {
    if (!nabu::link::ValidInfo(info)) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_stateLock, &irql);

    if (g_state.HaveContract && static_cast<LONG>(info->Sequence - g_state.ContractSequence) <= 0) {
        KeReleaseSpinLock(&g_stateLock, irql);
        return;
    }
    const bool active = info->Active != 0;
    const bool changed = !g_state.HaveContract || active != g_state.ContractActive ||
                         (active && (info->Milliamps != g_state.ContractMa || info->Millivolts != g_state.ContractMv));
    g_state.HaveContract = true;
    g_state.ContractSequence = info->Sequence;
    g_state.ContractActive = active;
    g_state.ContractMv = active ? info->Millivolts : 0;
    g_state.ContractMa = active ? info->Milliamps : 0;
    KeReleaseSpinLock(&g_stateLock, irql);
    if (!changed) return;
    TraceLoggingWrite(g_trace, "Contract",
        TraceLoggingString(source, "Source"),
        TraceLoggingUInt32(info->Sequence, "Sequence"),
        TraceLoggingUInt32(info->Active, "Active"),
        TraceLoggingUInt32(info->Millivolts, "Millivolts"),
        TraceLoggingUInt32(info->Milliamps, "Milliamps"));
    if (InterlockedCompareExchange(&g_started, 0, 0) != 0 && InterlockedCompareExchange(&g_stopped, 0, 0) == 0) {
        KeSetEvent(&g_worker.Wake, IO_NO_INCREMENT, FALSE);
    }
}

void ContractCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != nabu::link::kContractPublish) return;
    NoteContract(static_cast<const nabu::link::ContractInfo*>(argument2), "publish");
}

void QueryContract() {
    if (!g_worker.ContractObject) return;
    nabu::link::ContractInfo info = {sizeof(info), kNoPublisher, 0, 0, 0, 0};
    ExNotifyCallback(g_worker.ContractObject, reinterpret_cast<PVOID>(nabu::link::kContractQuery), &info);
    if (info.Version != kNoPublisher) NoteContract(&info, "query");
}

void NotePump(const nabu::pps::PumpState* p, PCSTR source) {
    if (!nabu::pps::Valid(p)) return;
    const bool valid = p->Valid != 0;
    const bool switching = valid && p->Mode == nabu::pps::PumpMode::Switching;
    const unsigned holdMa = switching ? p->BuckHoldIclMa : 0;
    KIRQL irql;
    KeAcquireSpinLock(&g_stateLock, &irql);
    if (g_state.HavePump && static_cast<LONG>(p->Sequence - g_state.PumpSequence) < 0) {
        KeReleaseSpinLock(&g_stateLock, irql);
        return;
    }
    const bool changed = !g_state.HavePump || valid != g_state.PumpValid || switching != g_state.PumpSwitching ||
                         holdMa != g_state.PumpHoldMa;
    g_state.HavePump = true;
    g_state.PumpSequence = p->Sequence;
    g_state.PumpValid = valid;
    g_state.PumpSwitching = switching;
    g_state.PumpHoldMa = holdMa;
    g_state.PumpAt = KeQueryInterruptTime();
    KeReleaseSpinLock(&g_stateLock, irql);
    if (!changed) return;
    TraceLoggingWrite(g_trace, "Pump",
        TraceLoggingString(source, "Source"),
        TraceLoggingUInt32(p->Sequence, "Sequence"),
        TraceLoggingUInt32(p->Valid, "Valid"),
        TraceLoggingUInt32(static_cast<ULONG>(p->Mode), "Mode"),
        TraceLoggingUInt32(p->BuckHoldIclMa, "BuckHoldIclMa"));
    if (InterlockedCompareExchange(&g_started, 0, 0) != 0 && InterlockedCompareExchange(&g_stopped, 0, 0) == 0) {
        KeSetEvent(&g_worker.Wake, IO_NO_INCREMENT, FALSE);
    }
}

void PumpCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != nabu::pps::kPublish) return;
    NotePump(static_cast<const nabu::pps::PumpState*>(argument2), "publish");
}

KSPIN_LOCK g_batteryLock = 0;
nabu::pps::BatteryInfo g_battery = {};

void BatteryCallback(PVOID, PVOID argument1, PVOID argument2) {
    if (reinterpret_cast<ULONG_PTR>(argument1) != nabu::pps::kQuery) return;
    auto* out = static_cast<nabu::pps::BatteryInfo*>(argument2);
    if (!out || out->Size < sizeof(nabu::pps::BatteryInfo)) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_batteryLock, &irql);
    *out = g_battery;
    KeReleaseSpinLock(&g_batteryLock, irql);
}

void PublishBattery(bool valid, int deciC, ULONG pollMs) {
    nabu::pps::BatteryInfo info;
    KIRQL irql;
    KeAcquireSpinLock(&g_batteryLock, &irql);
    g_battery.Size = sizeof(g_battery);
    g_battery.Version = nabu::pps::kBatteryInfoVersion;
    g_battery.Sequence++;
    g_battery.Valid = valid ? 1 : 0;
    g_battery.TempDeciC = valid ? deciC : 0;
    g_battery.PollMs = pollMs;
    info = g_battery;
    KeReleaseSpinLock(&g_batteryLock, irql);
    if (g_worker.BatteryObject) ExNotifyCallback(g_worker.BatteryObject, reinterpret_cast<PVOID>(nabu::pps::kPublish), &info);
}

void QueryPump() {
    if (!g_worker.PumpObject) return;
    nabu::pps::PumpState state = {};
    state.Size = sizeof(state);
    state.Version = nabu::pps::kPumpStateVersion;
    ExNotifyCallback(g_worker.PumpObject, reinterpret_cast<PVOID>(nabu::pps::kQuery), &state);
    if (state.Sequence != 0) NotePump(&state, "query");
}

void ApplyExact(const charge::Decision& d, PCSTR why) {
    const charge::Writes w = charge::ExactWrites(d);
    if (!w.Count) return;
    if (!g_worker.Pmic) {
        const NTSTATUS status = pmic::Open(g_worker.Device, &g_worker.Pmic);
        if (!NT_SUCCESS(status)) {
            TraceLoggingWrite(g_trace, "Exact", TraceLoggingString("open-failed", "Why"),
                              TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"));
            return;
        }
    }
    for (unsigned i = 0; i < w.Count; ++i) {
        unsigned readback = 0xFFFF;
        const NTSTATUS written = pmic::Write(g_worker.Pmic, w.Items[i].Addr, w.Items[i].Value);
        const NTSTATUS read = pmic::Read(g_worker.Pmic, w.Items[i].Addr, &readback);
        TraceLoggingWrite(g_trace, "Exact",
            TraceLoggingString(why, "Why"),
            TraceLoggingHexUInt16(static_cast<USHORT>(w.Items[i].Addr), "Addr"),
            TraceLoggingHexUInt8(w.Items[i].Value, "Value"),
            TraceLoggingHexUInt32(static_cast<ULONG>(written), "Status"),
            TraceLoggingHexUInt32(NT_SUCCESS(read) ? readback : 0xFFFFu, "Readback"));
    }
}

NTSTATUS SendIoctl(WDFIOTARGET target, ULONG code, PVOID in, ULONG inBytes,
                   PVOID out, ULONG outBytes, LONGLONG timeoutMs, ULONG_PTR* bytesOut) {
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    if (in && inBytes) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, in, inBytes);
    if (out && outBytes) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, out, outBytes);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(timeoutMs);
    ULONG_PTR bytes = 0;
    const NTSTATUS status = WdfIoTargetSendIoctlSynchronously(
        target, nullptr, code, (in && inBytes) ? &inDesc : nullptr,
        (out && outBytes) ? &outDesc : nullptr, &options, &bytes);
    if (bytesOut) *bytesOut = bytes;
    return status;
}

bool QueryBatteryTemperature(int* outDeciC, NTSTATUS* outStatus) {
    *outDeciC = 0;
    *outStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    if (!g_worker.Device) return false;

    PWSTR links = nullptr;
    NTSTATUS status = IoGetDeviceInterfaces(&kBatteryClassGuid, nullptr, 0, &links);
    if (!NT_SUCCESS(status) || !links) {
        *outStatus = status;
        return false;
    }

    bool found = false;
    for (PCWSTR p = links; *p != L'\0'; ) {
        UNICODE_STRING linkName;
        RtlInitUnicodeString(&linkName, p);
        p += (linkName.Length / sizeof(WCHAR)) + 1;
        if (InterlockedCompareExchange(&g_worker.StopRequested, 0, 0)) break;

        WDF_OBJECT_ATTRIBUTES attrs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = g_worker.Device;
        WDFIOTARGET target = nullptr;
        status = WdfIoTargetCreate(g_worker.Device, &attrs, &target);
        if (!NT_SUCCESS(status)) {
            *outStatus = status;
            continue;
        }

        WDF_IO_TARGET_OPEN_PARAMS openParams;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &linkName, GENERIC_READ);
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        status = WdfIoTargetOpen(target, &openParams);
        if (NT_SUCCESS(status)) {
            ULONG wait = 0, tag = 0;
            ULONG_PTR bytes = 0;
            status = SendIoctl(target, kIoctlBatteryQueryTag, &wait, sizeof(wait),
                               &tag, sizeof(tag), 500, &bytes);
            if (NT_SUCCESS(status) && bytes >= sizeof(tag) && tag != 0) {
                BatteryQueryInfo query = {tag, kBatteryTemperatureLevel, 0};
                ULONG deciK = 0;
                status = SendIoctl(target, kIoctlBatteryQueryInfo, &query, sizeof(query),
                                   &deciK, sizeof(deciK), 500, &bytes);
                if (NT_SUCCESS(status) && bytes >= sizeof(deciK) &&
                    deciK >= kMinPlausibleDeciK && deciK <= kMaxPlausibleDeciK) {
                    *outDeciC = static_cast<int>(deciK) - kZeroCelsiusDeciK;
                    *outStatus = STATUS_SUCCESS;
                    found = true;
                } else if (NT_SUCCESS(status)) {
                    status = STATUS_DATA_ERROR;
                }
            }
            WdfIoTargetClose(target);
        }
        WdfObjectDelete(target);
        *outStatus = status;
        if (found) break;
    }

    ExFreePool(links);
    return found;
}

void ReapplyIfNeeded(bool tempChanged, NTSTATUS tempStatus) {
    const bool enabled = GuardReadParam(L"Enabled", kEnabledDefault) == 1;
    const unsigned mode = OwnerMode();
    KeWaitForSingleObject(&g_sendMutex, Executive, KernelMode, FALSE, nullptr);
    const bool exactDue = InterlockedExchange(&g_worker.ExactPending, 0) != 0;

    KIRQL irql;
    KeAcquireSpinLock(&g_stateLock, &irql);
    const charge::Inputs in = InputsLocked(mode);
    const charge::Decision d = charge::Decide(in, charge::Limits{});
    const bool haveParams = g_state.HaveParams;
    const bool haveMode = g_state.HaveMode;
    const bool haveApplied = g_state.HaveApplied;
    const bool decisionChanged = !haveApplied || !charge::SameDecision(d, g_state.Applied);
    const bool restoreStock = !enabled && haveApplied && haveParams && g_worker.Extension != nullptr;
    const bool shouldReapply = enabled && decisionChanged && haveParams && g_worker.Extension != nullptr;
    unsigned fa0[5] = {};
    unsigned fd0[4] = {};
    if (haveParams) {
        for (int i = 0; i < 5; ++i) fa0[i] = g_state.LastParams[i];
    }
    if (haveMode) {
        for (int i = 0; i < 4; ++i) fd0[i] = g_state.LastMode[i];
    }
    KeReleaseSpinLock(&g_stateLock, irql);

    const unsigned orig[3] = {fa0[1], fa0[2], fa0[3]};
    if (tempChanged) {
        LogCharge(in.TempValid ? "temp" : "temp-fail", static_cast<ULONG>(tempStatus), in, orig, d);
    }

    if (InterlockedCompareExchange(&g_worker.StopRequested, 0, 0)) {
        KeReleaseMutex(&g_sendMutex, FALSE);
        return;
    }

    if (restoreStock) {
        ULONG out = 0;
        const NTSTATUS stParams = SendIoctl(g_worker.Extension, kChargeParams, fa0, sizeof(fa0),
                                            &out, sizeof(out), 250, nullptr);
        NTSTATUS stMode = STATUS_SUCCESS;
        if (haveMode) {
            out = 0;
            stMode = SendIoctl(g_worker.Extension, kIclMode, fd0, sizeof(fd0),
                               &out, sizeof(out), 250, nullptr);
        }
        if (NT_SUCCESS(stParams) && NT_SUCCESS(stMode)) {
            KeAcquireSpinLock(&g_stateLock, &irql);
            g_state.HaveApplied = false;
            KeReleaseSpinLock(&g_stateLock, irql);
        }
        KeReleaseMutex(&g_sendMutex, FALSE);
        LogCharge("restore", static_cast<ULONG>(NT_SUCCESS(stParams) ? stMode : stParams), in, orig,
                  charge::Decision{});
        return;
    }

    if (!shouldReapply) {

        if (enabled && exactDue && haveParams) ApplyExact(d, "request");
        KeReleaseMutex(&g_sendMutex, FALSE);
        return;
    }

    charge::RewriteChargeParams(fa0, d);
    ULONG out = 0;
    const NTSTATUS stParams = SendIoctl(g_worker.Extension, kChargeParams, fa0, sizeof(fa0),
                                        &out, sizeof(out), 250, nullptr);
    NTSTATUS stMode = STATUS_SUCCESS;
    if (haveMode) {
        charge::RewriteIclMode(fd0, d);
        out = 0;
        stMode = SendIoctl(g_worker.Extension, kIclMode, fd0, sizeof(fd0),
                           &out, sizeof(out), 250, nullptr);
    }

    if (NT_SUCCESS(stParams) && NT_SUCCESS(stMode)) {
        KeAcquireSpinLock(&g_stateLock, &irql);
        g_state.Applied = d;
        g_state.HaveApplied = true;
        KeReleaseSpinLock(&g_stateLock, irql);
    }
    ApplyExact(d, "reapply");
    KeReleaseMutex(&g_sendMutex, FALSE);
    LogCharge("reapply", static_cast<ULONG>(NT_SUCCESS(stParams) ? stMode : stParams), in, orig, d);
}

void ChargeThread(PVOID) {
    PKTHREAD self = KeGetCurrentThread();
    ObReferenceObject(self);
    g_worker.Thread = self;
    KeSetEvent(&g_worker.Started, IO_NO_INCREMENT, FALSE);

    ULONGLONG nextTemperature = 0;
    while (!InterlockedCompareExchange(&g_worker.StopRequested, 0, 0)) {
        bool tempChanged = false;
        NTSTATUS status = STATUS_SUCCESS;
        if (KeQueryInterruptTime() >= nextTemperature) {
            int deciC = 0;
            const bool valid = QueryBatteryTemperature(&deciC, &status);
            if (InterlockedCompareExchange(&g_worker.StopRequested, 0, 0)) break;
            KIRQL irql;
            KeAcquireSpinLock(&g_stateLock, &irql);
            tempChanged = (g_state.TempValid != valid) || (valid && g_state.TempDeciC != deciC);
            g_state.TempValid = valid;
            if (valid) g_state.TempDeciC = deciC;
            KeReleaseSpinLock(&g_stateLock, irql);
            nextTemperature = KeQueryInterruptTime() + (valid ? kPollPeriod : kRetryPeriod);
            PublishBattery(valid, deciC, static_cast<ULONG>((valid ? kPollPeriod : kRetryPeriod) / kMs));
        }
        QueryContract();

        ReapplyIfNeeded(tempChanged, status);

        const ULONGLONG now = KeQueryInterruptTime();
        ULONGLONG wakeAt = nextTemperature;
        KIRQL irql;
        KeAcquireSpinLock(&g_stateLock, &irql);
        if (g_state.HavePump && g_state.PumpSwitching) {
            const ULONGLONG staleAt = g_state.PumpAt + static_cast<ULONGLONG>(charge::kPumpStaleMs) * kMs + kMs;
            if (staleAt > now && staleAt < wakeAt) wakeAt = staleAt;
        }
        KeReleaseSpinLock(&g_stateLock, irql);
        LARGE_INTEGER wait;
        wait.QuadPart = -static_cast<LONGLONG>(wakeAt > now ? wakeAt - now : 0);
        KeWaitForSingleObject(&g_worker.Wake, Executive, KernelMode, FALSE, &wait);
    }

    if (InterlockedExchange(&g_stopped, 1) == 0) {
        KeSetEvent(&g_worker.Stopped, IO_NO_INCREMENT, FALSE);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void ContractLinkStop() {
    PVOID registration = InterlockedExchangePointer(&g_worker.ContractRegistration, nullptr);
    if (registration) ExUnregisterCallback(registration);
    PVOID object = InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_worker.ContractObject), nullptr);
    if (object) ObDereferenceObject(object);
    if (g_worker.BatteryObject) PublishBattery(false, 0, 0);
    registration = InterlockedExchangePointer(&g_worker.BatteryRegistration, nullptr);
    if (registration) ExUnregisterCallback(registration);
    object = InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_worker.BatteryObject), nullptr);
    if (object) ObDereferenceObject(object);
    registration = InterlockedExchangePointer(&g_worker.PumpRegistration, nullptr);
    if (registration) ExUnregisterCallback(registration);
    object = InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_worker.PumpObject), nullptr);
    if (object) ObDereferenceObject(object);
}

}

void ChargeStart(WDFDEVICE device, WDFIOTARGET extension) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;

    KeInitializeMutex(&g_sendMutex, 0);
    InterlockedExchange(&g_worker.MutexReady, 1);
    KeInitializeEvent(&g_worker.Started, NotificationEvent, FALSE);
    KeInitializeEvent(&g_worker.Stopped, NotificationEvent, FALSE);
    KeInitializeEvent(&g_worker.Wake, SynchronizationEvent, FALSE);
    g_worker.Device = device;
    g_worker.Extension = extension;
    const NTSTATUS linkStatus = nabu::link::OpenContractCallback(&g_worker.ContractObject);
    if (NT_SUCCESS(linkStatus)) {
        g_worker.ContractRegistration = ExRegisterCallback(g_worker.ContractObject, ContractCallback, nullptr);
    } else {
        g_worker.ContractObject = nullptr;
    }
    TraceLoggingWrite(g_trace, "ContractLink",
        TraceLoggingHexUInt32(static_cast<ULONG>(linkStatus), "Status"),
        TraceLoggingBoolean(g_worker.ContractRegistration != nullptr, "Registered"));
    const NTSTATUS pumpStatus = nabu::pps::OpenCallback(nabu::pps::kPumpStateName, &g_worker.PumpObject);
    if (NT_SUCCESS(pumpStatus)) {
        g_worker.PumpRegistration = ExRegisterCallback(g_worker.PumpObject, PumpCallback, nullptr);
    } else {
        g_worker.PumpObject = nullptr;
    }
    TraceLoggingWrite(g_trace, "PumpLink",
        TraceLoggingHexUInt32(static_cast<ULONG>(pumpStatus), "Status"),
        TraceLoggingBoolean(g_worker.PumpRegistration != nullptr, "Registered"));
    QueryPump();
    KeInitializeSpinLock(&g_batteryLock);
    const NTSTATUS batteryStatus = nabu::pps::OpenCallback(nabu::pps::kBatteryInfoName, &g_worker.BatteryObject);
    if (NT_SUCCESS(batteryStatus)) {
        g_worker.BatteryRegistration = ExRegisterCallback(g_worker.BatteryObject, BatteryCallback, nullptr);
    } else {
        g_worker.BatteryObject = nullptr;
    }

    HANDLE threadHandle = nullptr;
    const NTSTATUS status = PsCreateSystemThread(
        &threadHandle, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, ChargeThread, nullptr);
    if (NT_SUCCESS(status)) {
        ZwClose(threadHandle);
    } else {
        InterlockedExchange(&g_stopped, 1);
        KeSetEvent(&g_worker.Stopped, IO_NO_INCREMENT, FALSE);
        KeSetEvent(&g_worker.Started, IO_NO_INCREMENT, FALSE);
    }
}

void ChargeStop() {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (InterlockedCompareExchange(&g_started, 0, 0) == 0) return;
    KeWaitForSingleObject(&g_worker.Started, Executive, KernelMode, FALSE, nullptr);

    InterlockedExchange(&g_worker.StopRequested, 1);
    KeSetEvent(&g_worker.Wake, IO_NO_INCREMENT, FALSE);

    if (InterlockedExchange(&g_stopped, 1) == 0) {
        PKTHREAD thread = static_cast<PKTHREAD>(
            InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_worker.Thread), nullptr));
        if (thread && thread != KeGetCurrentThread()) {
            KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, nullptr);
            ObDereferenceObject(thread);
        }
        KeSetEvent(&g_worker.Stopped, IO_NO_INCREMENT, FALSE);
    } else {
        KeWaitForSingleObject(&g_worker.Stopped, Executive, KernelMode, FALSE, nullptr);
        PKTHREAD thread = static_cast<PKTHREAD>(
            InterlockedExchangePointer(reinterpret_cast<PVOID*>(&g_worker.Thread), nullptr));
        if (thread && thread != KeGetCurrentThread()) {
            KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, nullptr);
            ObDereferenceObject(thread);
        }
    }
    ContractLinkStop();
}

void ChargeOnRequest(ULONG code, PVOID input, size_t inputLength, ULONG* record) {
    if (!input) return;
    const unsigned mode = OwnerMode();
    if (code == kChargeParams && inputLength >= 5 * sizeof(ULONG)) {
        const bool lockHeld = InterlockedCompareExchange(&g_worker.MutexReady, 0, 0) != 0;
        if (lockHeld) KeWaitForSingleObject(&g_sendMutex, Executive, KernelMode, FALSE, nullptr);

        auto* p = static_cast<unsigned*>(input);
        const unsigned orig[3] = {p[1], p[2], p[3]};
        KIRQL irql;
        KeAcquireSpinLock(&g_stateLock, &irql);
        for (int i = 0; i < 5; ++i) g_state.LastParams[i] = p[i];
        g_state.HaveParams = true;
        if (p[3] == 0) {
            g_state.IclRequestMaxMa = 0;
        } else if (p[3] > g_state.IclRequestMaxMa) {
            g_state.IclRequestMaxMa = p[3];
        }
        const charge::Inputs in = InputsLocked(mode);
        const charge::Decision d = charge::Decide(in, charge::Limits{});
        g_state.Applied = d;
        g_state.HaveApplied = true;
        KeReleaseSpinLock(&g_stateLock, irql);

        const bool changed = charge::RewriteChargeParams(p, d);
        if (record) {
            for (int i = 0; i < 4; ++i) record[i] = p[i];
        }
        if (lockHeld) KeReleaseMutex(&g_sendMutex, FALSE);
        LogCharge(changed ? "rewrite-fa0" : "keep-fa0", 0, in, orig, d);
        return;
    }
    if (code == kIclMode && inputLength >= 4 * sizeof(ULONG)) {
        const bool lockHeld = InterlockedCompareExchange(&g_worker.MutexReady, 0, 0) != 0;
        if (lockHeld) KeWaitForSingleObject(&g_sendMutex, Executive, KernelMode, FALSE, nullptr);

        auto* p = static_cast<unsigned*>(input);
        const unsigned orig[3] = {p[1], p[2], p[3]};
        KIRQL irql;
        KeAcquireSpinLock(&g_stateLock, &irql);
        for (int i = 0; i < 4; ++i) g_state.LastMode[i] = p[i];
        g_state.HaveMode = true;
        const charge::Inputs in = InputsLocked(mode);
        const charge::Decision d = charge::Decide(in, charge::Limits{});
        g_state.Applied = d;
        g_state.HaveApplied = true;
        KeReleaseSpinLock(&g_stateLock, irql);

        const bool changed = charge::RewriteIclMode(p, d);
        if (record) {
            for (int i = 0; i < 4; ++i) record[i] = p[i];
        }
        if (lockHeld) KeReleaseMutex(&g_sendMutex, FALSE);
        LogCharge(changed ? "rewrite-fd0" : "keep-fd0", 0, in, orig, d);
    }
}

void ChargeOnCompleted(ULONG code, NTSTATUS status) {
    if (code != kChargeParams || !NT_SUCCESS(status)) return;
    if (InterlockedCompareExchange(&g_started, 0, 0) == 0 || InterlockedCompareExchange(&g_stopped, 0, 0) != 0) return;
    InterlockedExchange(&g_worker.ExactPending, 1);
    KeSetEvent(&g_worker.Wake, IO_NO_INCREMENT, FALSE);
}

void ChargeNoteRole(LONG role) {
    if (role == 0) return;
    const bool sink = (role == 2);
    KIRQL irql;
    KeAcquireSpinLock(&g_stateLock, &irql);
    const bool becameSink = sink && !g_state.SinkAttached;
    g_state.SinkAttached = sink;
    if (!sink) g_state.IclRequestMaxMa = 0;
    KeReleaseSpinLock(&g_stateLock, irql);

    if (becameSink &&
        InterlockedCompareExchange(&g_started, 0, 0) != 0 &&
        InterlockedCompareExchange(&g_stopped, 0, 0) == 0) {
        KeSetEvent(&g_worker.Wake, IO_NO_INCREMENT, FALSE);
    }
}

}
