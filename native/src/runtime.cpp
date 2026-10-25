#include "isaac_sync/frame_store.hpp"
#include "isaac_sync/dispatcher.hpp"
#include "isaac_sync/ipc.hpp"
#include "isaac_sync/runtime_hooks.hpp"
#include "isaac_sync/protocol.hpp"
#include "isaac_sync/runtime_ipc.hpp"
#include "isaac_sync/runtime_lua.hpp"
#include "isaac_sync/runtime_target.hpp"
#include "isaac_sync/runtime_steam.hpp"
#include "isaac_sync/runtime_roster.hpp"
#include "isaac_sync/session.hpp"
#include "isaac_sync/steam_transport.hpp"

#include <bcrypt.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using namespace isaac_sync;

namespace isaac_sync::runtime
{

    constexpr std::uint64_t kLuaCleanupTimeoutMs = 5000;
    constexpr std::uint32_t kSmokeVersion = 1;
    constexpr std::uint16_t kSmokePort = 43000;
    constexpr char kSmokeSyncId[] = "dev.isaacsync.typed-payload-smoke";


    using DispatchFn = std::uint32_t(__thiscall *)(void *, std::uint32_t);
    using SteamContextInitFn = void *(__cdecl *)(void *);
    using SteamSendFn = bool(__thiscall *)(void *, std::uint64_t, const void *, std::uint32_t, int, int);
    using SteamPacketAvailableFn = bool(__thiscall *)(void *, std::uint32_t *, int);
    using SteamReceiveFn = bool(__thiscall *)(void *, void *, std::uint32_t, std::uint32_t *,
                                              std::uint64_t *, int);
    constexpr int kLuaTypeNumber = 3;
    constexpr int kLuaTypeString = 4;
    constexpr int kLuaTypeTable = 5;
    constexpr int kLuaTypeFunction = 6;
    constexpr int kLuaRegistryIndex = -1001000;
    constexpr std::int64_t kLuaRegistryGlobals = 2;
    constexpr std::size_t kManifestCapacity = 64;

    struct LuaManifestEntry
    {
        char sync_id[97]{};
        std::uint16_t sync_id_length = 0;
        std::uint32_t version = 0;
        std::uint16_t port = 0;
        int handler_ref = -2;
    };

    HMODULE g_module = nullptr;
    HMODULE g_main_module = nullptr;
    std::uint8_t *g_main_base = nullptr;
    HANDLE g_mapping = nullptr;
    SharedControlBlockV5 *g_shared = nullptr;
    FrameUpdateFn g_frame_update = nullptr;
    DispatchFn g_dispatch = nullptr;
    SteamContextInitFn g_steam_context_init = nullptr;
    SteamSendFn g_steam_send = nullptr;
    SteamPacketAvailableFn g_steam_available = nullptr;
    SteamReceiveFn g_steam_receive = nullptr;
    void *g_steam_interface = nullptr;
    void **g_steam_vtable = nullptr;
    std::array<void *, 3> g_steam_functions{};
    std::uint64_t g_local_steam_id = 0;
    GroupRoster g_roster{};
    GroupRoster g_retired_roster{};
    std::uint32_t g_local_slot = UINT32_MAX;
    void *g_session_manager = nullptr;
    void *g_retired_session_manager = nullptr;
    std::uint32_t g_retired_baseline_frame = 0;
    bool g_require_new_baseline = false;
    std::uint32_t g_last_observed_frame = 0;
    Hash256 g_build_hash{};
    CallPatch g_frame_patch{};
    CallPatch g_dispatch_patch{};
    IatPatch g_steam_context_patch{};
    IatPatch g_lua_close_patch{};
    IatPatch g_lua_newstate_patch{};
    IatPatch g_lua_lnewstate_patch{};
    IatPatch g_lua_pcall_patch{};
    volatile LONG g_unloading = 0;
    volatile LONG g_shutdown_started = 0;
    volatile LONG g_active_hooks = 0;
    volatile LONG g_hooks_ready = 0;
    volatile LONG g_steam_context_state = 0;
    SRWLOCK g_steam_context_lock = SRWLOCK_INIT;

    LuaAdapter g_lua_adapter{};
    auto &g_lua_newstate = g_lua_adapter.newstate;
    auto &g_lua_lnewstate = g_lua_adapter.lnewstate;
    auto &g_lua_close = g_lua_adapter.close;
    auto &g_lua_pcallk = g_lua_adapter.pcallk;
    auto &g_lua_checkstack = g_lua_adapter.checkstack;
    auto &g_lua_gettop = g_lua_adapter.gettop;
    auto &g_lua_settop = g_lua_adapter.settop;
    auto &g_lua_type = g_lua_adapter.type;
    auto &g_lua_createtable = g_lua_adapter.createtable;
    auto &g_lua_pushcclosure = g_lua_adapter.pushcclosure;
    auto &g_lua_rawget = g_lua_adapter.rawget;
    auto &g_lua_rawgeti = g_lua_adapter.rawgeti;
    auto &g_lua_rawset = g_lua_adapter.rawset;
    auto &g_lua_pushnil = g_lua_adapter.pushnil;
    auto &g_lua_pushboolean = g_lua_adapter.pushboolean;
    auto &g_lua_pushinteger = g_lua_adapter.pushinteger;
    auto &g_lua_pushstring = g_lua_adapter.pushstring;
    auto &g_lua_tolstring = g_lua_adapter.tolstring;
    auto &g_lua_tointegerx = g_lua_adapter.tointegerx;
    auto &g_lua_isinteger = g_lua_adapter.isinteger;
    auto &g_lua_pushvalue = g_lua_adapter.pushvalue;
    auto &g_lua_ref = g_lua_adapter.ref;
    auto &g_lua_unref = g_lua_adapter.unref;
    LuaLifecycleTracker g_lua_lifecycle{};
    std::array<LuaManifestEntry, kManifestCapacity> g_manifest{};
    std::uint32_t g_manifest_count = 0;
    Hash256 g_manifest_hash{};
    Hash256 g_pending_manifest_hash{};
    ManifestStartGate g_manifest_start{};
    EmptyManifestAutoStartGate g_empty_manifest_auto_start{};
    ManifestInvalidLatch g_manifest_invalid{};
    volatile LONG g_lua_cleanup_pending = 0;
    volatile LONG g_lua_cleanup_complete = 0;
    volatile LONG g_manifest_reset_pending = 0;
    volatile LONG g_lua_handler_dispatch_depth = 0;
    thread_local std::uint32_t g_lua_create_depth = 0;

    LocalFrameSealer g_local_sealer{};
    std::array<FrameStore, kMaximumPeerCount> g_remote_stores{};
    bool g_frame_stores_initialized = false;
    bool g_warmup_prefix_retired = false;
    RosterEndTracker g_roster_end_tracker{};
    GroupRosterStabilityTracker g_roster_stability{};
    RetiredGroupSessionPacketFilter g_retired_session_filter{};
    ReadyStallTracker g_stall_tracker{};
    std::uint64_t g_test_delay_until_ms = 0;

    struct PreparedFanoutBatch
    {
        SealedFrameBatch batch{};
        std::uint32_t recipient_mask = 0;
        std::uint32_t sent_mask = 0;
        std::uint64_t first_attempt_ms = 0;
        bool sender_committed = false;
        bool active = false;

        void Reset() { *this = {}; }
    };
    PreparedFanoutBatch g_prepared_fanout{};
    bool g_roster_event_published = false;
    std::uint32_t g_last_published_ready_mask = 0;
    bool g_commit_event_published = false;

    class RuntimeSteamApi;
    class RuntimeCallbacks;
    std::unique_ptr<RuntimeSteamApi> g_runtime_steam_api;
    std::unique_ptr<SteamSidecarMeshTransport> g_runtime_transport;
    std::unique_ptr<RuntimeCallbacks> g_runtime_callbacks;
    std::unique_ptr<GroupSessionProtocolEngine> g_session;

    extern "C" bool __fastcall FrameUpdateHook(void *self, void *unused);
    void *__cdecl LuaNewstateHook(LuaAllocFn allocator, void *user_data);
    void *__cdecl LuaLNewstateHook();
    void __cdecl LuaCloseHook(void *state);
    extern "C" std::uint32_t __fastcall DispatchHook(void *self, void *unused,
                                                     std::uint32_t argument);
    void *__cdecl SteamContextInitHook(void *init_data);
    int __cdecl LuaPcallkHook(void *state, int nargs, int nresults, int errfunc,
                              std::intptr_t context, void *continuation);
    int __cdecl LuaRegister(void *state);
    int __cdecl LuaStart(void *state);
    int __cdecl LuaIsActive(void *state);
    int __cdecl LuaGetLocalSlot(void *state);
    int __cdecl LuaGetPeerCount(void *state);
    int __cdecl LuaGetPlayerIndexForSlot(void *state);
    int __cdecl LuaSend(void *state);
    bool EnsureFrameStores(std::uint32_t frame);

    void EnterHook() { InterlockedIncrement(&g_active_hooks); }
    void LeaveHook() { InterlockedDecrement(&g_active_hooks); }

