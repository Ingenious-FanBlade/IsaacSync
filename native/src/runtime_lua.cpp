#include "isaac_sync/runtime_lua.hpp"

#include <Windows.h>

namespace isaac_sync::runtime {

bool LuaAdapter::Resolve()
{
    HMODULE lua = GetModuleHandleW(L"Lua5.3.3r.dll");
    if (!lua)
        return false;
#define LUA_EXPORT(member, name)                                             \
    member = reinterpret_cast<decltype(member)>(GetProcAddress(lua, name));  \
    if (!member)                                                             \
        return false
    LUA_EXPORT(newstate, "lua_newstate");
    LUA_EXPORT(lnewstate, "luaL_newstate");
    LUA_EXPORT(close, "lua_close");
    LUA_EXPORT(pcallk, "lua_pcallk");
    LUA_EXPORT(checkstack, "lua_checkstack");
    LUA_EXPORT(gettop, "lua_gettop");
    LUA_EXPORT(settop, "lua_settop");
    LUA_EXPORT(type, "lua_type");
    LUA_EXPORT(createtable, "lua_createtable");
    LUA_EXPORT(pushcclosure, "lua_pushcclosure");
    LUA_EXPORT(rawget, "lua_rawget");
    LUA_EXPORT(rawgeti, "lua_rawgeti");
    LUA_EXPORT(rawset, "lua_rawset");
    LUA_EXPORT(pushnil, "lua_pushnil");
    LUA_EXPORT(pushboolean, "lua_pushboolean");
    LUA_EXPORT(pushinteger, "lua_pushinteger");
    LUA_EXPORT(pushnumber, "lua_pushnumber");
    LUA_EXPORT(pushstring, "lua_pushstring");
    LUA_EXPORT(tolstring, "lua_tolstring");
    LUA_EXPORT(tointegerx, "lua_tointegerx");
    LUA_EXPORT(tonumberx, "lua_tonumberx");
    LUA_EXPORT(isinteger, "lua_isinteger");
    LUA_EXPORT(pushvalue, "lua_pushvalue");
    LUA_EXPORT(ref, "luaL_ref");
    LUA_EXPORT(unref, "luaL_unref");
    LUA_EXPORT(tothread, "lua_tothread");
#undef LUA_EXPORT
    return true;
}

bool LuaAdapter::IsResolved() const
{
    return newstate && lnewstate && close && pcallk && checkstack && gettop && settop && type &&
           createtable && pushcclosure && rawget &&
           rawgeti && rawset && pushnil && pushboolean && pushinteger && pushnumber && pushstring &&
           tolstring && tointegerx && tonumberx && isinteger && pushvalue && ref && unref &&
           tothread;
}

CanonicalLuaRootResult ResolveCanonicalLuaRoot(const LuaAdapter& adapter, void* state,
                                               void*& root) noexcept
{
    constexpr int lua_registry_index = -1001000;
    constexpr std::int64_t lua_registry_main_thread = 1;
    constexpr int lua_type_thread = 8;
    root = nullptr;
    if (!state || !adapter.checkstack || !adapter.gettop || !adapter.settop || !adapter.rawgeti ||
        !adapter.type || !adapter.tothread)
        return CanonicalLuaRootResult::InvalidArgument;

    int top = 0;
    bool have_top = false;
    bool restored = false;
    CanonicalLuaRootResult result = CanonicalLuaRootResult::Exception;
    __try
    {
        top = adapter.gettop(state);
        have_top = true;
        if (!adapter.checkstack(state, 1))
        {
            result = CanonicalLuaRootResult::StackUnavailable;
        }
        else
        {
            adapter.rawgeti(state, lua_registry_index, lua_registry_main_thread);
            if (adapter.type(state, -1) != lua_type_thread)
            {
                result = CanonicalLuaRootResult::MainThreadMissing;
            }
            else
            {
                root = adapter.tothread(state, -1);
                result = root ? CanonicalLuaRootResult::Success
                              : CanonicalLuaRootResult::MainThreadMissing;
            }
        }
        adapter.settop(state, top);
        restored = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        root = nullptr;
        result = CanonicalLuaRootResult::Exception;
    }
    if (have_top && !restored)
    {
        __try
        {
            adapter.settop(state, top);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    return result;
}

LuaBindingDecision LuaLifecycleTracker::PrepareBinding(void* canonical_root) const noexcept
{
    if (!canonical_root)
        return {LuaBindingDisposition::InvalidRoot, 0};
    AcquireSRWLockShared(&lock_);
    LuaBindingDecision decision{};
    if (phase_ == LuaLifecyclePhase::Closing)
    {
        decision.disposition = LuaBindingDisposition::Closing;
    }
    else if (phase_ == LuaLifecyclePhase::Bound)
    {
        decision.disposition = bound_root_ == canonical_root
                                   ? LuaBindingDisposition::AlreadyBound
                                   : LuaBindingDisposition::DifferentRoot;
        decision.generation = generation_;
    }
    else if (next_generation_ == UINT64_MAX)
    {
        decision.disposition = LuaBindingDisposition::GenerationExhausted;
    }
    else
    {
        decision.disposition = LuaBindingDisposition::Bind;
        decision.generation = next_generation_ + 1;
    }
    ReleaseSRWLockShared(&lock_);
    return decision;
}

bool LuaLifecycleTracker::CommitBinding(void* canonical_root,
                                        std::uint64_t generation) noexcept
{
    if (!canonical_root || !generation)
        return false;
    AcquireSRWLockExclusive(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Unbound &&
                       next_generation_ != UINT64_MAX && generation == next_generation_ + 1;
    if (valid)
    {
        bound_root_ = canonical_root;
        closing_root_ = nullptr;
        generation_ = generation;
        closing_generation_ = 0;
        next_generation_ = generation;
        gate_open_ = true;
        unexpected_root_latched_ = false;
        phase_ = LuaLifecyclePhase::Bound;
    }
    ReleaseSRWLockExclusive(&lock_);
    return valid;
}

LuaCloseDisposition LuaLifecycleTracker::BeginClose(
    void* root, LuaLifecycleSnapshot& snapshot) noexcept
{
    snapshot = {};
    if (!root)
        return LuaCloseDisposition::NullState;
    AcquireSRWLockExclusive(&lock_);
    LuaCloseDisposition disposition = LuaCloseDisposition::Auxiliary;
    if (phase_ == LuaLifecyclePhase::Bound && bound_root_ == root)
    {
        phase_ = LuaLifecyclePhase::Closing;
        gate_open_ = false;
        closing_root_ = root;
        closing_generation_ = generation_;
        snapshot = {phase_, root, generation_, false, unexpected_root_latched_};
        disposition = LuaCloseDisposition::BoundOwner;
    }
    else if (phase_ == LuaLifecyclePhase::Closing && closing_root_ == root)
    {
        snapshot = {phase_, closing_root_, closing_generation_, false,
                    unexpected_root_latched_};
        disposition = LuaCloseDisposition::BoundReentrant;
    }
    ReleaseSRWLockExclusive(&lock_);
    return disposition;
}

bool LuaLifecycleTracker::LatchUnexpectedRoot(void* observed_root) noexcept
{
    AcquireSRWLockExclusive(&lock_);
    const bool first = phase_ == LuaLifecyclePhase::Bound && observed_root &&
                       observed_root != bound_root_ && !unexpected_root_latched_;
    if (phase_ == LuaLifecyclePhase::Bound && observed_root && observed_root != bound_root_)
    {
        gate_open_ = false;
        unexpected_root_latched_ = true;
    }
    ReleaseSRWLockExclusive(&lock_);
    return first;
}

void LuaLifecycleTracker::CloseGate() noexcept
{
    AcquireSRWLockExclusive(&lock_);
    gate_open_ = false;
    ReleaseSRWLockExclusive(&lock_);
}

bool LuaLifecycleTracker::ReopenGate(void* root, std::uint64_t generation) noexcept
{
    AcquireSRWLockExclusive(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Bound && bound_root_ == root &&
                       generation_ == generation && !unexpected_root_latched_;
    if (valid)
        gate_open_ = true;
    ReleaseSRWLockExclusive(&lock_);
    return valid;
}

bool LuaLifecycleTracker::UnregisterClosingRoot(void* root,
                                                std::uint64_t generation) noexcept
{
    AcquireSRWLockExclusive(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Closing && closing_root_ == root &&
                       closing_generation_ == generation;
    if (valid)
    {
        bound_root_ = nullptr;
        generation_ = 0;
    }
    ReleaseSRWLockExclusive(&lock_);
    return valid;
}

bool LuaLifecycleTracker::CompleteClose(void* root, std::uint64_t generation) noexcept
{
    AcquireSRWLockExclusive(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Closing && closing_root_ == root &&
                       closing_generation_ == generation;
    if (valid)
    {
        phase_ = LuaLifecyclePhase::Unbound;
        bound_root_ = nullptr;
        closing_root_ = nullptr;
        generation_ = 0;
        closing_generation_ = 0;
        gate_open_ = false;
        unexpected_root_latched_ = false;
    }
    ReleaseSRWLockExclusive(&lock_);
    return valid;
}

bool LuaLifecycleTracker::CompleteShutdownCleanup(void* root,
                                                  std::uint64_t generation) noexcept
{
    AcquireSRWLockExclusive(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Bound && bound_root_ == root &&
                       generation_ == generation;
    if (valid)
    {
        phase_ = LuaLifecyclePhase::Unbound;
        bound_root_ = nullptr;
        generation_ = 0;
        gate_open_ = false;
        unexpected_root_latched_ = false;
    }
    ReleaseSRWLockExclusive(&lock_);
    return valid;
}

bool LuaLifecycleTracker::IsBoundCall(void* canonical_root,
                                      std::uint64_t generation) const noexcept
{
    AcquireSRWLockShared(&lock_);
    const bool valid = phase_ == LuaLifecyclePhase::Bound && gate_open_ &&
                       bound_root_ == canonical_root &&
                       (generation == 0 || generation_ == generation);
    ReleaseSRWLockShared(&lock_);
    return valid;
}

LuaLifecycleSnapshot LuaLifecycleTracker::Snapshot() const noexcept
{
    AcquireSRWLockShared(&lock_);
    LuaLifecycleSnapshot snapshot{};
    snapshot.phase = phase_;
    snapshot.root = phase_ == LuaLifecyclePhase::Closing ? closing_root_ : bound_root_;
    snapshot.generation = phase_ == LuaLifecyclePhase::Closing ? closing_generation_ : generation_;
    snapshot.gate_open = gate_open_;
    snapshot.unexpected_root_latched = unexpected_root_latched_;
    ReleaseSRWLockShared(&lock_);
    return snapshot;
}

ManifestStartRequestResult ManifestStartGate::RequestStart(
    std::size_t manifest_count) noexcept
{
    (void)manifest_count;
    if (Frozen())
        return ManifestStartRequestResult::AlreadyFrozen;
    bool expected = false;
    if (!start_requested_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
    {
        return ManifestStartRequestResult::AlreadyRequested;
    }
    return ManifestStartRequestResult::Requested;
}

bool ManifestStartGate::TryFreeze(bool boundary_succeeded) noexcept
{
    if (!boundary_succeeded || !StartRequested())
        return false;
    bool expected = false;
    return frozen_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

EmptyManifestAutoStartResult EmptyManifestAutoStartGate::Observe(
    bool eligible, std::uint64_t now_ms) noexcept
{
    if (!eligible)
    {
        Reset();
        return EmptyManifestAutoStartResult::Ineligible;
    }
    if (!armed_ || now_ms < ready_since_ms_)
    {
        armed_ = true;
        ready_since_ms_ = now_ms;
        return EmptyManifestAutoStartResult::Waiting;
    }
    return now_ms - ready_since_ms_ >= kEmptyManifestAutoStartGraceMs
               ? EmptyManifestAutoStartResult::Ready
               : EmptyManifestAutoStartResult::Waiting;
}

LuaNumericResult ReadLuaNumericPayload(const LuaAdapter& adapter, void* state, int index,
                                       std::uint16_t port, std::uint16_t opcode,
                                       CustomFrameOpV2& output)
{
    constexpr int lua_type_number = 3;
    output = {};
    if (!state || !adapter.type || !adapter.isinteger || !adapter.tointegerx ||
        !adapter.tonumberx || adapter.type(state, index) != lua_type_number)
        return LuaNumericResult::NotNumber;
    if (adapter.isinteger(state, index))
    {
        int valid = 0;
        const std::int64_t payload = adapter.tointegerx(state, index, &valid);
        if (!valid || payload < INT32_MIN || payload > INT32_MAX)
            return LuaNumericResult::IntegerRange;
        output = MakeInt32Operation(port, opcode, static_cast<std::int32_t>(payload));
        return LuaNumericResult::Success;
    }
    int valid = 0;
    const double payload = adapter.tonumberx(state, index, &valid);
    if (!valid || !MakeFloat64Operation(port, opcode, payload, output))
        return LuaNumericResult::FloatNotFinite;
    return LuaNumericResult::Success;
}

bool PushLuaNumericPayload(const LuaAdapter& adapter, void* state,
                           const CustomFrameOpV2& operation)
{
    if (!state) return false;
    std::int32_t integer_payload = 0;
    if (DecodeInt32Payload(operation, integer_payload))
    {
        if (!adapter.pushinteger) return false;
        adapter.pushinteger(state, integer_payload);
        return true;
    }
    double float_payload = 0.0;
    if (DecodeFloat64Payload(operation, float_payload))
    {
        if (!adapter.pushnumber) return false;
        adapter.pushnumber(state, float_payload);
        return true;
    }
    return false;
}

} // namespace isaac_sync::runtime
