#pragma once

#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace isaac_sync {

constexpr std::uint64_t kHandshakeTimeoutMs = 10000;
constexpr std::uint32_t kRosterEndConfirmations = 2;

class RosterEndTracker {
public:
    bool ObserveMissing() {
        if (observations_ < kRosterEndConfirmations) ++observations_;
        return observations_ >= kRosterEndConfirmations;
    }
    void ObserveReady() { observations_ = 0; }
    void Reset() { observations_ = 0; }
    std::uint32_t Observations() const { return observations_; }

private:
    std::uint32_t observations_ = 0;
};

enum class RetiredPacketResult {
    NotRetired,
    Discarded,
    Invalid,
};

class RetiredGroupSessionPacketFilter {
public:
    void Reset();
    void Retire(const SessionId& session,
                const std::array<std::uint64_t, kMaximumPeerCount>& steam_ids,
                std::uint16_t peer_count, std::uint16_t local_slot,
                const std::array<std::uint64_t, kMaximumPeerCount>& next_receive_sequences);
    RetiredPacketResult Consume(const void* packet, std::size_t packet_size,
                                std::uint64_t remote_steam_id,
                                DecodedPacket& decoded, std::uint16_t& sender_slot);
    bool Active() const { return active_; }
    const SessionId& Session() const { return session_; }
    std::uint64_t NextReceiveSequence(std::uint16_t slot) const {
        return slot < kMaximumPeerCount ? next_receive_sequences_[slot] : 0;
    }

private:
    SessionId session_{};
    std::array<std::uint64_t, kMaximumPeerCount> steam_ids_{};
    std::array<std::uint64_t, kMaximumPeerCount> next_receive_sequences_{};
    std::uint16_t peer_count_ = 0;
    std::uint16_t local_slot_ = UINT16_MAX;
    bool active_ = false;
};

struct GroupSessionConfig {
    Hash256 build_hash{};
    Hash256 roster_hash{};
    Hash256 manifest_hash{};
    std::array<std::uint64_t, kMaximumPeerCount> steam_ids{};
    std::array<std::uint16_t, kMaximumPeerCount> player_slots_in_game_order{};
    std::uint16_t peer_count = 0;
    std::uint16_t local_slot = UINT16_MAX;
    std::uint32_t local_online_frame = 0;
    std::uint16_t lead_frames = kLeadFrames;
    std::uint16_t max_batch_records = kMaxBatchRecords;
    std::uint32_t capabilities = kRequiredCapabilities;
    std::uint16_t max_peer_count = kMaximumPeerCount;
    std::uint16_t topology_id = kFullMeshTopologyId;
};

class IGroupTransport {
public:
    virtual ~IGroupTransport() = default;
    virtual bool Send(std::uint16_t recipient_slot, const PacketBuffer& packet) = 0;
};

class IGroupCallbacks {
public:
    virtual ~IGroupCallbacks() = default;
    virtual ErrorCode OnFrameBatch(std::uint16_t sender_slot, bool sender_committed,
                                   const DecodedFrameBatch& batch,
                                   std::uint32_t local_online_frame) = 0;
};

class GroupSessionProtocolEngine {
public:
    GroupSessionProtocolEngine(IGroupTransport& transport, IGroupCallbacks& callbacks);

    bool Start(const GroupSessionConfig& config, const SessionId& coordinator_session,
               std::uint64_t now_ms);
    bool Receive(const void* packet, std::size_t packet_size,
                 std::uint16_t sender_slot, std::uint64_t transport_sender_steam_id,
                 std::uint32_t local_online_frame, std::uint64_t now_ms);
    bool SetActivationRecordsReady(bool all_slots_ready);
    bool SendFrameBatchTo(std::uint16_t recipient_slot, std::uint32_t first_frame,
                          const CustomFrameOpV2* records, std::uint16_t count,
                          bool sender_committed);
    bool EndGracefully();
    bool AbortForLifecycleReset();
    bool Tick(std::uint32_t local_online_frame, std::uint64_t now_ms);
    bool FailExternal(ErrorCode code, const char* text);
    void Reset();

    RuntimeState State() const { return state_; }
    ErrorCode Error() const { return error_; }
    const char* ErrorText() const { return error_text_.c_str(); }
    bool IsCoordinator() const { return config_.local_slot == 0; }
    bool SessionBound() const { return session_bound_; }
    bool Committed() const { return committed_; }
    bool BarrierRequired(std::uint32_t frame) const;
    bool ActivateIfReady(std::uint32_t frame);
    std::uint32_t ActivationFrame() const { return activation_frame_; }
    std::uint32_t ActivationReadyMask() const { return activation_ready_mask_; }
    std::uint32_t FrozenSlotMask() const;
    const SessionId& Session() const { return session_; }
    const GroupSessionConfig& Config() const { return config_; }
    std::uint64_t NextSendSequence(std::uint16_t slot) const;
    std::uint64_t NextReceiveSequence(std::uint16_t slot) const;
    std::uint64_t SentPackets(std::uint16_t slot) const;
    std::uint64_t ReceivedPackets(std::uint16_t slot) const;

private:
    bool ValidateConfig(const GroupSessionConfig& config) const;
    bool SendHello(MessageKind kind, std::uint16_t recipient_slot,
                   std::uint32_t online_frame);
    bool SendActivate(MessageKind kind, std::uint16_t recipient_slot);
    bool SendControl(MessageKind kind, std::uint16_t recipient_slot,
                     const void* payload, std::uint16_t payload_size);
    bool SendAbort();
    bool SendGoodbye();
    bool ValidateHello(const HelloPayloadV1& payload) const;
    bool ValidateActivate(const ActivatePayloadV1& payload) const;
    bool ComputeHandshakeHash(std::uint32_t activation_frame, Hash256& output) const;
    bool TryAdvanceCommit();
    bool Fail(ErrorCode code, const char* text, bool notify_group = true);
    void EnterWarmup(std::uint64_t now_ms);
    void CommitReceive(std::uint16_t sender_slot);

    IGroupTransport& transport_;
    IGroupCallbacks& callbacks_;
    GroupSessionConfig config_{};
    SessionId session_{};
    RuntimeState state_ = RuntimeState::Dormant;
    ErrorCode error_ = ErrorCode::None;
    std::string error_text_;
    bool session_bound_ = false;
    bool committed_ = false;
    bool activation_ready_sent_ = false;
    bool abort_sent_ = false;
    bool goodbye_sent_ = false;
    std::uint32_t hello_ack_mask_ = 0;
    std::uint32_t activation_ready_mask_ = 0;
    std::array<std::uint32_t, kMaximumPeerCount> hello_frames_{};
    std::uint32_t activation_frame_ = 0;
    std::uint64_t state_enter_ms_ = 0;
    std::array<std::uint64_t, kMaximumPeerCount> next_send_sequences_{};
    std::array<std::uint64_t, kMaximumPeerCount> next_receive_sequences_{};
    std::array<std::uint64_t, kMaximumPeerCount> sent_packets_{};
    std::array<std::uint64_t, kMaximumPeerCount> received_packets_{};
};

} // namespace isaac_sync
