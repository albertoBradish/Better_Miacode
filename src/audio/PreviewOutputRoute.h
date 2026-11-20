#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Transient per-mixer output, independent of persisted mix and export settings.
// A single atomic snapshot keeps both stereo gains coherent on the audio thread.
class PreviewOutputRoute {
public:
    void set(double gain, double pan) {
        gain = std::isfinite(gain) ? std::clamp(gain, 0.0, 1.0) : 0.0;
        pan = std::isfinite(pan) ? std::clamp(pan, -1.0, 1.0) : 0.0;
        const auto left = uint32_t(65535 * gain * std::min(1.0, 1.0 - pan));
        const auto right = uint32_t(65535 * gain * std::min(1.0, 1.0 + pan));
        gains_.store((left << 16) | right, std::memory_order_relaxed);
    }
    void process(float* samples, size_t count) {
        const auto gains = gains_.load(std::memory_order_relaxed);
        if (gains != target_) {
            target_ = gains;
            leftTarget_ = float(gains >> 16) / 65535.f;
            rightTarget_ = float(gains & 0xffff) / 65535.f;
            remaining_ = initialized_ ? 128 : 0;
            if (!remaining_) { left_ = leftTarget_; right_ = rightTarget_; }
        }
        initialized_ = true;
        if (!remaining_ && gains == 0xffffffffu) return;
        if (!remaining_ && !gains) { std::memset(samples, 0, count * sizeof(float)); return; }
        for (size_t i = 0; i + 1 < count; i += 2) {
            if (remaining_) {
                left_ += (leftTarget_ - left_) / remaining_;
                right_ += (rightTarget_ - right_) / remaining_;
                --remaining_;
            }
            samples[i] = limited(samples[i]) * left_;
            samples[i+1] = limited(samples[i+1]) * right_;
        }
    }
private:
    static float limited(float sample) {
        if (!std::isfinite(sample)) return 0;
        const float excess = std::abs(sample) - 0.9f;
        return excess > 0 ? std::copysign(1.f - 0.1f / (1.f + 10.f * excess), sample) : sample;
    }
    // Only the DSP callback touches the ramp; the UI publishes the next destination atomically.
    uint32_t target_ = 0xffffffffu;
    float left_ = 1, right_ = 1, leftTarget_ = 1, rightTarget_ = 1;
    unsigned remaining_ = 0;
    bool initialized_ = false;
    std::atomic<uint32_t> gains_{0xffffffffu};
};
