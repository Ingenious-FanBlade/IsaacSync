#include "isaac_sync/runtime_ipc.hpp"

#include <cstring>

namespace isaac_sync::runtime {

extern HANDLE g_mapping;
extern SharedControlBlockV5* g_shared;
extern volatile LONG g_unloading;

void CopyText(char* destination, std::size_t capacity, const char* text);

IpcEventV5* BeginEvent(IpcEventType type)
{
    if (!g_shared || InterlockedCompareExchange(&g_unloading, 0, 0) != 0)
        return nullptr;
    const LONG64 sequence = InterlockedIncrement64(&g_shared->write_sequence);
    auto& event = g_shared->events[static_cast<std::size_t>((sequence - 1) % kIpcEventCapacity)];
    const LONG64 previous = InterlockedCompareExchange64(&event.sequence, 0, 0);
    if (previous > 0 && sequence - previous >= kIpcEventCapacity)
        InterlockedIncrement64(&g_shared->overwritten_events);
    InterlockedExchange64(&event.sequence, -sequence);
    std::memset(reinterpret_cast<std::uint8_t*>(&event) + sizeof(event.sequence), 0,
                sizeof(event) - sizeof(event.sequence));
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.type = static_cast<std::uint32_t>(type);
    event.recipient_slot = UINT32_MAX;
    event.state = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_shared->runtime_state, 0, 0));
    event.error = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_shared->error_code, 0, 0));
    event.online_frame = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_shared->online_frame, 0, 0));
    return &event;
}

void CommitEvent(IpcEventV5* event)
{
    if (!event)
        return;
    const LONG64 sequence = -event->sequence;
    if (sequence <= 0)
        return;
    MemoryBarrier();
    InterlockedExchange64(&event->sequence, sequence);
}

void PublishEvent(IpcEventType type, const char* text, std::uint64_t value0,
                  std::uint64_t value1, std::uint64_t message_sequence,
                  std::uint64_t session_token, std::uint64_t peer_token,
                  std::uint32_t crc32, std::uint16_t kind, std::uint16_t count,
                  std::uint32_t operation_frame, std::uint32_t sender_slot,
                  std::uint16_t port, std::uint16_t opcode, std::int32_t payload,
                  std::uint32_t recipient_slot)
{
    (void)payload;
    auto* event = BeginEvent(type);
    if (!event)
        return;
    event->message_sequence = message_sequence;
    event->session_token = session_token;
    event->peer_token = peer_token;
    event->value0 = value0;
    event->value1 = value1;
    event->crc32 = crc32;
    event->kind = kind;
    event->count = count;
    event->operation_frame = operation_frame;
    event->sender_slot = sender_slot;
    event->recipient_slot = recipient_slot;
    event->port = port;
    event->opcode = opcode;
    CopyText(event->message, sizeof(event->message), text);
    CommitEvent(event);
}

void SetRuntimeState(RuntimeState state, const char* status)
{
    if (!g_shared)
        return;
    const auto previous = static_cast<RuntimeState>(
        InterlockedCompareExchange(&g_shared->runtime_state, 0, 0));
    CopyText(g_shared->status, sizeof(g_shared->status), status);
    InterlockedExchange(&g_shared->runtime_state, static_cast<LONG>(state));
    if (previous != state)
    {
        PublishEvent(IpcEventType::RuntimeStateChanged, status,
                     static_cast<std::uint32_t>(previous), static_cast<std::uint32_t>(state));
    }
}

void SetCompatibility(CompatibilityState state, ErrorCode error, const char* status,
                      const char* detail)
{
    if (!g_shared)
        return;
    const char* safe_status = status ? status : "";
    const char* safe_detail = detail ? detail : "";
    const bool changed = InterlockedCompareExchange(&g_shared->compatibility_state, 0, 0) !=
                             static_cast<LONG>(state) ||
                         InterlockedCompareExchange(&g_shared->error_code, 0, 0) !=
                             static_cast<LONG>(error) ||
                         std::strcmp(g_shared->status, safe_status) != 0 ||
                         std::strcmp(g_shared->error_detail, safe_detail) != 0;
    CopyText(g_shared->status, sizeof(g_shared->status), safe_status);
    CopyText(g_shared->error_detail, sizeof(g_shared->error_detail), safe_detail);
    InterlockedExchange(&g_shared->error_code, static_cast<LONG>(error));
    InterlockedExchange(&g_shared->compatibility_state, static_cast<LONG>(state));
    if (changed)
    {
        PublishEvent(state == CompatibilityState::Unsupported ? IpcEventType::Error
                                                               : IpcEventType::CompatibilityChanged,
                     detail ? detail : status);
    }
}

bool OpenSharedControlBlock()
{
    wchar_t name[64]{};
    BuildIpcMappingName(GetCurrentProcessId(), name);
    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapping)
        return false;
    g_shared = static_cast<SharedControlBlockV5*>(MapViewOfFile(
        g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControlBlockV5)));
    if (!g_shared || g_shared->magic != kIpcMagic || g_shared->abi_version != kIpcAbiVersion ||
        g_shared->struct_size != sizeof(SharedControlBlockV5) ||
        g_shared->process_id != GetCurrentProcessId())
    {
        if (g_shared)
            UnmapViewOfFile(g_shared);
        CloseHandle(g_mapping);
        g_shared = nullptr;
        g_mapping = nullptr;
        return false;
    }
    return true;
}

void CloseSharedControlBlock()
{
    if (g_shared)
    {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_mapping)
    {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

} // namespace isaac_sync::runtime
