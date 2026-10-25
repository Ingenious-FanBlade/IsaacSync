#include "isaac_sync/frame_store.hpp"

#include <algorithm>
#include <cstring>

namespace isaac_sync {

FrameStoreResult ComputeSendTarget(std::uint32_t online_frame, std::uint16_t lead_frames,
                                   std::uint32_t next_unsealed_frame, bool exhausted,
                                   std::uint32_t& target) {
    target = UINT32_MAX;
    if (exhausted || online_frame > UINT32_MAX - lead_frames) {
        return FrameStoreResult::Overflow;
    }
    target = (std::max)(online_frame + lead_frames, next_unsealed_frame);
    return FrameStoreResult::Inserted;
}

void FrameStore::Reset(std::uint32_t first_frame, FrameSlotState insertion_state) {
    slots_.fill({});
    insertion_state_ = insertion_state;
    first_frame_ = first_frame;
    through_ = 0;
    has_through_ = false;
}

FrameStoreResult FrameStore::Insert(std::uint32_t frame, const CustomFrameOpV2& operation,
                                    std::uint32_t current_frame) {
    return InsertAs(frame, operation, current_frame, insertion_state_);
}

FrameStoreResult FrameStore::InsertAs(std::uint32_t frame, const CustomFrameOpV2& operation,
                                      std::uint32_t current_frame, FrameSlotState state) {
    const FrameStoreResult validation = ValidateInsertAs(frame, operation, current_frame, state);
    if (validation != FrameStoreResult::Inserted) return validation;
    auto& slot = slots_[frame % slots_.size()];
    slot = {};
    slot.absolute_frame = frame;
    slot.operation = operation;
    slot.state = state;
    AdvanceThrough();
    return FrameStoreResult::Inserted;
}

FrameStoreResult FrameStore::ValidateInsertAs(std::uint32_t frame,
                                              const CustomFrameOpV2& operation,
                                              std::uint32_t current_frame,
                                              FrameSlotState state) const {
    if (!IsCanonicalOperation(operation) || state == FrameSlotState::Empty ||
        state == FrameSlotState::Consumed) return FrameStoreResult::InvalidOperation;
    if (static_cast<std::uint64_t>(frame) >
        static_cast<std::uint64_t>(current_frame) + kMaxFutureWindow) {
        return FrameStoreResult::Window;
    }
    if (current_frame > kFrameHistoryWindow && frame < current_frame - kFrameHistoryWindow) {
        return FrameStoreResult::Window;
    }
    const auto& slot = slots_[frame % slots_.size()];
    if (slot.state != FrameSlotState::Empty && slot.absolute_frame == frame) {
        if (slot.state == FrameSlotState::Consumed) return FrameStoreResult::StateViolation;
        return std::memcmp(&slot.operation, &operation, sizeof(operation)) == 0 &&
                       slot.state == state
            ? FrameStoreResult::Duplicate : FrameStoreResult::Conflict;
    }
    if (slot.state != FrameSlotState::Empty && slot.state != FrameSlotState::Consumed) {
        return FrameStoreResult::Capacity;
    }
    return FrameStoreResult::Inserted;
}

FrameStoreResult FrameStore::InsertBatchAtomic(std::uint32_t first_frame,
                                               const CustomFrameOpV2* operations,
                                               std::uint16_t count,
                                               std::uint32_t current_frame) {
    if (!operations || count == 0 || count > kMaxBatchRecords ||
        first_frame > UINT32_MAX - (count - 1u)) {
        return FrameStoreResult::Overflow;
    }
    std::array<FrameStoreResult, kMaxBatchRecords> decisions{};
    bool inserted = false;
    for (std::uint16_t index = 0; index < count; ++index) {
        decisions[index] = ValidateInsertAs(first_frame + index, operations[index],
                                            current_frame, insertion_state_);
        if (decisions[index] != FrameStoreResult::Inserted &&
            decisions[index] != FrameStoreResult::Duplicate) {
            return decisions[index];
        }
        inserted = inserted || decisions[index] == FrameStoreResult::Inserted;
    }
    for (std::uint16_t index = 0; index < count; ++index) {
        if (decisions[index] == FrameStoreResult::Inserted &&
            InsertAs(first_frame + index, operations[index], current_frame,
                     insertion_state_) != FrameStoreResult::Inserted) {
            return FrameStoreResult::StateViolation;
        }
    }
    return inserted ? FrameStoreResult::Inserted : FrameStoreResult::Duplicate;
}

FrameStoreResult FrameStore::Transition(std::uint32_t frame, FrameSlotState expected,
                                        FrameSlotState desired) {
    auto& slot = slots_[frame % slots_.size()];
    if (slot.absolute_frame != frame || slot.state != expected) {
        return FrameStoreResult::StateViolation;
    }
    slot.state = desired;
    AdvanceThrough();
    return FrameStoreResult::Inserted;
}

bool FrameStore::Contains(std::uint32_t frame) const {
    const auto& slot = slots_[frame % slots_.size()];
    return slot.absolute_frame == frame &&
           (slot.state == FrameSlotState::SealedLocal ||
            slot.state == FrameSlotState::ReceivedRemote);
}

const CustomFrameOpV2* FrameStore::Find(std::uint32_t frame) const {
    return Contains(frame) ? FindAny(frame) : nullptr;
}

const CustomFrameOpV2* FrameStore::FindAny(std::uint32_t frame) const {
    const auto& slot = slots_[frame % slots_.size()];
    return slot.state != FrameSlotState::Empty && slot.absolute_frame == frame
        ? &slot.operation : nullptr;
}

FrameSlotState FrameStore::StateOf(std::uint32_t frame) const {
    const auto& slot = slots_[frame % slots_.size()];
    return slot.absolute_frame == frame ? slot.state : FrameSlotState::Empty;
}

bool FrameStore::CanInsertAt(std::uint32_t frame) const {
    const auto& slot = slots_[frame % slots_.size()];
    return slot.state == FrameSlotState::Empty || slot.state == FrameSlotState::Consumed ||
           slot.absolute_frame == frame;
}

bool FrameStore::CanConsume(std::uint32_t frame) const {
    const auto& slot = slots_[frame % slots_.size()];
    return slot.absolute_frame == frame &&
           (slot.state == FrameSlotState::SealedLocal ||
            slot.state == FrameSlotState::ReceivedRemote);
}

FrameStoreResult FrameStore::Consume(std::uint32_t frame) {
    auto& slot = slots_[frame % slots_.size()];
    if (!CanConsume(frame)) {
        return FrameStoreResult::Missing;
    }
    slot.state = FrameSlotState::Consumed;
    return FrameStoreResult::Inserted;
}

std::uint32_t FrameStore::RetireBefore(std::uint32_t first_retained_frame) {
    std::uint32_t retired = 0;
    for (auto& slot : slots_) {
        if (slot.state != FrameSlotState::Empty && slot.absolute_frame < first_retained_frame) {
            slot = {};
            ++retired;
        }
    }
    return retired;
}

void FrameStore::AdvanceThrough() {
    std::uint32_t next = has_through_ ? through_ + 1u : first_frame_;
    if (has_through_ && through_ == UINT32_MAX) return;
    while (Contains(next)) {
        through_ = next;
        has_through_ = true;
        if (next == UINT32_MAX) break;
        ++next;
    }
}

void LocalFrameSealer::Reset(std::uint32_t first_frame) {
    store_.Reset(first_frame, FrameSlotState::PendingLocal);
    next_frame_ = first_frame;
    consumed_through_ = UINT32_MAX;
    pending_count_ = 0;
    next_queue_frame_ = first_frame;
    initialized_ = true;
    exhausted_ = false;
}

FrameStoreResult LocalFrameSealer::Queue(std::uint32_t frame,
                                         const CustomFrameOpV2& operation,
                                         std::uint32_t current_frame) {
    if (!initialized_) return FrameStoreResult::StateViolation;
    if (exhausted_ || frame < next_frame_) return FrameStoreResult::FrameOccupied;
    if (IsNoOp(operation) || !IsCanonicalOperation(operation)) {
        return FrameStoreResult::InvalidOperation;
    }
    if (!store_.CanInsertAt(frame)) return FrameStoreResult::Capacity;
    if (store_.StateOf(frame) != FrameSlotState::Empty) {
        return FrameStoreResult::FrameOccupied;
    }
    const auto result = store_.InsertAs(frame, operation, current_frame,
                                        FrameSlotState::PendingLocal);
    if (result == FrameStoreResult::Inserted) {
        ++pending_count_;
        next_queue_frame_ = (std::max)(next_queue_frame_,
                                       static_cast<std::uint64_t>(frame) + 1u);
    }
    return result;
}

FrameStoreResult LocalFrameSealer::QueueNext(std::uint32_t earliest_frame,
                                              const CustomFrameOpV2& operation,
                                              std::uint32_t current_frame,
                                              std::uint32_t& assigned_frame) {
    assigned_frame = UINT32_MAX;
    if (!initialized_) return FrameStoreResult::StateViolation;
    if (exhausted_) return FrameStoreResult::Overflow;
    if (IsNoOp(operation) || !IsCanonicalOperation(operation)) {
        return FrameStoreResult::InvalidOperation;
    }
    const std::uint64_t candidate = (std::max)(
        (std::max)(static_cast<std::uint64_t>(earliest_frame),
                   static_cast<std::uint64_t>(next_frame_)),
        next_queue_frame_);
    if (candidate > UINT32_MAX) return FrameStoreResult::Overflow;
    assigned_frame = static_cast<std::uint32_t>(candidate);
    return Queue(assigned_frame, operation, current_frame);
}

FrameStoreResult LocalFrameSealer::PrepareBatch(std::uint32_t seal_target,
                                                SealedFrameBatch& output) const {
    output = {};
    if (!initialized_) return FrameStoreResult::StateViolation;
    if (exhausted_ || next_frame_ > seal_target) return FrameStoreResult::Missing;
    output.first_frame = next_frame_;
    std::uint32_t cursor = next_frame_;
    while (output.count < kMaxBatchRecords && cursor <= seal_target) {
        if (!store_.CanInsertAt(cursor)) {
            output = {};
            return FrameStoreResult::Capacity;
        }
        const FrameSlotState state = store_.StateOf(cursor);
        if (state == FrameSlotState::PendingLocal) {
            const auto* operation = store_.FindAny(cursor);
            if (!operation) return FrameStoreResult::StateViolation;
            output.records[output.count] = *operation;
        } else if (state != FrameSlotState::Empty) {
            output = {};
            return FrameStoreResult::StateViolation;
        }
        ++output.count;
        if (cursor == UINT32_MAX) {
            if (cursor != seal_target) return FrameStoreResult::Overflow;
            break;
        }
        ++cursor;
    }
    return FrameStoreResult::Inserted;
}

FrameStoreResult LocalFrameSealer::CommitBatch(const SealedFrameBatch& batch) {
    if (!initialized_ || exhausted_ || batch.first_frame != next_frame_ || batch.count == 0 ||
        batch.count > kMaxBatchRecords) return FrameStoreResult::StateViolation;
    for (std::uint16_t index = 0; index < batch.count; ++index) {
        const std::uint32_t frame = batch.first_frame + index;
        if (frame < batch.first_frame) return FrameStoreResult::Overflow;
        const FrameSlotState state = store_.StateOf(frame);
        const auto* existing = store_.FindAny(frame);
        if (state == FrameSlotState::PendingLocal) {
            if (!existing || std::memcmp(existing, &batch.records[index], sizeof(*existing)) != 0) {
                return FrameStoreResult::Conflict;
            }
        } else if (state != FrameSlotState::Empty || !IsNoOp(batch.records[index]) ||
                   !store_.CanInsertAt(frame)) {
            return state == FrameSlotState::Empty ? FrameStoreResult::Capacity
                                                   : FrameStoreResult::StateViolation;
        }
    }
    for (std::uint16_t index = 0; index < batch.count; ++index) {
        const std::uint32_t frame = batch.first_frame + index;
        if (store_.StateOf(frame) == FrameSlotState::PendingLocal) {
            if (store_.Transition(frame, FrameSlotState::PendingLocal,
                                  FrameSlotState::SealedLocal) != FrameStoreResult::Inserted) {
                return FrameStoreResult::StateViolation;
            }
            --pending_count_;
        } else if (store_.InsertAs(frame, batch.records[index], batch.first_frame,
                                   FrameSlotState::SealedLocal) != FrameStoreResult::Inserted) {
            return FrameStoreResult::StateViolation;
        }
    }
    const std::uint32_t last = batch.first_frame + batch.count - 1u;
    if (last == UINT32_MAX) exhausted_ = true;
    else next_frame_ = last + 1u;
    return FrameStoreResult::Inserted;
}

FrameStoreResult LocalFrameSealer::Consume(std::uint32_t frame) {
    const auto result = store_.Consume(frame);
    if (result == FrameStoreResult::Inserted) consumed_through_ = frame;
    return result;
}

ReadyStallResult ReadyStallTracker::Observe(bool ready, std::uint64_t now_ms) {
    if (ready) {
        if (!active_) return ReadyStallResult::Ready;
        last_duration_ms_ = now_ms - started_ms_;
        active_ = false;
        return ReadyStallResult::Recovered;
    }
    if (!active_) {
        active_ = true;
        retries_ = 1;
        started_ms_ = now_ms;
        last_duration_ms_ = 0;
        return ReadyStallResult::Started;
    }
    if (retries_ != UINT32_MAX) ++retries_;
    return CurrentDuration(now_ms) >= kReadyStallTimeoutMs
        ? ReadyStallResult::TimedOut : ReadyStallResult::Waiting;
}

void ReadyStallTracker::Reset() {
    active_ = false;
    retries_ = 0;
    started_ms_ = 0;
    last_duration_ms_ = 0;
}

std::uint64_t ReadyStallTracker::CurrentDuration(std::uint64_t now_ms) const {
    return active_ ? now_ms - started_ms_ : 0;
}

} // namespace isaac_sync
