#pragma once

#include <ntddk.h>

namespace nabu::link {

constexpr ULONG_PTR kContractPublish = 1;
constexpr ULONG_PTR kContractQuery = 2;
constexpr ULONG kContractVersion = 1;

struct ContractInfo {
    ULONG Size;
    ULONG Version;
    ULONG Sequence;
    ULONG Active;
    ULONG Millivolts;
    ULONG Milliamps;
};

inline NTSTATUS OpenContractCallback(PCALLBACK_OBJECT* object) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Callback\\NabuTypeCContract");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_PERMANENT, nullptr, nullptr);
    return ExCreateCallback(object, &attributes, TRUE, TRUE);
}

inline bool ValidInfo(const ContractInfo* info) {
    return info && info->Size >= sizeof(ContractInfo) && info->Version == kContractVersion;
}

}
