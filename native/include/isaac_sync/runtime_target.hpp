#pragma once

#include "protocol.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>

namespace isaac_sync::runtime {

inline constexpr char kImplementationRevision[] =
    "typed-payload-derived-port-v5-lua-generation-empty-manifest-autostart-r3";

inline constexpr std::uint32_t kFrameCallRva = 0x50B8E5;
inline constexpr std::uint32_t kDispatchCallRva = 0x5110AB;
inline constexpr std::uint32_t kLuaRegistrationRva = 0x466960;
inline constexpr std::uint32_t kLuaNewstateCallRva = 0x46057B;
inline constexpr std::uint32_t kLuaLNewstateCallRva = 0x4605A3;
inline constexpr std::uint32_t kLuaCloseCallRva = 0x46E5C0;
inline constexpr std::uint32_t kLuaSimulationPcallCallRva = 0x4729CB;
inline constexpr std::uint32_t kLuaSimulationPcallReturnRva = 0x4729D1;
inline constexpr std::uint32_t kManagerPointerRva = 0x87169C;
inline constexpr std::uint32_t kSteamNetworkingContextRva = 0x85C51C;
inline constexpr std::uint32_t kSteamNetworkingContextInitRva = 0x676090;
inline constexpr std::uint32_t kNetworkBackendObjectRva = 0x879A58;
inline constexpr std::uint32_t kSteamBackendVtableRva = 0x7A2440;
inline constexpr std::uint32_t kEosBackendVtableRva = 0x7A2530;

inline constexpr std::array<std::uint8_t, 5> kFrameCallBytes{{0xE8, 0x96, 0x50, 0x00, 0x00}};
inline constexpr std::array<std::uint8_t, 5> kDispatchCallBytes{{0xE8, 0x90, 0xC5, 0xFF, 0xFF}};
inline constexpr std::array<std::uint8_t, 15> kDispatchFeature{{
    0x6A, 0x00, 0x8B, 0xCF, 0xE8, 0x90, 0xC5, 0xFF, 0xFF, 0x8B, 0x8F, 0xB0, 0x18,
    0x00, 0x00,
}};
inline constexpr std::array<int, 25> kLuaRegistrationFeature{{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, -1, -1, -1, -1, 0x64, 0xA1, -1, -1, -1,
    -1, 0x50, 0x81, 0xEC, 0x44, 0x01, 0x00, 0x00, 0x53, -1,
}};
static_assert(kLuaSimulationPcallCallRva + 6 == kLuaSimulationPcallReturnRva);

bool IsTargetImage();
bool ValidateCriticalSites(const std::uint8_t* main_base);
bool ComputeBuildFingerprint(Hash256& output);

} // namespace isaac_sync::runtime
