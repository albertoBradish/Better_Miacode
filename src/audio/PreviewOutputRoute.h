#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// A transient stereo route after the preview mixer, before its playback buffer.
class PreviewOutputRoute {
public:
    static constexpr unsigned kBufferMs = 24;

    explicit PreviewOutputRoute(bool buffered = false) : buffered_(buffered) {}
    bool buffered() const { return buffered_; }

    void set(double gain, double pan) { publish(gain, pan, false); }
    uint64_t fadeIn(double gain, double pan) { return publish(gain, pan, true); }
    void releaseFade(uint64_t request) { releasedRequest_.store(request, std::memory_order_release); }

    void attach(unsigned sampleRate, double queuedSeconds)
    {
        const auto tailFrames = static_cast<unsigned>(std::ceil(sampleRate * queuedSeconds));
        timing_.store((uint64_t(sampleRate) << 32) | tailFrames, std::memory_order_release);
    }

    void detach() { timing_.store(0, std::memory_order_release); }

    bool drained() const
    {
        if (!timing_.load(std::memory_order_acquire)) return true;
        const auto request = request_.load(std::memory_order_acquire);
        return drainedRequest_.load(std::memory_order_acquire) == request;
    }

    void process(float* samples, std::size_t count)
    {
        const auto request = request_.load(std::memory_order_acquire);
        const auto timing = timing_.load(std::memory_order_acquire);
        if (request != appliedRequest_ || timing != appliedTiming_) {
            appliedRequest_ = request;
            appliedTiming_ = timing;
            const auto gains = static_cast<uint32_t>(request);
            leftTarget_ = float(gains >> 16) / 65535.f;
            rightTarget_ = float(gains & 0xffff) / 65535.f;
            const auto rate = timing ? static_cast<unsigned>(timing >> 32) : 48000u;
            remaining_ = rate * 24 / 1000;
            tailFrames_ = static_cast<unsigned>(timing);
            acknowledgedSilence_ = false;
            waitingForSignal_ = (request & kSignalFlag) != 0;
            if (waitingForSignal_) {
                left_ = right_ = 0;
            } else if (!initialized_) {
                left_ = leftTarget_;
                right_ = rightTarget_;
                remaining_ = 0;
            }
        }
        initialized_ = true;
        const auto gains = static_cast<uint32_t>(request);
        if (waitingForSignal_ && releasedRequest_.load(std::memory_order_acquire) != request) {
            std::memset(samples, 0, count * sizeof(float));
            return;
        }
        if (!remaining_ && gains == 0xffffffffu) return;
        if (!remaining_ && !gains) {
            std::memset(samples, 0, count * sizeof(float));
            noteSilence(request, count / 2);
            return;
        }
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            if (waitingForSignal_) {
                // The mixer also renders silence while a source is still seeking.
                waitingForSignal_ = std::abs(samples[i]) < 1e-6f
                    && std::abs(samples[i + 1]) < 1e-6f;
            }
            if (remaining_ && !waitingForSignal_) {
                left_ += (leftTarget_ - left_) / remaining_;
                right_ += (rightTarget_ - right_) / remaining_;
                --remaining_;
            }
            samples[i] = scaled(samples[i], left_);
            samples[i + 1] = scaled(samples[i + 1], right_);
            if (!remaining_ && !gains) {
                noteSilence(request, (count - i) / 2);
                std::memset(samples + i, 0, (count - i) * sizeof(float));
                break;
            }
        }
    }

private:
    static constexpr uint64_t kSignalFlag = uint64_t(1) << 63;

    uint64_t publish(double gain, double pan, bool waitForSignal)
    {
        gain = std::isfinite(gain) ? std::clamp(gain, 0.0, 1.0) : 0.0;
        pan = std::isfinite(pan) ? std::clamp(pan, -1.0, 1.0) : 0.0;
        const auto left = uint32_t(65535 * gain * std::min(1.0, 1.0 - pan));
        const auto right = uint32_t(65535 * gain * std::min(1.0, 1.0 + pan));
        const uint32_t gains = (left << 16) | right;
        const auto previous = request_.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(previous) == gains && !waitForSignal) return previous;
        const auto sequence = ((previous >> 32) + 1) & 0x7fffffffu;
        const auto request = (sequence << 32) | gains | (waitForSignal && gains ? kSignalFlag : 0);
        request_.store(request, std::memory_order_release);
        return request;
    }

    void noteSilence(uint64_t request, std::size_t frames)
    {
        if (acknowledgedSilence_) return;
        if (tailFrames_ > frames) {
            tailFrames_ -= static_cast<unsigned>(frames);
        } else {
            tailFrames_ = 0;
            acknowledgedSilence_ = true;
            drainedRequest_.store(request, std::memory_order_release);
        }
    }

    static float scaled(float sample, float gain)
    {
        if (!std::isfinite(sample)) return 0;
        const float excess = std::abs(sample) - 0.9f;
        if (excess <= 0 || gain == 1) return sample * gain;
        const float limited = std::copysign(1.f - 0.1f / (1.f + 10.f * excess), sample);
        // Mix routes reserve at least half the output headroom. Blend back to
        // the unity path during an exclusive fade to keep its endpoint smooth.
        return std::lerp(limited, sample, std::max(0.f, 2.f * gain - 1.f)) * gain;
    }

    const bool buffered_;
    std::atomic<uint64_t> request_{0xffffffffu};
    std::atomic<uint64_t> timing_{0};
    std::atomic<uint64_t> drainedRequest_{0};
    std::atomic<uint64_t> releasedRequest_{0};

    // Only the mixer callback touches the current gains and frame countdowns.
    uint64_t appliedRequest_ = 0xffffffffu;
    uint64_t appliedTiming_ = 0;
    float left_ = 1, right_ = 1, leftTarget_ = 1, rightTarget_ = 1;
    unsigned remaining_ = 0, tailFrames_ = 0;
    bool initialized_ = false, waitingForSignal_ = false, acknowledgedSilence_ = false;
};
