#pragma once

#include <ntddk.h>
#include <wdf.h>

namespace srcguard::pmic {

NTSTATUS Open(WDFDEVICE parent, WDFIOTARGET* target);
NTSTATUS Read(WDFIOTARGET target, unsigned addr, unsigned* value);
NTSTATUS Write(WDFIOTARGET target, unsigned addr, unsigned value);

}
