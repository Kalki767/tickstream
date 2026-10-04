#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tickstream {

// A fixed-capacity multi-producer multi-consumer queue (mutex + condition
// variables) that connects the network thread to the database writer thread.
//
// Storage is a ring buffer allocated once in the constructor, so memory use is
// fixed and known at startup, and push/pop never allocate (std::deque would
// allocate a new node every few elements). T must be default-constructible
// and move-assignable.
//
// Two push flavours, because the two sources need opposite full-queue policies:
//   try_push: never blocks; if full, the item is dropped and counted. Used by
//             the live feed, where blocking would stall the event loop.
//   push:     blocks until there is room. Used by replay benchmarks, where
//             dropping would make a slow writer look fast.
//
// close() ends the stream: pushes fail from then on, and pops drain what is
// left and then report closed.
template <typename T>
class BoundedQueue {
public:
    enum class PopStatus { item, timeout, closed };

    explicit BoundedQueue(std::size_t capacity) : buffer_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be > 0");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Taking T by value lets the caller choose: pass an lvalue to copy, or
    // std::move it to move. Either way it is then moved into the buffer.
    bool try_push(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            if (count_ == buffer_.size()) {
                ++dropped_;
                return false;
            }
            emplace_locked(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    // Returns false only if the queue was closed.
    bool push(T value) {
        {
            std::unique_lock lock(mutex_);
            not_full_.wait(lock, [&] { return closed_ || count_ < buffer_.size(); });
            if (closed_) {
                return false;
            }
            emplace_locked(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available, or the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || count_ > 0; });
        if (count_ == 0) {
            return std::nullopt;  // closed and drained
        }
        std::optional<T> value(take_locked());
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    // Like pop(), but gives up at `deadline`. On PopStatus::item, `out` holds
    // the item.
    PopStatus pop_until(T& out, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        if (!not_empty_.wait_until(lock, deadline, [&] { return closed_ || count_ > 0; })) {
            return PopStatus::timeout;
        }
        if (count_ == 0) {
            return PopStatus::closed;
        }
        out = take_locked();
        lock.unlock();
        not_full_.notify_one();
        return PopStatus::item;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.size(); }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    // Items rejected by try_push because the queue was full.
    [[nodiscard]] std::uint64_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

    // The largest size() ever reached.
    [[nodiscard]] std::size_t high_water_mark() const {
        std::lock_guard lock(mutex_);
        return high_water_mark_;
    }

private:
    void emplace_locked(T&& value) {
        buffer_[(head_ + count_) % buffer_.size()] = std::move(value);
        ++count_;
        if (count_ > high_water_mark_) {
            high_water_mark_ = count_;
        }
    }

    T take_locked() {
        T value = std::move(buffer_[head_]);
        head_ = (head_ + 1) % buffer_.size();
        --count_;
        return value;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::vector<T> buffer_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t high_water_mark_ = 0;
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

}  // namespace tickstream
