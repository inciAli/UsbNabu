#pragma once

#include <ntddk.h>
#include <wdf.h>

namespace srcguard {

constexpr ULONG kChargeParams = 0x801B0FA0u;
constexpr ULONG kIclMode = 0x801B0FD0u;
constexpr ULONG kEnabledDefault = 1;

ULONG GuardReadParam(PCWSTR name, ULONG fallback);

void ChargeStart(WDFDEVICE device, WDFIOTARGET extension);

void ChargeStop();

void ChargeOnRequest(ULONG code, PVOID input, size_t inputLength, ULONG* record);

void ChargeOnCompleted(ULONG code, NTSTATUS status);

void ChargeNoteRole(LONG role);

}
