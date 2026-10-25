#include "isaac_sync/session.hpp"
#include "isaac_sync/version.hpp"

#include "isaac_sync/frame_store.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace isaac_sync {
namespace {

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool IsNonzeroSession(const SessionId& session) {
    for (const auto byte : session) {
        if (byte != 0) return true;
    }
    return false;
}

bool HasNonEmptyRecord(const DecodedFrameBatch& batch) {
    for (std::uint16_t index = 0; index < batch.count; ++index) {
        if (!IsNoOp(batch.records[index])) return true;
    }
    return false;
}

ErrorCode PacketErrorCode(PacketError error) {
    switch (error) {
    case PacketError::Size:
    case PacketError::PayloadSize: return ErrorCode::PacketSize;
    case PacketError::Session: return ErrorCode::PacketSession;
    case PacketError::Sender: return ErrorCode::PacketSender;
    case PacketError::Sequence: return ErrorCode::PacketSequence;
    case PacketError::Crc: return ErrorCode::PacketCrc;
    default: return ErrorCode::PacketHeader;
    }
}

} // namespace

void RetiredGroupSessionPacketFilter::Reset() {
    session_.fill(0);
    steam_ids_.fill(0);
    next_receive_sequences_.fill(0);
    peer_count_ = 0;
    local_slot_ = UINT16_MAX;
    active_ = false;
}

void RetiredGroupSessionPacketFilter::Retire(
        const SessionId& session,
        const std::array<std::uint64_t, kMaximumPeerCount>& steam_ids,
        std::uint16_t peer_count, std::uint16_t local_slot,
        const std::array<std::uint64_t, kMaximumPeerCount>& next_receive_sequences) {
    Reset();
    if (!IsNonzeroSession(session) || peer_count < 2 || peer_count > kMaximumPeerCount ||
        local_slot >= peer_count) return;
    session_ = session;
    steam_ids_ = steam_ids;
    next_receive_sequences_ = next_receive_sequences;
    peer_count_ = peer_count;
    local_slot_ = local_slot;
    active_ = true;
}

RetiredPacketResult RetiredGroupSessionPacketFilter::Consume(
        const void* packet, std::size_t packet_size, std::uint64_t remote_steam_id,
        DecodedPacket& decoded, std::uint16_t& sender_slot) {
    decoded = {};
    sender_slot = UINT16_MAX;
    if (!active_ || !remote_steam_id || !packet || packet_size < sizeof(SidecarHeaderV1))
        return RetiredPacketResult::NotRetired;
    for (std::uint16_t slot = 0; slot < peer_count_; ++slot) {
        if (slot != local_slot_ && steam_ids_[slot] == remote_steam_id) {
            sender_slot = slot;
            break;
        }
    }
    if (sender_slot == UINT16_MAX) return RetiredPacketResult::NotRetired;
    SidecarHeaderV1 header{};
    std::memcpy(&header, packet, sizeof(header));
    if (std::memcmp(header.session_id, session_.data(), session_.size()) != 0)
        return RetiredPacketResult::NotRetired;
    const PacketExpectations expected{&session_, remote_steam_id,
                                      next_receive_sequences_[sender_slot], false};
    if (DecodePacket(packet, packet_size, expected, decoded) != PacketError::None)
        return RetiredPacketResult::Invalid;
    if (next_receive_sequences_[sender_slot] != UINT64_MAX)
        ++next_receive_sequences_[sender_slot];
    return RetiredPacketResult::Discarded;
}

GroupSessionProtocolEngine::GroupSessionProtocolEngine(IGroupTransport& transport,
                                                       IGroupCallbacks& callbacks)
    : transport_(transport), callbacks_(callbacks) {}

void GroupSessionProtocolEngine::Reset() {
    config_ = {};
    session_.fill(0);
    state_ = RuntimeState::Dormant;
    error_ = ErrorCode::None;
    error_text_.clear();
    session_bound_ = false;
    committed_ = false;
    activation_ready_sent_ = false;
    abort_sent_ = false;
    goodbye_sent_ = false;
    hello_ack_mask_ = 0;
    activation_ready_mask_ = 0;
    hello_frames_.fill(0);
    activation_frame_ = 0;
    state_enter_ms_ = 0;
    next_send_sequences_.fill(1);
    next_receive_sequences_.fill(1);
    sent_packets_.fill(0);
    received_packets_.fill(0);
}

