#include "nabutypec.h"

#include <UcmTcpci/1.0/UcmTcpciCx.h>

namespace nabutypec {

namespace {

namespace tc = nabu::tcpc;
namespace pd = nabu::pdphy;

struct Port {
    UCMTCPCIPORTCONTROLLER Controller;
    WDFQUEUE Queue;
    bool Started;
    bool SourceAllowed;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Port, PortOf)

void Log(PCSTR what, ULONG a, ULONG b) {
    TraceLoggingWrite(g_typecTrace, "Port", TraceLoggingString(what, "What"),
                      TraceLoggingHexUInt32(a, "A"), TraceLoggingHexUInt32(b, "B"));
}

void Alert(void* context, const tc::Alerts& a, pd::Alert pdAlert, const pd::RxBuffer* rx) {
    Port* port = static_cast<Port*>(context);
    if (!port || !port->Started) return;
    UCMTCPCI_PORT_CONTROLLER_ALERT_DATA data[4];
    UCMTCPCI_PORT_CONTROLLER_RECEIVE_BUFFER receive = {};
    ULONG n = 0;
    auto next = [&]() -> UCMTCPCI_PORT_CONTROLLER_ALERT_DATA& {
        UCMTCPCI_PORT_CONTROLLER_ALERT_DATA_INIT(&data[n]);
        return data[n++];
    };
    if (a.Cc) { auto& d = next(); d.AlertType = UcmTcpciPortControllerAlertCCStatus; d.CCStatus.AsUInt8 = static_cast<UINT8>(a.CcImage); }
    if (a.Power) { auto& d = next(); d.AlertType = UcmTcpciPortControllerAlertPowerStatus; d.PowerStatus.AsUInt8 = static_cast<UINT8>(a.PowerImage); }
    if (a.Fault) { auto& d = next(); d.AlertType = UcmTcpciPortControllerAlertFault; d.FaultStatus.AsUInt8 = static_cast<UINT8>(a.FaultImage); }
    switch (pdAlert) {
    case pd::Alert::TransmitSuccessful: next().AlertType = UcmTcpciPortControllerAlertTransmitSOPMessageSuccessful; break;
    case pd::Alert::TransmitFailed: next().AlertType = UcmTcpciPortControllerAlertTransmitSOPMessageFailed; break;
    case pd::Alert::TransmitDiscarded: next().AlertType = UcmTcpciPortControllerAlertTransmitSOPMessageDiscarded; break;
    case pd::Alert::ReceivedHardReset: next().AlertType = UcmTcpciPortControllerAlertReceivedHardReset; break;
    case pd::Alert::RxOverflow: next().AlertType = UcmTcpciPortControllerAlertRxBufferOverflow; break;
    case pd::Alert::ReceiveMessage:
        if (rx) {
            receive.ReceiveByteCount = static_cast<UINT8>(rx->ByteCount);
            receive.ReceivedFrameType = static_cast<UINT8>(rx->FrameType);
            receive.Header[0] = rx->Header[0];
            receive.Header[1] = rx->Header[1];
            RtlCopyMemory(receive.DataObjects, rx->Data, sizeof(receive.DataObjects));
            auto& d = next();
            d.AlertType = UcmTcpciPortControllerAlertReceiveSOPMessageStatus;
            d.ReceiveBuffer = &receive;
        }
        break;
    case pd::Alert::None: break;
    }
    if (n) {
        UcmTcpciPortControllerAlert(port->Controller, data, n);

        Log("Alert", (a.Cc ? 1u : 0u) | (a.Power ? 2u : 0u) | (a.Fault ? 4u : 0u) | (static_cast<ULONG>(pdAlert) << 8),
            (a.CcImage << 16) | (a.PowerImage << 8) | a.FaultImage);
    }
}

template <class T>
NTSTATUS Input(WDFREQUEST request, T** in) {
    return WdfRequestRetrieveInputBuffer(request, sizeof(T), reinterpret_cast<PVOID*>(in), nullptr);
}

void EvtHardwareRequest(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    pd::Alert lateAlert = pd::Alert::None;
    tc::Alerts lateStatus{};
    switch (code) {
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_GET_STATUS: {
        UCMTCPCI_PORT_CONTROLLER_GET_STATUS_OUT_PARAMS* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        unsigned cc = 0, power = 0, fault = 0;
        if (NT_SUCCESS(status)) status = OwnerGetStatus(&cc, &power, &fault);
        if (NT_SUCCESS(status)) {
            out->CCStatus.AsUInt8 = static_cast<UINT8>(cc);
            out->PowerStatus.AsUInt8 = static_cast<UINT8>(power);
            out->FaultStatus.AsUInt8 = static_cast<UINT8>(fault);
            info = sizeof(*out);
        }
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_GET_CONTROL: {
        UCMTCPCI_PORT_CONTROLLER_GET_CONTROL_OUT_PARAMS* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        if (NT_SUCCESS(status)) {
            unsigned t = 0, rc = 0, f = 0, p = 0;
            OwnerGetControl(&t, &rc, &f, &p);
            out->TCPCControl.AsUInt8 = static_cast<UINT8>(t);
            out->RoleControl.AsUInt8 = static_cast<UINT8>(rc);
            out->FaultControl.AsUInt8 = static_cast<UINT8>(f);
            out->PowerControl.AsUInt8 = static_cast<UINT8>(p);
            info = sizeof(*out);
        }
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_CONTROL: {
        UCMTCPCI_PORT_CONTROLLER_SET_CONTROL_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        if (NT_SUCCESS(status)) {
            status = in->ControlType == UcmTcpciPortControllerRoleControl
                ? OwnerSetRoleControl(in->RoleControl.AsUInt8)
                : OwnerSetSimpleControl(in->ControlType, in->TCPCControl.AsUInt8);
        }
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_COMMAND: {
        UCMTCPCI_PORT_CONTROLLER_SET_COMMAND_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        bool supported = true;
        if (NT_SUCCESS(status)) status = OwnerCommand(static_cast<unsigned>(in->Command), &supported, &lateStatus);
        if (NT_SUCCESS(status) && !supported) status = STATUS_NOT_SUPPORTED;
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_MESSAGE_HEADER_INFO: {
        UCMTCPCI_PORT_CONTROLLER_SET_MESSAGE_HEADER_INFO_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        if (NT_SUCCESS(status)) status = OwnerSetMessageHeaderInfo(in->MessageHeaderInfo.AsUInt8);
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_RECEIVE_DETECT: {
        UCMTCPCI_PORT_CONTROLLER_SET_RECEIVE_DETECT_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        if (NT_SUCCESS(status)) status = OwnerSetReceiveDetect(in->ReceiveDetect.AsUInt8);
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_TRANSMIT_BUFFER: {
        UCMTCPCI_PORT_CONTROLLER_SET_TRANSMIT_BUFFER_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        if (NT_SUCCESS(status)) {
            pd::TxBuffer b = {};
            b.ByteCount = in->TransmitBuffer.TransmitByteCount;
            b.Header[0] = in->TransmitBuffer.Header[0];
            b.Header[1] = in->TransmitBuffer.Header[1];
            RtlCopyMemory(b.Data, in->TransmitBuffer.DataObjects, sizeof(b.Data));
            status = OwnerSetTransmitBuffer(b);
        }
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_TRANSMIT: {
        UCMTCPCI_PORT_CONTROLLER_SET_TRANSMIT_IN_PARAMS* in = nullptr;
        status = Input(request, &in);
        pd::TxResult result = pd::TxResult::Invalid;
        if (NT_SUCCESS(status)) status = OwnerTransmit(in->Transmit.AsUInt8, &result);

        if (NT_SUCCESS(status) && result != pd::TxResult::Started) {
            Log("TransmitNotStarted", static_cast<ULONG>(result), in->Transmit.AsUInt8);
            lateAlert = result == pd::TxResult::Discarded ? pd::Alert::TransmitDiscarded : pd::Alert::TransmitFailed;
        }
        break;
    }
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_SET_CONFIG_STANDARD_OUTPUT:
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_ALTERNATE_MODE_ENTERED:
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_ALTERNATE_MODE_EXITED:
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_DISPLAYPORT_CONFIGURED:
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_DISPLAYPORT_HPD_STATUS_CHANGED:
    case IOCTL_UCMTCPCI_PORT_CONTROLLER_DISPLAYPORT_DISPLAY_OUT_STATUS_CHANGED:
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    TraceLoggingWrite(g_typecTrace, "Request", TraceLoggingHexUInt32(code, "Code"),
                      TraceLoggingHexUInt32(static_cast<ULONG>(status), "Status"));
    WdfRequestCompleteWithInformation(request, status, info);
    if (lateStatus.Cc || lateStatus.Power || lateStatus.Fault || lateAlert != pd::Alert::None)
        Alert(PortOf(WdfIoQueueGetDevice(queue)), lateStatus, lateAlert, nullptr);
}

NTSTATUS EvtPrepareHardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
    Port* port = PortOf(device);
    if (!OwnerReady()) { Log("OwnerNotReady", 0, 0); return STATUS_DEVICE_NOT_READY; }

    UCMTCPCI_PORT_CONTROLLER_IDENTIFICATION id;
    UCMTCPCI_PORT_CONTROLLER_IDENTIFICATION_INIT(&id);

    id.TypeCRevisionInBcd = 0x12;
    id.PDRevisionAndVersionInBcd = 0x2011;
    id.PDInterfaceRevisionAndVersionInBcd = 0x1010;

    UCMTCPCI_PORT_CONTROLLER_CAPABILITIES caps;
    UCMTCPCI_PORT_CONTROLLER_CAPABILITIES_INIT(&caps);
    caps.IsPowerDeliveryCapable = TRUE;
    caps.DeviceCapabilities1.SinkVbus = 1;

    caps.DeviceCapabilities1.SourceVbus = 1;
    caps.DeviceCapabilities1.RolesSupported = port->SourceAllowed ? 5 : 2;
    caps.DeviceCapabilities1.SourceResistorSupported = 1;
    caps.DeviceCapabilities1.VbusOVPReporting = 1;
    caps.DeviceCapabilities1.VbusOCPReporting = 1;

    UCMTCPCI_PORT_CONTROLLER_CONFIG config;
    UCMTCPCI_PORT_CONTROLLER_CONFIG_INIT(&config, &id, &caps);

    NTSTATUS status = UcmTcpciPortControllerCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &port->Controller);
    if (!NT_SUCCESS(status)) { Log("CreateFailed", static_cast<ULONG>(status), 0); return status; }

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtHardwareRequest;
    queueConfig.PowerManaged = WdfFalse;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queueConfig, &queueAttributes, &port->Queue);
    if (!NT_SUCCESS(status)) return status;
    UcmTcpciPortControllerSetHardwareRequestQueue(port->Controller, port->Queue);

    status = UcmTcpciPortControllerStart(port->Controller);
    if (!NT_SUCCESS(status)) { Log("StartFailed", static_cast<ULONG>(status), 0); return status; }
    port->Started = true;
    status = OwnerAttachPort(PortCallbacks{port, Alert});
    if (!NT_SUCCESS(status)) {
        port->Started = false;
        WdfIoQueuePurgeSynchronously(port->Queue);
        UcmTcpciPortControllerStop(port->Controller);
    }
    Log("Started", static_cast<ULONG>(status), port->SourceAllowed);
    return status;
}

NTSTATUS EvtReleaseHardware(WDFDEVICE device, WDFCMRESLIST) {
    Port* port = PortOf(device);
    OwnerDetachPort();
    if (port->Started) {
        WdfIoQueuePurgeSynchronously(port->Queue);
        UcmTcpciPortControllerStop(port->Controller);
        port->Started = false;
    }
    if (port->Queue) { WdfObjectDelete(port->Queue); port->Queue = nullptr; }
    if (port->Controller) { WdfObjectDelete(port->Controller); port->Controller = nullptr; }
    Log("Stopped", 0, 0);
    return STATUS_SUCCESS;
}

}

NTSTATUS PortDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    NTSTATUS status = UcmTcpciDeviceInitInitialize(init);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, Port);
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    UCMTCPCI_DEVICE_CONFIG config;
    UCMTCPCI_DEVICE_CONFIG_INIT(&config);
    status = UcmTcpciDeviceInitialize(device, &config);
    if (!NT_SUCCESS(status)) return status;

    NTSTATUS iface = WdfDeviceCreateDeviceInterface(device, &kDisplayTypeCInterfaceGuid, nullptr);
    if (!NT_SUCCESS(iface)) Log("DisplayInterfaceFailed", static_cast<ULONG>(iface), 0);
    PortOf(device)->SourceAllowed = ReadParam(driver, L"Stage", kStageDefault) != 2;
    Log("PortAdded", PortOf(device)->SourceAllowed ? 3 : 2, 0);
    return STATUS_SUCCESS;
}

}
