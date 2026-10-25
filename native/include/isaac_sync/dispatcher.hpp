#pragma once

#include "frame_store.hpp"

#include <array>
#include <cstdint>

namespace isaac_sync {

enum class HandlerInvokeResult {
    Success,
    UnknownPort,
    InvalidRef,
    StackFailure,
    PcallFailure,
    YieldFailure,
    LuaFailure,
};

class IHandlerInvoker {
public:
    virtual ~IHandlerInvoker() = default;
    virtual HandlerInvokeResult Invoke(std::uint32_t sender_slot,
                               const CustomFrameOpV2& operation,
                                       std::uint32_t frame) = 0;
};

class IOriginalDispatch {
public:
    virtual ~IOriginalDispatch() = default;
    virtual std::uint32_t Call(void* self, std::uint32_t argument) = 0;
};

struct DispatchExecutionResult {
    std::uint32_t original_result = 0;
    ErrorCode error = ErrorCode::None;
    std::uint16_t attempted_invocations = 0;
    std::uint16_t successful_invocations = 0;
};

DispatchExecutionResult ExecuteDeterministicDispatch(
    std::uint32_t frame,
    const std::array<CustomFrameOpV2, kMaximumPeerCount>& operations,
    std::uint16_t peer_count,
    IHandlerInvoker& invoker, IOriginalDispatch& original,
    void* self, std::uint32_t argument);

} // namespace isaac_sync