bool GroupSessionProtocolEngine::ValidateConfig(const GroupSessionConfig& config) const {
    if (config.peer_count < 2 || config.peer_count > kMaximumPeerCount ||
        config.local_slot >= config.peer_count || config.lead_frames != kLeadFrames ||
        config.max_batch_records != kMaxBatchRecords ||
        config.capabilities != kRequiredCapabilities ||
        config.max_peer_count != kMaximumPeerCount ||
        config.topology_id != kFullMeshTopologyId)
        return false;
    std::uint64_t previous = 0;
    for (std::uint16_t slot = 0; slot < config.peer_count; ++slot) {
        if (!config.steam_ids[slot] || (slot != 0 && config.steam_ids[slot] <= previous))
            return false;
        previous = config.steam_ids[slot];
    }
    Hash256 roster_hash{};
    return ComputeRosterHash(config.steam_ids.data(),
                             config.player_slots_in_game_order.data(),
                             config.peer_count, roster_hash) &&
           roster_hash == config.roster_hash;
}

bool GroupSessionProtocolEngine::Start(const GroupSessionConfig& config,
                                       const SessionId& coordinator_session,
                                       std::uint64_t now_ms) {
    Reset();
    if (!ValidateConfig(config))
        return Fail(ErrorCode::GroupPeerCountMismatch,
                    "invalid local group configuration", false);
    config_ = config;
    hello_frames_[config_.local_slot] = config_.local_online_frame;
    state_ = RuntimeState::Discovering;
    state_enter_ms_ = now_ms;
    if (config_.local_slot != 0) return true;
    if (!IsNonzeroSession(coordinator_session))
        return Fail(ErrorCode::GroupHandshakeMismatch,
                    "coordinator requires a nonzero session", false);
    session_ = coordinator_session;
    session_bound_ = true;
    for (std::uint16_t slot = 1; slot < config_.peer_count; ++slot) {
        if (!SendHello(MessageKind::Hello, slot, config_.local_online_frame)) return false;
    }
    state_ = RuntimeState::Hello;
    return true;
}

std::uint32_t GroupSessionProtocolEngine::FrozenSlotMask() const {
    return config_.peer_count >= 2 && config_.peer_count <= kMaximumPeerCount
               ? (1u << config_.peer_count) - 1u : 0;
}

std::uint64_t GroupSessionProtocolEngine::NextSendSequence(std::uint16_t slot) const {
    return slot < kMaximumPeerCount ? next_send_sequences_[slot] : 0;
}

std::uint64_t GroupSessionProtocolEngine::NextReceiveSequence(std::uint16_t slot) const {
    return slot < kMaximumPeerCount ? next_receive_sequences_[slot] : 0;
}

std::uint64_t GroupSessionProtocolEngine::SentPackets(std::uint16_t slot) const {
    return slot < kMaximumPeerCount ? sent_packets_[slot] : 0;
}

std::uint64_t GroupSessionProtocolEngine::ReceivedPackets(std::uint16_t slot) const {
    return slot < kMaximumPeerCount ? received_packets_[slot] : 0;
}

bool GroupSessionProtocolEngine::Fail(ErrorCode code, const char* text, bool notify_group) {
    const bool fatal = state_ == RuntimeState::Active || state_ == RuntimeState::FailedFatal;
    if (notify_group && session_bound_ && !abort_sent_) SendAbort();
    state_ = fatal ? RuntimeState::FailedFatal : RuntimeState::FailedPreActive;
    error_ = code;
    error_text_ = text ? text : "group session failed";
    return false;
}

bool GroupSessionProtocolEngine::SendControl(MessageKind kind, std::uint16_t recipient_slot,
                                             const void* payload,
                                             std::uint16_t payload_size) {
    if (recipient_slot >= config_.peer_count || recipient_slot == config_.local_slot)
        return false;
    PacketBuffer packet{};
    if (!EncodePacket(kind, session_, config_.steam_ids[config_.local_slot],
                      next_send_sequences_[recipient_slot], payload, payload_size, 0, packet) ||
        !transport_.Send(recipient_slot, packet))
        return false;
    ++next_send_sequences_[recipient_slot];
    ++sent_packets_[recipient_slot];
    return true;
}

bool GroupSessionProtocolEngine::SendAbort() {
    if (abort_sent_ || !session_bound_) return true;
    abort_sent_ = true;
    bool all_sent = true;
    for (std::uint16_t slot = 0; slot < config_.peer_count; ++slot) {
        if (slot != config_.local_slot &&
            !SendControl(MessageKind::Abort, slot, nullptr, 0))
            all_sent = false;
    }
    return all_sent;
}

