#include "pmic.h"

namespace srcguard::pmic {

namespace {

constexpr ULONG kPmicIndex = 2;
constexpr ULONG kIoctlRead = 0x80110FACu;
constexpr ULONG kIoctlWrite = 0x802B0FA0u;
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

NTSTATUS Open(WDFDEVICE parent, WDFIOTARGET* target) {
    *target = nullptr;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = parent;
    WDFIOTARGET created = nullptr;
    NTSTATUS status = WdfIoTargetCreate(parent, &attributes, &created);
    if (!NT_SUCCESS(status)) return status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\DosDevices\\QCOMPMIC");
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ);
    status = WdfIoTargetOpen(created, &open);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(created);
        return status;
    }
    *target = created;
    return STATUS_SUCCESS;
}

NTSTATUS Read(WDFIOTARGET target, unsigned addr, unsigned* value) {
    ULONG in[2] = {kPmicIndex, addr};
    UCHAR out[8] = {};
    ULONG_PTR bytes = 0;
    NTSTATUS status = Send(target, kIoctlRead, in, sizeof(in), out, sizeof(out), &bytes);
    const ULONG pmicStatus = *reinterpret_cast<ULONG*>(out + 4);
    if (NT_SUCCESS(status) && (pmicStatus != 0 || bytes < 1)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    *value = NT_SUCCESS(status) ? out[0] : 0;
    return status;
}

NTSTATUS Write(WDFIOTARGET target, unsigned addr, unsigned value) {
    ULONG in[3] = {kPmicIndex, addr, value & 0xFF};
    ULONG out = 0xFFFFFFFFu;
    ULONG_PTR bytes = 0;
    NTSTATUS status = Send(target, kIoctlWrite, in, sizeof(in), &out, sizeof(out), &bytes);
    if (NT_SUCCESS(status) && out != 0) status = STATUS_DEVICE_PROTOCOL_ERROR;
    return status;
}

}
