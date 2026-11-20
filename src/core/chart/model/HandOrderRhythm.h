#pragma once

#include "core/chart/model/TimelineData.h"
#include <cmath>

namespace miacode::chart {
// A real-time ceiling for a soft alternation prior, not a nominal Simai
// denominator or a hard player-ability limit.
inline constexpr double handOrderMaximumInteractionGapSeconds = 0.2;
inline constexpr double handOrderFastRepeatGapSeconds = 0.13;
inline constexpr double handOrderMaximumReboundGapSeconds = 0.43;

inline bool handOrderHasStarStrike(const TimelineNoteMarker& marker)
{
    return !marker.isMine && !marker.headMine && marker.hasHeadStar
        && (marker.type == QLatin1String("slide") || marker.type == QLatin1String("wifi"));
}

inline bool handOrderIsButtonStrike(const TimelineNoteMarker& marker)
{
    return marker.lane >= 1 && marker.lane <= 8 && !marker.isMine
        && (marker.type == QLatin1String("tap") || handOrderHasStarStrike(marker));
}

inline bool handOrderIsEighthOrSlower(double gap, const TimelineNoteMarker& marker)
{
    return marker.tempoBpm > 0.0 && gap * marker.tempoBpm >= 27.0 - 1e-8;
}

inline bool handOrderFastRepeatStep(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    const double gap = second.second - first.second;
    return gap >= 0.025 - 1e-8 && gap <= handOrderFastRepeatGapSeconds + 1e-8
        && !handOrderIsEighthOrSlower(gap, first);
}

inline bool handOrderSameKeyRebound(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    const double gap = second.second - first.second;
    // Use the actual interval across timing controls and empty subdivisions.
    return handOrderIsButtonStrike(first) && handOrderIsButtonStrike(second)
        && first.lane == second.lane && gap >= 0.075 - 1e-8
        && gap <= handOrderMaximumReboundGapSeconds + 1e-8 && gap * first.tempoBpm < 60.0 - 1e-8
        && (gap >= 0.12 - 1e-8 || handOrderIsEighthOrSlower(gap, first));
}

inline double handOrderGridSeconds(const TimelineNoteMarker& marker)
{
    return std::isfinite(marker.tempoBpm) && marker.tempoBpm > 0 && marker.subdivision > 0
        ? 240.0 / (marker.tempoBpm * marker.subdivision) : 0;
}
inline double handOrderPseudoEachTolerance(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    Q_UNUSED(first); Q_UNUSED(second);
    // Inputs inside a modeled short 25ms contact are near-simultaneous. The
    // exact boundary remains sequential; no BPM or written denominator enters.
    return 0.025;
}
inline bool handOrderRegularInputStep(const TimelineNoteMarker& first, const TimelineNoteMarker& second,
    bool requireFast = false)
{
    const double grid = handOrderGridSeconds(first);
    const double gap = second.second - first.second;
    return grid > 0 && handOrderGridSeconds(second) > 0
        && (!requireFast || grid <= handOrderMaximumInteractionGapSeconds + 1e-8)
        && gap >= grid * 0.9 - 1e-8 && gap <= grid * 1.1 + 1e-8;
}
inline bool handOrderLongitudinalStep(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    return handOrderIsButtonStrike(first) && handOrderIsButtonStrike(second) && first.lane == second.lane
        && handOrderFastRepeatStep(first, second)
        && handOrderRegularInputStep(first, second, true);
}
} // namespace miacode::chart
