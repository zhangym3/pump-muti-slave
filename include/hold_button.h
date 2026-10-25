#pragma once
#include "protocol.h"

namespace pump {
// Require a stable release after boot and between presses. Release is immediate;
// only opening is debounced, so contact bounce can never extend an opening.
class HoldButton {
 public:
  enum class Event { None, Press, Release };
  explicit HoldButton(uint32_t debounceMs = 30) : debounceMs_(debounceMs) {}
  Event update(bool pressed, uint32_t now) {
    if (!sampled_) {
      sampled_ = true; candidate_ = pressed; changedAt_ = now;
      return Event::None;
    }
    if (pressed != candidate_) {
      candidate_ = pressed; changedAt_ = now;
      if (!pressed) armed_ = false;
    }
    if (!pressed) {
      const bool released = held_;
      held_ = false;
      if (elapsed(now, changedAt_, debounceMs_)) armed_ = true;
      return released ? Event::Release : Event::None;
    }
    if (armed_ && elapsed(now, changedAt_, debounceMs_)) {
      armed_ = false; held_ = true;
      return Event::Press;
    }
    return Event::None;
  }
 private:
  uint32_t debounceMs_, changedAt_ = 0;
  bool sampled_ = false, candidate_ = false, armed_ = false, held_ = false;
};
}
