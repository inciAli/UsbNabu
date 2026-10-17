#include <ntddk.h>
#include <wdf.h>
#include <TraceLoggingProvider.h>

#include "charge.h"
#include "sinkpdo_policy.h"
#include "upc_policy.h"

TRACELOGGING_DEFINE_PROVIDER(g_trace, "Nabu.SrcGuard",
    (0x7c88b2b5, 0xcdfe, 0x4ffe, 0x8d, 0x99, 0xbe, 0xa1, 0x5e, 0xd7, 0xb5, 0xd9));

namespace {

constexpr ULONG kGetTypecRole = 0x802A0FA4u;
constexpr ULONG kSlot = 1;
constexpr ULONG kBootAttemptLimit = 3;
constexpr LONG kHealthyAfterRequests = 64;

enum class StackKind : ULONG { Other = 0, Extension = 1, Tcpm = 3, UsbFn = 4 };

struct RequestRecord {
    ULONG Code;
    ULONG InLength;
    ULONG OutLength;
    ULONG In[4];
    BOOLEAN Internal;
    BOOLEAN SinkDsd;
    BOOLEAN UpcQuery;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RequestRecord, RecordOf)

struct GuardContext {
    StackKind Kind;
    WDFIOTARGET Lower;
    volatile LONG Requests;
    volatile LONG HealthReported;
    ULONG SinkPdoMode;
    ULONG UpcFix;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GuardContext, GuardOf)

WDFDRIVER g_driver = nullptr;
ULONG g_noAttach = 0;

ULONG ReadParam(PCWSTR name, ULONG fallback) {
    ULONG data = fallback;
    WDFKEY key = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_driver) return fallback;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_driver, KEY_READ,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) return fallback;
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(key, &value, &data))) data = fallback;
    WdfRegistryClose(key);
    return data;
}

void WriteParam(PCWSTR name, ULONG data) {
    WDFKEY key = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_driver) return;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_driver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    WdfRegistryAssignULong(key, &value, data);
    WdfRegistryClose(key);
}

bool RulesEnabled() { return ReadParam(L"Enabled", srcguard::kEnabledDefault) == 1; }

void LogDecision(PCSTR action, ULONG a, ULONG b) {
    TraceLoggingWrite(g_trace, "Decision",
        TraceLoggingString(action, "Action"),
        TraceLoggingHexUInt32(a, "A"),
        TraceLoggingHexUInt32(b, "B"));
}

void LogIoctl(StackKind kind, const RequestRecord* r, NTSTATUS status, ULONG_PTR info,
              const ULONG* out, ULONG outWords) {
    TraceLoggingWrite(g_trace, "Ioctl",
        TraceLoggingUInt32(static_cast<ULONG>(kind), "Stack"),
        TraceLoggingBoolean(r->Internal, "Internal"),
        TraceLoggingHexUInt32(r->Code, "Code"),
        TraceLoggingUInt32(r->InLength, "InLen"),
        TraceLoggingUInt32(r->OutLength, "OutLen"),
        TraceLoggingHexUInt32(r->In[0], "In0"),
        TraceLoggingHexUInt32(r->In[1], "In1"),
        TraceLoggingHexUInt32(r->In[2], "In2"),
        TraceLoggingHexUInt32(r->In[3], "In3"),
        TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"),
        TraceLoggingUInt32(static_cast<ULONG>(info), "Info"),
        TraceLoggingHexUInt32(outWords > 0 ? out[0] : 0, "Out0"),
        TraceLoggingHexUInt32(outWords > 1 ? out[1] : 0, "Out1"),
        TraceLoggingHexUInt32(outWords > 2 ? out[2] : 0, "Out2"));
}

void LogSinkPdo(ULONG mode, NTSTATUS status, ULONG_PTR info, const srcguard::sinkpdo::Result& r) {
    TraceLoggingWrite(g_trace, "SinkPdo",
        TraceLoggingUInt32(mode, "Mode"),
        TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"),
        TraceLoggingUInt32(static_cast<ULONG>(info), "Info"),
        TraceLoggingUInt32(static_cast<ULONG>(r.Status), "Outcome"),
        TraceLoggingUInt32(r.PdoCount, "Count"),
        TraceLoggingHexUInt32(r.Pdos[0], "Pdo0"),
        TraceLoggingHexUInt32(r.Pdos[1], "Pdo1"),
        TraceLoggingHexUInt32(r.Pdos[2], "Pdo2"),
        TraceLoggingHexUInt32(r.Pdos[3], "Pdo3"));
}

NTSTATUS SendOwn(WDFIOTARGET target, ULONG code, ULONG* in, ULONG inBytes, ULONG* out, ULONG outBytes) {
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, in, inBytes);
    if (out) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, out, outBytes);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(250);
    ULONG_PTR bytes = 0;
    return WdfIoTargetSendIoctlSynchronously(target, nullptr, code, &inDesc,
                                             out ? &outDesc : nullptr, &options, &bytes);
}

