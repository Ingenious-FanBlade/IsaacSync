#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ipc.hpp"

namespace isaac_sync {

constexpr std::uint16_t kProtocolVersion = 5;
constexpr std::uint8_t kCustomChannel = 0xFD;
constexpr std::uint16_t kLeadFrames = 12;
constexpr std::uint16_t kMaxBatchRecords = 16;
constexpr std::uint16_t kMaximumPeerCount = 4;
constexpr std::uint16_t kFullMeshTopologyId = 1;
constexpr std::uint32_t kCapabilityMultiPeerMesh = 1u << 0;
constexpr std::uint32_t kCapabilityGroupActivationCommit = 1u << 1;
constexpr std::uint32_t kCapabilitySenderCommittedFrameBatch = 1u << 2;
constexpr std::uint32_t kCapabilityTypedNumericPayload = 1u << 3;
constexpr std::uint32_t kRequiredCapabilities = kCapabilityMultiPeerMesh |
                                                 kCapabilityGroupActivationCommit |
                                                 kCapabilitySenderCommittedFrameBatch |
                                                 kCapabilityTypedNumericPayload;
constexpr std::uint16_t kPacketFlagSenderCommitted = 1u << 0;
constexpr std::uint32_t kMaxFutureWindow = 1024;
constexpr std::uint32_t kFrameHistoryWindow = 32;
constexpr std::size_t kPacketBufferSize = 512;
constexpr std::size_t kMaximumWirePacketSize = 312;
constexpr std::uint8_t kSidecarMagic[4] = {'I', 'S', 'C', 'S'};

enum class PayloadKind : std::uint8_t {
    None = 0,
    Int32 = 1,
    Float64 = 2,
};

constexpr std::uint32_t kSupportedPayloadKindMask =
    (1u << static_cast<std::uint8_t>(PayloadKind::None)) |
    (1u << static_cast<std::uint8_t>(PayloadKind::Int32)) |
    (1u << static_cast<std::uint8_t>(PayloadKind::Float64));

enum class MessageKind : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    Activate = 3,
    ActivateReady = 4,
    FrameBatch = 5,
    Abort = 6,
    Goodbye = 7,
    ActivateCommit = 8,
};

#pragma pack(push, 1)

struct SidecarHeaderV1 {
    std::uint8_t magic[4];
    std::uint16_t protocol_version;
    std::uint8_t kind;
    std::uint8_t header_size;
    std::uint8_t session_id[16];
    std::uint64_t sender_steam_id;
    std::uint64_t message_sequence;
    std::uint16_t payload_size;
    std::uint16_t flags;
    std::uint32_t crc32;
};

struct HelloPayloadV1 {
    std::uint8_t build_hash[32];
    std::uint8_t roster_hash[32];
    std::uint8_t manifest_hash[32];
    std::uint32_t online_frame;
    std::uint16_t lead_frames;
    std::uint16_t max_batch_records;
    std::uint32_t capabilities;
};

struct ActivatePayloadV1 {
    std::uint32_t activation_frame;
    std::uint16_t lead_frames;
    std::uint16_t reserved;
    std::uint8_t handshake_hash[32];
};

struct CustomFrameOpV2 {
    std::uint16_t port;
    std::uint16_t opcode;
    std::uint8_t payload_kind;
    std::uint8_t reserved[3];
    std::uint64_t payload_bits;
};

struct FrameBatchPrefixV1 {
    std::uint32_t first_frame;
    std::uint16_t record_count;
    std::uint16_t record_size;
};

#pragma pack(pop)

static_assert(sizeof(SidecarHeaderV1) == 48, "sidecar header ABI changed");
static_assert(offsetof(SidecarHeaderV1, crc32) == 44, "sidecar CRC coverage changed");
static_assert(sizeof(HelloPayloadV1) == 108, "HELLO payload ABI changed");
static_assert(sizeof(ActivatePayloadV1) == 40, "ACTIVATE payload ABI changed");
static_assert(sizeof(CustomFrameOpV2) == 16, "frame operation ABI changed");
static_assert(offsetof(CustomFrameOpV2, payload_bits) == 8,
              "frame payload bits offset changed");
