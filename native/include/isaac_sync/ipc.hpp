#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "isaac_sync/version.hpp"

namespace isaac_sync {

constexpr std::uint32_t kIpcMagic = 0x30315349u; // IS10
constexpr std::uint32_t kIpcAbiVersion = 5;
constexpr std::uint32_t kIpcEventCapacity = 8192;
constexpr wchar_t kIpcMappingPrefix[] =
    L"Local\\IsaacSync." ISAAC_SYNC_WIDEN(ISAAC_SYNC_PRODUCT_VERSION) L".";

enum class RuntimeState : LONG {
    Empty = 0,
    Initializing = 1,
    Dormant = 2,
    Discovering = 3,
    Hello = 4,
    Warmup = 5,
    Active = 6,
    Stopping = 7,
    Stopped = 8,
    FailedPreActive = 9,
    FailedFatal = 10,
    Unloading = 11,
    Unloaded = 12,
};

enum class CompatibilityState : LONG {
    Unknown = 0,
    Checking = 1,
    Ready = 2,
    WaitingForSteam = 3,
    WaitingForRoster = 4,
    Unsupported = 5,
};

enum class ErrorCode : LONG {
    None = 0,
    MappingMissing = 1,
    AbiMismatch = 2,
    TargetImageMismatch = 3,
    UnsupportedArchitecture = 4,
    BuildFingerprintFailed = 5,
    CallsiteMismatch = 6,
    BackendMismatch = 7,
    SteamContextMismatch = 8,
    SteamVtableOwnerMismatch = 9,
    SteamExportsMissing = 10,
    RosterUnavailable = 11,
    RosterShape = 12,
    LocalIdentityMismatch = 13,
    ThirdPartyHook = 14,
    HookInstallFailed = 15,
    ShutdownUnsafe = 16,
    PacketSize = 100,
    PacketHeader = 101,
    PacketSession = 102,
    PacketSender = 103,
    PacketSequence = 104,
    PacketCrc = 105,
    HandshakeMismatch = 106,
    HandshakeState = 107,
    HandshakeActivationFrame = 108,
    TransportFailure = 109,
    HandshakeTimeout = 110,
    ActivationBarrierTimeout = 111,
    ActivationCommitMismatch = 112,
    FrameBatchSize = 120,
    FrameWindow = 121,
    FrameConflict = 122,
    NonEmptyOperationUnsupported = 123,
    FrameStoreCapacity = 124,
    FrameReadyTimeout = 125,
    FrameStateViolation = 126,
    LuaRuntimeUnavailable = 200,
    LuaRegistrationFailed = 201,
    LuaArgumentInvalid = 202,
    ManifestFrozen = 203,
    LuaStateChanged = 204,
    LuaCleanupUnsafe = 205,
    HandlerInvokedUnexpectedly = 206,
    SteamFunctionsChanged = 300,
    LuaSendInactive = 301,
    LuaSendArgument = 302,
    LuaSendPort = 303,
    LuaSendFrameOccupied = 304,
    LuaSendFrameOverflow = 305,
    FrameUnknownPort = 306,
    DispatchStateViolation = 307,
    DispatchLuaFailure = 308,
    DispatchRefInvalid = 309,
    GroupPeerCountMismatch = 310,
    GroupHandshakeMismatch = 311,
    GroupActivationMismatch = 312,
    GroupFanoutTimeout = 313,
    GroupCoordinatorLost = 314,
    ManifestDerivedPortCollision = 315,
    FramePayloadInvalid = 316,
};

enum class BackendKind : std::uint32_t {
    Unknown = 0,
    Steam = 2,
    Eos = 3,
};

enum class IpcEventType : std::uint32_t {
    RuntimeStateChanged = 1,
    CompatibilityChanged = 2,
    HookInstalled = 3,
    HookRestored = 4,
    SteamContext = 5,
    Roster = 6,
    Error = 7,
    Shutdown = 8,
    LuaBindBegin = 19,
    LuaBound = 20,
    LuaManifestFrozen = 21,
    LuaCleanupComplete = 22,
    LuaStateCreated = 23,
    LuaStateClosing = 24,
    LuaStateRetired = 25,
    SyncHandshake = 30,
    ActivationBarrier = 31,
    SyncFrameSend = 32,
    SyncFrameReceive = 33,
    SyncReadyStallBegin = 34,
    SyncReadyStallEnd = 35,
    SyncFailure = 36,
    SyncSummary = 37,
    TestDelay = 40,
    LuaSendAccepted = 41,
    LuaSendRejected = 42,
    SyncRecordSealed = 43,
    SyncRecordReceived = 44,
    SyncDispatch = 45,
    SyncDispatchFailure = 46,
    GroupRosterFrozen = 47,
    GroupPeerLink = 48,
    GroupFanoutProgress = 49,
    GroupActivationReady = 50,
    GroupActivationCommit = 51,
    GroupSessionEnded = 52,
    SyncOriginalDispatchCompleted = 53,
};

enum class DispatchGuardState : LONG {
    Idle = 0,
    Prepared = 1,
    Dispatched = 2,
    Failed = 3,
};

enum IpcFlags : std::uint32_t {
    IpcFlagLuaBound = 1u << 0,
    IpcFlagStartRequested = 1u << 1,
    IpcFlagManifestFrozen = 1u << 2,
    IpcFlagSessionBound = 1u << 3,
    IpcFlagCoordinator = 1u << 4,
    IpcFlagActivationCommitted = 1u << 5,
    IpcFlagLuaCleanupPending = 1u << 6,
};

enum class TestCommandResult : LONG {
    None = 0,
    Accepted = 1,
    NotActive = 2,
    WrongManifest = 3,
    InvalidDelay = 4,
    Busy = 5,
};

#pragma pack(push, 8)

struct alignas(8) IpcEventV5 {
    volatile LONG64 sequence;
    LONG64 qpc;
    std::uint32_t type;
    std::uint32_t state;
    std::uint32_t error;
    std::uint32_t online_frame;
    std::uint64_t message_sequence;
    std::uint64_t session_token;
    std::uint64_t peer_token;
    std::uint64_t value0;
    std::uint64_t value1;
    std::uint32_t crc32;
    std::uint16_t kind;
    std::uint16_t count;
    std::uint32_t operation_frame;
    std::uint32_t sender_slot;
    std::uint32_t recipient_slot;
    std::uint16_t port;
    std::uint16_t opcode;
    std::uint32_t payload_kind;
    std::uint32_t reserved_typed_payload;
    std::uint64_t payload_bits;
    char message[128];
};

struct alignas(8) SharedControlBlockV5 {
    std::uint32_t magic;
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint32_t process_id;
    volatile LONG runtime_state;
    volatile LONG compatibility_state;
    volatile LONG error_code;
    std::uint32_t backend;
    volatile LONG online_frame;
    volatile LONG flags;
    volatile LONG64 write_sequence;
    volatile LONG64 overwritten_events;
    volatile LONG64 channel_fd_calls;
    volatile LONG64 channel_fd_sends;
    volatile LONG64 channel_fd_polls;
    volatile LONG64 channel_fd_receives;
    volatile LONG64 packets_sent;
    volatile LONG64 packets_received;
    volatile LONG64 packets_rejected;
    volatile LONG64 batches_sent;
    volatile LONG64 batches_received;
    volatile LONG64 records_sent;
    volatile LONG64 records_received;
    volatile LONG64 duplicate_records;
    volatile LONG64 handler_invocations;
    std::uint64_t local_peer_token;
    std::uint64_t remote_peer_token;
    std::uint32_t module_base;
    std::uint32_t main_thread_id;
    std::uint32_t manifest_count;
    volatile LONG activation_frame;
    volatile LONG sealed_through;
    volatile LONG received_through;
    volatile LONG consumed_through;
    volatile LONG stall_frame;
    volatile LONG stall_retries;
    volatile LONG stall_current_ms;
    volatile LONG64 stall_total_ms;
    volatile LONG64 stall_max_ms;
    volatile LONG64 test_command_sequence;
    volatile LONG64 test_ack_sequence;
    volatile LONG test_delay_ms;
    volatile LONG test_command_result;
    std::uint8_t build_hash[32];
    std::uint8_t manifest_hash[32];
    std::uint8_t session_id[16];
    char status[192];
    char error_detail[256];
    volatile LONG64 send_accepted;
    volatile LONG64 send_rejected;
    volatile LONG64 pending_local_records;
    volatile LONG64 non_empty_sent;
    volatile LONG64 non_empty_received;
    volatile LONG64 dispatch_failures;
    volatile LONG last_send_target;
    volatile LONG last_send_reject_error;
    volatile LONG dispatch_guard_frame;
    volatile LONG dispatch_guard_state;
    volatile LONG last_dispatch_frame;
    volatile LONG last_dispatch_slot;
    std::uint16_t last_dispatch_port;
    std::uint16_t last_dispatch_opcode;
    std::int32_t last_dispatch_payload;
    std::uint32_t peer_count;
    std::uint32_t local_slot;
    std::uint32_t coordinator_slot;
    std::uint32_t frozen_slot_mask;
    volatile LONG activation_ready_mask;
    volatile LONG fanout_pending_mask;
    std::uint64_t peer_tokens[4];
    volatile LONG received_through_by_slot[4];
    volatile LONG64 next_receive_sequence_by_slot[4];
    volatile LONG64 packets_sent_by_slot[4];
    volatile LONG64 packets_received_by_slot[4];
    volatile LONG64 records_received_by_slot[4];
    volatile LONG64 handler_invocations_by_slot[4];
    volatile LONG64 fanout_retry_count;
    std::uint32_t last_dispatch_payload_kind;
    std::uint32_t reserved_typed_payload;
    std::uint64_t last_dispatch_payload_bits;
    IpcEventV5 events[kIpcEventCapacity];
};

#pragma pack(pop)

inline void BuildIpcMappingName(DWORD process_id, wchar_t (&buffer)[64]) {
    _snwprintf_s(buffer, sizeof(buffer) / sizeof(buffer[0]), _TRUNCATE, L"%s%lu",
                 kIpcMappingPrefix, static_cast<unsigned long>(process_id));
}

static_assert(sizeof(void*) == 4, "Isaac Sync V2 native code must be Win32");
static_assert(sizeof(IpcEventV5) == 240, "IPC ABI 5 event layout changed");
static_assert(offsetof(IpcEventV5, operation_frame) == 80, "IPC operation frame moved");
static_assert(offsetof(IpcEventV5, sender_slot) == 84, "IPC sender slot moved");
static_assert(offsetof(IpcEventV5, recipient_slot) == 88, "IPC recipient slot moved");
static_assert(offsetof(IpcEventV5, port) == 92, "IPC port moved");
static_assert(offsetof(IpcEventV5, payload_kind) == 96, "IPC payload kind moved");
static_assert(offsetof(IpcEventV5, payload_bits) == 104, "IPC payload bits moved");
static_assert(offsetof(IpcEventV5, message) == 112, "IPC message moved");
static_assert(offsetof(SharedControlBlockV5, write_sequence) == 40, "IPC common header changed");
static_assert(offsetof(SharedControlBlockV5, send_accepted) == 784,
              "IPC send counters moved");
static_assert(offsetof(SharedControlBlockV5, peer_count) == 864, "IPC peer count moved");
static_assert(offsetof(SharedControlBlockV5, peer_tokens) == 888, "IPC peer tokens moved");
static_assert(offsetof(SharedControlBlockV5, received_through_by_slot) == 920,
              "IPC receive progress moved");
static_assert(offsetof(SharedControlBlockV5, fanout_retry_count) == 1096,
              "IPC fanout retry count moved");
static_assert(offsetof(SharedControlBlockV5, last_dispatch_payload_kind) == 1104,
              "IPC typed dispatch kind moved");
static_assert(offsetof(SharedControlBlockV5, last_dispatch_payload_bits) == 1112,
              "IPC typed dispatch bits moved");
static_assert(offsetof(SharedControlBlockV5, events) == 1120,
              "IPC ABI 5 event offset changed");
static_assert(sizeof(SharedControlBlockV5) == 1967200, "IPC ABI 5 mapping size changed");

} // namespace isaac_sync