void QueryRole(WDFIOTARGET target) {
    ULONG slot = kSlot;
    ULONG reply[2] = {0xFFFFFFFFu, 0};
    NTSTATUS status = SendOwn(target, kGetTypecRole, &slot, sizeof(slot), reply, sizeof(reply));
    const ULONG role = (NT_SUCCESS(status) && reply[0] == 0) ? reply[1] : 0;
    srcguard::ChargeNoteRole(static_cast<LONG>(role));
}

void EvtForwardComplete(WDFREQUEST request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS params,
                        WDFCONTEXT context) {
    GuardContext* guard = static_cast<GuardContext*>(context);
    const RequestRecord* record = RecordOf(request);
    const NTSTATUS status = params->IoStatus.Status;
    const ULONG_PTR info = params->IoStatus.Information;
    ULONG out[3] = {};
    ULONG words = 0;
    PVOID buffer = nullptr;
    size_t size = 0;
    const bool haveOut = record->OutLength > 0 && !record->Internal &&
        NT_SUCCESS(WdfRequestRetrieveOutputBuffer(request, 1, &buffer, &size)) && buffer;

    if (haveOut && NT_SUCCESS(status) && info > 0) {
        size_t avail = size < info ? size : info;
        if (avail > sizeof(out)) avail = sizeof(out);
        RtlCopyMemory(out, buffer, avail);
        words = static_cast<ULONG>((avail + 3) / 4);
    }
    if (guard->Kind == StackKind::Tcpm && record->SinkDsd) {

        srcguard::sinkpdo::Result r{};
        r.Status = srcguard::sinkpdo::Outcome::NoReply;
        if (haveOut && NT_SUCCESS(status)) {
            const size_t valid = size < info ? size : info;
            r = srcguard::sinkpdo::PatchSinkPdos(static_cast<unsigned char*>(buffer), valid,
                                                 guard->SinkPdoMode == 1);
        }
        LogSinkPdo(guard->SinkPdoMode, status, info, r);
    }
    if (guard->Kind == StackKind::Extension) {
        srcguard::ChargeOnCompleted(record->Code, status);
        if (record->Code == kGetTypecRole && NT_SUCCESS(status) && words >= 2 && out[0] == 0) {
            srcguard::ChargeNoteRole(static_cast<LONG>(out[1]));
        }
    }
    if (guard->Kind == StackKind::UsbFn && record->UpcQuery) {

        size_t written = 0;
        PVOID reply = nullptr;
        size_t replySize = 0;
        if (guard->UpcFix == 1 && !NT_SUCCESS(status) &&
            NT_SUCCESS(WdfRequestRetrieveOutputBuffer(request, srcguard::upc::kReplyBytes, &reply, &replySize))) {
            written = srcguard::upc::BuildUpcReply(reply, replySize);
        }
        LogDecision(written ? "UpcSupplied" : "UpcForwarded", static_cast<ULONG>(status), guard->UpcFix);
        if (written) {
            LogIoctl(guard->Kind, record, STATUS_SUCCESS, written, nullptr, 0);
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, written);
            return;
        }
    }
    LogIoctl(guard->Kind, record, status, info, out, words);
    WdfRequestCompleteWithInformation(request, status, info);
}

void ForwardObserved(GuardContext* guard, WDFREQUEST request) {
    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request, EvtForwardComplete, guard);
    if (!WdfRequestSend(request, guard->Lower, WDF_NO_SEND_OPTIONS)) {
        const NTSTATUS status = WdfRequestGetStatus(request);
        LogIoctl(guard->Kind, RecordOf(request), status, 0, nullptr, 0);
        WdfRequestComplete(request, status);
    }
}

void Capture(WDFREQUEST request, ULONG code, size_t inLen, size_t outLen, bool internal) {
    RequestRecord* record = RecordOf(request);
    RtlZeroMemory(record, sizeof(*record));
    record->Code = code;
    record->InLength = static_cast<ULONG>(inLen);
    record->OutLength = static_cast<ULONG>(outLen);
    record->Internal = internal;
    if (inLen && !internal) {
        PVOID in = nullptr;
        size_t inSize = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 1, &in, &inSize)) && in) {
            RtlCopyMemory(record->In, in, inSize < sizeof(record->In) ? inSize : sizeof(record->In));
        }
    }
}

void NoteHealthy(GuardContext* guard) {
    if (InterlockedIncrement(&guard->Requests) >= kHealthyAfterRequests &&
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
        InterlockedCompareExchange(&guard->HealthReported, 1, 0) == 0) {
        WriteParam(L"BootAttempts", 0);
    }
}

void EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outLen, size_t inLen, ULONG code) {
    GuardContext* guard = GuardOf(WdfIoQueueGetDevice(queue));
    Capture(request, code, inLen, outLen, false);
    NoteHealthy(guard);
    RequestRecord* record = RecordOf(request);

    if (guard->Kind == StackKind::Tcpm) {

        PVOID in = nullptr;
        size_t inSize = 0;
        if (code == srcguard::sinkpdo::kIoctlAcpiEvalMethodEx && outLen > 0 &&
            NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 1, &in, &inSize)) && in &&
            srcguard::sinkpdo::IsConnectorDsdRequest(in, inSize)) {
            record->SinkDsd = TRUE;
        }
        ForwardObserved(guard, request);
        return;
    }

    if (guard->Kind == StackKind::Extension && KeGetCurrentIrql() == PASSIVE_LEVEL && RulesEnabled() &&
        (code == srcguard::kChargeParams || code == srcguard::kIclMode)) {
        QueryRole(guard->Lower);
        PVOID in = nullptr;
        size_t inSize = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 1, &in, &inSize)) && in) {
            srcguard::ChargeOnRequest(code, in, inSize, record->In);
        }
    }
    ForwardObserved(guard, request);
}

void EvtIoInternalDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outLen, size_t inLen,
                                ULONG code) {
    GuardContext* guard = GuardOf(WdfIoQueueGetDevice(queue));
    Capture(request, code, inLen, outLen, true);
    NoteHealthy(guard);
    if (guard->Kind == StackKind::UsbFn && code == srcguard::upc::kIoctlAcpiEvalMethod) {
        PVOID in = nullptr;
        size_t inSize = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 8, &in, &inSize)) &&
            srcguard::upc::IsUpcRequest(in, inSize)) {
            RecordOf(request)->UpcQuery = TRUE;
        }
    }
    ForwardObserved(guard, request);
}

NTSTATUS EvtSelfManagedIoInit(WDFDEVICE device) {
    GuardContext* guard = GuardOf(device);
    if (guard->Kind == StackKind::Extension) srcguard::ChargeStart(device, guard->Lower);
    return STATUS_SUCCESS;
}

void EvtSelfManagedIoCleanup(WDFDEVICE device) {
    if (GuardOf(device)->Kind == StackKind::Extension) srcguard::ChargeStop();
}

void EvtDeviceCleanup(WDFOBJECT object) {
    if (GuardOf(static_cast<WDFDEVICE>(object))->Kind == StackKind::Extension) srcguard::ChargeStop();
}

bool Contains(const WCHAR* ids, ULONG count, const WCHAR* needle) {
    ULONG n = 0;
    while (needle[n]) ++n;
    for (ULONG i = 0; i + n <= count; ++i) {
        ULONG j = 0;
        while (j < n && ids[i + j] == needle[j]) ++j;
        if (j == n) return true;
    }
    return false;
}

StackKind ClassifyStack(PWDFDEVICE_INIT init) {
    WCHAR ids[512] = {};
    ULONG bytes = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - sizeof(WCHAR),
                                            ids, &bytes))) return StackKind::Other;
    const ULONG count = bytes / sizeof(WCHAR);
    if (Contains(ids, count, L"QCOM05E5")) return StackKind::Extension;
    if (Contains(ids, count, L"QCOM057D")) return StackKind::Tcpm;

    if (Contains(ids, count, L"DEV_0597") && Contains(ids, count, L"&FUNCTION")) return StackKind::UsbFn;
    return StackKind::Other;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    if (g_noAttach == 1) return STATUS_SUCCESS;
    const StackKind kind = ClassifyStack(init);
    WdfFdoInitSetFilter(init);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceSelfManagedIoInit = EvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = EvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, RequestRecord);
    WdfDeviceInitSetRequestAttributes(init, &requestAttributes);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GuardContext);
    attributes.EvtCleanupCallback = EvtDeviceCleanup;
    WDFDEVICE device = nullptr;
    NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    GuardContext* guard = GuardOf(device);
    guard->Kind = kind;
    guard->Lower = WdfDeviceGetIoTarget(device);
    guard->SinkPdoMode = kind == StackKind::Tcpm ? ReadParam(L"SinkPdoPatch", 1) : 0;
    guard->UpcFix = kind == StackKind::UsbFn ? ReadParam(L"UpcFix", 1) : 0;

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
    if (NT_SUCCESS(status)) LogDecision("Attached", static_cast<ULONG>(kind), 0);
    return status;
}

void EvtDriverUnload(WDFDRIVER) {
    srcguard::ChargeStop();
    TraceLoggingUnregister(g_trace);
}

}

namespace srcguard {
ULONG GuardReadParam(PCWSTR name, ULONG fallback) { return ReadParam(name, fallback); }
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    TraceLoggingRegister(g_trace);
    NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES,
                                      &config, &g_driver);
    if (!NT_SUCCESS(status)) { TraceLoggingUnregister(g_trace); return status; }
    g_noAttach = ReadParam(L"NoAttach", 0);
    const ULONG attempts = ReadParam(L"BootAttempts", 0);
    if (attempts >= kBootAttemptLimit) g_noAttach = 1;
    WriteParam(L"BootAttempts", attempts + 1);
    return STATUS_SUCCESS;
}