bool GroupSessionProtocolEngine::SendGoodbye() {
    if (goodbye_sent_ || !session_bound_) return true;
    goodbye_sent_ = true;
    bool all_sent = true;
    for (std::uint16_t slot = 0; slot < config_.peer_count; ++slot) {
        if (slot != config_.local_slot &&
            !SendControl(MessageKind::Goodbye, slot, nullptr, 0))
            all_sent = false;
    }
    return all_sent;
}

bool GroupSessionProtocolEngine::EndGracefully() {
    if (state_ == RuntimeState::Stopping || state_ == RuntimeState::Stopped ||
        state_ == RuntimeState::Dormant) return true;
    if (state_ == RuntimeState::FailedFatal) return false;
    const bool sent = !session_bound_ || SendGoodbye();
    state_ = RuntimeState::Stopping;
    error_ = ErrorCode::None;
    error_text_ = "group sidecar session ended normally";
    return sent;
}

bool GroupSessionProtocolEngine::AbortForLifecycleReset() {
    return !session_bound_ || SendAbort();
}

bool GroupSessionProtocolEngine::SendHello(MessageKind kind,
                                           std::uint16_t recipient_slot,
                                           std::uint32_t online_frame) {
    HelloPayloadV1 payload{};
    std::memcpy(payload.build_hash, config_.build_hash.data(), config_.build_hash.size());
    std::memcpy(payload.roster_hash, config_.roster_hash.data(), config_.roster_hash.size());
    std::memcpy(payload.manifest_hash, config_.manifest_hash.data(), config_.manifest_hash.size());
    payload.online_frame = online_frame;
    payload.lead_frames = config_.lead_frames;
    payload.max_batch_records = config_.max_batch_records;
    payload.capabilities = config_.capabilities;
    if (!SendControl(kind, recipient_slot, &payload, sizeof(payload)))
        return Fail(ErrorCode::TransportFailure, "failed to send group HELLO", false);
    return true;
}

bool GroupSessionProtocolEngine::ComputeHandshakeHash(std::uint32_t activation_frame,
                                                      Hash256& output) const {
    if (!activation_frame || !session_bound_) return false;
    std::vector<std::uint8_t> bytes;
    static constexpr char domain[] =
        "IsaacSync/" ISAAC_SYNC_PRODUCT_VERSION "/handshake-v5";
    bytes.insert(bytes.end(), domain, domain + sizeof(domain) - 1);
    AppendU16(bytes, kProtocolVersion);
    bytes.insert(bytes.end(), session_.begin(), session_.end());
    AppendU16(bytes, config_.peer_count);
    for (std::uint16_t slot = 0; slot < config_.peer_count; ++slot)
        AppendU64(bytes, config_.steam_ids[slot]);
    for (std::uint16_t player_index = 0; player_index < config_.peer_count; ++player_index)
        AppendU16(bytes, config_.player_slots_in_game_order[player_index]);
    bytes.insert(bytes.end(), config_.build_hash.begin(), config_.build_hash.end());
    bytes.insert(bytes.end(), config_.manifest_hash.begin(), config_.manifest_hash.end());
    AppendU32(bytes, activation_frame);
    AppendU16(bytes, config_.lead_frames);
    AppendU16(bytes, config_.max_batch_records);
    AppendU32(bytes, config_.capabilities);
    AppendU16(bytes, config_.max_peer_count);
    AppendU16(bytes, config_.topology_id);
    return Sha256(bytes.data(), bytes.size(), output);
}

bool GroupSessionProtocolEngine::SendActivate(MessageKind kind,
                                              std::uint16_t recipient_slot) {
    ActivatePayloadV1 payload{};
    payload.activation_frame = activation_frame_;
    payload.lead_frames = config_.lead_frames;
    Hash256 hash{};
    if (!ComputeHandshakeHash(activation_frame_, hash))
        return Fail(ErrorCode::GroupHandshakeMismatch,
                    "group handshake hash calculation failed", false);
    std::memcpy(payload.handshake_hash, hash.data(), hash.size());
    if (!SendControl(kind, recipient_slot, &payload, sizeof(payload)))
        return Fail(ErrorCode::TransportFailure,
                    "failed to send group activation control", false);
    return true;
}

