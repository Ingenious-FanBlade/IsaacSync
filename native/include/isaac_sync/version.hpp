#pragma once

#ifndef ISAAC_SYNC_PRODUCT_VERSION
#error ISAAC_SYNC_PRODUCT_VERSION must be defined by the build configuration
#endif

#define ISAAC_SYNC_WIDEN_IMPL(value) L##value
#define ISAAC_SYNC_WIDEN(value) ISAAC_SYNC_WIDEN_IMPL(value)

namespace isaac_sync {

inline constexpr char kProductVersion[] = ISAAC_SYNC_PRODUCT_VERSION;
inline constexpr wchar_t kProductVersionWide[] =
    ISAAC_SYNC_WIDEN(ISAAC_SYNC_PRODUCT_VERSION);

} // namespace isaac_sync