static_assert(sizeof(FrameBatchPrefixV1) == 8, "frame batch prefix ABI changed");
static_assert(sizeof(SidecarHeaderV1) + sizeof(FrameBatchPrefixV1) +
                  kMaxBatchRecords * sizeof(CustomFrameOpV2) == kMaximumWirePacketSize,
              "maximum packet size changed");

using Hash256 = std::array<std::uint8_t, 32>;
using SessionId = std::array<std::uint8_t, 16>;

struct PacketBuffer {
    std::array<std::uint8_t, kPacketBufferSize> bytes{};
    std::uint16_t size = 0;
};

struct DecodedPacket {
    SidecarHeaderV1 header{};
    const std::uint8_t* payload = nullptr;
    std::uint16_t payload_size = 0;
};

struct DecodedFrameBatch {
    std::uint32_t first_frame = 0;
    std::uint16_t count = 0;
    std::array<CustomFrameOpV2, kMaxBatchRecords> records{};
};

enum class PacketError {
    None,
    Size,
    Magic,
    Version,
    Kind,
    Flags,
    PayloadSize,
    Session,
    Sender,
    Sequence,
    Crc,
};

enum class FrameBatchError {
    None,
    Size,
    Count,
    RecordSize,
    Overflow,
    Payload,
};

struct PacketExpectations {
    const SessionId* session = nullptr;
    std::uint64_t sender_steam_id = 0;
    std::uint64_t next_sequence = 0;
    bool allow_unbound_hello = false;
};

struct ManifestEntry {
    std::string sync_id;
    std::uint32_t version = 0;
    std::uint16_t port = 0;
};

enum class ManifestRegistrationCheck {
    Available,
    DuplicateSyncId,
    DerivedPortCollision,
};

std::uint32_t Crc32(const void* data, std::size_t size);
bool IsKnownMessageKind(std::uint8_t value);
bool EncodePacket(MessageKind kind, const SessionId& session, std::uint64_t sender_steam_id,
                  std::uint64_t sequence, const void* payload, std::uint16_t payload_size,
                  std::uint16_t flags, PacketBuffer& output);
PacketError DecodePacket(const void* packet, std::size_t packet_size,
                         const PacketExpectations& expected, DecodedPacket& output);
bool EncodeFrameBatch(const SessionId& session, std::uint64_t sender_steam_id,
                      std::uint64_t sequence, std::uint32_t first_frame,
                      const CustomFrameOpV2* records, std::uint16_t count,
                      bool sender_committed, PacketBuffer& output);
FrameBatchError DecodeFrameBatch(const DecodedPacket& packet, DecodedFrameBatch& output);

bool IsNoOp(const CustomFrameOpV2& operation);
bool IsCanonicalOperation(const CustomFrameOpV2& operation);
CustomFrameOpV2 MakeInt32Operation(std::uint16_t port, std::uint16_t opcode,
                                   std::int32_t payload);
bool MakeFloat64Operation(std::uint16_t port, std::uint16_t opcode, double payload,
                          CustomFrameOpV2& output);
bool DecodeInt32Payload(const CustomFrameOpV2& operation, std::int32_t& output);
bool DecodeFloat64Payload(const CustomFrameOpV2& operation, double& output);
bool DerivePort(const std::string& sync_id, std::uint16_t& output, std::string& error);
ManifestRegistrationCheck ClassifyManifestRegistration(
    const std::vector<ManifestEntry>& entries, const std::string& sync_id,
    std::uint16_t derived_port);

bool Sha256(const void* data, std::size_t size, Hash256& output);
bool CanonicalizeManifest(const std::vector<ManifestEntry>& entries,
                          std::vector<std::uint8_t>& serialized, std::string& error);
bool ComputeManifestHash(const std::vector<ManifestEntry>& entries, Hash256& output,
                         std::string& error);
bool ComputeRosterHash(const std::uint64_t* sorted_steam_ids,
                       const std::uint16_t* player_slots_in_game_order,
                       std::uint16_t peer_count, Hash256& output);
bool ComputePeerToken(const SessionId& session, std::uint64_t steam_id,
                      std::uint64_t& output);

} // namespace isaac_sync
