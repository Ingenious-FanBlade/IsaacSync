#pragma once

#include <array>

namespace isaac_sync::runtime {

struct SteamNetworking006Context {
    void* interface_pointer = nullptr;
    void** vtable = nullptr;
    std::array<void*, 3> functions{};
};

enum class SteamContextCaptureResult {
    Captured,
    Invalid,
    OwnerMismatch,
};

bool ValidateSteamNetworking006(const SteamNetworking006Context& context);
SteamContextCaptureResult CaptureSteamNetworking006(void* context,
                                                    SteamNetworking006Context& output);

} // namespace isaac_sync::runtime
