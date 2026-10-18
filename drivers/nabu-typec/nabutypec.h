#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <TraceLoggingProvider.h>

#include "contract.h"
#include "contract_link.h"
#include "pdphy.h"
#include "tcpc_policy.h"

TRACELOGGING_DECLARE_PROVIDER(g_typecTrace);

namespace nabutypec {

NTSTATUS PmicOpen(WDFDEVICE parent, WDFIOTARGET* target);
NTSTATUS PmicRead(WDFIOTARGET target, unsigned addr, unsigned* value);
NTSTATUS PmicWrite(WDFIOTARGET target, unsigned addr, unsigned value);

constexpr ULONG kIrqCount = 11;
constexpr ULONG kIrqCc = 0;
constexpr ULONG kIrqPdphyFirst = 1;
constexpr ULONG kIrqOtgOc = 8;
constexpr ULONG kIrqSwitcherOk = 9;
constexpr ULONG kIrqUsbinOv = 10;

struct PortCallbacks {
    void* Context;

    void (*Alert)(void* context, const nabu::tcpc::Alerts& tcpcAlerts, nabu::pdphy::Alert pdAlert,
                  const nabu::pdphy::RxBuffer* rx);
};

bool OwnerReady();
NTSTATUS OwnerAttachPort(const PortCallbacks& callbacks);
void OwnerDetachPort();

NTSTATUS OwnerGetStatus(unsigned* cc, unsigned* power, unsigned* fault);
void OwnerGetControl(unsigned* tcpcControl, unsigned* roleControl, unsigned* faultControl, unsigned* powerControl);
NTSTATUS OwnerSetRoleControl(unsigned roleControl);
NTSTATUS OwnerSetSimpleControl(ULONG controlType, unsigned value);

NTSTATUS OwnerCommand(unsigned command, bool* supported, nabu::tcpc::Alerts* alerts);
NTSTATUS OwnerSetMessageHeaderInfo(unsigned info);
NTSTATUS OwnerSetReceiveDetect(unsigned receiveDetect);
NTSTATUS OwnerSetTransmitBuffer(const nabu::pdphy::TxBuffer& buffer);
NTSTATUS OwnerTransmit(unsigned transmit, nabu::pdphy::TxResult* result);

NTSTATUS OwnerDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init);
NTSTATUS PortDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init);

constexpr GUID kDisplayTypeCInterfaceGuid = {0x86261F2E, 0xB53A, 0x4A26,
                                             {0x8F, 0x26, 0x32, 0x79, 0x00, 0x33, 0x1C, 0xF8}};

ULONG ReadParam(WDFDRIVER driver, PCWSTR name, ULONG fallback);
constexpr ULONG kStageDefault = 3;
void ClearBootAttempts();

}
