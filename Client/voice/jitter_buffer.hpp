// One talker's incoming frames, in order, with the delay that network jitter needs. It only sorts:
// it has no codec, so tests can run it on any numbers. Adapts between 40 and 120 ms.
#ifndef KCDS_VOICE_JITTER_BUFFER_HPP
#define KCDS_VOICE_JITTER_BUFFER_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace kcds::voice {

struct JitterFrame {
  // Nothing arrived for this slot: conceal it.
  bool missing = true;
  // What arrived. With `next` set, the following frame can repair this one (Opus FEC).
  std::vector<std::uint8_t> data;
  const std::vector<std::uint8_t>* next = nullptr;
};

class JitterBuffer {
 public:
  static constexpr int kFrameMs = 20;
  static constexpr int kMinFrames = 2;
  static constexpr int kMaxFrames = 6;
  // Slots kept; more than this behind means the talker started anew.
  static constexpr std::size_t kSlots = 64;

  // A frame by the talker's 16-bit sequence. False when it came too late for its slot.
  bool push(std::uint16_t sequence, const std::uint8_t* data, std::size_t size) {
    if (!started_) {
      started_ = true;
      next_ = sequence;
      buffering_ = true;
    }
    const std::int16_t ahead = static_cast<std::int16_t>(sequence - next_);
    if (ahead < 0) {
      late_ += 1;
      // A run of late frames means the delay is too small.
      if (target_ < kMaxFrames && ++late_run_ >= 2) {
        target_ += 1;
        late_run_ = 0;
      }
      return false;
    }
    if (static_cast<std::size_t>(ahead) >= kSlots) {
      // A long silence or a new start: follow the talker.
      reset_to(sequence);
    }
    Slot& slot = slots_[sequence % kSlots];
    slot.sequence = sequence;
    slot.full = true;
    slot.data.assign(data, data + size);
    const int held = static_cast<int>(static_cast<std::int16_t>(sequence - next_)) + 1;
    if (held > newest_held_) {
      newest_held_ = held;
    }
    return true;
  }

  // Whether a block is due from this talker now: it has started and has built its delay.
  bool ready() {
    if (!started_) {
      return false;
    }
    if (buffering_ && newest_held_ >= target_) {
      buffering_ = false;
    }
    return !buffering_;
  }

  // The next slot, each 20 ms while ready.
  JitterFrame pop() {
    JitterFrame frame;
    Slot& slot = slots_[next_ % kSlots];
    if (slot.full && slot.sequence == next_) {
      frame.missing = false;
      frame.data = std::move(slot.data);
      slot.data.clear();
      slot.full = false;
      calm_ += 1;
      // A long quiet stretch without lateness can give the delay back.
      if (calm_ >= 250 && target_ > kMinFrames) {
        target_ -= 1;
        calm_ = 0;
      }
    } else {
      calm_ = 0;
      const std::uint16_t after = static_cast<std::uint16_t>(next_ + 1);
      const Slot& following = slots_[after % kSlots];
      if (following.full && following.sequence == after) {
        frame.next = &following.data;
      }
    }
    next_ = static_cast<std::uint16_t>(next_ + 1);
    newest_held_ = std::max(0, newest_held_ - 1);
    return frame;
  }

  // Nothing held and nothing came for a while: the stream ended.
  bool empty() const {
    return newest_held_ == 0;
  }
  void restart() {
    started_ = false;
    buffering_ = true;
    newest_held_ = 0;
    for (Slot& slot : slots_) {
      slot.full = false;
    }
  }
  int target_frames() const {
    return target_;
  }
  std::uint64_t late() const {
    return late_;
  }

 private:
  struct Slot {
    std::uint16_t sequence = 0;
    bool full = false;
    std::vector<std::uint8_t> data;
  };

  void reset_to(std::uint16_t sequence) {
    for (Slot& slot : slots_) {
      slot.full = false;
    }
    next_ = sequence;
    newest_held_ = 0;
    buffering_ = true;
  }

  std::array<Slot, kSlots> slots_{};
  std::uint16_t next_ = 0;
  bool started_ = false;
  bool buffering_ = true;
  int newest_held_ = 0;
  int target_ = kMinFrames;
  int late_run_ = 0;
  int calm_ = 0;
  std::uint64_t late_ = 0;
};

}  // namespace kcds::voice

#endif
