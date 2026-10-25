#pragma once

#include "session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace isaac_sync {

constexpr int kSteamReliableSendMode = 2;
constexpr std::uint32_t kSteamDrainLimit = 256;

class ISteamNetworking006Api {
public:
    virtual ~ISteamNetworking006Api() = default;
    virtual bool Send(std::uint64_t remote_steam_id, const void* data,
                      std::uint32_t size, int send_mode, int channel) = 0;
    virtual bool Available(std::uint32_t* size, int channel) = 0;
    virtual bool Read(void* buffer, std::uint32_t capacity, std::uint32_t* size,
                      std::uint64_t* remote_steam_id, int channel) = 0;
};

class ISteamPacketConsumer {
public:
    virtual ~ISteamPacketConsumer() = default;
    virtual bool Consume(const std::uint8_t* packet, std::uint32_t size,
                         std::uint64_t remote_steam_id) = 0;
};

enum class SteamDrainResult {
    None,
    InvalidPacketSize,
    ReadFailure,
    WrongPeer,
    ConsumerRejected,
};

class SteamSidecarMeshTransport final : public IGroupTransport {
public:
    SteamSidecarMeshTransport(
        ISteamNetworking006Api& api,
        const std::array<std::uint64_t, kMaximumPeerCount>& steam_ids,
        std::uint16_t peer_count, std::uint16_t local_slot)
        : api_(api), steam_ids_(steam_ids), peer_count_(peer_count),
          local_slot_(local_slot) {}

    bool Send(std::uint16_t recipient_slot, const PacketBuffer& packet) override;
    SteamDrainResult Drain(ISteamPacketConsumer& consumer,
                           std::uint32_t maximum_packets = kSteamDrainLimit,
                           std::uint32_t* drained_packets = nullptr);
    std::uint16_t FindCurrentSlot(std::uint64_t steam_id) const;
    void SetRetiredRoster(
        const std::array<std::uint64_t, kMaximumPeerCount>& steam_ids,
        std::uint16_t peer_count, std::uint16_t local_slot);

private:
    bool IsRetiredSender(std::uint64_t steam_id) const;

    ISteamNetworking006Api& api_;
    std::array<std::uint64_t, kMaximumPeerCount> steam_ids_{};
    std::array<std::uint64_t, kMaximumPeerCount> retired_steam_ids_{};
    std::uint16_t peer_count_ = 0;
    std::uint16_t local_slot_ = UINT16_MAX;
    std::uint16_t retired_peer_count_ = 0;
    std::uint16_t retired_local_slot_ = UINT16_MAX;
};

} // namespace isaac_sync
