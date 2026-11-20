#pragma once

#include "core/analysis/hand_order/HandOrderFullReplay.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace miacode::hand_order {

inline bool holdHasRequiredBody(const TimelineNoteMarker& marker)
{
    const bool touch = marker.type == QStringLiteral("touch_hold");
    if (!touch && marker.type != QStringLiteral("hold")) return false;
    const double headIgnore = touch ? FullReplayProfile::touchHoldHeadIgnoreSeconds
                                    : FullReplayProfile::holdHeadIgnoreSeconds;
    return marker.endSecond - marker.second > headIgnore
        + FullReplayProfile::holdTailIgnoreSeconds + 1e-8;
}

// Default analysis always holds through the native tail, including short Holds.
// Tap-like short-Hold releases belong to explicit special-play candidates only.
inline QVector<ContactAction> holdReleaseCandidates(const ContactAction& full,
    const TimelineNoteMarker& marker, double pressSeconds)
{
    Q_UNUSED(marker); Q_UNUSED(pressSeconds);
    return {full};
}

} // namespace miacode::hand_order
