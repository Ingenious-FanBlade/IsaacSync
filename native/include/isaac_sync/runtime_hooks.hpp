#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace isaac_sync::runtime {

// The target routine returns a C++ bool in x86 AL.  EAX[31:8] is not part of
// the ABI and may retain arbitrary data when AL is zero.
using FrameUpdateFn = bool(__thiscall*)(void*);

enum class FrameDispatchTransition : std::uint8_t {
    BaseStalled,
    BaseAdvanced,
    DispatchDuringStall,
    DispatchMissing,
};

constexpr FrameDispatchTransition ClassifyFrameDispatchTransition(
    bool frame_update_succeeded, bool dispatch_completed) noexcept
{
    if (!frame_update_succeeded)
        return dispatch_completed ? FrameDispatchTransition::DispatchDuringStall
                                  : FrameDispatchTransition::BaseStalled;
    return dispatch_completed ? FrameDispatchTransition::BaseAdvanced
                              : FrameDispatchTransition::DispatchMissing;
}

struct CallPatch {
    std::uint8_t* call = nullptr;
    std::int32_t original_displacement = 0;
    std::int32_t replacement_displacement = 0;
    bool installed = false;
};

struct IatPatch {
    void** slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
    bool installed = false;
};

bool InstallRelativeCallPatch(std::uint8_t* call, const std::uint8_t* expected,
                              std::size_t expected_size, void* replacement,
                              CallPatch& patch, void* volatile* published_original);
bool RestoreRelativeCallPatch(CallPatch& patch);

void** FindIatSlot(HMODULE module, const char* name);
bool ValidateIatCallsite(const std::uint8_t* call, void** expected_slot);
bool InstallIatPatch(HMODULE module, const char* name, void* replacement, IatPatch& patch,
                     void* volatile* published_original, void* expected_original = nullptr);
bool RestoreIatPatch(IatPatch& patch);

} // namespace isaac_sync::runtime
