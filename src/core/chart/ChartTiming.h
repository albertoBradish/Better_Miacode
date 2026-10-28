#pragma once
#include "core/chart/model/TimelineData.h"
#include <algorithm>

namespace miacode::chart {
inline TimelineTimingPoint timingAt(const QVector<TimelineTimingPoint>& points, double second)
{
    if (points.isEmpty()) return {};
    auto it = std::upper_bound(points.cbegin(), points.cend(), second,
        [](double time, const TimelineTimingPoint& point) { return time < point.second; });
    if (it != points.cbegin()) --it;
    TimelineTimingPoint state = *it;
    state.quarterBeat += (second - state.second) * state.bpm / 60.0;
    state.second = second;
    return state;
}
} // namespace miacode::chart
