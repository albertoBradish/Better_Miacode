#pragma once
#include <cmath>
#include <limits>

namespace miacode::preview_video {
// Presentation-only gate. Audio, decoding and offline export retain their clocks.
class FrameRateGate {
public:
    void reset() { next_ = std::numeric_limits<double>::quiet_NaN(); last_ = next_; }
    bool accept(double pts, double limit, double speed, bool bypass) {
        if (bypass || !std::isfinite(pts) || !std::isfinite(limit) || limit <= 0.0) {
            reset();
            return true;
        }
        const double interval = (std::isfinite(speed) && speed > 0.0 ? speed : 1.0) / limit;
        if (!std::isfinite(next_) || pts < last_ || pts - next_ > interval * 4.0) next_ = pts;
        last_ = pts;
        if (pts + 1e-6 < next_) return false;
        // Advance the sampling grid without accumulating source-frame rounding.
        next_ += (std::floor((pts - next_ + 1e-6) / interval) + 1.0) * interval;
        return true;
    }
private:
    double next_ = std::numeric_limits<double>::quiet_NaN();
    double last_ = std::numeric_limits<double>::quiet_NaN();
};
} // namespace miacode::preview_video
