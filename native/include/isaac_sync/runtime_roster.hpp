#pragma once

#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace isaac_sync::runtime {

enum class LocalSteamIdResult {
    Waiting,
    Ready,
    MissingExports,
};

enum class RosterReadResult {
    Waiting,
    FewerThanTwo,
    Ready,
    TooMany,
    Invalid,
    LocalMismatch,
};

struct GroupRoster {
    std::array<std::uint64_t, kMaximumPeerCount> steam_ids{};
    std::array<std::uint16_t, kMaximumPeerCount> player_slots_in_game_order{};
    std::array<std::uint16_t, kMaximumPeerCount> player_index_by_slot{};
    Hash256 roster_hash{};
    std::uint16_t peer_count = 0;
    std::uint16_t local_slot = UINT16_MAX;
    std::uint32_t frozen_slot_mask = 0;

    bool operator==(const GroupRoster& other) const;
    bool operator!=(const GroupRoster& other) const { return !(*this == other); }
};

enum class PlayerIndexQueryResult {
    Unavailable,
    Ready,
    OutOfRange,
};

enum class RosterStabilityResult {
    Waiting,
    Frozen,
    Stable,
    Changed,
};

class GroupRosterStabilityTracker {
public:
    RosterStabilityResult Observe(const GroupRoster& roster, bool already_frozen);
    void Reset();
    const GroupRoster& Frozen() const { return frozen_; }

private:
    GroupRoster candidate_{};
    GroupRoster frozen_{};
    std::uint32_t matching_observations_ = 0;
    std::uint32_t changed_observations_ = 0;
    bool has_candidate_ = false;
    bool has_frozen_ = false;
};

LocalSteamIdResult ResolveLocalSteamId(std::uint64_t& steam_id);
RosterReadResult CanonicalizeGroupRoster(const std::uint64_t* ids, std::size_t count,
                                         std::uint64_t local_id, GroupRoster& roster);
RosterReadResult ReadGroupSteamRoster(void* sync_manager, std::uint64_t local_id,
                                      GroupRoster& roster);
PlayerIndexQueryResult QueryPlayerIndexForSlot(const GroupRoster& roster,
                                               std::int64_t sender_slot,
                                               std::uint16_t& player_index);

} // namespace isaac_sync::runtime
