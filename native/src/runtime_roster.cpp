#include "isaac_sync/runtime_roster.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace isaac_sync::runtime {
namespace {

using SteamGetHUserFn = std::int32_t(__cdecl*)();
using SteamFindUserInterfaceFn = void*(__cdecl*)(std::int32_t, const char*);
using SteamGetIdFn = std::uint64_t(__cdecl*)(void*);

} // namespace

bool GroupRoster::operator==(const GroupRoster& other) const
{
    if (peer_count != other.peer_count || local_slot != other.local_slot ||
        frozen_slot_mask != other.frozen_slot_mask)
        return false;
    for (std::uint16_t slot = 0; slot < peer_count; ++slot)
    {
        if (steam_ids[slot] != other.steam_ids[slot] ||
            player_slots_in_game_order[slot] != other.player_slots_in_game_order[slot] ||
            player_index_by_slot[slot] != other.player_index_by_slot[slot])
            return false;
    }
    return true;
}

void GroupRosterStabilityTracker::Reset()
{
    candidate_ = {};
    frozen_ = {};
    matching_observations_ = 0;
    changed_observations_ = 0;
    has_candidate_ = false;
    has_frozen_ = false;
}

RosterStabilityResult GroupRosterStabilityTracker::Observe(const GroupRoster& roster,
                                                           bool already_frozen)
{
    if (already_frozen || has_frozen_)
    {
        if (!has_frozen_)
        {
            frozen_ = roster;
            has_frozen_ = true;
            changed_observations_ = 0;
            return RosterStabilityResult::Stable;
        }
        if (roster == frozen_)
        {
            changed_observations_ = 0;
            return RosterStabilityResult::Stable;
        }
        if (changed_observations_ < 2)
            ++changed_observations_;
        return changed_observations_ >= 2 ? RosterStabilityResult::Changed
                                          : RosterStabilityResult::Waiting;
    }

    if (!has_candidate_ || roster != candidate_)
    {
        candidate_ = roster;
        matching_observations_ = 1;
        has_candidate_ = true;
        return RosterStabilityResult::Waiting;
    }
    if (matching_observations_ < 2)
        ++matching_observations_;
    if (matching_observations_ < 2)
        return RosterStabilityResult::Waiting;
    frozen_ = candidate_;
    has_frozen_ = true;
    return RosterStabilityResult::Frozen;
}

LocalSteamIdResult ResolveLocalSteamId(std::uint64_t& steam_id)
{
    steam_id = 0;
    HMODULE steam_api = GetModuleHandleW(L"steam_api.dll");
    if (!steam_api)
        return LocalSteamIdResult::Waiting;
    const auto get_h_user =
        reinterpret_cast<SteamGetHUserFn>(GetProcAddress(steam_api, "SteamAPI_GetHSteamUser"));
    const auto find_interface = reinterpret_cast<SteamFindUserInterfaceFn>(
        GetProcAddress(steam_api, "SteamInternal_FindOrCreateUserInterface"));
    const auto get_id = reinterpret_cast<SteamGetIdFn>(
        GetProcAddress(steam_api, "SteamAPI_ISteamUser_GetSteamID"));
    if (!get_h_user || !find_interface || !get_id)
        return LocalSteamIdResult::MissingExports;
    static constexpr const char* versions[] = {
        "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020", "SteamUser019",
    };
    const std::int32_t user = get_h_user();
    if (!user)
        return LocalSteamIdResult::Waiting;
    for (const char* version : versions)
    {
        void* interface_pointer = find_interface(user, version);
        if (!interface_pointer)
            continue;
        steam_id = get_id(interface_pointer);
        if (steam_id)
            return LocalSteamIdResult::Ready;
    }
    return LocalSteamIdResult::Waiting;
}

