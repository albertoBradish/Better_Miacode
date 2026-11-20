#pragma once

#include <QtGlobal>

namespace miacode::chart {

// Integer BPM of an equivalent 16th-note sweep: 300 also means 32nds at 150 BPM.
inline constexpr int handOrderDefaultSweepThreshold = 160;
inline constexpr int handOrderMinSweepThreshold = 60;
inline constexpr int handOrderMaxSweepThreshold = 600;

inline double handOrderSweepGapSeconds(int threshold)
{
    return 15.0 / qBound(handOrderMinSweepThreshold, threshold, handOrderMaxSweepThreshold);
}

} // namespace miacode::chart
