#include "isaac_sync/dispatcher.hpp"

namespace isaac_sync {

DispatchExecutionResult ExecuteDeterministicDispatch(
    std::uint32_t frame,
    const std::array<CustomFrameOpV2, kMaximumPeerCount>& operations,
    std::uint16_t peer_count,
    IHandlerInvoker& invoker, IOriginalDispatch& original,
    void* self, std::uint32_t argument) {
    DispatchExecutionResult result{};
    if (peer_count < 2 || peer_count > operations.size()) {
        result.error = ErrorCode::DispatchStateViolation;
        result.original_result = original.Call(self, argument);
        return result;
    }
    for (std::uint32_t slot = 0; slot < peer_count; ++slot) {
        const auto& operation = operations[slot];
        if (IsNoOp(operation)) continue;
        ++result.attempted_invocations;
        const HandlerInvokeResult invoke = operation.port == 0
            ? HandlerInvokeResult::UnknownPort : invoker.Invoke(slot, operation, frame);
        if (invoke == HandlerInvokeResult::Success) {
            ++result.successful_invocations;
            continue;
        }
        result.error = invoke == HandlerInvokeResult::UnknownPort
            ? ErrorCode::FrameUnknownPort
            : invoke == HandlerInvokeResult::InvalidRef
                ? ErrorCode::DispatchRefInvalid : ErrorCode::DispatchLuaFailure;
        break;
    }
    result.original_result = original.Call(self, argument);
    return result;
}

} // namespace isaac_sync
