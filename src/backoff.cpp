#include "tickstream/backoff.hpp"

#include <algorithm>

namespace tickstream {

Backoff::Backoff(std::chrono::seconds initial, std::chrono::seconds max)
    : initial_(initial), max_(max), current_(initial) {}

std::chrono::seconds Backoff::next_delay() {
    const std::chrono::seconds delay = current_;
    current_ = std::min(current_ * 2, max_);
    return delay;
}

void Backoff::reset() {
    current_ = initial_;
}

}  // namespace tickstream
