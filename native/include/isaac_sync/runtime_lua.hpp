#pragma once

#include "protocol.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace isaac_sync::runtime {

using LuaPcallkFn = int(__cdecl *)(void*, int, int, int, std::intptr_t, void*);
using LuaAllocFn = void*(__cdecl *)(void*, void*, std::size_t, std::size_t);
using LuaNewStateFn = void*(__cdecl *)(LuaAllocFn, void*);
using LuaLNewStateFn = void*(__cdecl *)();
using LuaCloseFn = void(__cdecl *)(void*);
using LuaCFunction = int(__cdecl *)(void*);
using LuaCheckStackFn = int(__cdecl *)(void*, int);
using LuaGetTopFn = int(__cdecl *)(void*);
using LuaSetTopFn = void(__cdecl *)(void*, int);
using LuaTypeFn = int(__cdecl *)(void*, int);
using LuaCreateTableFn = void(__cdecl *)(void*, int, int);
using LuaPushCClosureFn = void(__cdecl *)(void*, LuaCFunction, int);
using LuaRawGetFn = int(__cdecl *)(void*, int);
using LuaRawGetIFn = int(__cdecl *)(void*, int, std::int64_t);
using LuaRawSetFn = void(__cdecl *)(void*, int);
using LuaPushNilFn = void(__cdecl *)(void*);
using LuaPushBooleanFn = void(__cdecl *)(void*, int);
using LuaPushIntegerFn = void(__cdecl *)(void*, std::int64_t);
using LuaPushNumberFn = void(__cdecl *)(void*, double);
using LuaPushStringFn = const char*(__cdecl *)(void*, const char*);
using LuaToStringFn = const char*(__cdecl *)(void*, int, std::size_t*);
using LuaToIntegerFn = std::int64_t(__cdecl *)(void*, int, int*);
using LuaToNumberFn = double(__cdecl *)(void*, int, int*);
using LuaIsIntegerFn = int(__cdecl *)(void*, int);
using LuaPushValueFn = void(__cdecl *)(void*, int);
using LuaRefFn = int(__cdecl *)(void*, int);
using LuaUnrefFn = void(__cdecl *)(void*, int, int);
using LuaToThreadFn = void*(__cdecl *)(void*, int);

struct LuaAdapter {
    LuaNewStateFn newstate = nullptr;
    LuaLNewStateFn lnewstate = nullptr;
    LuaCloseFn close = nullptr;
    LuaPcallkFn pcallk = nullptr;
    LuaCheckStackFn checkstack = nullptr;
    LuaGetTopFn gettop = nullptr;
    LuaSetTopFn settop = nullptr;
    LuaTypeFn type = nullptr;
    LuaCreateTableFn createtable = nullptr;
    LuaPushCClosureFn pushcclosure = nullptr;
    LuaRawGetFn rawget = nullptr;
    LuaRawGetIFn rawgeti = nullptr;
    LuaRawSetFn rawset = nullptr;
    LuaPushNilFn pushnil = nullptr;
    LuaPushBooleanFn pushboolean = nullptr;
    LuaPushIntegerFn pushinteger = nullptr;
    LuaPushNumberFn pushnumber = nullptr;
    LuaPushStringFn pushstring = nullptr;
    LuaToStringFn tolstring = nullptr;
    LuaToIntegerFn tointegerx = nullptr;
    LuaToNumberFn tonumberx = nullptr;
    LuaIsIntegerFn isinteger = nullptr;
    LuaPushValueFn pushvalue = nullptr;
    LuaRefFn ref = nullptr;
    LuaUnrefFn unref = nullptr;
    LuaToThreadFn tothread = nullptr;

    bool Resolve();
    bool IsResolved() const;
};

enum class CanonicalLuaRootResult {
    Success,
    InvalidArgument,
    StackUnavailable,
    MainThreadMissing,
    Exception,
};

CanonicalLuaRootResult ResolveCanonicalLuaRoot(const LuaAdapter& adapter, void* state,
                                               void*& root) noexcept;

enum class LuaLifecyclePhase : std::uint8_t {
    Unbound,
    Bound,
    Closing,
};

enum class LuaBindingDisposition : std::uint8_t {
    Bind,
    AlreadyBound,
    DifferentRoot,
    Closing,
    GenerationExhausted,
    InvalidRoot,
};

enum class LuaCloseDisposition : std::uint8_t {
    BoundOwner,
    BoundReentrant,
    Auxiliary,
    NullState,
};

