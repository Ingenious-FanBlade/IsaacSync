#include "isaac_sync/steam_transport.hpp"

#include <array>

namespace isaac_sync {

bool SteamSidecarMeshTransport::Send(std::uint16_t recipient_slot,
                                     const PacketBuffer& packet) {
    return recipient_slot < peer_count_ && recipient_slot != local_slot_ &&
           steam_ids_[recipient_slot] != 0 &&
           packet.size >= sizeof(SidecarHeaderV1) && packet.size <= kPacketBufferSize &&
           api_.Send(steam_ids_[recipient_slot], packet.bytes.data(), packet.size,
                     kSteamReliableSendMode, kCustomChannel);
}

std::uint16_t SteamSidecarMeshTransport::FindCurrentSlot(std::uint64_t steam_id) const {
    for (std::uint16_t slot = 0; slot < peer_count_; ++slot) {
        if (slot != local_slot_ && steam_ids_[slot] == steam_id) return slot;
    }
    return UINT16_MAX;
}

void SteamSidecarMeshTransport::SetRetiredRoster(
        const std::array<std::uint64_t, kMaximumPeerCount>& steam_ids,
        std::uint16_t peer_count, std::uint16_t local_slot) {
    retired_steam_ids_ = steam_ids;
    retired_peer_count_ = peer_count <= kMaximumPeerCount ? peer_count : 0;
    retired_local_slot_ = local_slot < retired_peer_count_ ? local_slot : UINT16_MAX;
}

bool SteamSidecarMeshTransport::IsRetiredSender(std::uint64_t steam_id) const {
    for (std::uint16_t slot = 0; slot < retired_peer_count_; ++slot) {
        if (slot != retired_local_slot_ && retired_steam_ids_[slot] == steam_id) return true;
    }
    return false;
}

SteamDrainResult SteamSidecarMeshTransport::Drain(ISteamPacketConsumer& consumer,
                                                   std::uint32_t maximum_packets,
                                                   std::uint32_t* drained_packets) {
    if (drained_packets) *drained_packets = 0;
    std::array<std::uint8_t, kPacketBufferSize> buffer{};
    for (std::uint32_t drained = 0; drained < maximum_packets; ++drained) {
        std::uint32_t available_size = 0;
        if (!api_.Available(&available_size, kCustomChannel)) return SteamDrainResult::None;
        if (available_size < sizeof(SidecarHeaderV1) || available_size > buffer.size())
            return SteamDrainResult::InvalidPacketSize;
        std::uint32_t received_size = 0;
        std::uint64_t remote = 0;
        if (!api_.Read(buffer.data(), static_cast<std::uint32_t>(buffer.size()),
                       &received_size, &remote, kCustomChannel) ||
            received_size != available_size)
            return SteamDrainResult::ReadFailure;
        if (FindCurrentSlot(remote) == UINT16_MAX && !IsRetiredSender(remote))
            return SteamDrainResult::WrongPeer;
        if (!consumer.Consume(buffer.data(), received_size, remote))
            return SteamDrainResult::ConsumerRejected;
        if (drained_packets) *drained_packets = drained + 1;
    }
    return SteamDrainResult::None;
}

} // namespace isaac_sync