RosterReadResult CanonicalizeGroupRoster(const std::uint64_t* ids, std::size_t count,
                                         std::uint64_t local_id, GroupRoster& roster)
{
    roster = {};
    if (count < 2)
        return RosterReadResult::FewerThanTwo;
    if (count > kMaximumPeerCount)
        return RosterReadResult::TooMany;
    if (!ids || !local_id)
        return RosterReadResult::Invalid;
    std::array<std::uint64_t, kMaximumPeerCount> raw_ids{};
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!ids[index])
            return RosterReadResult::Invalid;
        raw_ids[index] = ids[index];
        roster.steam_ids[index] = ids[index];
    }
    std::sort(roster.steam_ids.begin(), roster.steam_ids.begin() + count);
    std::uint16_t local_count = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0 && roster.steam_ids[index] == roster.steam_ids[index - 1])
            return RosterReadResult::Invalid;
        if (roster.steam_ids[index] == local_id)
        {
            roster.local_slot = static_cast<std::uint16_t>(index);
            ++local_count;
        }
    }
    if (local_count != 1)
        return RosterReadResult::LocalMismatch;
    roster.player_slots_in_game_order.fill(UINT16_MAX);
    roster.player_index_by_slot.fill(UINT16_MAX);
    std::uint32_t player_slot_mask = 0;
    for (std::size_t player_index = 0; player_index < count; ++player_index)
    {
        const auto found = std::lower_bound(roster.steam_ids.begin(),
                                            roster.steam_ids.begin() + count,
                                            raw_ids[player_index]);
        if (found == roster.steam_ids.begin() + count || *found != raw_ids[player_index])
            return RosterReadResult::Invalid;
        const auto slot = static_cast<std::uint16_t>(found - roster.steam_ids.begin());
        if (slot >= count || (player_slot_mask & (1u << slot)) != 0 ||
            roster.player_index_by_slot[slot] != UINT16_MAX)
            return RosterReadResult::Invalid;
        roster.player_slots_in_game_order[player_index] = slot;
        roster.player_index_by_slot[slot] = static_cast<std::uint16_t>(player_index);
        player_slot_mask |= 1u << slot;
    }
    if (player_slot_mask != (1u << count) - 1u)
        return RosterReadResult::Invalid;
    roster.peer_count = static_cast<std::uint16_t>(count);
    roster.frozen_slot_mask = (1u << roster.peer_count) - 1u;
    if (!ComputeRosterHash(roster.steam_ids.data(), roster.player_slots_in_game_order.data(),
                           roster.peer_count, roster.roster_hash))
        return RosterReadResult::Invalid;
    return RosterReadResult::Ready;
}

RosterReadResult ReadGroupSteamRoster(void* sync_manager, std::uint64_t local_id,
                                      GroupRoster& roster)
{
    roster = {};
    if (!sync_manager)
        return RosterReadResult::Waiting;
    __try
    {
        auto** first = *reinterpret_cast<void***>(sync_manager);
        auto** last = *reinterpret_cast<void***>(static_cast<std::uint8_t*>(sync_manager) + 4);
        auto** capacity = *reinterpret_cast<void***>(static_cast<std::uint8_t*>(sync_manager) + 8);
        if (!first && !last)
            return RosterReadResult::FewerThanTwo;
        if (!first || !last || last < first || capacity < last)
            return RosterReadResult::Waiting;
        const std::ptrdiff_t count = last - first;
        if (count < 2)
            return RosterReadResult::FewerThanTwo;
        if (count > kMaximumPeerCount)
            return RosterReadResult::TooMany;
        std::array<std::uint64_t, kMaximumPeerCount> ids{};
        for (std::ptrdiff_t index = 0; index < count; ++index)
        {
            auto* device = static_cast<std::uint8_t*>(first[index]);
            auto* identity = device ? *reinterpret_cast<std::uint8_t**>(device + 0x374) : nullptr;
            if (!identity)
                return RosterReadResult::Waiting;
            ids[static_cast<std::size_t>(index)] =
                *reinterpret_cast<std::uint64_t*>(identity + 8);
        }
        return CanonicalizeGroupRoster(ids.data(), static_cast<std::size_t>(count),
                                       local_id, roster);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return RosterReadResult::Waiting;
    }
}

PlayerIndexQueryResult QueryPlayerIndexForSlot(const GroupRoster& roster,
                                               std::int64_t sender_slot,
                                               std::uint16_t& player_index)
{
    player_index = UINT16_MAX;
    if (roster.peer_count < 2 || roster.peer_count > kMaximumPeerCount)
        return PlayerIndexQueryResult::Unavailable;
    if (sender_slot < 0 || sender_slot >= roster.peer_count)
        return PlayerIndexQueryResult::OutOfRange;
    const auto index = roster.player_index_by_slot[static_cast<std::size_t>(sender_slot)];
    if (index >= roster.peer_count)
        return PlayerIndexQueryResult::Unavailable;
    player_index = index;
    return PlayerIndexQueryResult::Ready;
}

} // namespace isaac_sync::runtime
