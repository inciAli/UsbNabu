#include "nabutypec.h"

TRACELOGGING_DEFINE_PROVIDER(g_typecTrace, "Nabu.TypeC",
    (0x5b1e8c3a, 0x7d2f, 0x4c61, 0x9a, 0x0e, 0x3f, 0x6d, 0x21, 0xb4, 0x88, 0x17));

namespace nabutypec {

ULONG ReadParam(WDFDRIVER driver, PCWSTR name, ULONG fallback) {
    ULONG data = fallback;
    WDFKEY key = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return fallback;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) return fallback;
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(key, &value, &data))) data = fallback;
    WdfRegistryClose(key);
    return data;
}

namespace {

WDFDRIVER g_driver = nullptr;
ULONG g_noStart = 0;
constexpr ULONG kBootAttemptLimit = 3;

void WriteParam(PCWSTR name, ULONG data) {
    WDFKEY key = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_driver) return;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_driver, KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    WdfRegistryAssignULong(key, &value, data);
    HANDLE raw = WdfRegistryWdmGetHandle(key);
    if (raw) ZwFlushKey(raw);
    WdfRegistryClose(key);
}

enum class Kind { Other, Owner, Port };

Kind Classify(PWDFDEVICE_INIT init) {
    WCHAR ids[512] = {};
    ULONG bytes = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - sizeof(WCHAR), ids, &bytes)))
        return Kind::Other;
    const ULONG count = bytes / sizeof(WCHAR);
    for (ULONG i = 0; i + 8 <= count; ++i) {
        if (ids[i] == L'Q' && ids[i + 1] == L'C' && ids[i + 2] == L'O' && ids[i + 3] == L'M' && ids[i + 4] == L'0' &&
            ids[i + 5] == L'5') {
            if (ids[i + 6] == L'8' && ids[i + 7] == L'2') return Kind::Owner;
            if (ids[i + 6] == L'7' && ids[i + 7] == L'D') return Kind::Port;
        }
    }
    return Kind::Other;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    if (g_noStart) return STATUS_DEVICE_CONFIGURATION_ERROR;
    switch (Classify(init)) {
    case Kind::Owner: return OwnerDeviceAdd(driver, init);
    case Kind::Port: return PortDeviceAdd(driver, init);
    default: return STATUS_NOT_SUPPORTED;
    }
}

void EvtDriverUnload(WDFDRIVER) { TraceLoggingUnregister(g_typecTrace); }

}

void ClearBootAttempts() { WriteParam(L"BootAttempts", 0); }

void DriverLoaded(WDFDRIVER driver) {
    g_driver = driver;
    const ULONG attempts = ReadParam(driver, L"BootAttempts", 0);
    g_noStart = ReadParam(driver, L"NoStart", 0) == 1 || attempts >= kBootAttemptLimit;
    WriteParam(L"BootAttempts", attempts + 1);
    TraceLoggingWrite(g_typecTrace, "Loaded", TraceLoggingUInt32(attempts, "BootAttempts"),
                      TraceLoggingUInt32(g_noStart, "NoStart"));
}

}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, nabutypec::EvtDeviceAdd);
    config.EvtDriverUnload = nabutypec::EvtDriverUnload;
    TraceLoggingRegister(g_typecTrace);
    WDFDRIVER driver = nullptr;
    NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) { TraceLoggingUnregister(g_typecTrace); return status; }
    nabutypec::DriverLoaded(driver);
    return STATUS_SUCCESS;
}
