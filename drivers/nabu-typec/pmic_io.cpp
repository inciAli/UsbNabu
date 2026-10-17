#include "nabutypec.h"

namespace nabutypec {

namespace {

constexpr ULONG kTimeoutMs = 50;

NTSTATUS Send(WDFIOTARGET target, ULONG code, void* in, ULONG inBytes, void* out, ULONG outBytes, ULONG_PTR* bytes) {
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, in, inBytes);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, out, outBytes);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(kTimeoutMs);
    return WdfIoTargetSendIoctlSynchronously(target, nullptr, code, &inDesc, &outDesc, &options, bytes);
}

}

NTSTATUS PmicOpen(WDFDEVICE parent, WDFIOTARGET* target) {
    PAGED_CODE();
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = parent;
    NTSTATUS status = WdfIoTargetCreate(parent, &attributes, target);
    if (!NT_SUCCESS(status)) return status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\DosDevices\\QCOMPMIC");
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ);
    status = WdfIoTargetOpen(*target, &open);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(*target);
        *target = nullptr;
    }
    TraceLoggingWrite(g_typecTrace, "PmicOpen", TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"));
    return status;
}

NTSTATUS PmicRead(WDFIOTARGET target, unsigned addr, unsigned* value) {
    ULONG in[2] = {nabu::pm8150b::kPmicIndex, addr};
    UCHAR out[8] = {};
    ULONG_PTR bytes = 0;
    NTSTATUS status = Send(target, nabu::pm8150b::kIoctlRead, in, sizeof(in), out, sizeof(out), &bytes);
    const ULONG pmicStatus = *reinterpret_cast<ULONG*>(out + 4);
    if (NT_SUCCESS(status) && (pmicStatus != 0 || bytes < 1)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    *value = NT_SUCCESS(status) ? out[0] : 0;
    return status;
}

NTSTATUS PmicWrite(WDFIOTARGET target, unsigned addr, unsigned value) {
    ULONG in[3] = {nabu::pm8150b::kPmicIndex, addr, value & 0xFF};
    ULONG out = 0xFFFFFFFFu;
    ULONG_PTR bytes = 0;
    NTSTATUS status = Send(target, nabu::pm8150b::kIoctlWrite, in, sizeof(in), &out, sizeof(out), &bytes);
    if (NT_SUCCESS(status) && out != 0) status = STATUS_DEVICE_PROTOCOL_ERROR;
    TraceLoggingWrite(g_typecTrace, "PmicWrite",
        TraceLoggingHexUInt16(static_cast<USHORT>(addr), "Addr"),
        TraceLoggingHexUInt8(static_cast<UCHAR>(value), "Value"),
        TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"),
        TraceLoggingHexUInt32(out, "PmicStatus"));
    return status;
}

}
