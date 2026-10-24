#pragma once

#include <ntddk.h>

namespace nabu::pps {

constexpr ULONG_PTR kPublish = 1;
constexpr ULONG_PTR kQuery = 2;
constexpr ULONG_PTR kCommand = 3;

constexpr ULONG kPdSnapshotVersion = 1;
constexpr ULONG kPpsCommandVersion = 1;
constexpr ULONG kPumpStateVersion = 1;
constexpr ULONG kBatteryInfoVersion = 1;

constexpr ULONG kMaxPdos = 7;
constexpr ULONG kExitHoldMs = 15;
constexpr ULONG kRequestTimeoutMs = 600;

enum class PpsStatus : ULONG {
    None = 0,
    InFlight = 1,
    Complete = 2,
    Rejected = 3,
    TimedOut = 4,
    Ended = 5,
};

enum class ExitReason : ULONG {
    None = 0,
    FrameworkRequest = 1,
    HardReset = 2,
    Detach = 3,
    RoleChange = 4,
    RequestFailed = 5,
    Released = 6,
    Disabled = 7,
    SoftReset = 8,
};

struct PdSnapshot {
    ULONG Size;
    ULONG Version;
    ULONG Sequence;
    ULONG Valid;
    ULONG ProxyEnabled;
    ULONG AttachedAsSink;
    ULONG PdRevision;
    ULONG FixedContractMv;
    ULONG FixedContractMa;
    ULONG PdoCount;
    ULONG Pdos[kMaxPdos];
    ULONG PpsActive;
    ULONG PpsObjectPos;
    ULONG PpsMillivolts;
    ULONG PpsMilliamps;
    ULONG LastRequestId;
    PpsStatus LastStatus;
    ULONG ExitPending;
    ExitReason Reason;
};

enum class PpsCommandKind : ULONG {
    Request = 1,
    Release = 2,
    StandbyAck = 3,
};

enum class PpsCommandResult : ULONG {
    Queued = 0,
    NotEnabled = 1,
    NoContract = 2,
    Busy = 3,
    Invalid = 4,
    BadVersion = 5,
};

struct PpsCommand {
    ULONG Size;
    ULONG Version;
    ULONG RequestId;
    PpsCommandKind Kind;
    ULONG ObjectPos;
    ULONG Millivolts;
    ULONG Milliamps;
    PpsCommandResult Result;
};

enum class PumpMode : ULONG {
    Absent = 0,
    Off = 1,
    Standby = 2,
    Switching = 3,
    Bypass = 4,
    Fault = 5,
};

struct PumpState {
    ULONG Size;
    ULONG Version;
    ULONG Sequence;
    ULONG Valid;
    PumpMode Mode;
    ULONG BuckHoldIclMa;
    ULONG IbattCapMa;
    ULONG VbusMv;
    ULONG IinMa;
    ULONG VbatMv;
    LONG DieDeciC;
    ULONG StopReason;
};

struct BatteryInfo {
    ULONG Size;
    ULONG Version;
    ULONG Sequence;
    ULONG Valid;
    LONG TempDeciC;
    ULONG PollMs;
};

static_assert(sizeof(PdSnapshot) == 25 * 4, "PdSnapshot layout");
static_assert(sizeof(BatteryInfo) == 6 * 4, "BatteryInfo layout");
static_assert(sizeof(PpsCommand) == 8 * 4, "PpsCommand layout");
static_assert(sizeof(PumpState) == 12 * 4, "PumpState layout");

inline NTSTATUS OpenCallback(PCWSTR path, PCALLBACK_OBJECT* object) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, path);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_PERMANENT, nullptr, nullptr);
    return ExCreateCallback(object, &attributes, TRUE, TRUE);
}

inline constexpr PCWSTR kPdSnapshotName = L"\\Callback\\NabuPdSnapshot";
inline constexpr PCWSTR kPpsControlName = L"\\Callback\\NabuPpsControl";
inline constexpr PCWSTR kPumpStateName = L"\\Callback\\NabuPumpState";
inline constexpr PCWSTR kBatteryInfoName = L"\\Callback\\NabuBatteryInfo";

inline bool Valid(const PdSnapshot* s) { return s && s->Size >= sizeof(PdSnapshot) && s->Version == kPdSnapshotVersion; }
inline bool Valid(const PpsCommand* c) { return c && c->Size >= sizeof(PpsCommand) && c->Version == kPpsCommandVersion; }
inline bool Valid(const PumpState* p) { return p && p->Size >= sizeof(PumpState) && p->Version == kPumpStateVersion; }
inline bool Valid(const BatteryInfo* b) { return b && b->Size >= sizeof(BatteryInfo) && b->Version == kBatteryInfoVersion; }

}