bool GroupSessionProtocolEngine::ValidateHello(const HelloPayloadV1& payload) const {
    return std::memcmp(payload.build_hash, config_.build_hash.data(), config_.build_hash.size()) == 0 &&
           std::memcmp(payload.roster_hash, config_.roster_hash.data(), config_.roster_hash.size()) == 0 &&
           std::memcmp(payload.manifest_hash, config_.manifest_hash.data(), config_.manifest_hash.size()) == 0 &&
           payload.lead_frames == config_.lead_frames &&
           payload.max_batch_records == config_.max_batch_records &&
           payload.capabilities == kRequiredCapabilities;
}

bool GroupSessionProtocolEngine::ValidateActivate(const ActivatePayloadV1& payload) const {
    Hash256 expected{};
    return payload.activation_frame == activation_frame_ &&
           payload.lead_frames == config_.lead_frames && payload.reserved == 0 &&
           ComputeHandshakeHash(activation_frame_, expected) &&
           std::memcmp(payload.handshake_hash, expected.data(), expected.size()) == 0;
}

void GroupSessionProtocolEngine::EnterWarmup(std::uint64_t now_ms) {
    state_ = RuntimeState::Warmup;
    state_enter_ms_ = now_ms;
}

void GroupSessionProtocolEngine::CommitReceive(std::uint16_t sender_slot) {
    ++next_receive_sequences_[sender_slot];
    ++received_packets_[sender_slot];
}

