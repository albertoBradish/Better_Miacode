#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <cstdint>

namespace miacode::task {
// One worker's cooperative pause and idle duty cycle. Waiting keeps the full
// algorithm stack alive. Cancellation must call wake() after raising its flag.
class ExecutionControl final {
public:
    void setPaused(bool value)
    {
        { std::lock_guard lock(mutex_); if (pauseRequested_ != value) ++estimateEpoch_; pauseRequested_ = value; }
        condition_.notify_all();
    }
    bool pauseRequested() const
    {
        std::lock_guard lock(mutex_); return pauseRequested_;
    }
    void setBackground(bool value)
    {
        { std::lock_guard lock(mutex_); if (background_ != value) ++estimateEpoch_; background_ = value; workStart_ = Clock::now(); }
        condition_.notify_all();
    }
    bool background() const
    {
        std::lock_guard lock(mutex_); return background_;
    }
    std::uint64_t estimateEpoch() const { return estimateEpoch_.load(std::memory_order_relaxed); }
    void wake()
    {
        // Pair the external cancellation store with the wait predicate's lock
        // so notification cannot race between its check and entering wait().
        { std::lock_guard lock(mutex_); }
        condition_.notify_all();
    }
    bool waitUntilPaused(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return suspended_; });
    }
    void checkpoint(const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (cancelled->load(std::memory_order_relaxed)) return;
            if (pauseRequested_) {
                suspended_ = true; condition_.notify_all();
                condition_.wait(lock, [&] {
                    return !pauseRequested_ || cancelled->load(std::memory_order_relaxed);
                });
                suspended_ = false; workStart_ = Clock::now(); condition_.notify_all();
                continue;
            }
            if (background_ && Clock::now() - workStart_ >= std::chrono::milliseconds(3)) {
                // Roughly 20% of one core; checkpoint spacing and scheduling
                // make this a cooperative target rather than a real-time cap.
                condition_.wait_for(lock, std::chrono::milliseconds(12), [&] {
                    return pauseRequested_ || !background_ || cancelled->load(std::memory_order_relaxed);
                });
                workStart_ = Clock::now();
                continue;
            }
            return;
        }
    }
private:
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool pauseRequested_ = false, suspended_ = false, background_ = false;
    std::atomic<std::uint64_t> estimateEpoch_{1};
    Clock::time_point workStart_ = Clock::now();
};
} // namespace miacode::task
