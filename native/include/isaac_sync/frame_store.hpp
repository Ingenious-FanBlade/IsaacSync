#pragma once

#include "protocol.hpp"

#include <array>
#include <cstdint>

namespace isaac_sync {

constexpr std::size_t kFrameStoreCapacity = 2048;
constexpr std::uint64_t kReadyStallTimeoutMs = 5000;

enum class FrameSlotState : std::uint8_t {
    Empty = 0,
    PendingLocal = 1,
    SealedLocal = 2,
    ReceivedRemote = 3,
    Consumed = 4,
};

struct FrameSlotV2 {
    std::uint32_t absolute_frame = 0;
    FrameSlotState state = FrameSlotState::Empty;
    std::uint8_t reserved[3]{};
    CustomFrameOpV2 operation{};
};

static_assert(sizeof(FrameSlotV2) == 24, "frame slot layout changed");
static_assert(offsetof(FrameSlotV2, state) == 4, "frame slot state offset changed");
static_assert(offsetof(FrameSlotV2, operation) == 8, "frame slot operation offset changed");

enum class FrameStoreResult {
    Inserted,
    Duplicate,
    Missing,
    Conflict,
    Capacity,
    Window,
    Overflow,
    InvalidOperation,
    FrameOccupied,
    StateViolation,
};

struct SealedFrameBatch {
    std::uint32_t first_frame = 0;
    std::uint16_t count = 0;
    std::array<CustomFrameOpV2, kMaxBatchRecords> records{};
};

class FrameStore {
public:
    void Reset(std::uint32_t first_frame, FrameSlotState insertion_state);
    FrameStoreResult Insert(std::uint32_t frame, const CustomFrameOpV2& operation,
                            std::uint32_t current_frame);
    FrameStoreResult InsertAs(std::uint32_t frame, const CustomFrameOpV2& operation,
                              std::uint32_t current_frame, FrameSlotState state);
    FrameStoreResult InsertBatchAtomic(std::uint32_t first_frame,
                                       const CustomFrameOpV2* operations,
                                       std::uint16_t count,
                                       std::uint32_t current_frame);
    FrameStoreResult Transition(std::uint32_t frame, FrameSlotState expected,
                                FrameSlotState desired);
    bool Contains(std::uint32_t frame) const;
    const CustomFrameOpV2* Find(std::uint32_t frame) const;
    const CustomFrameOpV2* FindAny(std::uint32_t frame) const;
    FrameSlotState StateOf(std::uint32_t frame) const;
    bool CanInsertAt(std::uint32_t frame) const;
    bool CanConsume(std::uint32_t frame) const;
    FrameStoreResult Consume(std::uint32_t frame);
    // Records before activation were exchanged only to warm the sidecar and
    // are never consumed by the ACTIVE ready gate. Free their ring slots.
    std::uint32_t RetireBefore(std::uint32_t first_retained_frame);

    bool HasThrough() const { return has_through_; }
    std::uint32_t Through() const { return through_; }
    std::uint32_t DisplayThrough() const { return has_through_ ? through_ : UINT32_MAX; }

private:
    FrameStoreResult ValidateInsertAs(std::uint32_t frame,
                                      const CustomFrameOpV2& operation,
                                      std::uint32_t current_frame,
                                      FrameSlotState state) const;
    void AdvanceThrough();

    std::array<FrameSlotV2, kFrameStoreCapacity> slots_{};
    FrameSlotState insertion_state_ = FrameSlotState::Empty;
    std::uint32_t first_frame_ = 0;
    std::uint32_t through_ = 0;
    bool has_through_ = false;
};

class LocalFrameSealer {
public:
    void Reset(std::uint32_t first_frame);
    FrameStoreResult Queue(std::uint32_t frame, const CustomFrameOpV2& operation,
                           std::uint32_t current_frame);
    FrameStoreResult QueueNext(std::uint32_t earliest_frame,
                               const CustomFrameOpV2& operation,
                               std::uint32_t current_frame,
                               std::uint32_t& assigned_frame);
    FrameStoreResult PrepareBatch(std::uint32_t seal_target, SealedFrameBatch& output) const;
    FrameStoreResult CommitBatch(const SealedFrameBatch& batch);
    bool Contains(std::uint32_t frame) const { return store_.Contains(frame); }
    const CustomFrameOpV2* Find(std::uint32_t frame) const { return store_.Find(frame); }
    FrameStoreResult Consume(std::uint32_t frame);
    bool CanConsume(std::uint32_t frame) const { return store_.CanConsume(frame); }
    std::uint32_t RetireBefore(std::uint32_t first_retained_frame) {
        return store_.RetireBefore(first_retained_frame);
    }
    std::uint32_t SealedThrough() const { return store_.DisplayThrough(); }
    std::uint32_t ConsumedThrough() const { return consumed_through_; }
    bool Initialized() const { return initialized_; }
    bool Exhausted() const { return exhausted_; }
    std::uint32_t NextUnsealedFrame() const { return next_frame_; }
    std::uint32_t PendingCount() const { return pending_count_; }

private:
    FrameStore store_{};
    std::uint32_t next_frame_ = 0;
    std::uint32_t consumed_through_ = UINT32_MAX;
    std::uint32_t pending_count_ = 0;
    std::uint64_t next_queue_frame_ = 0;
    bool initialized_ = false;
    bool exhausted_ = false;
};

enum class ReadyStallResult {
    Ready,
    Started,
    Waiting,
    Recovered,
    TimedOut,
};

class ReadyStallTracker {
public:
    ReadyStallResult Observe(bool ready, std::uint64_t now_ms);
    void Reset();
    bool Active() const { return active_; }
    std::uint32_t Retries() const { return retries_; }
    std::uint64_t CurrentDuration(std::uint64_t now_ms) const;
    std::uint64_t LastDuration() const { return last_duration_ms_; }

private:
    bool active_ = false;
    std::uint32_t retries_ = 0;
    std::uint64_t started_ms_ = 0;
    std::uint64_t last_duration_ms_ = 0;
};

FrameStoreResult ComputeSendTarget(std::uint32_t online_frame, std::uint16_t lead_frames,
                                   std::uint32_t next_unsealed_frame, bool exhausted,
                                   std::uint32_t& target);

} // namespace isaac_sync
