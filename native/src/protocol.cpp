#include "isaac_sync/protocol.hpp"
#include "isaac_sync/version.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#pragma comment(lib, "bcrypt.lib")

namespace isaac_sync {
namespace {

std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

std::uint32_t PacketCrc(const std::uint8_t* packet, std::size_t payload_size) {
    std::uint32_t crc = Crc32Update(0xFFFFFFFFu, packet, offsetof(SidecarHeaderV1, crc32));
    crc = Crc32Update(crc, packet + sizeof(SidecarHeaderV1), payload_size);
    return ~crc;
}

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool IsValidUtf8(const std::string& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index++];
        if (first <= 0x7F) {
            if (first == 0) return false;
            continue;
        }
        unsigned continuation = 0;
        std::uint32_t value = 0;
        if ((first & 0xE0) == 0xC0) {
            continuation = 1;
            value = first & 0x1F;
            if (value < 2) return false;
        } else if ((first & 0xF0) == 0xE0) {
            continuation = 2;
            value = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            continuation = 3;
            value = first & 0x07;
        } else {
            return false;
        }
        if (index + continuation > text.size()) return false;
        for (unsigned count = 0; count < continuation; ++count) {
            const unsigned char next = bytes[index++];
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if ((continuation == 2 && value < 0x800) ||
            (continuation == 3 && value < 0x10000) || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::uint32_t Crc32(const void* data, std::size_t size) {
    return ~Crc32Update(0xFFFFFFFFu, static_cast<const std::uint8_t*>(data), size);
}

bool IsKnownMessageKind(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(MessageKind::Hello) &&
           value <= static_cast<std::uint8_t>(MessageKind::ActivateCommit);
}

bool IsNoOp(const CustomFrameOpV2& operation) {
    const CustomFrameOpV2 empty{};
    return std::memcmp(&operation, &empty, sizeof(operation)) == 0;
}

bool IsCanonicalOperation(const CustomFrameOpV2& operation) {
    if (IsNoOp(operation)) return true;
    if (!operation.port || operation.reserved[0] || operation.reserved[1] ||
        operation.reserved[2]) {
        return false;
    }
    const auto kind = static_cast<PayloadKind>(operation.payload_kind);
    if (kind == PayloadKind::Int32) {
        return (operation.payload_bits >> 32) == 0;
    }
    if (kind == PayloadKind::Float64) {
        constexpr std::uint64_t exponent_mask = 0x7FF0000000000000ull;
        constexpr std::uint64_t negative_zero = 0x8000000000000000ull;
        return operation.payload_bits != negative_zero &&
               (operation.payload_bits & exponent_mask) != exponent_mask;
    }
    return false;
}

CustomFrameOpV2 MakeInt32Operation(std::uint16_t port, std::uint16_t opcode,
                                   std::int32_t payload) {
    CustomFrameOpV2 operation{};
    operation.port = port;
    operation.opcode = opcode;
    operation.payload_kind = static_cast<std::uint8_t>(PayloadKind::Int32);
    operation.payload_bits = static_cast<std::uint32_t>(payload);
    return operation;
}

bool MakeFloat64Operation(std::uint16_t port, std::uint16_t opcode, double payload,
                          CustomFrameOpV2& output) {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "typed payloads require IEEE-754 binary64 lua_Number");
    output = {};
    if (!port || !std::isfinite(payload)) return false;
    output.port = port;
    output.opcode = opcode;
    output.payload_kind = static_cast<std::uint8_t>(PayloadKind::Float64);
    if (payload != 0.0) {
        std::memcpy(&output.payload_bits, &payload, sizeof(payload));
    }
    return IsCanonicalOperation(output);
}

bool DecodeInt32Payload(const CustomFrameOpV2& operation, std::int32_t& output) {
    output = 0;
    if (!IsCanonicalOperation(operation) ||
        operation.payload_kind != static_cast<std::uint8_t>(PayloadKind::Int32)) {
        return false;
    }
    const std::uint32_t bits = static_cast<std::uint32_t>(operation.payload_bits);
    std::memcpy(&output, &bits, sizeof(output));
    return true;
}

bool DecodeFloat64Payload(const CustomFrameOpV2& operation, double& output) {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "typed payloads require IEEE-754 binary64 lua_Number");
    output = 0.0;
    if (!IsCanonicalOperation(operation) ||
        operation.payload_kind != static_cast<std::uint8_t>(PayloadKind::Float64)) {
        return false;
    }
    std::memcpy(&output, &operation.payload_bits, sizeof(output));
    return true;
}

bool EncodePacket(MessageKind kind, const SessionId& session, std::uint64_t sender_steam_id,
                   std::uint64_t sequence, const void* payload, std::uint16_t payload_size,
                   std::uint16_t flags, PacketBuffer& output) {
    const std::size_t total_size = sizeof(SidecarHeaderV1) + payload_size;
    const bool frame_batch = kind == MessageKind::FrameBatch;
    if (!IsKnownMessageKind(static_cast<std::uint8_t>(kind)) || !sender_steam_id || !sequence ||
        total_size > output.bytes.size() || (payload_size && !payload) ||
        (!frame_batch && flags != 0) ||
        (frame_batch && (flags & ~kPacketFlagSenderCommitted) != 0)) {
        return false;
    }
    output = {};
    SidecarHeaderV1 header{};
    std::memcpy(header.magic, kSidecarMagic, sizeof(header.magic));
    header.protocol_version = kProtocolVersion;
    header.kind = static_cast<std::uint8_t>(kind);
    header.header_size = static_cast<std::uint8_t>(sizeof(header));
    std::memcpy(header.session_id, session.data(), session.size());
    header.sender_steam_id = sender_steam_id;
    header.message_sequence = sequence;
    header.payload_size = payload_size;
    header.flags = flags;
    std::memcpy(output.bytes.data(), &header, sizeof(header));
    if (payload_size) std::memcpy(output.bytes.data() + sizeof(header), payload, payload_size);
    header.crc32 = PacketCrc(output.bytes.data(), payload_size);
    std::memcpy(output.bytes.data(), &header, sizeof(header));
    output.size = static_cast<std::uint16_t>(total_size);
    return true;
}

PacketError DecodePacket(const void* packet, std::size_t packet_size,
                         const PacketExpectations& expected, DecodedPacket& output) {
    output = {};
    if (!packet || packet_size < sizeof(SidecarHeaderV1) || packet_size > kPacketBufferSize) {
        return PacketError::Size;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(packet);
    SidecarHeaderV1 header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (std::memcmp(header.magic, kSidecarMagic, sizeof(header.magic)) != 0) return PacketError::Magic;
    if (header.protocol_version != kProtocolVersion || header.header_size != sizeof(header)) {
        return PacketError::Version;
    }
    if (!IsKnownMessageKind(header.kind)) return PacketError::Kind;
    if (header.kind == static_cast<std::uint8_t>(MessageKind::FrameBatch)) {
        if ((header.flags & ~kPacketFlagSenderCommitted) != 0) return PacketError::Flags;
    } else if (header.flags != 0) {
        return PacketError::Flags;
    }
    if (packet_size != sizeof(header) + header.payload_size) return PacketError::PayloadSize;
    const bool unbound_hello = expected.allow_unbound_hello && !expected.session &&
        header.kind == static_cast<std::uint8_t>(MessageKind::Hello) &&
        header.message_sequence == 1;
    if (expected.session &&
        std::memcmp(header.session_id, expected.session->data(), expected.session->size()) != 0) {
        return PacketError::Session;
    }
    if (!expected.session && !unbound_hello) return PacketError::Session;
    if (expected.sender_steam_id && header.sender_steam_id != expected.sender_steam_id) {
        return PacketError::Sender;
    }
    if (header.crc32 != PacketCrc(bytes, header.payload_size)) return PacketError::Crc;
    if (expected.next_sequence && header.message_sequence != expected.next_sequence) {
        return PacketError::Sequence;
    }
    output.header = header;
    output.payload = bytes + sizeof(header);
    output.payload_size = header.payload_size;
    return PacketError::None;
}

bool EncodeFrameBatch(const SessionId& session, std::uint64_t sender_steam_id,
                       std::uint64_t sequence, std::uint32_t first_frame,
                       const CustomFrameOpV2* records, std::uint16_t count,
                       bool sender_committed, PacketBuffer& output) {
    if (!records || count == 0 || count > kMaxBatchRecords) return false;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (!IsCanonicalOperation(records[index])) return false;
    }
    std::array<std::uint8_t, sizeof(FrameBatchPrefixV1) +
        kMaxBatchRecords * sizeof(CustomFrameOpV2)> payload{};
    FrameBatchPrefixV1 prefix{first_frame, count, sizeof(CustomFrameOpV2)};
    std::memcpy(payload.data(), &prefix, sizeof(prefix));
    std::memcpy(payload.data() + sizeof(prefix), records,
                static_cast<std::size_t>(count) * sizeof(CustomFrameOpV2));
    const auto payload_size = static_cast<std::uint16_t>(
        sizeof(prefix) + static_cast<std::size_t>(count) * sizeof(CustomFrameOpV2));
    return EncodePacket(MessageKind::FrameBatch, session, sender_steam_id, sequence,
                        payload.data(), payload_size,
                        sender_committed ? kPacketFlagSenderCommitted : 0, output);
}

FrameBatchError DecodeFrameBatch(const DecodedPacket& packet, DecodedFrameBatch& output) {
    output = {};
    if (packet.header.kind != static_cast<std::uint8_t>(MessageKind::FrameBatch) ||
        packet.payload_size < sizeof(FrameBatchPrefixV1)) {
        return FrameBatchError::Size;
    }
    FrameBatchPrefixV1 prefix{};
    std::memcpy(&prefix, packet.payload, sizeof(prefix));
    if (prefix.record_count == 0 || prefix.record_count > kMaxBatchRecords) {
        return FrameBatchError::Count;
    }
    if (prefix.record_size != sizeof(CustomFrameOpV2)) return FrameBatchError::RecordSize;
    const std::size_t expected = sizeof(prefix) +
        static_cast<std::size_t>(prefix.record_count) * sizeof(CustomFrameOpV2);
    if (packet.payload_size != expected) return FrameBatchError::Size;
    if (prefix.first_frame > UINT32_MAX - (prefix.record_count - 1u)) {
        return FrameBatchError::Overflow;
    }
    DecodedFrameBatch candidate{};
    candidate.first_frame = prefix.first_frame;
    candidate.count = prefix.record_count;
    std::memcpy(candidate.records.data(), packet.payload + sizeof(prefix),
                static_cast<std::size_t>(prefix.record_count) * sizeof(CustomFrameOpV2));
    for (std::uint16_t index = 0; index < candidate.count; ++index) {
        if (!IsCanonicalOperation(candidate.records[index])) return FrameBatchError::Payload;
    }
    output = candidate;
    return FrameBatchError::None;
}

bool Sha256(const void* data, std::size_t size, Hash256& output) {
    output.fill(0);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    std::vector<std::uint8_t> object;
    bool success = false;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                          reinterpret_cast<PUCHAR>(&object_size),
                                          sizeof(object_size), &result_size, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    object.resize(object_size);
    if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                                        nullptr, 0, 0))) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        const bool input_ok = size == 0 || (bytes && size <= static_cast<std::size_t>(MAXDWORD) &&
            BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(bytes),
                                          static_cast<ULONG>(size), 0)));
        success = input_ok && BCRYPT_SUCCESS(BCryptFinishHash(
            hash, output.data(), static_cast<ULONG>(output.size()), 0));
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) output.fill(0);
    return success;
}