struct LuaBindingDecision {
    LuaBindingDisposition disposition = LuaBindingDisposition::InvalidRoot;
    std::uint64_t generation = 0;
};

struct LuaLifecycleSnapshot {
    LuaLifecyclePhase phase = LuaLifecyclePhase::Unbound;
    void* root = nullptr;
    std::uint64_t generation = 0;
    bool gate_open = false;
    bool unexpected_root_latched = false;
};

// Synchronizes the game-thread Lua lifecycle with the shutdown worker.  No Lua
// callback is invoked while this object's lock is held.
class LuaLifecycleTracker {
public:
    explicit LuaLifecycleTracker(std::uint64_t next_generation = 0) noexcept
        : next_generation_(next_generation) {}

    LuaBindingDecision PrepareBinding(void* canonical_root) const noexcept;
    bool CommitBinding(void* canonical_root, std::uint64_t generation) noexcept;
    LuaCloseDisposition BeginClose(void* root, LuaLifecycleSnapshot& snapshot) noexcept;
    bool LatchUnexpectedRoot(void* observed_root) noexcept;
    void CloseGate() noexcept;
    bool ReopenGate(void* root, std::uint64_t generation) noexcept;
    bool UnregisterClosingRoot(void* root, std::uint64_t generation) noexcept;
    bool CompleteClose(void* root, std::uint64_t generation) noexcept;
    bool CompleteShutdownCleanup(void* root, std::uint64_t generation) noexcept;
    bool IsBoundCall(void* canonical_root, std::uint64_t generation = 0) const noexcept;
    LuaLifecycleSnapshot Snapshot() const noexcept;

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    LuaLifecyclePhase phase_ = LuaLifecyclePhase::Unbound;
    void* bound_root_ = nullptr;
    void* closing_root_ = nullptr;
    std::uint64_t generation_ = 0;
    std::uint64_t closing_generation_ = 0;
    std::uint64_t next_generation_ = 0;
    bool gate_open_ = false;
    bool unexpected_root_latched_ = false;
};

class ManifestInvalidLatch {
public:
    void Invalidate() noexcept {
        invalid_.store(true, std::memory_order_release);
    }
    bool IsInvalid() const noexcept {
        return invalid_.load(std::memory_order_acquire);
    }
    bool Clear() noexcept {
        return invalid_.exchange(false, std::memory_order_acq_rel);
    }

private:
    std::atomic<bool> invalid_{false};
};

enum class ManifestStartRequestResult {
    Requested,
    AlreadyRequested,
    AlreadyFrozen,
};

class ManifestStartGate {
public:
    bool CanRegister() const noexcept {
        return !Frozen();
    }
    bool StartRequested() const noexcept {
        return start_requested_.load(std::memory_order_acquire);
    }
    bool Frozen() const noexcept {
        return frozen_.load(std::memory_order_acquire);
    }
    ManifestStartRequestResult RequestStart(std::size_t manifest_count) noexcept;
    bool TryFreeze(bool boundary_succeeded) noexcept;
    void CancelStartRequest() noexcept {
        start_requested_.store(false, std::memory_order_release);
    }
    void Reset() noexcept {
        start_requested_.store(false, std::memory_order_release);
        frozen_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> start_requested_{false};
    std::atomic<bool> frozen_{false};
};

inline constexpr std::uint64_t kEmptyManifestAutoStartGraceMs = 2000;

enum class EmptyManifestAutoStartResult {
    Ineligible,
    Waiting,
    Ready,
};

class EmptyManifestAutoStartGate {
public:
    EmptyManifestAutoStartResult Observe(bool eligible, std::uint64_t now_ms) noexcept;
    void Reset() noexcept {
        armed_ = false;
        ready_since_ms_ = 0;
    }

private:
    bool armed_ = false;
    std::uint64_t ready_since_ms_ = 0;
};

enum class LuaNumericResult {
    Success,
    NotNumber,
    IntegerRange,
    FloatNotFinite,
};

LuaNumericResult ReadLuaNumericPayload(const LuaAdapter& adapter, void* state, int index,
                                       std::uint16_t port, std::uint16_t opcode,
                                       CustomFrameOpV2& output);
bool PushLuaNumericPayload(const LuaAdapter& adapter, void* state,
                           const CustomFrameOpV2& operation);

} // namespace isaac_sync::runtime