bool GroupSessionProtocolEngine::Receive(const void* packet, std::size_t packet_size,
                                         std::uint16_t sender_slot,
                                         std::uint64_t transport_sender_steam_id,
                                         std::uint32_t local_online_frame,
                                         std::uint64_t now_ms) {
    if (state_ == RuntimeState::Dormant || state_ == RuntimeState::FailedPreActive ||
        state_ == RuntimeState::FailedFatal || sender_slot >= config_.peer_count ||
        sender_slot == config_.local_slot ||
        config_.steam_ids[sender_slot] != transport_sender_steam_id)
        return Fail(ErrorCode::PacketSender, "packet sender is outside the live group", false);

    PacketExpectations expectations{};
    expectations.session = session_bound_ ? &session_ : nullptr;
    expectations.sender_steam_id = transport_sender_steam_id;
    expectations.next_sequence = next_receive_sequences_[sender_slot];
    expectations.allow_unbound_hello = !session_bound_ && config_.local_slot != 0 &&
                                        sender_slot == 0 &&
                                        state_ == RuntimeState::Discovering;
    DecodedPacket decoded{};
    const PacketError packet_error = DecodePacket(packet, packet_size, expectations, decoded);
    if (packet_error != PacketError::None)
        return Fail(PacketErrorCode(packet_error), "group packet validation failed");
    const auto kind = static_cast<MessageKind>(decoded.header.kind);

    if (kind == MessageKind::Abort) {
        if (decoded.payload_size != 0)
            return Fail(ErrorCode::PacketSize, "ABORT payload must be empty", false);
        CommitReceive(sender_slot);
        return Fail(state_ == RuntimeState::Active ? ErrorCode::FrameStateViolation
                                                   : ErrorCode::HandshakeState,
                    "group peer aborted the sidecar session", false);
    }
    if (kind == MessageKind::Goodbye) {
        if (decoded.payload_size != 0)
            return Fail(ErrorCode::PacketSize, "GOODBYE payload must be empty", false);
        CommitReceive(sender_slot);
        state_ = RuntimeState::Stopping;
        error_ = ErrorCode::None;
        error_text_ = "group peer ended the sidecar session";
        return true;
    }
    if (kind == MessageKind::FrameBatch) {
        if (!session_bound_ || (state_ != RuntimeState::Hello &&
            state_ != RuntimeState::Warmup && state_ != RuntimeState::Active))
            return Fail(ErrorCode::HandshakeState,
                        "FRAME_BATCH arrived before the group session was bound");
        DecodedFrameBatch batch{};
        const FrameBatchError batch_error = DecodeFrameBatch(decoded, batch);
        if (batch_error != FrameBatchError::None)
            return Fail(batch_error == FrameBatchError::Overflow
                            ? ErrorCode::FrameWindow
                            : batch_error == FrameBatchError::Payload
                                ? ErrorCode::FramePayloadInvalid
                                : ErrorCode::FrameBatchSize,
                        "FRAME_BATCH payload validation failed");
        const bool sender_committed =
            (decoded.header.flags & kPacketFlagSenderCommitted) != 0;
        const bool non_empty = HasNonEmptyRecord(batch);
        if ((state_ == RuntimeState::Hello && non_empty) ||
            (state_ == RuntimeState::Warmup && non_empty && !sender_committed) ||
            (state_ == RuntimeState::Active && !sender_committed))
            return Fail(ErrorCode::GroupActivationMismatch,
                        "FRAME_BATCH committed flag violated the activation state");
        const ErrorCode callback_error = callbacks_.OnFrameBatch(
            sender_slot, sender_committed, batch, local_online_frame);
        if (callback_error != ErrorCode::None)
            return Fail(callback_error, "FRAME_BATCH was rejected by its sender store");
        CommitReceive(sender_slot);
        return true;
    }

    if (kind == MessageKind::Hello && config_.local_slot != 0 && sender_slot == 0 &&
        state_ == RuntimeState::Discovering) {
        if (decoded.payload_size != sizeof(HelloPayloadV1))
            return Fail(ErrorCode::PacketSize, "HELLO payload size mismatch");
        HelloPayloadV1 payload{};
        std::memcpy(&payload, decoded.payload, sizeof(payload));
        if (!ValidateHello(payload))
            return Fail(ErrorCode::GroupHandshakeMismatch, "group HELLO mismatch", false);
        SessionId received_session{};
        std::memcpy(received_session.data(), decoded.header.session_id,
                    received_session.size());
        if (!IsNonzeroSession(received_session))
            return Fail(ErrorCode::PacketSession, "unbound HELLO used a zero session ID", false);
        session_ = received_session;
        session_bound_ = true;
        hello_frames_[0] = payload.online_frame;
        CommitReceive(sender_slot);
        if (!SendHello(MessageKind::HelloAck, 0, local_online_frame)) return false;
        state_ = RuntimeState::Hello;
        state_enter_ms_ = now_ms;
        return true;
    }

    if (kind == MessageKind::HelloAck && config_.local_slot == 0 &&
        state_ == RuntimeState::Hello) {
        if (decoded.payload_size != sizeof(HelloPayloadV1))
            return Fail(ErrorCode::PacketSize, "HELLO_ACK payload size mismatch");
        HelloPayloadV1 payload{};
        std::memcpy(&payload, decoded.payload, sizeof(payload));
        if (!ValidateHello(payload))
            return Fail(ErrorCode::GroupHandshakeMismatch, "group HELLO_ACK mismatch");
        hello_frames_[sender_slot] = payload.online_frame;
        hello_ack_mask_ |= 1u << sender_slot;
        CommitReceive(sender_slot);
        const std::uint32_t participant_mask = FrozenSlotMask() & ~1u;
        if (hello_ack_mask_ != participant_mask) return true;
        std::uint32_t latest = hello_frames_[0];
        for (std::uint16_t slot = 1; slot < config_.peer_count; ++slot)
            latest = (std::max)(latest, hello_frames_[slot]);
        if (latest > UINT32_MAX - 60u)
            return Fail(ErrorCode::HandshakeActivationFrame,
                        "group activation frame overflow");
        activation_frame_ = latest + 60u;
        for (std::uint16_t slot = 1; slot < config_.peer_count; ++slot) {
            if (!SendActivate(MessageKind::Activate, slot)) return false;
        }
        EnterWarmup(now_ms);
        return true;
    }

    if (kind == MessageKind::Activate && config_.local_slot != 0 && sender_slot == 0 &&
        state_ == RuntimeState::Hello) {
        if (decoded.payload_size != sizeof(ActivatePayloadV1))
            return Fail(ErrorCode::PacketSize, "ACTIVATE payload size mismatch");
        ActivatePayloadV1 payload{};
        std::memcpy(&payload, decoded.payload, sizeof(payload));
        if (local_online_frame > UINT32_MAX - config_.lead_frames ||
            payload.activation_frame < local_online_frame + config_.lead_frames)
            return Fail(ErrorCode::HandshakeActivationFrame,
                        "ACTIVATE frame is too early");
        activation_frame_ = payload.activation_frame;
        if (!ValidateActivate(payload))
            return Fail(ErrorCode::GroupActivationMismatch, "group ACTIVATE mismatch");
        CommitReceive(sender_slot);
        EnterWarmup(now_ms);
        return true;
    }

    if (kind == MessageKind::ActivateReady && config_.local_slot == 0 &&
        state_ == RuntimeState::Warmup) {
        if (decoded.payload_size != sizeof(ActivatePayloadV1))
            return Fail(ErrorCode::PacketSize, "ACTIVATE_READY payload size mismatch");
        ActivatePayloadV1 payload{};
        std::memcpy(&payload, decoded.payload, sizeof(payload));
        if (!ValidateActivate(payload))
            return Fail(ErrorCode::GroupActivationMismatch,
                        "group ACTIVATE_READY mismatch");
        activation_ready_mask_ |= 1u << sender_slot;
        CommitReceive(sender_slot);
        return TryAdvanceCommit();
    }

    if (kind == MessageKind::ActivateCommit && config_.local_slot != 0 &&
        sender_slot == 0 && state_ == RuntimeState::Warmup) {
        if (decoded.payload_size != sizeof(ActivatePayloadV1))
            return Fail(ErrorCode::PacketSize, "ACTIVATE_COMMIT payload size mismatch");
        ActivatePayloadV1 payload{};
        std::memcpy(&payload, decoded.payload, sizeof(payload));
        if (!ValidateActivate(payload) ||
            (activation_ready_mask_ & (1u << config_.local_slot)) == 0)
            return Fail(ErrorCode::GroupActivationMismatch,
                        "group ACTIVATE_COMMIT arrived before local readiness");
        CommitReceive(sender_slot);
        committed_ = true;
        return true;
    }

    return Fail(ErrorCode::HandshakeState,
                "unexpected packet for current group session state");
}