    std::uint64_t MonotonicMs()
    {
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart <= 0)
            return GetTickCount64();
        const auto whole_seconds = counter.QuadPart / frequency.QuadPart;
        const auto remainder = counter.QuadPart % frequency.QuadPart;
        return static_cast<std::uint64_t>(whole_seconds) * 1000u +
               static_cast<std::uint64_t>((remainder * 1000) / frequency.QuadPart);
    }

    void CopyText(char *destination, std::size_t capacity, const char *text)
    {
        if (!destination || !capacity)
            return;
        strncpy_s(destination, capacity, text ? text : "", _TRUNCATE);
    }

    void FormatAnonymousPlayerMapping(const GroupRoster &roster, char (&buffer)[128])
    {
        CopyText(buffer, sizeof(buffer), ISAAC_SYNC_PRODUCT_VERSION " roster");
        for (std::uint16_t slot = 0; slot < roster.peer_count; ++slot)
        {
            const std::size_t used = std::strlen(buffer);
            if (used >= sizeof(buffer) - 1)
                return;
            _snprintf_s(buffer + used, sizeof(buffer) - used, _TRUNCATE,
                        slot == 0 ? " slot%u->player%u" : ", slot%u->player%u",
                        static_cast<unsigned>(slot),
                        static_cast<unsigned>(roster.player_index_by_slot[slot]));
        }
    }

    std::uint64_t SessionToken(const SessionId &session)
    {
        std::uint64_t value = 0;
        std::memcpy(&value, session.data(), sizeof(value));
        return value;
    }

    void FailRuntime(ErrorCode error, const char *detail, bool fatal)
    {
        if (!g_shared)
            return;
        if (g_session)
        {
            const RuntimeState session_state = g_session->State();
            if (session_state == RuntimeState::Discovering || session_state == RuntimeState::Hello ||
                session_state == RuntimeState::Warmup || session_state == RuntimeState::Active)
            {
                g_session->FailExternal(error, detail);
            }
            fatal = fatal || g_session->State() == RuntimeState::FailedFatal;
        }
        InterlockedExchange(&g_shared->error_code, static_cast<LONG>(error));
        CopyText(g_shared->error_detail, sizeof(g_shared->error_detail), detail);
        SetRuntimeState(fatal ? RuntimeState::FailedFatal : RuntimeState::FailedPreActive,
                        fatal ? "FAILED_FATAL：同步扩展已停止 frame 推进"
                              : "FAILED_PRE_ACTIVE：sidecar 已关闭，原版继续");
        PublishEvent(IpcEventType::SyncFailure, detail,
                     static_cast<std::uint32_t>(error), fatal ? 1u : 0u);
    }

    bool InstallFramePatch()
    {
        auto *call = g_main_base + kFrameCallRva;
        return InstallRelativeCallPatch(
            call, kFrameCallBytes.data(), kFrameCallBytes.size(),
            reinterpret_cast<void *>(&FrameUpdateHook), g_frame_patch,
            reinterpret_cast<void *volatile *>(&g_frame_update));
    }

    bool InstallDispatchPatch()
    {
        auto *call = g_main_base + kDispatchCallRva;
        if (std::memcmp(g_main_base + kDispatchCallRva - 4, kDispatchFeature.data(),
                        kDispatchFeature.size()) != 0)
            return false;
        return InstallRelativeCallPatch(
            call, kDispatchCallBytes.data(), kDispatchCallBytes.size(),
            reinterpret_cast<void *>(&DispatchHook), g_dispatch_patch,
            reinterpret_cast<void *volatile *>(&g_dispatch));
    }

    bool RestoreFramePatch()
    {
        return RestoreRelativeCallPatch(g_frame_patch);
    }

    bool RestoreDispatchPatch()
    {
        return RestoreRelativeCallPatch(g_dispatch_patch);
    }

    bool ValidateSteamFunctions()
    {
        return ValidateSteamNetworking006(
            {g_steam_interface, g_steam_vtable, g_steam_functions});
    }

    bool TryCaptureSteamContextUnsafe(void *context)
    {
        SteamNetworking006Context captured{};
        const SteamContextCaptureResult result = CaptureSteamNetworking006(context, captured);
        if (result == SteamContextCaptureResult::OwnerMismatch)
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::SteamVtableOwnerMismatch,
                             "DORMANT：检测到未知 Steam 网络 Hook",
                             "SteamNetworking006 前三个函数并非全部属于 steamclient.dll");
            InterlockedExchange(&g_steam_context_state, -1);
            return false;
        }
        if (result != SteamContextCaptureResult::Captured)
            return false;
        g_steam_interface = captured.interface_pointer;
        g_steam_vtable = captured.vtable;
        g_steam_functions = captured.functions;
        g_steam_send = reinterpret_cast<SteamSendFn>(captured.functions[0]);
        g_steam_available = reinterpret_cast<SteamPacketAvailableFn>(captured.functions[1]);
        g_steam_receive = reinterpret_cast<SteamReceiveFn>(captured.functions[2]);
        InterlockedExchange(&g_steam_context_state, 1);
        PublishEvent(IpcEventType::SteamContext,
                     "SteamNetworking006 已捕获并缓存原函数（未修改虚表）",
                     reinterpret_cast<std::uintptr_t>(captured.interface_pointer),
                     reinterpret_cast<std::uintptr_t>(captured.vtable));
        return true;
    }

    bool TryCaptureSteamContext(void *context)
    {
        if (InterlockedCompareExchange(&g_steam_context_state, 0, 0) != 0)
        {
            return g_steam_interface != nullptr;
        }
        AcquireSRWLockExclusive(&g_steam_context_lock);
        bool result = g_steam_interface != nullptr;
        if (!result && InterlockedCompareExchange(&g_steam_context_state, 0, 0) == 0)
        {
            __try
            {
                result = TryCaptureSteamContextUnsafe(context);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = false;
            }
        }
        ReleaseSRWLockExclusive(&g_steam_context_lock);
        return result;
    }

    bool DiscoverCompatibility(void *sync_manager, std::uint32_t frame)
    {
        if (!g_shared || InterlockedCompareExchange(&g_unloading, 0, 0) != 0)
            return false;
        if (g_require_new_baseline)
        {
            if (sync_manager == g_retired_session_manager &&
                frame >= g_retired_baseline_frame)
            {
                SetCompatibility(CompatibilityState::WaitingForRoster, ErrorCode::None,
                                 "DORMANT：等待新 session manager 或 online frame baseline");
                return false;
            }
            g_require_new_baseline = false;
            g_retired_session_manager = nullptr;
            g_roster_stability.Reset();
        }
        if (GetModuleHandleW(L"tractor_beam_native_hook.dll"))
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::ThirdPartyHook,
                             "DORMANT：不支持当前第三方网络 Hook",
                             "检测到 tractor_beam_native_hook.dll");
            return false;
        }
        void *backend = nullptr;
        void **backend_vtable = nullptr;
        __try
        {
            backend = *reinterpret_cast<void **>(g_main_base + kNetworkBackendObjectRva);
            backend_vtable = backend ? *reinterpret_cast<void ***>(backend) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            backend = nullptr;
        }
        if (!backend || !backend_vtable)
        {
            SetCompatibility(CompatibilityState::WaitingForSteam, ErrorCode::None,
                             "DORMANT：等待 Steam 联机后端");
            return false;
        }
        if (backend_vtable != reinterpret_cast<void **>(g_main_base + kSteamBackendVtableRva))
        {
            g_shared->backend = backend_vtable == reinterpret_cast<void **>(
                                                      g_main_base + kEosBackendVtableRva)
                                    ? static_cast<std::uint32_t>(BackendKind::Eos)
                                    : static_cast<std::uint32_t>(BackendKind::Unknown);
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::BackendMismatch,
                             "DORMANT：当前网络后端不受支持",
                             "Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION
                             " 仅支持 backend 2 / SteamNetworking006");
            return false;
        }
        g_shared->backend = static_cast<std::uint32_t>(BackendKind::Steam);
        if (!g_steam_interface && g_steam_context_init)
        {
            bool descriptor_matches = false;
            __try
            {
                descriptor_matches = *reinterpret_cast<void **>(g_main_base + kSteamNetworkingContextRva) ==
                                     g_main_base + kSteamNetworkingContextInitRva;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                descriptor_matches = false;
            }
            if (descriptor_matches)
            {
                void *context = g_steam_context_init(g_main_base + kSteamNetworkingContextRva);
                TryCaptureSteamContext(context);
            }
        }
        if (!g_steam_interface || !ValidateSteamFunctions())
        {
            SetCompatibility(CompatibilityState::WaitingForSteam, ErrorCode::None,
                             "DORMANT：等待 SteamNetworking006 上下文");
            return false;
        }
        if (!g_local_steam_id)
        {
            const auto identity = ResolveLocalSteamId(g_local_steam_id);
            if (identity == LocalSteamIdResult::MissingExports)
            {
                SetCompatibility(CompatibilityState::Unsupported, ErrorCode::SteamExportsMissing,
                                 "DORMANT：无法读取本地 Steam 身份",
                                 "steam_api.dll 缺少 V2 所需 flat API exports");
                return false;
            }
            if (identity == LocalSteamIdResult::Waiting)
            {
                SetCompatibility(CompatibilityState::WaitingForSteam, ErrorCode::None,
                                 "DORMANT：等待本地 Steam 用户身份");
                return false;
            }
        }
        GroupRoster observed{};
        const auto roster = ReadGroupSteamRoster(sync_manager, g_local_steam_id, observed);
        if (roster == RosterReadResult::Waiting || roster == RosterReadResult::FewerThanTwo)
        {
            SetCompatibility(CompatibilityState::WaitingForRoster, ErrorCode::None,
                             "DORMANT：等待稳定的 2–4 Steam peer roster");
            return false;
        }
        if (roster != RosterReadResult::Ready)
        {
            const ErrorCode error = roster == RosterReadResult::LocalMismatch
                                        ? ErrorCode::LocalIdentityMismatch
                                        : ErrorCode::RosterShape;
            SetCompatibility(CompatibilityState::Unsupported, error,
                             "DORMANT：不支持当前玩家布局",
                             "Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION
                             " 要求 2–4 个不同 SteamID64、本地身份唯一且玩家映射有效");
            return false;
        }
        const RosterStabilityResult stability =
            g_roster_stability.Observe(observed, g_roster.peer_count != 0);
        if (stability == RosterStabilityResult::Waiting)
        {
            SetCompatibility(CompatibilityState::WaitingForRoster, ErrorCode::None,
                             "DORMANT：等待第二次一致 roster 观察");
            return false;
        }
        if (stability == RosterStabilityResult::Changed)
            return false;
        const bool roster_changed = g_roster.peer_count == 0 || g_roster != observed;
        g_roster = g_roster_stability.Frozen();
        g_local_slot = g_roster.local_slot;
        g_shared->peer_count = g_roster.peer_count;
        g_shared->local_slot = g_local_slot;
        g_shared->coordinator_slot = 0;
        g_shared->frozen_slot_mask = g_roster.frozen_slot_mask;
        const char *ready_detail = !g_manifest_start.StartRequested()
                                       ? g_manifest_count == 0
                                             ? "DORMANT：Steam group Ready；空 manifest 注册窗口 2 秒"
                                             : "DORMANT：Steam group Ready；等待 Lua Start"
                                   : !g_manifest_start.Frozen()
                                       ? "Steam group Ready；等待 Lua callback 冻结 manifest"
                                       : "Steam group Ready；等待正式 session";
        SetCompatibility(CompatibilityState::Ready, ErrorCode::None, ready_detail);
        if (roster_changed)
        {
            char mapping[128]{};
            FormatAnonymousPlayerMapping(g_roster, mapping);
            PublishEvent(IpcEventType::Roster, mapping,
                         g_roster.peer_count, g_roster.frozen_slot_mask);
        }
        return true;
    }

    enum class SessionEnvironmentResult
    {
        Ready,
        PendingEnd,
        Ended,
        Invalid
    };

    SessionEnvironmentResult ValidateSessionEnvironment(void *sync_manager)
    {
        if (!g_session)
            return SessionEnvironmentResult::Invalid;
        const bool fatal = g_session->State() == RuntimeState::Active;
        const auto fail = [fatal](ErrorCode error, const char *detail)
        {
            FailRuntime(error, detail, fatal);
            return SessionEnvironmentResult::Invalid;
        };
        if (GetModuleHandleW(L"tractor_beam_native_hook.dll"))
        {
            return fail(ErrorCode::ThirdPartyHook,
                        "third-party networking hook appeared after session freeze");
        }
        void *backend = nullptr;
        void **backend_vtable = nullptr;
        __try
        {
            backend = *reinterpret_cast<void **>(g_main_base + kNetworkBackendObjectRva);
            backend_vtable = backend ? *reinterpret_cast<void ***>(backend) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            backend = nullptr;
        }
        if (!backend || !backend_vtable)
        {
            return SessionEnvironmentResult::Ended;
        }
        if (backend_vtable != reinterpret_cast<void **>(g_main_base + kSteamBackendVtableRva))
        {
            return fail(ErrorCode::BackendMismatch,
                        "Steam backend changed after session freeze");
        }
        if (!ValidateSteamFunctions())
        {
            return fail(ErrorCode::SteamFunctionsChanged,
                        "SteamNetworking006 function pointers changed after validation");
        }
        GroupRoster observed{};
        const RosterReadResult roster =
            ReadGroupSteamRoster(sync_manager, g_local_steam_id, observed);
        if (roster == RosterReadResult::Waiting)
        {
            // A transient vector/identity read is not proof that the original
            // peer left. Keep the current gate semantics and wait for either a
            // valid roster or the explicit fewer-than-two observation.
            g_roster_end_tracker.ObserveReady();
            return SessionEnvironmentResult::PendingEnd;
        }
        if (roster != RosterReadResult::Ready)
        {
            if (!g_roster_end_tracker.ObserveMissing())
            {
                return SessionEnvironmentResult::PendingEnd;
            }
            return SessionEnvironmentResult::Ended;
        }
        g_roster_end_tracker.ObserveReady();
        const RosterStabilityResult stability = g_roster_stability.Observe(observed, true);
        if (stability == RosterStabilityResult::Changed)
        {
            return SessionEnvironmentResult::Ended;
        }
        return stability == RosterStabilityResult::Stable
                   ? SessionEnvironmentResult::Ready
                   : SessionEnvironmentResult::PendingEnd;
    }

    void *__cdecl SteamContextInitHook(void *init_data)
    {
        EnterHook();
        void *context = g_steam_context_init(init_data);
        if (InterlockedCompareExchange(&g_hooks_ready, 0, 0) != 0 &&
            InterlockedCompareExchange(&g_unloading, 0, 0) == 0 &&
            init_data == g_main_base + kSteamNetworkingContextRva)
        {
            bool descriptor_matches = false;
            __try
            {
                descriptor_matches = *reinterpret_cast<void **>(init_data) ==
                                     g_main_base + kSteamNetworkingContextInitRva;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                descriptor_matches = false;
            }
            if (descriptor_matches)
                TryCaptureSteamContext(context);
        }
        LeaveHook();
        return context;
    }

    ErrorCode FrameStoreError(FrameStoreResult result)
    {
        switch (result)
        {
        case FrameStoreResult::InvalidOperation:
            return ErrorCode::FramePayloadInvalid;
        case FrameStoreResult::Window:
        case FrameStoreResult::Overflow:
            return ErrorCode::FrameWindow;
        case FrameStoreResult::Conflict:
            return ErrorCode::FrameConflict;
        case FrameStoreResult::Capacity:
            return ErrorCode::FrameStoreCapacity;
        default:
            return ErrorCode::FrameStateViolation;
        }
    }

    LuaManifestEntry *FindManifest(std::uint16_t port)
    {
        for (std::uint32_t index = 0; index < g_manifest_count; ++index)
        {
            if (g_manifest[index].port == port)
                return &g_manifest[index];
        }
        return nullptr;
    }

    void MarkManifestDerivedPortCollision()
    {
        g_manifest_invalid.Invalidate();
        if (!g_shared)
            return;
        InterlockedExchange(&g_shared->error_code,
                            static_cast<LONG>(ErrorCode::ManifestDerivedPortCollision));
        CopyText(g_shared->error_detail, sizeof(g_shared->error_detail),
                 "manifest has derived port collision");
        PublishEvent(IpcEventType::Error, "manifest derived port collision");
    }

    std::uint64_t PeerTokenForSlot(std::uint32_t slot)
    {
        return g_shared && slot < kMaximumPeerCount ? g_shared->peer_tokens[slot] : 0;
    }

    void UpdateReceivedThroughSummary()
    {
        if (!g_shared || g_roster.peer_count < 2)
            return;
        std::uint32_t minimum = UINT32_MAX;
        for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
        {
            const std::uint32_t through = slot == g_local_slot
                                              ? g_local_sealer.SealedThrough()
                                              : g_remote_stores[slot].DisplayThrough();
            InterlockedExchange(&g_shared->received_through_by_slot[slot],
                                static_cast<LONG>(through));
            if (slot != g_local_slot)
                minimum = (std::min)(minimum, through);
        }
        InterlockedExchange(&g_shared->received_through, static_cast<LONG>(minimum));
    }

    void PublishOperationEvent(IpcEventType type, const char *text, std::uint32_t frame,
                               std::uint32_t sender_slot, const CustomFrameOpV2 &operation,
                               ErrorCode error = ErrorCode::None)
    {
        auto *event = BeginEvent(type);
        if (!event)
            return;
        event->error = static_cast<std::uint32_t>(error);
        event->session_token = SessionToken(g_session ? g_session->Session() : SessionId{});
        event->peer_token = PeerTokenForSlot(sender_slot);
        event->operation_frame = frame;
        event->sender_slot = sender_slot;
        event->recipient_slot = UINT32_MAX;
        event->port = operation.port;
        event->opcode = operation.opcode;
        event->payload_kind = operation.payload_kind;
        event->payload_bits = operation.payload_bits;
        CopyText(event->message, sizeof(event->message), text);
        CommitEvent(event);
    }

    class RuntimeCallbacks final : public IGroupCallbacks
    {
    public:
        ErrorCode OnFrameBatch(std::uint16_t sender_slot, bool sender_committed,
                               const DecodedFrameBatch &batch,
                               std::uint32_t local_online_frame) override
        {
            (void)sender_committed;
            if (!EnsureFrameStores(local_online_frame))
                return ErrorCode::FrameStateViolation;
            if (sender_slot >= g_roster.peer_count || sender_slot == g_local_slot)
                return ErrorCode::PacketSender;
            std::array<bool, kMaxBatchRecords> new_records{};
            for (std::uint16_t index = 0; index < batch.count; ++index)
            {
                const std::uint32_t frame = batch.first_frame + index;
                const auto &operation = batch.records[index];
                if (!IsCanonicalOperation(operation))
                    return ErrorCode::FramePayloadInvalid;
                if (!IsNoOp(operation) && !FindManifest(operation.port))
                    return ErrorCode::FrameUnknownPort;
                const auto *existing = g_remote_stores[sender_slot].FindAny(frame);
                new_records[index] = existing == nullptr;
                if (existing && std::memcmp(existing, &operation, sizeof(operation)) != 0)
                    return ErrorCode::FrameConflict;
            }
            const FrameStoreResult result = g_remote_stores[sender_slot].InsertBatchAtomic(
                batch.first_frame, batch.records.data(), batch.count, local_online_frame);
            if (result != FrameStoreResult::Inserted && result != FrameStoreResult::Duplicate)
                return FrameStoreError(result);
            std::uint64_t inserted = 0;
            std::uint64_t duplicates = 0;
            for (std::uint16_t index = 0; index < batch.count; ++index)
            {
                if (new_records[index])
                {
                    ++inserted;
                    if (!IsNoOp(batch.records[index]))
                    {
                        InterlockedIncrement64(&g_shared->non_empty_received);
                        PublishOperationEvent(IpcEventType::SyncRecordReceived,
                                              "non-empty record received",
                                              batch.first_frame + index, sender_slot,
                                              batch.records[index]);
                    }
                }
                else
                    ++duplicates;
            }
            if (g_shared)
            {
                InterlockedIncrement64(&g_shared->batches_received);
                InterlockedAdd64(&g_shared->records_received, static_cast<LONG64>(inserted));
                InterlockedAdd64(&g_shared->duplicate_records, static_cast<LONG64>(duplicates));
                InterlockedAdd64(&g_shared->records_received_by_slot[sender_slot],
                                 static_cast<LONG64>(inserted));
                UpdateReceivedThroughSummary();
            }
            return ErrorCode::None;
        }
    };

    class RuntimeSteamApi final : public ISteamNetworking006Api
    {
    public:
        bool Send(std::uint64_t remote_steam_id, const void *data, std::uint32_t size,
                  int send_mode, int channel) override
        {
            std::uint16_t recipient_slot = UINT16_MAX;
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                if (slot != g_local_slot && g_roster.steam_ids[slot] == remote_steam_id)
                {
                    recipient_slot = slot;
                    break;
                }
            }
            if (!g_shared || !ValidateSteamFunctions() || !g_steam_send ||
                recipient_slot == UINT16_MAX)
            {
                return false;
            }
            InterlockedIncrement64(&g_shared->channel_fd_calls);
            InterlockedIncrement64(&g_shared->channel_fd_sends);
            const bool sent = g_steam_send(g_steam_interface, remote_steam_id, data, size,
                                           send_mode, channel);
            SidecarHeaderV1 header{};
            if (size >= sizeof(header))
                std::memcpy(&header, data, sizeof(header));
            if (sent)
            {
                InterlockedIncrement64(&g_shared->packets_sent);
                InterlockedIncrement64(&g_shared->packets_sent_by_slot[recipient_slot]);
                std::uint32_t first_frame = 0;
                std::uint16_t count = 0;
                if (header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch) &&
                    size >= sizeof(header) + sizeof(FrameBatchPrefixV1))
                {
                    FrameBatchPrefixV1 prefix{};
                    std::memcpy(&prefix, static_cast<const std::uint8_t *>(data) + sizeof(header),
                                sizeof(prefix));
                    first_frame = prefix.first_frame;
                    count = prefix.record_count;
                }
                PublishEvent(header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch)
                                 ? IpcEventType::SyncFrameSend
                                 : IpcEventType::SyncHandshake,
                             "channel 0xFD packet sent", first_frame, size,
                             header.message_sequence, SessionToken(g_session ? g_session->Session() : SessionId{}),
                              PeerTokenForSlot(recipient_slot), header.crc32, header.kind, count,
                              0, g_local_slot, 0, 0, 0, recipient_slot);
            }
            return sent;
        }

        bool Available(std::uint32_t *size, int channel) override
        {
            if (!g_shared || !ValidateSteamFunctions() || !g_steam_available)
                return false;
            InterlockedIncrement64(&g_shared->channel_fd_calls);
            InterlockedIncrement64(&g_shared->channel_fd_polls);
            return g_steam_available(g_steam_interface, size, channel);
        }

        bool Read(void *buffer, std::uint32_t capacity, std::uint32_t *size,
                  std::uint64_t *remote_steam_id, int channel) override
        {
            if (!g_shared || !ValidateSteamFunctions() || !g_steam_receive)
                return false;
            InterlockedIncrement64(&g_shared->channel_fd_calls);
            InterlockedIncrement64(&g_shared->channel_fd_receives);
            return g_steam_receive(g_steam_interface, buffer, capacity, size,
                                   remote_steam_id, channel);
        }
    };

    class RuntimePacketConsumer final : public ISteamPacketConsumer
    {
    public:
        RuntimePacketConsumer(std::uint32_t frame, std::uint64_t now_ms)
            : frame_(frame), now_ms_(now_ms) {}

        bool Consume(const std::uint8_t *packet, std::uint32_t size,
                     std::uint64_t remote_steam_id) override
        {
            DecodedPacket retired{};
            std::uint16_t sender_slot = UINT16_MAX;
            const RetiredPacketResult retired_result = g_retired_session_filter.Consume(
                packet, size, remote_steam_id, retired, sender_slot);
            if (retired_result == RetiredPacketResult::Discarded)
            {
                InterlockedIncrement64(&g_shared->packets_received);
                std::uint32_t first_frame = 0;
                std::uint16_t count = 0;
                if (retired.header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch) &&
                    retired.payload_size >= sizeof(FrameBatchPrefixV1))
                {
                    FrameBatchPrefixV1 prefix{};
                    std::memcpy(&prefix, retired.payload, sizeof(prefix));
                    first_frame = prefix.first_frame;
                    count = prefix.record_count;
                }
                PublishEvent(retired.header.kind == static_cast<std::uint8_t>(
                                                        MessageKind::FrameBatch)
                                 ? IpcEventType::SyncFrameReceive
                                 : IpcEventType::SyncHandshake,
                             "channel 0xFD stale packet received and discarded",
                             first_frame, size, retired.header.message_sequence,
                             SessionToken(g_retired_session_filter.Session()),
                              0, retired.header.crc32, retired.header.kind, count,
                              0, sender_slot, 0, 0, 0, g_local_slot);
                return true;
            }
            if (retired_result == RetiredPacketResult::Invalid)
            {
                FailRuntime(ErrorCode::PacketSequence,
                            "retired group packet failed tombstone validation",
                            g_session && g_session->State() == RuntimeState::Active);
                return false;
            }
            sender_slot = g_runtime_transport
                              ? g_runtime_transport->FindCurrentSlot(remote_steam_id)
                              : UINT16_MAX;
            if (!g_session || sender_slot == UINT16_MAX ||
                !g_session->Receive(packet, size, sender_slot, remote_steam_id,
                                    frame_, now_ms_))
            {
                if (g_session)
                {
                    FailRuntime(g_session->Error(), g_session->ErrorText(),
                                g_session->State() == RuntimeState::FailedFatal);
                }
                return false;
            }
            InterlockedIncrement64(&g_shared->packets_received);
            InterlockedIncrement64(&g_shared->packets_received_by_slot[sender_slot]);
            SidecarHeaderV1 header{};
            std::memcpy(&header, packet, sizeof(header));
            std::uint32_t first_frame = 0;
            std::uint16_t count = 0;
            if (header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch) &&
                size >= sizeof(header) + sizeof(FrameBatchPrefixV1))
            {
                FrameBatchPrefixV1 prefix{};
                std::memcpy(&prefix, packet + sizeof(header), sizeof(prefix));
                first_frame = prefix.first_frame;
                count = prefix.record_count;
            }
            PublishEvent(header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch)
                             ? IpcEventType::SyncFrameReceive
                             : IpcEventType::SyncHandshake,
                         "channel 0xFD packet received", first_frame, size,
                         header.message_sequence, SessionToken(g_session->Session()),
                          PeerTokenForSlot(sender_slot), header.crc32, header.kind, count,
                          0, sender_slot, 0, 0, 0, g_local_slot);
            return true;
        }

    private:
        std::uint32_t frame_ = 0;
        std::uint64_t now_ms_ = 0;
    };

    bool IsSmokeManifest()
    {
        return g_manifest_count == 1 && g_manifest[0].version == kSmokeVersion &&
               g_manifest[0].port == kSmokePort &&
               std::strcmp(g_manifest[0].sync_id, kSmokeSyncId) == 0;
    }

    void ProcessTestCommand(std::uint64_t now_ms)
    {
        if (!g_shared)
            return;
        const LONG64 command = InterlockedCompareExchange64(&g_shared->test_command_sequence, 0, 0);
        const LONG64 ack = InterlockedCompareExchange64(&g_shared->test_ack_sequence, 0, 0);
        if (command == 0 || command == ack)
            return;
        TestCommandResult result = TestCommandResult::None;
        const LONG delay = InterlockedCompareExchange(&g_shared->test_delay_ms, 0, 0);
        const auto state = static_cast<RuntimeState>(
            InterlockedCompareExchange(&g_shared->runtime_state, 0, 0));
        if (state != RuntimeState::Active)
            result = TestCommandResult::NotActive;
        else if (!IsSmokeManifest())
            result = TestCommandResult::WrongManifest;
        else if (delay < 1 || delay > 1000)
            result = TestCommandResult::InvalidDelay;
        else if (now_ms < g_test_delay_until_ms)
            result = TestCommandResult::Busy;
        else
        {
            g_test_delay_until_ms = now_ms + static_cast<std::uint64_t>(delay);
            result = TestCommandResult::Accepted;
            PublishEvent(IpcEventType::TestDelay, "测试命令：暂停 sidecar receive drain",
                         static_cast<std::uint32_t>(delay), command);
        }
        InterlockedExchange(&g_shared->test_command_result, static_cast<LONG>(result));
        InterlockedExchange64(&g_shared->test_ack_sequence, command);
    }

    bool DrainPackets(std::uint32_t frame, std::uint64_t now_ms)
    {
        if (!g_session || !g_shared || !g_runtime_transport ||
            now_ms < g_test_delay_until_ms)
            return true;
        const bool fatal = g_session->State() == RuntimeState::Active;
        RuntimePacketConsumer consumer(frame, now_ms);
        const SteamDrainResult result = g_runtime_transport->Drain(
            consumer, kSteamDrainLimit);
        if (result == SteamDrainResult::None)
            return true;
        InterlockedIncrement64(&g_shared->packets_rejected);
        if (result == SteamDrainResult::ConsumerRejected)
            return false;
        if (result == SteamDrainResult::InvalidPacketSize)
        {
            FailRuntime(ErrorCode::PacketSize, "Steam reported an invalid 0xFD packet size", fatal);
        }
        else if (result == SteamDrainResult::WrongPeer)
        {
            FailRuntime(ErrorCode::PacketSender,
                        "Steam channel 0xFD returned a packet from the wrong peer", fatal);
        }
        else
        {
            FailRuntime(ErrorCode::TransportFailure,
                        "Steam channel 0xFD receive failed or returned the wrong size", fatal);
        }
        return false;
    }

    bool GenerateSessionId(SessionId &session)
    {
        session.fill(0);
        return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, session.data(),
                                              static_cast<ULONG>(session.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    void PublishSessionFlags()
    {
        if (!g_shared || !g_session)
            return;
        LONG flags = InterlockedCompareExchange(&g_shared->flags, 0, 0);
        flags &= ~(IpcFlagSessionBound | IpcFlagCoordinator | IpcFlagActivationCommitted);
        if (g_session->SessionBound())
            flags |= IpcFlagSessionBound;
        if (g_session->IsCoordinator())
            flags |= IpcFlagCoordinator;
        if (g_session->Committed())
            flags |= IpcFlagActivationCommitted;
        InterlockedExchange(&g_shared->flags, flags);
        std::memcpy(g_shared->session_id, g_session->Session().data(), g_session->Session().size());
        InterlockedExchange(&g_shared->activation_frame,
                            static_cast<LONG>(g_session->ActivationFrame()));
        InterlockedExchange(&g_shared->activation_ready_mask,
                            static_cast<LONG>(g_session->ActivationReadyMask()));
        if (g_session->ActivationReadyMask() != g_last_published_ready_mask)
        {
            g_last_published_ready_mask = g_session->ActivationReadyMask();
            PublishEvent(IpcEventType::GroupActivationReady,
                         "group activation ready mask advanced",
                         g_last_published_ready_mask, g_roster.frozen_slot_mask,
                         0, SessionToken(g_session->Session()));
        }
        if (g_session->Committed() && !g_commit_event_published)
        {
            g_commit_event_published = true;
            PublishEvent(IpcEventType::GroupActivationCommit,
                         "group activation committed",
                         g_session->ActivationFrame(), g_roster.frozen_slot_mask,
                         0, SessionToken(g_session->Session()));
        }
        for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
        {
            InterlockedExchange64(&g_shared->next_receive_sequence_by_slot[slot],
                                  static_cast<LONG64>(slot == g_local_slot
                                                          ? 0
                                                          : g_session->NextReceiveSequence(slot)));
        }
        if (g_session->SessionBound())
        {
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                std::uint64_t token = 0;
                if (ComputePeerToken(g_session->Session(), g_roster.steam_ids[slot], token))
                    g_shared->peer_tokens[slot] = token;
            }
            g_shared->local_peer_token = PeerTokenForSlot(g_local_slot);
            g_shared->remote_peer_token = 0;
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                if (slot != g_local_slot)
                {
                    g_shared->remote_peer_token = PeerTokenForSlot(slot);
                    break;
                }
            }
            if (!g_roster_event_published)
            {
                std::uint64_t roster_hash_prefix = 0;
                std::memcpy(&roster_hash_prefix, g_roster.roster_hash.data(),
                            sizeof(roster_hash_prefix));
                char mapping[128]{};
                FormatAnonymousPlayerMapping(g_roster, mapping);
                PublishEvent(IpcEventType::GroupRosterFrozen,
                             mapping, roster_hash_prefix,
                             g_roster.frozen_slot_mask, 0,
                             SessionToken(g_session->Session()),
                             PeerTokenForSlot(g_local_slot), 0, 0,
                             g_roster.peer_count, 0, g_local_slot);
                g_roster_event_published = true;
                for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
                {
                    if (slot == g_local_slot)
                        continue;
                    PublishEvent(IpcEventType::GroupPeerLink,
                                 "full-mesh peer link frozen", g_local_slot, slot,
                                 0, SessionToken(g_session->Session()),
                                 PeerTokenForSlot(slot), 0, 0, 0, 0,
                                 g_local_slot, 0, 0, 0, slot);
                }
            }
        }
    }

    bool StartSession(void *manager, std::uint32_t frame, std::uint64_t now_ms)
    {
        if (g_session)
            return true;
        if (!g_runtime_steam_api || !g_runtime_callbacks)
            return false;
        GroupSessionConfig config{};
        config.build_hash = g_build_hash;
        config.manifest_hash = g_manifest_hash;
        config.roster_hash = g_roster.roster_hash;
        config.steam_ids = g_roster.steam_ids;
        config.player_slots_in_game_order = g_roster.player_slots_in_game_order;
        config.peer_count = g_roster.peer_count;
        config.local_slot = g_roster.local_slot;
        config.local_online_frame = frame;
        if (config.peer_count < 2 || config.local_slot >= config.peer_count)
        {
            FailRuntime(ErrorCode::GroupPeerCountMismatch,
                        "frozen group roster is unavailable", false);
            return false;
        }
        SessionId session{};
        if (g_local_slot == 0 && !GenerateSessionId(session))
        {
            FailRuntime(ErrorCode::GroupHandshakeMismatch,
                        "failed to generate group session ID", false);
            return false;
        }
        g_runtime_transport = std::make_unique<SteamSidecarMeshTransport>(
            *g_runtime_steam_api, g_roster.steam_ids, g_roster.peer_count,
            g_roster.local_slot);
        if (g_retired_roster.peer_count != 0)
            g_runtime_transport->SetRetiredRoster(g_retired_roster.steam_ids,
                                                  g_retired_roster.peer_count,
                                                  g_retired_roster.local_slot);
        g_session = std::make_unique<GroupSessionProtocolEngine>(*g_runtime_transport,
                                                                 *g_runtime_callbacks);
        if (!g_session->Start(config, session, now_ms))
        {
            FailRuntime(g_session->Error(), g_session->ErrorText(), false);
            return false;
        }
        g_session_manager = manager;
        SetRuntimeState(g_session->State(), g_session->IsCoordinator()
                                                ? "HELLO：coordinator 已发送正式 HELLO"
                                                : "DISCOVERING：等待 coordinator HELLO");
        PublishSessionFlags();
        PublishEvent(IpcEventType::SyncHandshake,
                     "正式 " ISAAC_SYNC_PRODUCT_VERSION " group session 已启动",
                     g_local_slot, frame, 0, SessionToken(g_session->Session()));
        return true;
    }

    bool EnsureFrameStores(std::uint32_t frame)
    {
        if (g_frame_stores_initialized)
            return true;
        g_local_sealer.Reset(frame);
        for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
        {
            if (slot != g_local_slot)
                g_remote_stores[slot].Reset(frame, FrameSlotState::ReceivedRemote);
        }
        g_frame_stores_initialized = true;
        g_warmup_prefix_retired = false;
        if (g_shared)
        {
            InterlockedExchange(&g_shared->sealed_through, static_cast<LONG>(UINT32_MAX));
            InterlockedExchange(&g_shared->received_through, static_cast<LONG>(UINT32_MAX));
            InterlockedExchange(&g_shared->consumed_through, static_cast<LONG>(UINT32_MAX));
        }
        return true;
    }

    bool SealAndSend(std::uint32_t frame, std::uint64_t now_ms)
    {
        if (!g_session || !EnsureFrameStores(frame))
            return false;
        if (frame > UINT32_MAX - kLeadFrames)
        {
            FailRuntime(ErrorCode::FrameWindow, "F + leadFrames overflowed",
                        g_session->State() == RuntimeState::Active);
            return false;
        }
        const std::uint32_t target = frame + kLeadFrames;
        for (;;)
        {
            if (!g_prepared_fanout.active)
            {
                SealedFrameBatch batch{};
                const auto result = g_local_sealer.PrepareBatch(target, batch);
                if (result == FrameStoreResult::Missing)
                    break;
                if (result != FrameStoreResult::Inserted)
                {
                    FailRuntime(FrameStoreError(result), "local frame batch prepare failed",
                                g_session->State() == RuntimeState::Active);
                    return false;
                }
                g_prepared_fanout.batch = batch;
                g_prepared_fanout.recipient_mask =
                    g_roster.frozen_slot_mask & ~(1u << g_local_slot);
                g_prepared_fanout.sent_mask = 0;
                g_prepared_fanout.first_attempt_ms = now_ms;
                g_prepared_fanout.sender_committed = g_session->Committed();
                g_prepared_fanout.active = true;
            }
            bool send_failed = false;
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                const std::uint32_t bit = 1u << slot;
                if ((g_prepared_fanout.recipient_mask & bit) == 0 ||
                    (g_prepared_fanout.sent_mask & bit) != 0)
                    continue;
                if (g_session->SendFrameBatchTo(
                        slot, g_prepared_fanout.batch.first_frame,
                        g_prepared_fanout.batch.records.data(),
                        g_prepared_fanout.batch.count,
                        g_prepared_fanout.sender_committed))
                {
                    g_prepared_fanout.sent_mask |= bit;
                    PublishEvent(IpcEventType::GroupFanoutProgress,
                                 "frame batch fan-out recipient accepted",
                                 g_prepared_fanout.sent_mask,
                                 g_prepared_fanout.recipient_mask, 0,
                                 SessionToken(g_session->Session()),
                                 PeerTokenForSlot(slot), 0,
                                 static_cast<std::uint16_t>(MessageKind::FrameBatch),
                                 g_prepared_fanout.batch.count,
                                 g_prepared_fanout.batch.first_frame,
                                 g_local_slot, 0, 0, 0, slot);
                }
                else
                {
                    send_failed = true;
                    InterlockedIncrement64(&g_shared->fanout_retry_count);
                }
            }
            const std::uint32_t pending = g_prepared_fanout.recipient_mask &
                                          ~g_prepared_fanout.sent_mask;
            InterlockedExchange(&g_shared->fanout_pending_mask, static_cast<LONG>(pending));
            if (pending != 0)
            {
                if (now_ms - g_prepared_fanout.first_attempt_ms >= kReadyStallTimeoutMs)
                {
                    FailRuntime(ErrorCode::GroupFanoutTimeout,
                                "frame batch fan-out timed out after five seconds",
                                g_session->State() == RuntimeState::Active);
                    return false;
                }
                return !send_failed;
            }
            const SealedFrameBatch batch = g_prepared_fanout.batch;
            if (g_local_sealer.CommitBatch(batch) != FrameStoreResult::Inserted)
            {
                FailRuntime(ErrorCode::FrameStateViolation, "sealed batch could not be marked sent",
                            g_session->State() == RuntimeState::Active);
                return false;
            }
            if (g_shared)
            {
                InterlockedIncrement64(&g_shared->batches_sent);
                InterlockedAdd64(&g_shared->records_sent, static_cast<LONG64>(batch.count));
                InterlockedExchange64(&g_shared->pending_local_records,
                                      static_cast<LONG64>(g_local_sealer.PendingCount()));
                InterlockedExchange(&g_shared->sealed_through,
                                    static_cast<LONG>(g_local_sealer.SealedThrough()));
                UpdateReceivedThroughSummary();
            }
            for (std::uint16_t index = 0; index < batch.count; ++index)
            {
                if (IsNoOp(batch.records[index]))
                    continue;
                InterlockedIncrement64(&g_shared->non_empty_sent);
                PublishOperationEvent(IpcEventType::SyncRecordSealed,
                                      "non-empty record sealed and sent",
                                      batch.first_frame + index, g_local_slot,
                                       batch.records[index]);
            }
            g_prepared_fanout.Reset();
            InterlockedExchange(&g_shared->fanout_pending_mask, 0);
        }
        const std::uint32_t activation = g_session->ActivationFrame();
        if (g_session->State() == RuntimeState::Warmup && activation != 0)
        {
            bool all_slots_ready = g_local_sealer.Contains(activation);
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                if (slot != g_local_slot)
                    all_slots_ready = all_slots_ready &&
                                      g_remote_stores[slot].Contains(activation);
            }
            if (!g_session->SetActivationRecordsReady(all_slots_ready))
            {
                FailRuntime(g_session->Error(), g_session->ErrorText(), false);
                return false;
            }
        }
        PublishSessionFlags();
        return true;
    }

    void PublishRecoveredStall(std::uint32_t frame)
    {
        if (!g_shared)
            return;
        const std::uint64_t duration = g_stall_tracker.LastDuration();
        InterlockedAdd64(&g_shared->stall_total_ms, static_cast<LONG64>(duration));
        LONG64 previous_max = InterlockedCompareExchange64(&g_shared->stall_max_ms, 0, 0);
        while (static_cast<LONG64>(duration) > previous_max)
        {
            const LONG64 observed = InterlockedCompareExchange64(
                &g_shared->stall_max_ms, static_cast<LONG64>(duration), previous_max);
            if (observed == previous_max)
                break;
            previous_max = observed;
        }
        PublishEvent(IpcEventType::SyncReadyStallEnd, "custom ready stall 已恢复",
                     frame, duration);
        InterlockedExchange(&g_shared->stall_current_ms, 0);
    }

    bool CustomReady(std::uint32_t frame, std::uint64_t now_ms)
    {
        bool ready = !g_prepared_fanout.active && g_local_sealer.Contains(frame);
        for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
        {
            if (slot != g_local_slot)
                ready = ready && g_remote_stores[slot].Contains(frame);
        }
        const ReadyStallResult observation = g_stall_tracker.Observe(ready, now_ms);
        if (observation == ReadyStallResult::Ready)
            return true;
        if (observation == ReadyStallResult::Recovered)
        {
            PublishRecoveredStall(frame);
            return true;
        }
        if (observation == ReadyStallResult::Started)
        {
            InterlockedExchange(&g_shared->stall_frame, static_cast<LONG>(frame));
            InterlockedExchange(&g_shared->stall_retries, 1);
            PublishEvent(IpcEventType::SyncReadyStallBegin, "等待当前 frame 的 sidecar record", frame);
        }
        else
        {
            InterlockedExchange(&g_shared->stall_retries,
                                static_cast<LONG>(g_stall_tracker.Retries()));
        }
        const std::uint64_t elapsed = g_stall_tracker.CurrentDuration(now_ms);
        InterlockedExchange(&g_shared->stall_current_ms, static_cast<LONG>(elapsed));
        if (observation == ReadyStallResult::TimedOut)
        {
            FailRuntime(ErrorCode::FrameReadyTimeout,
                        "ACTIVE current frame record missing for five seconds", true);
        }
        return false;
    }

    enum class ManifestResetMode
    {
        Preserve,
        Clear
    };

    void RequestSessionReset(bool notify_remote = true,
                             ManifestResetMode manifest_mode = ManifestResetMode::Preserve)
    {
        if (g_session)
        {
            g_retired_session_manager = g_session_manager;
            g_retired_baseline_frame = g_last_observed_frame;
            g_require_new_baseline = g_retired_session_manager != nullptr;
        }
        if (g_session && g_session->SessionBound())
        {
            std::array<std::uint64_t, kMaximumPeerCount> next_receive{};
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                if (slot != g_local_slot)
                    next_receive[slot] = g_session->NextReceiveSequence(slot);
            }
            g_retired_session_filter.Retire(g_session->Session(), g_roster.steam_ids,
                                            g_roster.peer_count, g_roster.local_slot,
                                            next_receive);
            g_retired_roster = g_roster;
            PublishEvent(IpcEventType::GroupSessionEnded,
                         ISAAC_SYNC_PRODUCT_VERSION " group session retired", g_roster.peer_count,
                         g_roster.frozen_slot_mask, 0,
                         SessionToken(g_session->Session()));
        }
        if (g_session && notify_remote)
            g_session->EndGracefully();
        g_session.reset();
        g_runtime_transport.reset();
        g_local_sealer = {};
        g_remote_stores = {};
        g_prepared_fanout.Reset();
        g_frame_stores_initialized = false;
        g_warmup_prefix_retired = false;
        g_roster_end_tracker.Reset();
        g_roster_stability.Reset();
        g_session_manager = nullptr;
        g_roster = {};
        g_local_slot = UINT32_MAX;
        g_roster_event_published = false;
        g_last_published_ready_mask = 0;
        g_commit_event_published = false;
        g_stall_tracker.Reset();
        g_test_delay_until_ms = 0;
        g_empty_manifest_auto_start.Reset();
        if (manifest_mode == ManifestResetMode::Clear)
        {
            g_manifest_start.CancelStartRequest();
            InterlockedExchange(&g_manifest_reset_pending, 1);
        }
        else
        {
            // Register/Start belongs to the lifetime of this Lua state, not to one
            // Steam lobby. Keep the frozen manifest armed for the next group
            // session; shutdown and Lua state replacement still clear the refs.
            InterlockedExchange(&g_manifest_reset_pending, 0);
        }
        if (g_shared)
        {
            const bool manifest_invalid = g_manifest_invalid.IsInvalid();
            InterlockedExchange(&g_shared->error_code,
                                static_cast<LONG>(manifest_invalid
                                    ? ErrorCode::ManifestDerivedPortCollision
                                    : ErrorCode::None));
            CopyText(g_shared->error_detail, sizeof(g_shared->error_detail),
                     manifest_invalid ? "manifest has derived port collision" : "");
            InterlockedAnd(&g_shared->flags,
                           static_cast<LONG>(~(IpcFlagSessionBound | IpcFlagCoordinator |
                                               IpcFlagActivationCommitted)));
            InterlockedExchange(&g_shared->activation_frame, static_cast<LONG>(UINT32_MAX));
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64 *>(&g_shared->remote_peer_token), 0);
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64 *>(&g_shared->local_peer_token), 0);
            g_shared->peer_count = 0;
            g_shared->local_slot = UINT32_MAX;
            g_shared->coordinator_slot = UINT32_MAX;
            g_shared->frozen_slot_mask = 0;
            InterlockedExchange(&g_shared->activation_ready_mask, 0);
            InterlockedExchange(&g_shared->fanout_pending_mask, 0);
            std::memset(g_shared->peer_tokens, 0, sizeof(g_shared->peer_tokens));
            for (std::size_t slot = 0; slot < kMaximumPeerCount; ++slot)
            {
                InterlockedExchange(&g_shared->received_through_by_slot[slot],
                                    static_cast<LONG>(UINT32_MAX));
                InterlockedExchange64(&g_shared->next_receive_sequence_by_slot[slot], 0);
                InterlockedExchange64(&g_shared->packets_sent_by_slot[slot], 0);
                InterlockedExchange64(&g_shared->packets_received_by_slot[slot], 0);
                InterlockedExchange64(&g_shared->records_received_by_slot[slot], 0);
                InterlockedExchange64(&g_shared->handler_invocations_by_slot[slot], 0);
            }
            InterlockedExchange64(&g_shared->fanout_retry_count, 0);
            InterlockedExchange64(&g_shared->pending_local_records, 0);
            InterlockedExchange64(&g_shared->handler_invocations, 0);
            InterlockedExchange64(&g_shared->dispatch_failures, 0);
            InterlockedExchange(&g_shared->dispatch_guard_frame, static_cast<LONG>(UINT32_MAX));
            InterlockedExchange(&g_shared->dispatch_guard_state,
                                static_cast<LONG>(DispatchGuardState::Idle));
            InterlockedExchange(&g_shared->last_dispatch_frame, static_cast<LONG>(UINT32_MAX));
            InterlockedExchange(&g_shared->last_dispatch_slot, static_cast<LONG>(UINT32_MAX));
            g_shared->last_dispatch_port = 0;
            g_shared->last_dispatch_opcode = 0;
            g_shared->last_dispatch_payload = 0;
            g_shared->last_dispatch_payload_kind = 0;
            g_shared->reserved_typed_payload = 0;
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64 *>(&g_shared->last_dispatch_payload_bits), 0);
        }
        SetRuntimeState(manifest_mode == ManifestResetMode::Preserve
                            ? RuntimeState::Dormant
                            : RuntimeState::Stopping,
                        manifest_mode == ManifestResetMode::Preserve
                            ? "DORMANT：会话已结束；保留 manifest 等待新的 group 会话"
                            : "联机会话结束；等待 Lua manifest 清理");
    }

    void HandleUnexpectedLuaRoot(void *observed_root)
    {
        if (!g_lua_lifecycle.LatchUnexpectedRoot(observed_root))
            return;
        const RuntimeState session_state = g_session ? g_session->State() : RuntimeState::Dormant;
        const bool fatal = session_state == RuntimeState::Active ||
                           session_state == RuntimeState::FailedFatal;
        if (g_session)
        {
            if (fatal)
                g_session->AbortForLifecycleReset();
            else if (session_state == RuntimeState::Discovering ||
                     session_state == RuntimeState::Hello ||
                     session_state == RuntimeState::Warmup)
                g_session->EndGracefully();
        }
        RequestSessionReset(false, ManifestResetMode::Preserve);
        if (g_shared)
        {
            InterlockedExchange(&g_shared->error_code,
                                static_cast<LONG>(ErrorCode::LuaStateChanged));
            CopyText(g_shared->error_detail, sizeof(g_shared->error_detail),
                     "a different canonical Lua root was observed without lua_close");
        }
        SetRuntimeState(fatal ? RuntimeState::FailedFatal : RuntimeState::FailedPreActive,
                        fatal ? "FAILED_FATAL：Lua root 未经 close 被替换"
                              : "FAILED_PRE_ACTIVE：Lua root 未经 close 被替换");
        PublishEvent(IpcEventType::SyncFailure,
                     "a different canonical Lua root was observed without lua_close",
                     static_cast<std::uint32_t>(ErrorCode::LuaStateChanged), fatal ? 1u : 0u);
    }

    int LuaError(void *state, const char *message)
    {
        g_lua_pushnil(state);
        g_lua_pushstring(state, message);
        return 2;
    }

    bool IsMainLuaCall(void *return_address)
    {
        return g_shared && g_shared->main_thread_id != 0 &&
               GetCurrentThreadId() == g_shared->main_thread_id &&
               static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(return_address) -
                                          reinterpret_cast<std::uintptr_t>(g_main_base)) ==
                   kLuaSimulationPcallReturnRva;
    }

    void ResetManifestNativeState()
    {
        g_manifest = {};
        g_manifest_count = 0;
        g_manifest_hash.fill(0);
        g_pending_manifest_hash.fill(0);
        g_manifest_start.Reset();
        g_empty_manifest_auto_start.Reset();
        const bool clear_collision_error =
            g_manifest_invalid.Clear() && g_shared &&
            InterlockedCompareExchange(&g_shared->error_code, 0, 0) ==
                static_cast<LONG>(ErrorCode::ManifestDerivedPortCollision);
        if (g_shared)
        {
            if (clear_collision_error)
            {
                InterlockedExchange(&g_shared->error_code,
                                    static_cast<LONG>(ErrorCode::None));
                CopyText(g_shared->error_detail, sizeof(g_shared->error_detail), "");
            }
            g_shared->manifest_count = 0;
            std::memset(g_shared->manifest_hash, 0, sizeof(g_shared->manifest_hash));
            LONG flags = InterlockedCompareExchange(&g_shared->flags, 0, 0);
            flags &= ~(IpcFlagStartRequested | IpcFlagManifestFrozen | IpcFlagSessionBound |
                       IpcFlagCoordinator | IpcFlagActivationCommitted);
            InterlockedExchange(&g_shared->flags, flags);
        }
    }

    void ClearManifestRefs(void *state, std::uint16_t &released)
    {
        if (state)
        {
            for (std::uint32_t index = 0; index < g_manifest_count; ++index)
            {
                if (g_manifest[index].handler_ref >= 0)
                {
                    g_lua_unref(state, kLuaRegistryIndex, g_manifest[index].handler_ref);
                    ++released;
                }
            }
        }
        ResetManifestNativeState();
    }

    void ClearManifestRefs(void *state)
    {
        std::uint16_t released = 0;
        ClearManifestRefs(state, released);
    }

    bool PushRawIsaacTable(void *state)
    {
        if (!g_lua_checkstack(state, 10))
            return false;
        g_lua_rawgeti(state, kLuaRegistryIndex, kLuaRegistryGlobals);
        if (g_lua_type(state, -1) != kLuaTypeTable)
            return false;
        g_lua_pushstring(state, "Isaac");
        g_lua_rawget(state, -2);
        return g_lua_type(state, -1) == kLuaTypeTable;
    }

    void RawSetFunction(void *state, const char *name, LuaCFunction function)
    {
        // The fresh Sync table is at -1 before the key/value pair is pushed.
        g_lua_pushstring(state, name);
        g_lua_pushcclosure(state, function, 0);
        g_lua_rawset(state, -3);
    }

    bool RemoveLuaApi(void *state)
    {
        if (!state)
            return true;
        int top = 0;
        bool have_top = false;
        bool restored = false;
        bool success = false;
        __try
        {
            top = g_lua_gettop(state);
            have_top = true;
            if (PushRawIsaacTable(state))
            {
                g_lua_pushstring(state, "Sync");
                g_lua_pushnil(state);
                g_lua_rawset(state, -3);
                success = true;
            }
            g_lua_settop(state, top);
            restored = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            success = false;
        }
        if (have_top && !restored)
        {
            __try
            {
                g_lua_settop(state, top);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return success;
    }

    std::uint64_t LuaRootValue(void *root)
    {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(root)));
    }

    bool ValidateBoundLuaCall(void *state, LuaLifecycleSnapshot *bound = nullptr)
    {
        if (!g_shared || !state || GetCurrentThreadId() != g_shared->main_thread_id)
            return false;
        void *root = nullptr;
        if (ResolveCanonicalLuaRoot(g_lua_adapter, state, root) !=
            CanonicalLuaRootResult::Success)
            return false;
        const LuaLifecycleSnapshot snapshot = g_lua_lifecycle.Snapshot();
        if (snapshot.phase != LuaLifecyclePhase::Bound || !snapshot.gate_open ||
            snapshot.root != root)
            return false;
        if (bound)
            *bound = snapshot;
        return true;
    }

    bool RegisterLuaApi(void *state, std::uint64_t generation)
    {
        if (!state || !generation)
            return false;
        int top = 0;
        bool success = false;
        bool attached = false;
        PublishEvent(IpcEventType::LuaBindBegin,
                     "在已验证 canonical Lua root 上开始 raw Isaac.Sync 绑定",
                     generation, LuaRootValue(state), 0, 0, 0, 0, 3);
        __try
        {
            top = g_lua_gettop(state);
            if (PushRawIsaacTable(state))
            {
                g_lua_createtable(state, 0, 7);
                RawSetFunction(state, "Register", &LuaRegister);
                RawSetFunction(state, "Start", &LuaStart);
                RawSetFunction(state, "IsActive", &LuaIsActive);
                RawSetFunction(state, "GetLocalSlot", &LuaGetLocalSlot);
                RawSetFunction(state, "GetPeerCount", &LuaGetPeerCount);
                RawSetFunction(state, "GetPlayerIndexForSlot", &LuaGetPlayerIndexForSlot);
                RawSetFunction(state, "Send", &LuaSend);

                // Stack: globals, Isaac, Sync. Attach with rawset so a read-only
                // Isaac API metatable cannot raise an unprotected Lua error.
                g_lua_pushstring(state, "Sync");
                g_lua_pushvalue(state, -2);
                g_lua_rawset(state, -4);
                attached = true;
                success = true;
            }
            g_lua_settop(state, top);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            success = false;
        }
        if (!success)
        {
            if (attached)
                RemoveLuaApi(state);
            return false;
        }
        if (!g_lua_lifecycle.CommitBinding(state, generation))
        {
            RemoveLuaApi(state);
            return false;
        }
        if (g_shared)
            InterlockedOr(&g_shared->flags, IpcFlagLuaBound);
        PublishEvent(IpcEventType::LuaBound, "Isaac.Sync 七项 API 已绑定",
                     generation, LuaRootValue(state), 0, 0, 0, 0, 3);
        return true;
    }

    std::vector<ManifestEntry> SnapshotManifestEntries()
    {
        std::vector<ManifestEntry> entries;
        entries.reserve(g_manifest_count);
        for (std::uint32_t index = 0; index < g_manifest_count; ++index)
        {
            entries.push_back({std::string(g_manifest[index].sync_id,
                                           g_manifest[index].sync_id_length),
                               g_manifest[index].version, g_manifest[index].port});
        }
        return entries;
    }

    bool RequestCurrentManifestStart(std::string &error)
    {
        error.clear();
        if (g_manifest_invalid.IsInvalid())
        {
            error = "manifest has derived port collision";
            return false;
        }
        if (g_manifest_start.Frozen() || g_manifest_start.StartRequested())
            return true;
        const std::vector<ManifestEntry> entries = SnapshotManifestEntries();
        if (!ComputeManifestHash(entries, g_pending_manifest_hash, error))
            return false;
        if (g_manifest_start.RequestStart(g_manifest_count) !=
            ManifestStartRequestResult::Requested)
        {
            error = "manifest start state changed unexpectedly";
            return false;
        }
        if (g_shared)
            InterlockedOr(&g_shared->flags, IpcFlagStartRequested);
        return true;
    }

    bool CommitPreparedManifest(const char *message)
    {
        if (g_manifest_invalid.IsInvalid() || !g_manifest_start.TryFreeze(true))
            return false;
        g_manifest_hash = g_pending_manifest_hash;
        if (g_shared)
        {
            std::memcpy(g_shared->manifest_hash, g_manifest_hash.data(), g_manifest_hash.size());
            InterlockedOr(&g_shared->flags,
                          IpcFlagManifestFrozen | IpcFlagStartRequested);
        }
        PublishEvent(IpcEventType::LuaManifestFrozen, message);
        return true;
    }

    int __cdecl LuaRegister(void *state)
    {
        if (!ValidateBoundLuaCall(state))
        {
            return LuaError(state, "Isaac.Sync.Register must run on the bound main Lua state");
        }
        if (!g_manifest_start.CanRegister())
        {
            return LuaError(state, "manifest frozen");
        }
        if (g_manifest_count >= g_manifest.size())
            return LuaError(state, "manifest capacity reached");
        if (g_lua_gettop(state) != 3 || g_lua_type(state, 1) != kLuaTypeString ||
            !g_lua_isinteger(state, 2) || g_lua_type(state, 3) != kLuaTypeFunction)
        {
            return LuaError(state, "expected syncId, version, handler");
        }
        std::size_t id_length = 0;
        const char *sync_id = g_lua_tolstring(state, 1, &id_length);
        int version_ok = 0;
        const std::int64_t version = g_lua_tointegerx(state, 2, &version_ok);
        if (!sync_id || !version_ok || version < 0 || version > UINT32_MAX)
        {
            return LuaError(state, "manifest argument out of range");
        }
        const std::string candidate(sync_id, id_length);
        std::uint16_t port = 0;
        std::string derive_error;
        if (!DerivePort(candidate, port, derive_error))
            return LuaError(state, derive_error.c_str());
        std::vector<ManifestEntry> existing;
        existing.reserve(g_manifest_count);
        for (std::uint32_t index = 0; index < g_manifest_count; ++index)
        {
            existing.push_back({std::string(g_manifest[index].sync_id,
                                            g_manifest[index].sync_id_length),
                                g_manifest[index].version, g_manifest[index].port});
        }
        switch (ClassifyManifestRegistration(existing, candidate, port))
        {
        case ManifestRegistrationCheck::DuplicateSyncId:
            return LuaError(state, "syncId already registered");
        case ManifestRegistrationCheck::DerivedPortCollision:
            MarkManifestDerivedPortCollision();
            return LuaError(state, "derived port collision");
        case ManifestRegistrationCheck::Available:
            break;
        }
        Hash256 candidate_hash{};
        if (g_manifest_start.StartRequested())
        {
            std::vector<ManifestEntry> candidate_entries = existing;
            candidate_entries.push_back(
                {candidate, static_cast<std::uint32_t>(version), port});
            std::string hash_error;
            if (!ComputeManifestHash(candidate_entries, candidate_hash, hash_error))
                return LuaError(state, hash_error.c_str());
        }
        auto &entry = g_manifest[g_manifest_count];
        std::memcpy(entry.sync_id, sync_id, id_length);
        entry.sync_id[id_length] = 0;
        entry.sync_id_length = static_cast<std::uint16_t>(id_length);
        entry.version = static_cast<std::uint32_t>(version);
        entry.port = port;
        g_lua_pushvalue(state, 3);
        entry.handler_ref = g_lua_ref(state, kLuaRegistryIndex);
        ++g_manifest_count;
        g_empty_manifest_auto_start.Reset();
        if (g_manifest_start.StartRequested())
            g_pending_manifest_hash = candidate_hash;
        if (g_shared)
            g_shared->manifest_count = g_manifest_count;
        g_lua_pushinteger(state, port);
        return 1;
    }

    int __cdecl LuaStart(void *state)
    {
        if (!ValidateBoundLuaCall(state))
        {
            return LuaError(state, "Isaac.Sync.Start must run on the bound main Lua state");
        }
        if (g_lua_gettop(state) != 0)
            return LuaError(state, "Start takes no arguments");
        if (g_manifest_invalid.IsInvalid())
            return LuaError(state, "manifest has derived port collision");
        if (g_manifest_start.Frozen() || g_manifest_start.StartRequested())
        {
            g_empty_manifest_auto_start.Reset();
            g_lua_pushboolean(state, 1);
            return 1;
        }
        std::string error;
        g_empty_manifest_auto_start.Reset();
        if (!RequestCurrentManifestStart(error))
            return LuaError(state, error.c_str());
        g_lua_pushboolean(state, 1);
        return 1;
    }

    void FreezeManifestAfterLuaCallback(void *callback_root, std::uint64_t generation,
                                        int callback_status)
    {
        if (callback_status != 0 ||
            !g_lua_lifecycle.IsBoundCall(callback_root, generation) ||
            g_manifest_invalid.IsInvalid())
        {
            return;
        }
        CommitPreparedManifest(
            "manifest 已在完整 Lua callback 后冻结；等待兼容 Steam group");
    }

    void UpdateEmptyManifestAutoStart(bool compatible, std::uint64_t now_ms)
    {
        const bool eligible = compatible && !g_session && g_manifest_count == 0 &&
                              !g_manifest_invalid.IsInvalid() &&
                              !g_manifest_start.StartRequested() &&
                              !g_manifest_start.Frozen();
        if (g_empty_manifest_auto_start.Observe(eligible, now_ms) !=
            EmptyManifestAutoStartResult::Ready)
        {
            return;
        }
        std::string error;
        if (!RequestCurrentManifestStart(error))
        {
            g_empty_manifest_auto_start.Reset();
            FailRuntime(ErrorCode::GroupHandshakeMismatch,
                        error.empty() ? "failed to prepare automatic empty manifest"
                                      : error.c_str(),
                        false);
            return;
        }
        if (!CommitPreparedManifest(
                "空 manifest 已在 2 秒注册窗口后自动冻结；等待正式 session"))
        {
            g_empty_manifest_auto_start.Reset();
            FailRuntime(ErrorCode::GroupHandshakeMismatch,
                        "failed to freeze automatic empty manifest", false);
        }
    }

    int __cdecl LuaIsActive(void *state)
    {
        if (!ValidateBoundLuaCall(state))
            return LuaError(state, "Isaac.Sync.IsActive must run on the bound Lua VM");
        if (g_lua_gettop(state) != 0)
            return LuaError(state, "IsActive takes no arguments");
        const bool active = g_shared &&
                            static_cast<RuntimeState>(InterlockedCompareExchange(&g_shared->runtime_state, 0, 0)) ==
                                RuntimeState::Active;
        g_lua_pushboolean(state, active ? 1 : 0);
        return 1;
    }

    int __cdecl LuaGetLocalSlot(void *state)
    {
        if (!ValidateBoundLuaCall(state))
            return LuaError(state, "Isaac.Sync.GetLocalSlot must run on the bound Lua VM");
        if (g_lua_gettop(state) != 0)
            return LuaError(state, "GetLocalSlot takes no arguments");
        if (g_roster.peer_count < 2 || g_local_slot >= g_roster.peer_count)
            g_lua_pushnil(state);
        else
            g_lua_pushinteger(state, g_local_slot);
        return 1;
    }

    int __cdecl LuaGetPeerCount(void *state)
    {
        if (!ValidateBoundLuaCall(state))
            return LuaError(state, "Isaac.Sync.GetPeerCount must run on the bound Lua VM");
        if (g_lua_gettop(state) != 0)
            return LuaError(state, "GetPeerCount takes no arguments");
        if (g_roster.peer_count < 2 || g_roster.peer_count > kMaximumPeerCount)
            g_lua_pushnil(state);
        else
            g_lua_pushinteger(state, g_roster.peer_count);
        return 1;
    }

    int __cdecl LuaGetPlayerIndexForSlot(void *state)
    {
        if (!ValidateBoundLuaCall(state))
        {
            return LuaError(state,
                            "GetPlayerIndexForSlot must run on the bound main Lua state");
        }
        if (g_lua_gettop(state) != 1 || !g_lua_isinteger(state, 1))
            return LuaError(state, "GetPlayerIndexForSlot expects one integer senderSlot");
        int valid = 0;
        const std::int64_t sender_slot = g_lua_tointegerx(state, 1, &valid);
        if (!valid)
            return LuaError(state, "GetPlayerIndexForSlot expects one integer senderSlot");
        std::uint16_t player_index = UINT16_MAX;
        switch (QueryPlayerIndexForSlot(g_roster, sender_slot, player_index))
        {
        case PlayerIndexQueryResult::Unavailable:
            g_lua_pushnil(state);
            return 1;
        case PlayerIndexQueryResult::OutOfRange:
            return LuaError(state, "senderSlot is outside the frozen roster");
        case PlayerIndexQueryResult::Ready:
            g_lua_pushinteger(state, player_index);
            return 1;
        }
        return LuaError(state, "player mapping is unavailable");
    }

    int RejectLuaSend(void *state, ErrorCode error, const char *message,
                      std::uint32_t target = UINT32_MAX,
                      const CustomFrameOpV2 &operation = {})
    {
        if (g_shared)
        {
            InterlockedIncrement64(&g_shared->send_rejected);
            InterlockedExchange(&g_shared->last_send_target, static_cast<LONG>(target));
            InterlockedExchange(&g_shared->last_send_reject_error, static_cast<LONG>(error));
            PublishOperationEvent(IpcEventType::LuaSendRejected, message, target,
                                  g_local_slot < g_roster.peer_count ? g_local_slot : UINT32_MAX,
                                  operation, error);
        }
        return LuaError(state, message);
    }

    int __cdecl LuaSend(void *state)
    {
        if (!ValidateBoundLuaCall(state))
        {
            return RejectLuaSend(state, ErrorCode::LuaSendInactive,
                                 "Isaac.Sync.Send must run on the bound main Lua state");
        }
        const auto runtime = static_cast<RuntimeState>(
            InterlockedCompareExchange(&g_shared->runtime_state, 0, 0));
        if (!g_manifest_start.Frozen() ||
            runtime != RuntimeState::Active || !g_session)
        {
            return RejectLuaSend(state, ErrorCode::LuaSendInactive, "sync inactive");
        }
        if (g_lua_gettop(state) != 3 || !g_lua_isinteger(state, 1) ||
            !g_lua_isinteger(state, 2) || g_lua_type(state, 3) != kLuaTypeNumber)
        {
            return RejectLuaSend(state, ErrorCode::LuaSendArgument,
                                 "expected integer port, integer opcode, numeric payload");
        }
        int port_ok = 0;
        int opcode_ok = 0;
        const std::int64_t port = g_lua_tointegerx(state, 1, &port_ok);
        const std::int64_t opcode = g_lua_tointegerx(state, 2, &opcode_ok);
        if (!port_ok || !opcode_ok || port < 1 || port > UINT16_MAX ||
            opcode < 0 || opcode > UINT16_MAX)
        {
            return RejectLuaSend(state, ErrorCode::LuaSendArgument,
                                 "send argument out of range");
        }
        CustomFrameOpV2 operation{};
        const LuaNumericResult numeric = ReadLuaNumericPayload(
            g_lua_adapter, state, 3, static_cast<std::uint16_t>(port),
            static_cast<std::uint16_t>(opcode), operation);
        if (numeric != LuaNumericResult::Success)
        {
            return RejectLuaSend(
                state, ErrorCode::LuaSendArgument,
                numeric == LuaNumericResult::IntegerRange
                    ? "integer payload out of int32 range"
                    : numeric == LuaNumericResult::FloatNotFinite
                        ? "float payload must be finite"
                        : "payload must be a Lua number");
        }
        if (!FindManifest(operation.port))
        {
            return RejectLuaSend(state, ErrorCode::LuaSendPort,
                                 "port not registered", UINT32_MAX, operation);
        }
        const std::uint32_t online_frame = static_cast<std::uint32_t>(
            InterlockedCompareExchange(&g_shared->online_frame, 0, 0));
        if (online_frame == UINT32_MAX || !g_local_sealer.Initialized())
        {
            return RejectLuaSend(state, ErrorCode::LuaSendFrameOverflow,
                                 "target frame unavailable", UINT32_MAX, operation);
        }
        std::uint32_t earliest_target = UINT32_MAX;
        if (ComputeSendTarget(online_frame, kLeadFrames, g_local_sealer.NextUnsealedFrame(),
                              g_local_sealer.Exhausted(), earliest_target) !=
            FrameStoreResult::Inserted)
        {
            return RejectLuaSend(state, ErrorCode::LuaSendFrameOverflow,
                                 "target frame overflow", UINT32_MAX, operation);
        }
        std::uint32_t target = UINT32_MAX;
        const FrameStoreResult queued = g_local_sealer.QueueNext(
            earliest_target, operation, online_frame, target);
        if (queued != FrameStoreResult::Inserted)
        {
            const bool queue_full = queued == FrameStoreResult::Window ||
                                    queued == FrameStoreResult::Capacity;
            return RejectLuaSend(
                state, ErrorCode::LuaSendFrameOverflow,
                queue_full ? "send queue full"
                           : queued == FrameStoreResult::Overflow
                               ? "target frame overflow"
                               : "target frame unavailable",
                target, operation);
        }
        InterlockedIncrement64(&g_shared->send_accepted);
        InterlockedExchange64(&g_shared->pending_local_records,
                              static_cast<LONG64>(g_local_sealer.PendingCount()));
        InterlockedExchange(&g_shared->last_send_target, static_cast<LONG>(target));
        InterlockedExchange(&g_shared->last_send_reject_error,
                            static_cast<LONG>(ErrorCode::None));
        PublishOperationEvent(IpcEventType::LuaSendAccepted, "Lua Send accepted",
                              target, g_local_slot, operation);
        g_lua_pushinteger(state, target);
        return 1;
    }

    bool RetireSessionForLuaClose()
    {
        const RuntimeState session_state = g_session ? g_session->State() : RuntimeState::Dormant;
        const bool fatal = session_state == RuntimeState::Active ||
                           session_state == RuntimeState::FailedFatal;
        if (g_session)
        {
            if (fatal)
                g_session->AbortForLifecycleReset();
            else if (session_state == RuntimeState::Discovering ||
                     session_state == RuntimeState::Hello ||
                     session_state == RuntimeState::Warmup)
                g_session->EndGracefully();
        }
        RequestSessionReset(false, ManifestResetMode::Clear);
        InterlockedExchange(&g_manifest_reset_pending, 0);
        return fatal;
    }

    void RecordLuaCleanupUnsafe(bool fatal, const char *detail)
    {
        if (g_shared)
        {
            InterlockedExchange(&g_shared->error_code,
                                static_cast<LONG>(ErrorCode::LuaCleanupUnsafe));
            CopyText(g_shared->error_detail, sizeof(g_shared->error_detail), detail);
        }
        SetRuntimeState(fatal ? RuntimeState::FailedFatal : RuntimeState::FailedPreActive,
                        fatal ? "FAILED_FATAL：Lua close 清理不安全"
                              : "FAILED_PRE_ACTIVE：Lua close 清理不安全");
        PublishEvent(IpcEventType::SyncFailure, detail,
                     static_cast<std::uint32_t>(ErrorCode::LuaCleanupUnsafe), fatal ? 1u : 0u);
    }

    bool TryShutdownLuaCleanup(void *root, const LuaLifecycleSnapshot &snapshot)
    {
        if (!g_shared || snapshot.phase != LuaLifecyclePhase::Bound || snapshot.root != root ||
            GetCurrentThreadId() != g_shared->main_thread_id ||
            InterlockedCompareExchange(&g_lua_handler_dispatch_depth, 0, 0) != 0)
            return false;
        bool success = false;
        __try
        {
            ClearManifestRefs(root);
            success = RemoveLuaApi(root);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            success = false;
            ResetManifestNativeState();
        }
        if (!success ||
            !g_lua_lifecycle.CompleteShutdownCleanup(root, snapshot.generation))
            return false;
        InterlockedExchange(&g_manifest_reset_pending, 0);
        InterlockedExchange(&g_lua_cleanup_pending, 0);
        InterlockedExchange(&g_lua_cleanup_complete, 1);
        if (g_shared)
        {
            InterlockedAnd(&g_shared->flags,
                           static_cast<LONG>(~(IpcFlagLuaBound | IpcFlagLuaCleanupPending |
                                               IpcFlagStartRequested | IpcFlagManifestFrozen)));
        }
        PublishEvent(IpcEventType::LuaCleanupComplete,
                     "Lua refs 与 Isaac.Sync 已为 DLL 主动卸载清理",
                     snapshot.generation, LuaRootValue(root), 0, 0, 0, 0, 3);
        return true;
    }

    void *__cdecl LuaNewstateHook(LuaAllocFn allocator, void *user_data)
    {
        EnterHook();
        const bool outermost = g_lua_create_depth++ == 0;
        void *state = nullptr;
        __try
        {
            state = g_lua_newstate(allocator, user_data);
            if (outermost && state &&
                InterlockedCompareExchange(&g_hooks_ready, 0, 0) != 0 &&
                InterlockedCompareExchange(&g_unloading, 0, 0) == 0)
            {
                PublishEvent(IpcEventType::LuaStateCreated, "lua_newstate 创建 Lua root",
                             0, LuaRootValue(state), 0, 0, 0, 0, 1);
            }
        }
        __finally
        {
            --g_lua_create_depth;
            LeaveHook();
        }
        return state;
    }

    void *__cdecl LuaLNewstateHook()
    {
        EnterHook();
        const bool outermost = g_lua_create_depth++ == 0;
        void *state = nullptr;
        __try
        {
            state = g_lua_lnewstate();
            if (outermost && state &&
                InterlockedCompareExchange(&g_hooks_ready, 0, 0) != 0 &&
                InterlockedCompareExchange(&g_unloading, 0, 0) == 0)
            {
                PublishEvent(IpcEventType::LuaStateCreated, "luaL_newstate 创建 Lua root",
                             0, LuaRootValue(state), 0, 0, 0, 0, 2);
            }
        }
        __finally
        {
            --g_lua_create_depth;
            LeaveHook();
        }
        return state;
    }

    void __cdecl LuaCloseHook(void *state)
    {
        EnterHook();
        LuaLifecycleSnapshot closing{};
        LuaCloseDisposition disposition = LuaCloseDisposition::Auxiliary;
        std::uint16_t released_refs = 0;
        bool original_returned = false;
        bool cleanup_safe = true;
        bool session_was_fatal = false;
        const bool observe = InterlockedCompareExchange(&g_hooks_ready, 0, 0) != 0 &&
                             InterlockedCompareExchange(&g_unloading, 0, 0) == 0;
        __try
        {
            if (observe)
                disposition = g_lua_lifecycle.BeginClose(state, closing);
            if (!observe || disposition == LuaCloseDisposition::BoundReentrant)
            {
                g_lua_close(state);
                original_returned = true;
            }
            else if (disposition == LuaCloseDisposition::NullState)
            {
                PublishEvent(IpcEventType::Error, "lua_close 收到空 state；保持 active generation 不变");
                g_lua_close(state);
                original_returned = true;
            }
            else if (disposition == LuaCloseDisposition::Auxiliary)
            {
                PublishEvent(IpcEventType::LuaStateClosing, "辅助 Lua root 正在关闭",
                             0, LuaRootValue(state), 0, 0, 0, 0, 4);
                g_lua_close(state);
                original_returned = true;
                PublishEvent(IpcEventType::LuaStateRetired, "辅助 Lua root 已关闭",
                             0, LuaRootValue(state), 0, 0, 0, 0, 4);
            }
            else
            {
                PublishEvent(IpcEventType::LuaStateClosing, "bound Lua generation 正在关闭",
                             closing.generation, LuaRootValue(state), 0, 0, 0, 0, 3);
                cleanup_safe = g_shared &&
                               GetCurrentThreadId() == g_shared->main_thread_id &&
                               InterlockedCompareExchange(&g_lua_handler_dispatch_depth, 0, 0) == 0;
                session_was_fatal = RetireSessionForLuaClose();
                if (cleanup_safe)
                {
                    __try
                    {
                        ClearManifestRefs(state, released_refs);
                        cleanup_safe = RemoveLuaApi(state);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        cleanup_safe = false;
                    }
                }
                if (!cleanup_safe)
                {
                    ResetManifestNativeState();
                    RecordLuaCleanupUnsafe(session_was_fatal,
                                           "bound lua_close could not safely release Lua refs/API");
                }
                if (g_shared)
                {
                    InterlockedAnd(&g_shared->flags,
                                   static_cast<LONG>(~(IpcFlagLuaBound |
                                                       IpcFlagStartRequested |
                                                       IpcFlagManifestFrozen |
                                                       IpcFlagSessionBound |
                                                       IpcFlagCoordinator |
                                                       IpcFlagActivationCommitted |
                                                       IpcFlagLuaCleanupPending)));
                }
                g_lua_lifecycle.UnregisterClosingRoot(state, closing.generation);
                g_lua_close(state);
                original_returned = true;
                if (g_lua_lifecycle.CompleteClose(state, closing.generation))
                {
                    PublishEvent(IpcEventType::LuaStateRetired, "bound Lua generation 已退役",
                                 closing.generation, LuaRootValue(state), 0, 0, 0, 0, 3,
                                 released_refs);
                    if (InterlockedCompareExchange(&g_shutdown_started, 0, 0) != 0)
                    {
                        InterlockedExchange(&g_lua_cleanup_pending, 0);
                        InterlockedExchange(&g_lua_cleanup_complete, 1);
                    }
                    if (InterlockedCompareExchange(&g_shutdown_started, 0, 0) == 0 &&
                        cleanup_safe)
                    {
                        if (g_shared &&
                            InterlockedCompareExchange(&g_shared->error_code, 0, 0) ==
                                static_cast<LONG>(ErrorCode::LuaStateChanged))
                        {
                            InterlockedExchange(&g_shared->error_code,
                                                static_cast<LONG>(ErrorCode::None));
                            CopyText(g_shared->error_detail, sizeof(g_shared->error_detail), "");
                        }
                        SetRuntimeState(RuntimeState::Dormant,
                                        "DORMANT：Lua generation 已退役；等待新 root");
                    }
                }
            }
        }
        __finally
        {
            // If original lua_close raises, CompleteClose is deliberately skipped;
            // the lifecycle remains Closing and its API gate stays shut.
            (void)original_returned;
            LeaveHook();
        }
    }

    int __cdecl LuaPcallkHook(void *state, int nargs, int nresults, int errfunc,
                              std::intptr_t context, void *continuation)
    {
        void *const return_address = _ReturnAddress();
        EnterHook();
        int result = 0;
        void *callback_root = nullptr;
        std::uint64_t callback_generation = 0;
        const bool main_lua_call = IsMainLuaCall(return_address);
        __try
        {
            if (InterlockedCompareExchange(&g_unloading, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_hooks_ready, 0, 0) != 0 && main_lua_call)
            {
                if (ResolveCanonicalLuaRoot(g_lua_adapter, state, callback_root) !=
                    CanonicalLuaRootResult::Success)
                {
                    FailRuntime(ErrorCode::LuaRegistrationFailed,
                                "failed to resolve canonical Lua main thread", false);
                    callback_root = nullptr;
                }
                else
                {
                    const LuaLifecycleSnapshot snapshot = g_lua_lifecycle.Snapshot();
                    if (InterlockedCompareExchange(&g_lua_cleanup_pending, 0, 0) != 0)
                    {
                        if (snapshot.root == callback_root)
                            TryShutdownLuaCleanup(callback_root, snapshot);
                    }
                    else if (snapshot.phase == LuaLifecyclePhase::Bound &&
                             snapshot.root == callback_root &&
                             InterlockedCompareExchange(&g_manifest_reset_pending, 0, 0) != 0)
                    {
                        InterlockedExchange(&g_manifest_reset_pending, 0);
                        ClearManifestRefs(callback_root);
                        SetRuntimeState(RuntimeState::Dormant,
                                        "DORMANT：会话已清理，等待重新 Register/Start");
                        callback_generation = snapshot.generation;
                    }
                    else
                    {
                        const LuaBindingDecision decision =
                            g_lua_lifecycle.PrepareBinding(callback_root);
                        if (decision.disposition == LuaBindingDisposition::Bind)
                        {
                            if (RegisterLuaApi(callback_root, decision.generation))
                                callback_generation = decision.generation;
                            else
                                FailRuntime(ErrorCode::LuaRegistrationFailed,
                                            "failed to bind Isaac.Sync transactionally", false);
                        }
                        else if (decision.disposition == LuaBindingDisposition::AlreadyBound)
                        {
                            const LuaLifecycleSnapshot current = g_lua_lifecycle.Snapshot();
                            if (current.gate_open)
                                callback_generation = decision.generation;
                        }
                        else if (decision.disposition == LuaBindingDisposition::DifferentRoot)
                        {
                            HandleUnexpectedLuaRoot(callback_root);
                        }
                        else if (decision.disposition ==
                                 LuaBindingDisposition::GenerationExhausted)
                        {
                            FailRuntime(ErrorCode::LuaRegistrationFailed,
                                        "Lua generation counter exhausted", false);
                        }
                    }
                }
            }
            result = g_lua_pcallk(state, nargs, nresults, errfunc, context, continuation);
            if (main_lua_call && callback_root && callback_generation &&
                InterlockedCompareExchange(&g_unloading, 0, 0) == 0)
            {
                FreezeManifestAfterLuaCallback(callback_root, callback_generation, result);
            }
        }
        __finally
        {
            LeaveHook();
        }
        return result;
    }

    std::uint32_t ReadOnlineFrame(void *self)
    {
        __try
        {
            return *reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(self) + 0x0C);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return UINT32_MAX;
        }
    }

    class RuntimeHandlerInvoker final : public IHandlerInvoker
    {
    public:
        HandlerInvokeResult Invoke(std::uint32_t sender_slot,
                                   const CustomFrameOpV2 &operation,
                                   std::uint32_t frame) override
        {
            LuaManifestEntry *manifest = FindManifest(operation.port);
            if (!manifest)
                return HandlerInvokeResult::UnknownPort;
            if (manifest->handler_ref < 0)
                return HandlerInvokeResult::InvalidRef;
            const LuaLifecycleSnapshot lifecycle = g_lua_lifecycle.Snapshot();
            void *const root = lifecycle.root;
            if (lifecycle.phase != LuaLifecyclePhase::Bound || !lifecycle.gate_open || !root ||
                !g_shared || GetCurrentThreadId() != g_shared->main_thread_id ||
                !g_lua_checkstack(root, 5))
                return HandlerInvokeResult::StackFailure;
            const int top = g_lua_gettop(root);
            HandlerInvokeResult result = HandlerInvokeResult::LuaFailure;
            InterlockedIncrement(&g_lua_handler_dispatch_depth);
            __try
            {
                g_lua_rawgeti(root, kLuaRegistryIndex, manifest->handler_ref);
                if (g_lua_type(root, -1) != kLuaTypeFunction)
                {
                    result = HandlerInvokeResult::InvalidRef;
                }
                else
                {
                    g_lua_pushinteger(root, sender_slot);
                    g_lua_pushinteger(root, operation.opcode);
                    if (!PushLuaNumericPayload(g_lua_adapter, root, operation))
                    {
                        result = HandlerInvokeResult::LuaFailure;
                    }
                    else
                    {
                        g_lua_pushinteger(root, frame);
                        const int status = g_lua_pcallk(root, 4, 0, 0, 0, nullptr);
                        result = status == 0   ? HandlerInvokeResult::Success
                                 : status == 1 ? HandlerInvokeResult::YieldFailure
                                               : HandlerInvokeResult::PcallFailure;
                    }
                }
                if (g_lua_lifecycle.IsBoundCall(root, lifecycle.generation))
                    g_lua_settop(root, top);
                else
                    result = HandlerInvokeResult::LuaFailure;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = HandlerInvokeResult::LuaFailure;
                __try
                {
                    if (g_lua_lifecycle.IsBoundCall(root, lifecycle.generation))
                        g_lua_settop(root, top);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            InterlockedDecrement(&g_lua_handler_dispatch_depth);
            if (result == HandlerInvokeResult::Success)
            {
                InterlockedIncrement64(&g_shared->handler_invocations);
                if (sender_slot < kMaximumPeerCount)
                    InterlockedIncrement64(&g_shared->handler_invocations_by_slot[sender_slot]);
                InterlockedExchange(&g_shared->last_dispatch_frame, static_cast<LONG>(frame));
                InterlockedExchange(&g_shared->last_dispatch_slot, static_cast<LONG>(sender_slot));
                g_shared->last_dispatch_port = operation.port;
                g_shared->last_dispatch_opcode = operation.opcode;
                std::int32_t legacy_payload = 0;
                DecodeInt32Payload(operation, legacy_payload);
                g_shared->last_dispatch_payload = legacy_payload;
                g_shared->last_dispatch_payload_kind = operation.payload_kind;
                InterlockedExchange64(
                    reinterpret_cast<volatile LONG64 *>(&g_shared->last_dispatch_payload_bits),
                    static_cast<LONG64>(operation.payload_bits));
                PublishOperationEvent(IpcEventType::SyncDispatch, "Lua handler dispatched",
                                      frame, sender_slot, operation);
            }
            return result;
        }
    };

    class RuntimeOriginalDispatch final : public IOriginalDispatch
    {
    public:
        std::uint32_t Call(void *self, std::uint32_t argument) override
        {
            return g_dispatch(self, argument);
        }
    };

    void PublishOriginalDispatchCompleted(std::uint32_t frame,
                                          std::uint16_t successful_invocations)
    {
        IpcEventV5 *event = BeginEvent(IpcEventType::SyncOriginalDispatchCompleted);
        if (!event)
            return;
        event->online_frame = frame;
        event->session_token = SessionToken(g_session ? g_session->Session() : SessionId{});
        event->count = successful_invocations;
        event->operation_frame = frame;
        event->sender_slot = UINT32_MAX;
        event->recipient_slot = UINT32_MAX;
        CopyText(event->message, sizeof(event->message),
                 "original dispatch completed after custom operations");
        CommitEvent(event);
    }

    extern "C" std::uint32_t __fastcall DispatchHook(void *self, void *,
                                                     std::uint32_t argument)
    {
        EnterHook();
        RuntimeHandlerInvoker invoker;
        RuntimeOriginalDispatch original;
        if (!g_shared || !g_session || g_session->State() != RuntimeState::Active ||
            InterlockedCompareExchange(&g_hooks_ready, 0, 0) == 0 ||
            InterlockedCompareExchange(&g_unloading, 0, 0) != 0)
        {
            const std::uint32_t passthrough = original.Call(self, argument);
            LeaveHook();
            return passthrough;
        }
        const std::uint32_t frame = ReadOnlineFrame(self);
        ErrorCode failure = ErrorCode::None;
        std::uint32_t result = 0;
        const auto guard = g_shared ? static_cast<DispatchGuardState>(
                                          InterlockedCompareExchange(&g_shared->dispatch_guard_state, 0, 0))
                                    : DispatchGuardState::Idle;
        const std::uint32_t guard_frame = g_shared ? static_cast<std::uint32_t>(
                                                         InterlockedCompareExchange(&g_shared->dispatch_guard_frame, 0, 0))
                                                   : UINT32_MAX;
        if (guard != DispatchGuardState::Prepared || frame == UINT32_MAX || guard_frame != frame ||
            g_roster.peer_count < 2 || g_roster.peer_count > kMaximumPeerCount ||
            g_local_slot >= g_roster.peer_count)
        {
            failure = ErrorCode::DispatchStateViolation;
            result = original.Call(self, argument);
        }
        else
        {
            std::array<CustomFrameOpV2, kMaximumPeerCount> operations{};
            bool complete = true;
            for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
            {
                const CustomFrameOpV2 *operation = slot == g_local_slot
                                                       ? g_local_sealer.Find(frame)
                                                       : g_remote_stores[slot].Find(frame);
                if (!operation)
                {
                    complete = false;
                    break;
                }
                operations[slot] = *operation;
            }
            if (!complete)
            {
                failure = ErrorCode::DispatchStateViolation;
                result = original.Call(self, argument);
            }
            else
            {
                const DispatchExecutionResult execution = ExecuteDeterministicDispatch(
                    frame, operations, g_roster.peer_count, invoker, original, self, argument);
                result = execution.original_result;
                failure = execution.error;
                if (execution.attempted_invocations != 0)
                    PublishOriginalDispatchCompleted(frame, execution.successful_invocations);
            }
        }
        if (g_shared)
        {
            if (failure == ErrorCode::None)
            {
                InterlockedExchange(&g_shared->dispatch_guard_state,
                                    static_cast<LONG>(DispatchGuardState::Dispatched));
            }
            else
            {
                InterlockedIncrement64(&g_shared->dispatch_failures);
                InterlockedExchange(&g_shared->dispatch_guard_state,
                                    static_cast<LONG>(DispatchGuardState::Failed));
                const CustomFrameOpV2 empty{};
                PublishOperationEvent(IpcEventType::SyncDispatchFailure,
                                      "deterministic dispatch failed", frame,
                                      UINT32_MAX, empty, failure);
                FailRuntime(failure, failure == ErrorCode::DispatchStateViolation ? "dispatch guard did not match current online frame" : failure == ErrorCode::DispatchRefInvalid ? "dispatch registry ref was invalid"
                                                                                                                                      : failure == ErrorCode::FrameUnknownPort     ? "dispatch port was absent from frozen manifest"
                                                                                                                                                                                   : "Lua handler failed or attempted to yield",
                            true);
            }
        }
        LeaveHook();
        return result;
    }

    extern "C" bool __fastcall FrameUpdateHook(void *self, void *)
    {
        EnterHook();
        if (InterlockedCompareExchange(&g_hooks_ready, 0, 0) == 0 ||
            InterlockedCompareExchange(&g_unloading, 0, 0) != 0)
        {
            const bool passthrough = g_frame_update(self);
            LeaveHook();
            return passthrough;
        }
        const LuaLifecycleSnapshot lua_lifecycle = g_lua_lifecycle.Snapshot();
        if (lua_lifecycle.phase == LuaLifecyclePhase::Bound &&
            lua_lifecycle.unexpected_root_latched && g_shared &&
            static_cast<RuntimeState>(InterlockedCompareExchange(
                &g_shared->runtime_state, 0, 0)) == RuntimeState::FailedFatal)
        {
            LeaveHook();
            return false;
        }
        const std::uint64_t now_ms = MonotonicMs();
        const std::uint32_t frame = ReadOnlineFrame(self);
        if (g_shared && frame != UINT32_MAX)
        {
            if (!g_shared->main_thread_id)
                g_shared->main_thread_id = GetCurrentThreadId();
            InterlockedExchange(&g_shared->online_frame, static_cast<LONG>(frame));
        }
        bool call_original = true;
        if (g_shared && frame != UINT32_MAX &&
            InterlockedCompareExchange(&g_unloading, 0, 0) == 0)
        {
            bool reset_this_frame = false;
            if (g_session && (self != g_session_manager || frame < g_last_observed_frame))
            {
                RequestSessionReset();
                reset_this_frame = true;
            }
            bool compatible = false;
            if (g_session)
            {
                const RuntimeState session_state = g_session->State();
                if (session_state != RuntimeState::FailedPreActive &&
                    session_state != RuntimeState::FailedFatal &&
                    session_state != RuntimeState::Stopping &&
                    session_state != RuntimeState::Stopped)
                {
                    const SessionEnvironmentResult environment =
                        ValidateSessionEnvironment(self);
                    if (environment == SessionEnvironmentResult::Ended)
                    {
                        PublishEvent(IpcEventType::Roster,
                                     "原版 peer roster 已缩减；正常结束 sidecar session",
                                     frame, g_roster_end_tracker.Observations());
                        RequestSessionReset();
                    }
                    else
                    {
                        compatible = environment == SessionEnvironmentResult::Ready ||
                                     environment == SessionEnvironmentResult::PendingEnd;
                    }
                }
            }
            else if (!reset_this_frame)
            {
                compatible = DiscoverCompatibility(self, frame);
            }
            g_last_observed_frame = frame;
            UpdateEmptyManifestAutoStart(compatible, now_ms);
            if (!g_session && compatible &&
                g_manifest_start.StartRequested() && g_manifest_start.Frozen())
            {
                StartSession(self, frame, now_ms);
            }
            ProcessTestCommand(now_ms);
            if (g_session)
            {
                const RuntimeState state = g_session->State();
                if (compatible &&
                    (state == RuntimeState::Discovering || state == RuntimeState::Hello ||
                     state == RuntimeState::Warmup || state == RuntimeState::Active) &&
                    !DrainPackets(frame, now_ms))
                {
                    if (g_session->State() == RuntimeState::Active)
                        call_original = false;
                }
                if (g_session && g_session->State() == RuntimeState::Stopping)
                {
                    PublishEvent(IpcEventType::SyncHandshake,
                                 "收到 GOODBYE；正常结束 sidecar session", frame);
                    RequestSessionReset(false);
                }
                if (g_session && !g_session->Tick(frame, now_ms))
                {
                    FailRuntime(g_session->Error(), g_session->ErrorText(),
                                g_session->State() == RuntimeState::FailedFatal);
                }
                if (g_session && (g_session->State() == RuntimeState::Warmup ||
                                  g_session->State() == RuntimeState::Active))
                {
                    if (!SealAndSend(frame, now_ms) &&
                        g_session->State() == RuntimeState::FailedFatal)
                    {
                        call_original = false;
                    }
                }
                if (g_session)
                {
                    PublishSessionFlags();
                    if (g_session->BarrierRequired(frame))
                    {
                        SetRuntimeState(RuntimeState::Warmup,
                                        "WARMUP：activation barrier 等待 ACK commit");
                        PublishEvent(IpcEventType::ActivationBarrier,
                                     "activation barrier 阻止原 frame update", frame,
                                     g_session->ActivationFrame());
                        call_original = false;
                    }
                    else
                    {
                        if (g_session->ActivateIfReady(frame))
                        {
                            if (!g_warmup_prefix_retired)
                            {
                                const std::uint32_t activation = g_session->ActivationFrame();
                                const std::uint32_t local_retired =
                                    g_local_sealer.RetireBefore(activation);
                                std::uint32_t remote_retired = 0;
                                for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
                                {
                                    if (slot != g_local_slot)
                                        remote_retired +=
                                            g_remote_stores[slot].RetireBefore(activation);
                                }
                                g_warmup_prefix_retired = true;
                                PublishEvent(IpcEventType::ActivationBarrier,
                                             "activation 前 warmup records 已退役",
                                             activation,
                                             local_retired, 0, 0, 0, 0, 0,
                                             static_cast<std::uint16_t>(remote_retired));
                            }
                            SetRuntimeState(RuntimeState::Active,
                                             "ACTIVE：" ISAAC_SYNC_PRODUCT_VERSION
                                             " typed all-slot ready gate 与确定性分发已启用");
                            PublishSessionFlags();
                        }
                        // The ready gate is required for every ACTIVE frame, not only
                        // for the transition frame.  It must run before the frame
                        // wrapper prepares the deterministic-dispatch guard.
                        if (g_session && g_session->State() == RuntimeState::Active)
                        {
                            if (!CustomReady(frame, now_ms))
                                call_original = false;
                        }
                        else if (g_session && g_session->State() == RuntimeState::Warmup)
                        {
                            SetRuntimeState(RuntimeState::Warmup,
                                            "WARMUP：交换 activation 前 no-op records");
                        }
                        else if (g_session && (g_session->State() == RuntimeState::Hello ||
                                               g_session->State() == RuntimeState::Discovering))
                        {
                            SetRuntimeState(g_session->State(),
                                            g_session->State() == RuntimeState::Hello
                                                ? "HELLO：正式握手进行中"
                                                : "DISCOVERING：轮询 coordinator HELLO");
                        }
                        else if (g_session && g_session->State() == RuntimeState::FailedFatal)
                        {
                            call_original = false;
                        }
                    }
                }
            }
        }
        bool dispatch_guard_prepared = false;
        if (call_original && g_session && g_session->State() == RuntimeState::Active)
        {
            const auto guard = static_cast<DispatchGuardState>(
                InterlockedCompareExchange(&g_shared->dispatch_guard_state, 0, 0));
            if (guard != DispatchGuardState::Idle)
            {
                FailRuntime(ErrorCode::DispatchStateViolation,
                            "dispatch guard was not idle before frame update", true);
                call_original = false;
            }
            else
            {
                InterlockedExchange(&g_shared->dispatch_guard_frame, static_cast<LONG>(frame));
                InterlockedExchange(&g_shared->dispatch_guard_state,
                                    static_cast<LONG>(DispatchGuardState::Prepared));
                dispatch_guard_prepared = true;
            }
        }
        if (!call_original)
        {
            LeaveHook();
            return 0;
        }
        const bool result = g_frame_update(self);
        const std::uint32_t after = ReadOnlineFrame(self);
        if (g_shared && after != UINT32_MAX)
        {
            InterlockedExchange(&g_shared->online_frame, static_cast<LONG>(after));
        }
        const auto completed_guard = g_shared ? static_cast<DispatchGuardState>(
            InterlockedCompareExchange(&g_shared->dispatch_guard_state, 0, 0))
                                              : DispatchGuardState::Idle;
        const FrameDispatchTransition transition = ClassifyFrameDispatchTransition(
            result, completed_guard == DispatchGuardState::Dispatched);
        if (dispatch_guard_prepared &&
            transition == FrameDispatchTransition::BaseStalled)
        {
            if (completed_guard == DispatchGuardState::Prepared)
            {
                InterlockedExchange(&g_shared->dispatch_guard_state,
                                    static_cast<LONG>(DispatchGuardState::Idle));
                InterlockedExchange(&g_shared->dispatch_guard_frame, static_cast<LONG>(UINT32_MAX));
            }
        }
        else if (dispatch_guard_prepared &&
                 transition == FrameDispatchTransition::DispatchDuringStall)
        {
            InterlockedExchange(&g_shared->dispatch_guard_state,
                                static_cast<LONG>(DispatchGuardState::Failed));
            FailRuntime(ErrorCode::DispatchStateViolation,
                        "dispatch ran although base frame update did not advance", true);
        }
        if (dispatch_guard_prepared &&
            transition == FrameDispatchTransition::DispatchMissing && g_session &&
            g_session->State() == RuntimeState::Active)
        {
            InterlockedExchange(&g_shared->dispatch_guard_state,
                                static_cast<LONG>(DispatchGuardState::Failed));
            FailRuntime(ErrorCode::DispatchStateViolation,
                        "successful frame update did not dispatch exactly once", true);
        }
        if (g_session && g_session->State() == RuntimeState::Active && result)
        {
            if (frame == UINT32_MAX || after != frame + 1u)
            {
                FailRuntime(ErrorCode::FrameStateViolation,
                            "base frame update reported success without F to F+1", true);
            }
            else
            {
                bool can_consume = g_local_sealer.CanConsume(frame);
                for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
                {
                    if (slot != g_local_slot)
                        can_consume = can_consume && g_remote_stores[slot].CanConsume(frame);
                }
                if (!can_consume)
                {
                    FailRuntime(ErrorCode::FrameStateViolation,
                                "not all custom records were consumable after base update", true);
                }
                else
                {
                    bool consumed = g_local_sealer.Consume(frame) == FrameStoreResult::Inserted;
                    for (std::uint16_t slot = 0; slot < g_roster.peer_count; ++slot)
                    {
                        if (slot != g_local_slot)
                            consumed = consumed &&
                                       g_remote_stores[slot].Consume(frame) ==
                                           FrameStoreResult::Inserted;
                    }
                    if (!consumed)
                    {
                        FailRuntime(ErrorCode::FrameStateViolation,
                                    "custom records failed atomic consume", true);
                    }
                    else if (g_shared)
                    {
                        InterlockedExchange(&g_shared->consumed_through,
                                            static_cast<LONG>(frame));
                        InterlockedExchange(&g_shared->dispatch_guard_state,
                                            static_cast<LONG>(DispatchGuardState::Idle));
                        InterlockedExchange(&g_shared->dispatch_guard_frame,
                                            static_cast<LONG>(UINT32_MAX));
                    }
                }
            }
        }
        LeaveHook();
        return result;
    }

    DWORD WINAPI InitializeRuntime(void *)
    {
        if (!OpenSharedControlBlock())
        {
            FreeLibraryAndExitThread(g_module, static_cast<DWORD>(ErrorCode::MappingMissing));
        }
    SetRuntimeState(RuntimeState::Initializing,
                    "正在验证 Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION " 运行环境");
        InterlockedExchange(&g_shared->compatibility_state,
                            static_cast<LONG>(CompatibilityState::Checking));
        InterlockedExchange(&g_shared->online_frame, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->activation_frame, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->sealed_through, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->received_through, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->consumed_through, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->stall_frame, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->last_send_target, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->dispatch_guard_frame, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->dispatch_guard_state,
                            static_cast<LONG>(DispatchGuardState::Idle));
        InterlockedExchange(&g_shared->last_dispatch_frame, static_cast<LONG>(UINT32_MAX));
        InterlockedExchange(&g_shared->last_dispatch_slot, static_cast<LONG>(UINT32_MAX));
        g_shared->last_dispatch_payload_kind = 0;
        g_shared->reserved_typed_payload = 0;
        InterlockedExchange64(
            reinterpret_cast<volatile LONG64 *>(&g_shared->last_dispatch_payload_bits), 0);
        g_shared->peer_count = 0;
        g_shared->local_slot = UINT32_MAX;
        g_shared->coordinator_slot = UINT32_MAX;
        for (std::size_t slot = 0; slot < kMaximumPeerCount; ++slot)
            InterlockedExchange(&g_shared->received_through_by_slot[slot],
                                static_cast<LONG>(UINT32_MAX));
        g_main_module = GetModuleHandleW(nullptr);
        g_main_base = reinterpret_cast<std::uint8_t *>(g_main_module);
        g_shared->module_base = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(g_main_base));
        if (!IsTargetImage())
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::TargetImageMismatch,
                             "加载失败：目标不是 isaac-ng.exe");
            SetRuntimeState(RuntimeState::FailedPreActive, "目标映像校验失败");
            return 1;
        }
        if (!ComputeBuildFingerprint(g_build_hash))
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::BuildFingerprintFailed,
                             "加载失败：无法计算游戏构建指纹");
            SetRuntimeState(RuntimeState::FailedPreActive, "游戏构建指纹计算失败");
            return 2;
        }
        std::memcpy(g_shared->build_hash, g_build_hash.data(), g_build_hash.size());
        if (!ValidateCriticalSites(g_main_base))
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::CallsiteMismatch,
                             "加载失败：游戏版本或关键机器码不匹配");
            SetRuntimeState(RuntimeState::FailedPreActive, "关键 Hook 点验证失败");
            return 3;
        }
        if (!g_lua_adapter.Resolve())
        {
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::LuaRuntimeUnavailable,
                             "加载失败：Lua5.3.3r exports 不完整");
            SetRuntimeState(RuntimeState::FailedPreActive, "Lua runtime 校验失败");
            return 4;
        }
        g_runtime_steam_api = std::make_unique<RuntimeSteamApi>();
        g_runtime_callbacks = std::make_unique<RuntimeCallbacks>();
        InterlockedExchange(&g_hooks_ready, 0);
        const bool hooks_installed =
            InstallIatPatch(g_main_module, "lua_close",
                            reinterpret_cast<void *>(&LuaCloseHook), g_lua_close_patch,
                            reinterpret_cast<void *volatile *>(&g_lua_close),
                            reinterpret_cast<void *>(g_lua_close)) &&
            InstallIatPatch(g_main_module, "lua_newstate",
                            reinterpret_cast<void *>(&LuaNewstateHook), g_lua_newstate_patch,
                            reinterpret_cast<void *volatile *>(&g_lua_newstate),
                            reinterpret_cast<void *>(g_lua_newstate)) &&
            InstallIatPatch(g_main_module, "luaL_newstate",
                            reinterpret_cast<void *>(&LuaLNewstateHook), g_lua_lnewstate_patch,
                            reinterpret_cast<void *volatile *>(&g_lua_lnewstate),
                            reinterpret_cast<void *>(g_lua_lnewstate)) &&
            InstallIatPatch(g_main_module, "lua_pcallk",
                            reinterpret_cast<void *>(&LuaPcallkHook), g_lua_pcall_patch,
                            reinterpret_cast<void *volatile *>(&g_lua_pcallk),
                            reinterpret_cast<void *>(g_lua_pcallk)) &&
            InstallFramePatch() && InstallDispatchPatch() &&
            InstallIatPatch(g_main_module, "SteamInternal_ContextInit",
                            reinterpret_cast<void *>(&SteamContextInitHook), g_steam_context_patch,
                            reinterpret_cast<void *volatile *>(&g_steam_context_init));
        if (!hooks_installed)
        {
            RestoreIatPatch(g_steam_context_patch);
            RestoreDispatchPatch();
            RestoreFramePatch();
            RestoreIatPatch(g_lua_pcall_patch);
            RestoreIatPatch(g_lua_lnewstate_patch);
            RestoreIatPatch(g_lua_newstate_patch);
            RestoreIatPatch(g_lua_close_patch);
            SetCompatibility(CompatibilityState::Unsupported, ErrorCode::HookInstallFailed,
                             "加载失败：生命周期 Hook 事务安装或回滚失败");
            SetRuntimeState(RuntimeState::FailedPreActive, "Hook 安装失败");
            return 5;
        }
        InterlockedExchange(&g_hooks_ready, 1);
        PublishEvent(IpcEventType::HookInstalled,
                     "已事务安装 Lua lifecycle、frame、dispatch 与 Steam wrappers");
        SetCompatibility(CompatibilityState::WaitingForSteam, ErrorCode::None,
                         "DORMANT：等待 Steam 联机环境与 Lua Register/Start");
        SetRuntimeState(RuntimeState::Dormant,
                        "DORMANT：Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION
                        " 已加载，未启动 sidecar");
        return 0;
    }

} // namespace isaac_sync::runtime

