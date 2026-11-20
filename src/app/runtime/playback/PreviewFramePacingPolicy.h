#pragma once

#include <QtTypes>

namespace miacode::runtime::preview_pacing {

inline qint64 nextLowPerformanceFrameDeadline(qint64 scheduled, qint64 now, qint64 interval)
{
    // Absorb small timer jitter without drifting. After a stall, schedule a full
    // interval from now instead of presenting overdue frames in a burst.
    const bool needsResync = scheduled < 0 || now - scheduled > interval / 2;
    return (needsResync ? now : scheduled) + interval;
}

}  // namespace miacode::runtime::preview_pacing