bool DerivePort(const std::string& sync_id, std::uint16_t& output, std::string& error) {
    output = 0;
    error.clear();
    if (sync_id.empty() || sync_id.size() > 96 || !IsValidUtf8(sync_id)) {
        error = "syncId must be 1..96 bytes of valid UTF-8 without NUL";
        return false;
    }
    std::vector<std::uint8_t> input;
    static constexpr char domain[] = "IsaacSync/port-v1";
    input.insert(input.end(), domain, domain + sizeof(domain) - 1);
    AppendU16(input, static_cast<std::uint16_t>(sync_id.size()));
    input.insert(input.end(), sync_id.begin(), sync_id.end());
    Hash256 digest{};
    if (!Sha256(input.data(), input.size(), digest)) {
        error = "port SHA-256 failed";
        return false;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(digest[0]) |
                                (static_cast<std::uint32_t>(digest[1]) << 8) |
                                (static_cast<std::uint32_t>(digest[2]) << 16) |
                                (static_cast<std::uint32_t>(digest[3]) << 24);
    output = static_cast<std::uint16_t>(1u + value % 65535u);
    return true;
}

ManifestRegistrationCheck ClassifyManifestRegistration(
        const std::vector<ManifestEntry>& entries, const std::string& sync_id,
        std::uint16_t derived_port) {
    for (const auto& entry : entries) {
        if (entry.sync_id == sync_id) return ManifestRegistrationCheck::DuplicateSyncId;
    }
    for (const auto& entry : entries) {
        if (entry.port == derived_port) {
            return ManifestRegistrationCheck::DerivedPortCollision;
        }
    }
    return ManifestRegistrationCheck::Available;
}

bool CanonicalizeManifest(const std::vector<ManifestEntry>& entries,
                          std::vector<std::uint8_t>& serialized, std::string& error) {
    serialized.clear();
    error.clear();
    if (entries.size() > 64) {
        error = "manifest has more than 64 entries";
        return false;
    }
    std::vector<ManifestEntry> sorted = entries;
    std::set<std::uint16_t> ports;
    for (const auto& entry : sorted) {
        if (entry.sync_id.empty() || entry.sync_id.size() > 96 || !IsValidUtf8(entry.sync_id)) {
            error = "syncId must be 1..96 bytes of valid UTF-8 without NUL";
            return false;
        }
        if (!entry.port || !ports.insert(entry.port).second) {
            error = "manifest ports must be nonzero and unique";
            return false;
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const ManifestEntry& left, const ManifestEntry& right) {
        if (left.sync_id != right.sync_id) {
            return std::lexicographical_compare(
                left.sync_id.begin(), left.sync_id.end(), right.sync_id.begin(), right.sync_id.end(),
                [](char a, char b) {
                    return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
                });
        }
        if (left.version != right.version) return left.version < right.version;
        return left.port < right.port;
    });
    AppendU16(serialized, static_cast<std::uint16_t>(sorted.size()));
    for (const auto& entry : sorted) {
        AppendU16(serialized, static_cast<std::uint16_t>(entry.sync_id.size()));
        serialized.insert(serialized.end(), entry.sync_id.begin(), entry.sync_id.end());
        AppendU32(serialized, entry.version);
        AppendU16(serialized, entry.port);
    }
    return true;
}

bool ComputeManifestHash(const std::vector<ManifestEntry>& entries, Hash256& output,
                         std::string& error) {
    std::vector<std::uint8_t> serialized;
    return CanonicalizeManifest(entries, serialized, error) &&
           Sha256(serialized.data(), serialized.size(), output);
}

bool ComputeRosterHash(const std::uint64_t* sorted_steam_ids,
                       const std::uint16_t* player_slots_in_game_order,
                       std::uint16_t peer_count, Hash256& output) {
    if (!sorted_steam_ids || !player_slots_in_game_order || peer_count < 2 ||
        peer_count > kMaximumPeerCount) return false;
    std::vector<std::uint8_t> bytes;
    static constexpr char domain[] =
        "IsaacSync/" ISAAC_SYNC_PRODUCT_VERSION "/roster-v5";
    bytes.insert(bytes.end(), domain, domain + sizeof(domain) - 1);
    AppendU16(bytes, peer_count);
    std::uint64_t previous = 0;
    for (std::uint16_t slot = 0; slot < peer_count; ++slot) {
        const std::uint64_t current = sorted_steam_ids[slot];
        if (!current || (slot != 0 && current <= previous)) return false;
        AppendU64(bytes, current);
        previous = current;
    }
    std::uint32_t player_slot_mask = 0;
    for (std::uint16_t player_index = 0; player_index < peer_count; ++player_index) {
        const std::uint16_t slot = player_slots_in_game_order[player_index];
        if (slot >= peer_count || (player_slot_mask & (1u << slot)) != 0) return false;
        player_slot_mask |= 1u << slot;
        AppendU16(bytes, slot);
    }
    if (player_slot_mask != (1u << peer_count) - 1u) return false;
    return Sha256(bytes.data(), bytes.size(), output);
}

bool ComputePeerToken(const SessionId& session, std::uint64_t steam_id,
                      std::uint64_t& output) {
    output = 0;
    if (!steam_id) return false;
    std::vector<std::uint8_t> bytes;
    static constexpr char domain[] =
        "IsaacSync/" ISAAC_SYNC_PRODUCT_VERSION "/peer-token";
    bytes.insert(bytes.end(), domain, domain + sizeof(domain) - 1);
    bytes.insert(bytes.end(), session.begin(), session.end());
    AppendU64(bytes, steam_id);
    Hash256 hash{};
    if (!Sha256(bytes.data(), bytes.size(), hash)) return false;
    for (unsigned shift = 0; shift < 64; shift += 8)
        output |= static_cast<std::uint64_t>(hash[shift / 8]) << shift;
    return true;
}

} // namespace isaac_sync
