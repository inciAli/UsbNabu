#pragma once

namespace nabu::pm8150b {

constexpr unsigned kPmicIndex = 2;

constexpr unsigned kIoctlRead = 0x80110FACu;
constexpr unsigned kIoctlWrite = 0x802B0FA0u;

constexpr unsigned kTypecBase = 0x1500;

constexpr unsigned kTypecSnkStatus = kTypecBase + 0x06;
constexpr unsigned kSnkRpStd = 1u << 3;
constexpr unsigned kSnkRp1p5 = 1u << 2;
constexpr unsigned kSnkRp3p0 = 1u << 1;
constexpr unsigned kSnkTypeMask = 0x7F;

constexpr unsigned kTypecSrcStatus = kTypecBase + 0x08;
constexpr unsigned kSrcRdOpen = 1u << 3;
constexpr unsigned kSrcRdRaVconn = 1u << 2;
constexpr unsigned kSrcRaOpen = 1u << 1;
constexpr unsigned kSrcAudioRaRa = 1u << 0;
constexpr unsigned kSrcTypeMask = 0x1F;

constexpr unsigned kTypecSmStatus = kTypecBase + 0x0A;
constexpr unsigned kSmVbusVsafe5v = 1u << 5;
constexpr unsigned kSmVbusVsafe0v = 1u << 6;

constexpr unsigned kTypecMiscStatus = kTypecBase + 0x0B;
constexpr unsigned kMiscSnkSrcMode = 1u << 6;
constexpr unsigned kMiscVbusDetect = 1u << 5;
constexpr unsigned kMiscVbusError = 1u << 4;
constexpr unsigned kMiscDebounceDone = 1u << 3;
constexpr unsigned kMiscCcOrientation = 1u << 1;
constexpr unsigned kMiscCcAttached = 1u << 0;

constexpr unsigned kTypecModeCfg = kTypecBase + 0x44;
constexpr unsigned kModeEnTrySnk = 1u << 4;
constexpr unsigned kModeEnTrySrc = 1u << 3;
constexpr unsigned kModeEnSrcOnly = 1u << 2;
constexpr unsigned kModeEnSnkOnly = 1u << 1;
constexpr unsigned kModeDisableCmd = 1u << 0;

constexpr unsigned kTypecVconnControl = kTypecBase + 0x46;
constexpr unsigned kVconnEnOrientation = 1u << 2;
constexpr unsigned kVconnEnValue = 1u << 1;
constexpr unsigned kVconnEnSrc = 1u << 0;

constexpr unsigned kTypecExitStateCfg = kTypecBase + 0x50;
constexpr unsigned kExitSelSrcUpperRef = 1u << 2;
constexpr unsigned kExitUseTpdForAttachSrc = 1u << 1;

constexpr unsigned kTypecCurrsrcCfg = kTypecBase + 0x52;
constexpr unsigned kRpSel80uA = 0;
constexpr unsigned kRpSel180uA = 1;
constexpr unsigned kRpSel330uA = 2;
constexpr unsigned kRpSelMask = 0x3;

constexpr unsigned kTypecIntEnCfg1 = kTypecBase + 0x5E;
constexpr unsigned kTypecIntEnCfg2 = kTypecBase + 0x60;

constexpr unsigned kPdphyBase = 0x1700;

constexpr unsigned kPdphyMsgConfig = kPdphyBase + 0x40;
constexpr unsigned kMsgCfgDataRoleHost = 1u << 3;
constexpr unsigned kMsgCfgPowerRoleSource = 1u << 2;
constexpr unsigned kMsgCfgSpecRevMask = 0x3;

constexpr unsigned kPdphyTxSize = kPdphyBase + 0x42;

constexpr unsigned kPdphyTxControl = kPdphyBase + 0x44;
constexpr unsigned TxRetryCount(unsigned n) { return (n & 0x3) << 5; }
constexpr unsigned TxFrameType(unsigned n) { return (n & 0x7) << 2; }
constexpr unsigned kTxSendSignal = 1u << 1;
constexpr unsigned kTxSendMsg = 1u << 0;

constexpr unsigned kPdphyEnControl = kPdphyBase + 0x46;
constexpr unsigned kPdphyEnable = 1u << 0;

constexpr unsigned kPdphyRxSize = kPdphyBase + 0x48;
constexpr unsigned kPdphyRxStatus = kPdphyBase + 0x4A;
constexpr unsigned kRxFrameTypeMask = 0x7;
constexpr unsigned kPdphyRxAcknowledge = kPdphyBase + 0x4B;

constexpr unsigned kPdphyFrameFilter = kPdphyBase + 0x4C;
constexpr unsigned kFilterHardReset = 1u << 5;
constexpr unsigned kFilterSop = 1u << 0;

constexpr unsigned kPdphyTxBufferHdr = kPdphyBase + 0x60;
constexpr unsigned kPdphyTxBufferData = kPdphyBase + 0x62;
constexpr unsigned kPdphyRxBuffer = kPdphyBase + 0x80;

constexpr unsigned kPdMaxDataObjects = 7;
constexpr unsigned kPdMaxMessageBytes = 2 + 4 * kPdMaxDataObjects;

constexpr unsigned kDcdcBase = 0x1100;

constexpr unsigned kDcdcOtgStatus = kDcdcBase + 0x0D;
constexpr unsigned kOtgStatusMask = 0x7;
constexpr unsigned kOtgStatusEnabled = 2;

constexpr unsigned kDcdcCmdOtg = kDcdcBase + 0x40;
constexpr unsigned kOtgEn = 1u << 0;
constexpr unsigned kDcdcFswSel = kDcdcBase + 0x50;
constexpr unsigned kDcdcOtgCurrentLimitCfg = kDcdcBase + 0x52;
constexpr unsigned kDcdcOtgCfg = kDcdcBase + 0x53;
constexpr unsigned kOtgEnSrcCfg = 1u << 1;
constexpr unsigned kOtgFaultConditionCfg = kDcdcBase + 0x56;

constexpr unsigned kUsbinBase = 0x1300;

constexpr unsigned kUsbinIntRtSts = kUsbinBase + 0x10;
constexpr unsigned kUsbinPluginRtSts = 1u << 4;
constexpr unsigned kUsbinCmdIl = kUsbinBase + 0x40;
constexpr unsigned kUsbinSuspend = 1u << 0;
constexpr unsigned kUsbinCmdIclOverride = kUsbinBase + 0x42;
constexpr unsigned kUsbCmdPulldown = kUsbinBase + 0x45;
constexpr unsigned kUsbinLoadCfg = kUsbinBase + 0x65;
constexpr unsigned kUsbinIclOptions = kUsbinBase + 0x66;
constexpr unsigned kUsbinCurrentLimitCfg = kUsbinBase + 0x70;

}
