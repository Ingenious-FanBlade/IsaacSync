#pragma once

#include "ipc.hpp"

#include <cstddef>
#include <cstdint>

namespace isaac_sync::runtime {

IpcEventV5* BeginEvent(IpcEventType type);
void CommitEvent(IpcEventV5* event);
void PublishEvent(IpcEventType type, const char* text = nullptr,
                  std::uint64_t value0 = 0, std::uint64_t value1 = 0,
                  std::uint64_t message_sequence = 0, std::uint64_t session_token = 0,
                  std::uint64_t peer_token = 0, std::uint32_t crc32 = 0,
                  std::uint16_t kind = 0, std::uint16_t count = 0,
                  std::uint32_t operation_frame = 0, std::uint32_t sender_slot = 0,
                  std::uint16_t port = 0, std::uint16_t opcode = 0,
                  std::int32_t payload = 0,
                  std::uint32_t recipient_slot = UINT32_MAX);

void SetRuntimeState(RuntimeState state, const char* status);
void SetCompatibility(CompatibilityState state, ErrorCode error, const char* status,
                      const char* detail = nullptr);

bool OpenSharedControlBlock();
void CloseSharedControlBlock();

} // namespace isaac_sync::runtime
