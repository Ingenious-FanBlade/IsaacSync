#pragma once

#include <Windows.h>

namespace isaac_sync {

// The wrapper becomes callable the instant the IAT slot changes.  Keep the
// original target publication and replacement swap in one helper so callers
// cannot accidentally reintroduce a null-target race.
template <typename Function>
inline void PublishOriginalBeforeIatReplacement(Function& published_original,
                                                void* original,
                                                void* volatile* slot,
                                                void* replacement) noexcept {
    published_original = reinterpret_cast<Function>(original);
    MemoryBarrier();
    InterlockedExchangePointer(slot, replacement);
}

} // namespace isaac_sync
