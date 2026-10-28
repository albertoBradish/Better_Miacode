#include "core/chart/SlideJudgementTiming.h"
#include "core/chart/model/TimelineData.h"

#include <algorithm>
#include <cmath>

namespace miacode::slide_judge {

Timing timingForMarker(const TimelineNoteMarker& marker)
{
    Timing result;
    if (marker.type != QLatin1String("slide") && marker.type != QLatin1String("wifi")) return result;
    const double head = marker.second, trace = marker.slideTraceSecond, end = marker.endSecond;
    if (!std::isfinite(head) || !std::isfinite(trace) || !std::isfinite(end)
        || trace < head || end <= trace) return result;

    double duration = end - trace;
    double proportion = marker.wifiCriticalProportion;
    const bool wifi = marker.type == QLatin1String("wifi");
    if (!wifi) {
        const int count = marker.slideSegmentKeys.size();
        if (count == 0 || marker.slideSegmentDurations.size() != count
            || marker.slideSegmentCriticalProportions.size() != count
            || marker.slideSegmentShootSeconds.size() != count) return result;
        duration = marker.slideSegmentDurations.constLast();
        proportion = marker.slideSegmentCriticalProportions.constLast();
        if (!std::isfinite(marker.slideSegmentShootSeconds.constLast())
            || std::abs(marker.slideSegmentShootSeconds.constLast() + duration - end) > 1e-5) return result;
    }
    if (!std::isfinite(duration) || duration <= 0.0 || !std::isfinite(proportion)
        || proportion <= 0.0 || proportion >= 1.0) return result;

    const double tail = duration * (1.0 - proportion);
    const double judgement = end - tail;
    if (judgement <= head || judgement < trace || !std::isfinite(judgement)) return result;
    constexpr double frame = 1.0 / 60.0;
    const double halfWidth = std::min(36.0 * frame, 14.0 * frame + tail / 4.0);
    result.valid = true;
    result.wifiReference = wifi;
    result.headSecond = head;
    result.traceSecond = trace;
    result.endSecond = end;
    result.judgementSecond = judgement;
    result.lastAreaDuration = tail;
    // WiFi retains the explicit symmetric reference model until cabinet data
    // establishes whether the normal slide's extra fast interval applies.
    result.perfectStartSecond = judgement - (wifi ? halfWidth : std::max(17.0 * frame, halfWidth));
    result.perfectEndSecond = judgement + halfWidth;
    return result;
}

double brightnessProgress(const Timing& timing, double chartSecond)
{
    if (!timing.valid || !std::isfinite(chartSecond)
        || !std::isfinite(timing.headSecond) || !std::isfinite(timing.judgementSecond)
        || timing.judgementSecond <= timing.headSecond) return 0.0;
    return std::clamp((chartSecond - timing.headSecond) / (timing.judgementSecond - timing.headSecond), 0.0, 1.0);
}

} // namespace miacode::slide_judge
