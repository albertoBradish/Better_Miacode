#include "core/chart/SlideJudgementTiming.h"
#include "core/chart/model/TimelineData.h"

#include <algorithm>
#include <cmath>

namespace miacode::slide_judge {
namespace {
constexpr double kCoincidenceEpsilon = 1e-6;
}

LaunchBeatIndex::LaunchBeatIndex(const QVector<TimelineNoteMarker>& markers)
{
    for (const auto& marker:markers) {
        if (!std::isfinite(marker.second)) continue;
        int lane=marker.lane;
        bool playable=false;
        if (marker.type==QLatin1String("tap") || marker.type==QLatin1String("hold")) {
            playable=!marker.isMine;
        } else if (marker.type==QLatin1String("slide") || marker.type==QLatin1String("wifi")) {
            playable=marker.hasHeadStar && !marker.headMine;
        } else if (marker.type==QLatin1String("touch") || marker.type==QLatin1String("touch_hold")) {
            const QString pad=marker.touchPad.trimmed().toUpper();
            if (pad.size()==2 && pad[0]==QLatin1Char('A') && pad[1]>=QLatin1Char('1') && pad[1]<=QLatin1Char('8')) {
                lane=pad[1].digitValue();
                playable=!marker.isMine;
            }
        }
        if (playable && lane>=1 && lane<=8) moments_[lane-1].append(marker.second);
    }
    for (auto& moments:moments_) {
        std::sort(moments.begin(),moments.end());
        moments.erase(std::unique(moments.begin(),moments.end()),moments.end());
    }
}

double LaunchBeatIndex::startSecondFor(const TimelineNoteMarker& marker) const
{
    if (marker.lane<1 || marker.lane>8 || !std::isfinite(marker.slideTraceSecond)
        || marker.slideTraceSecond<=marker.second+kCoincidenceEpsilon) return marker.second;
    const auto& moments=moments_[marker.lane-1];
    const auto it=std::lower_bound(moments.cbegin(),moments.cend(),marker.slideTraceSecond-kCoincidenceEpsilon);
    return it!=moments.cend() && std::abs(*it-marker.slideTraceSecond)<=kCoincidenceEpsilon
        ? *it : marker.second;
}

Timing timingForMarker(const TimelineNoteMarker& marker, const LaunchBeatIndex* launchBeats)
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
    result.brightnessStartSecond = launchBeats ? launchBeats->startSecondFor(marker) : head;
    result.usesLaunchBeat = result.brightnessStartSecond>head+kCoincidenceEpsilon;
    result.traceSecond = trace;
    result.endSecond = end;
    result.judgementSecond = judgement;
    result.lastAreaDuration = tail;
    // WiFi retains the explicit symmetric reference model until cabinet data
    // establishes whether the normal slide's extra fast interval applies.
    result.perfectStartSecond = judgement - (wifi ? halfWidth : std::max(17.0 * frame, halfWidth));
    result.perfectEndSecond = judgement + halfWidth;
    result.brightnessCompleteSecond = std::max(result.brightnessStartSecond, result.perfectStartSecond);
    return result;
}

double brightnessProgress(const Timing& timing, double chartSecond)
{
    if (!timing.valid || !std::isfinite(chartSecond)
        || !std::isfinite(timing.brightnessStartSecond) || !std::isfinite(timing.brightnessCompleteSecond)) return 0.0;
    if (chartSecond < timing.brightnessStartSecond) return 0.0;
    if (timing.brightnessCompleteSecond <= timing.brightnessStartSecond) return 1.0;
    return std::clamp((chartSecond - timing.brightnessStartSecond) / (timing.brightnessCompleteSecond - timing.brightnessStartSecond), 0.0, 1.0);
}

} // namespace miacode::slide_judge