bool GroupSessionProtocolEngine::TryAdvanceCommit() {
    if (state_ != RuntimeState::Warmup) return false;
    if (committed_) return true;
    if (config_.local_slot != 0 || activation_ready_mask_ != FrozenSlotMask()) return true;
    for (std::uint16_t slot = 1; slot < config_.peer_count; ++slot) {
        if (!SendActivate(MessageKind::ActivateCommit, slot)) return false;
    }
    committed_ = true;
    return true;
}

bool GroupSessionProtocolEngine::SetActivationRecordsReady(bool all_slots_ready) {
    if (state_ != RuntimeState::Warmup)
        return Fail(ErrorCode::HandshakeState,
                    "activation readiness supplied outside WARMUP");
    if (!all_slots_ready) return true;
    const std::uint32_t local_bit = 1u << config_.local_slot;
    activation_ready_mask_ |= local_bit;
    if (config_.local_slot != 0 && !activation_ready_sent_) {
        if (!SendActivate(MessageKind::ActivateReady, 0)) return false;
        activation_ready_sent_ = true;
    }
    return TryAdvanceCommit();
}

bool GroupSessionProtocolEngine::SendFrameBatchTo(
        std::uint16_t recipient_slot, std::uint32_t first_frame,
        const CustomFrameOpV2* records, std::uint16_t count,
        bool sender_committed) {
    if ((state_ != RuntimeState::Warmup && state_ != RuntimeState::Active) ||
        recipient_slot >= config_.peer_count || recipient_slot == config_.local_slot)
        return false;
    PacketBuffer packet{};
    if (!EncodeFrameBatch(session_, config_.steam_ids[config_.local_slot],
                          next_send_sequences_[recipient_slot], first_frame,
                          records, count, sender_committed, packet) ||
        !transport_.Send(recipient_slot, packet))
        return false;
    ++next_send_sequences_[recipient_slot];
    ++sent_packets_[recipient_slot];
    return true;
}

bool GroupSessionProtocolEngine::Tick(std::uint32_t, std::uint64_t now_ms) {
    if ((state_ == RuntimeState::Discovering || state_ == RuntimeState::Hello) &&
        now_ms - state_enter_ms_ >= kHandshakeTimeoutMs)
        return Fail(ErrorCode::HandshakeTimeout,
                    "group HELLO/ACTIVATE handshake timed out");
    if (state_ == RuntimeState::Warmup && !committed_ &&
        now_ms - state_enter_ms_ >= kHandshakeTimeoutMs)
        return Fail(ErrorCode::ActivationBarrierTimeout,
                    "group activation commit timed out");
    return true;
}

bool GroupSessionProtocolEngine::FailExternal(ErrorCode code, const char* text) {
    return Fail(code, text);
}

bool GroupSessionProtocolEngine::BarrierRequired(std::uint32_t frame) const {
    return state_ == RuntimeState::Warmup && activation_frame_ != 0 &&
           frame >= activation_frame_ && !committed_;
}

bool GroupSessionProtocolEngine::ActivateIfReady(std::uint32_t frame) {
    if (state_ == RuntimeState::Active) return true;
    if (state_ != RuntimeState::Warmup || !committed_ ||
        activation_frame_ == 0 || frame < activation_frame_)
        return false;
    state_ = RuntimeState::Active;
    return true;
}

} // namespace isaac_sync