using namespace isaac_sync::runtime;

extern "C" DWORD WINAPI IsaacSyncShutdown(void *)
{
    if (InterlockedExchange(&g_shutdown_started, 1) != 0)
        return 1;
    if (!g_shared)
        return 2;
    const auto state = static_cast<RuntimeState>(
        InterlockedCompareExchange(&g_shared->runtime_state, 0, 0));
    if (state == RuntimeState::Active || state == RuntimeState::FailedFatal)
    {
        SetCompatibility(static_cast<CompatibilityState>(g_shared->compatibility_state),
                         ErrorCode::ShutdownUnsafe, g_shared->status,
                         "ACTIVE 或 FAILED_FATAL 状态拒绝卸载；请先返回菜单");
        InterlockedExchange(&g_shutdown_started, 0);
        return 3;
    }
    const LuaLifecycleSnapshot lua_snapshot = g_lua_lifecycle.Snapshot();
    if (lua_snapshot.phase != LuaLifecyclePhase::Unbound)
    {
        g_lua_lifecycle.CloseGate();
        InterlockedExchange(&g_lua_cleanup_complete, 0);
        InterlockedExchange(&g_lua_cleanup_pending, 1);
        InterlockedOr(&g_shared->flags, IpcFlagLuaCleanupPending);
        const ULONGLONG deadline = GetTickCount64() + kLuaCleanupTimeoutMs;
        while (GetTickCount64() < deadline &&
               InterlockedCompareExchange(&g_lua_cleanup_complete, 0, 0) == 0)
            Sleep(1);
        if (InterlockedCompareExchange(&g_lua_cleanup_complete, 0, 0) == 0)
        {
            InterlockedExchange(&g_lua_cleanup_pending, 0);
            InterlockedAnd(&g_shared->flags,
                           static_cast<LONG>(~IpcFlagLuaCleanupPending));
            g_lua_lifecycle.ReopenGate(lua_snapshot.root, lua_snapshot.generation);
            SetCompatibility(static_cast<CompatibilityState>(g_shared->compatibility_state),
                             ErrorCode::LuaCleanupUnsafe,
                             "卸载失败：无法在主线程清理 Lua refs",
                             "5 秒内没有命中已绑定 state 的安全 lua_pcallk");
            InterlockedExchange(&g_shutdown_started, 0);
            return 4;
        }
    }
    SetRuntimeState(RuntimeState::Unloading, "正在恢复 V2 Hook 并安全卸载");
    InterlockedExchange(&g_hooks_ready, 0);
    InterlockedExchange(&g_unloading, 1);
    bool hooks_restored = true;
    hooks_restored = RestoreIatPatch(g_steam_context_patch) && hooks_restored;
    hooks_restored = RestoreDispatchPatch() && hooks_restored;
    hooks_restored = RestoreFramePatch() && hooks_restored;
    hooks_restored = RestoreIatPatch(g_lua_pcall_patch) && hooks_restored;
    hooks_restored = RestoreIatPatch(g_lua_lnewstate_patch) && hooks_restored;
    hooks_restored = RestoreIatPatch(g_lua_newstate_patch) && hooks_restored;
    // Keep lua_close installed until every closure-producing/consuming hook has
    // been restored, so a concurrent VM teardown can never jump into freed code.
    hooks_restored = RestoreIatPatch(g_lua_close_patch) && hooks_restored;
    if (!hooks_restored)
    {
        SetCompatibility(static_cast<CompatibilityState>(g_shared->compatibility_state),
                         ErrorCode::ShutdownUnsafe,
                         "卸载失败：无法证明所有 Hook 已恢复",
                         "DLL 保持加载；可排除内存保护冲突后重试安全卸载");
        InterlockedExchange(&g_shutdown_started, 0);
        return 6;
    }
    const ULONGLONG deadline = GetTickCount64() + 5000;
    ULONGLONG zero_since = 0;
    while (GetTickCount64() < deadline)
    {
        if (InterlockedCompareExchange(&g_active_hooks, 0, 0) == 0)
        {
            if (!zero_since)
                zero_since = GetTickCount64();
            if (GetTickCount64() - zero_since >= 100)
                break;
        }
        else
        {
            zero_since = 0;
        }
        Sleep(1);
    }
    if (!zero_since || GetTickCount64() - zero_since < 100)
    {
        SetCompatibility(static_cast<CompatibilityState>(g_shared->compatibility_state),
                         ErrorCode::ShutdownUnsafe, "卸载失败：仍有 Hook 正在执行",
                         "目标字节已恢复，但 DLL 为安全起见保持加载");
        InterlockedExchange(&g_shutdown_started, 0);
        return 5;
    }
    InterlockedExchange(&g_unloading, 0);
    PublishEvent(IpcEventType::HookRestored,
                 "frame/dispatch、Lua lifecycle IAT 与 Steam context IAT 已恢复");
    PublishEvent(IpcEventType::SyncSummary,
                 ISAAC_SYNC_PRODUCT_VERSION " group session summary",
                 static_cast<std::uint64_t>(g_shared->records_sent),
                 static_cast<std::uint64_t>(g_shared->records_received));
    PublishEvent(IpcEventType::Shutdown,
                 "Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION " 已安全卸载");
    SetRuntimeState(RuntimeState::Unloaded,
                    "Isaac Sync " ISAAC_SYNC_PRODUCT_VERSION " 已卸载");
    InterlockedExchange(&g_unloading, 1);
    g_session.reset();
    g_runtime_callbacks.reset();
    g_runtime_transport.reset();
    g_runtime_steam_api.reset();
    CloseSharedControlBlock();
    FreeLibraryAndExitThread(g_module, 0);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &InitializeRuntime, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
