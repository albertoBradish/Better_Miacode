#pragma once

#include "core/chart/model/TimelineData.h"
#include <algorithm>

namespace miacode::hand_order {
inline bool isLockingHold(const TimelineNoteMarker& marker)
{
    return (marker.type == "hold" || marker.type == "touch_hold") && marker.endSecond > marker.second;
}
inline bool concreteHoldHand(HandOrderHand hand)
{
    return hand == HandOrderHand::Left || hand == HandOrderHand::Right;
}
inline bool holdLocksHead(const TimelineNoteMarker& hold, const TimelineNoteMarker& note, HandOrderHand hand)
{
    if (!isLockingHold(hold) || !concreteHoldHand(hold.handOrderHead) || hand != hold.handOrderHead
        || note.parseOrder == hold.parseOrder || note.second < hold.second - 1e-8
        || note.second > hold.endSecond + 1e-8) return false;
    if ((note.type == "slide" || note.type == "wifi") && !note.hasHeadStar) return false;
    const bool touch = note.type == "touch" || note.type == "touch_hold";
    const bool samePad = hold.type == "touch_hold" ? touch && note.touchPad == hold.touchPad
        : !touch && note.lane == hold.lane;
    return !samePad;
}
inline bool respectsHoldLocks(const QVector<TimelineNoteMarker>& chart, const QVector<TimelineNoteMarker>& proposed)
{
    for (const auto& hold : chart) {
        if (!isLockingHold(hold) || !concreteHoldHand(hold.handOrderHead)) continue;
        for (const auto& note : proposed) {
            if (holdLocksHead(hold, note, note.handOrderHead)) return false;
            if (note.parseOrder == hold.parseOrder || (note.type != "slide" && note.type != "wifi")) continue;
            const auto conflict = [&](const QVector<HandOrderTrackStop>& stops) {
                for (int i = 0; i < stops.size(); ++i) {
                    if (stops.at(i).hand != hold.handOrderHead) continue;
                    const double duration = note.endSecond - note.slideTraceSecond;
                    const double first = note.slideTraceSecond + duration * stops.at(i).progress;
                    const double last = i + 1 < stops.size() ? note.slideTraceSecond + duration * stops.at(i + 1).progress : note.endSecond;
                    if (first <= hold.endSecond + 1e-8 && last >= hold.second - 1e-8) return true;
                }
                return false;
            };
            if (note.handOrderWifiMode == HandOrderWifiMode::Split) {
                for (const auto& sector : note.handOrderWifiSectorStops) if (conflict(sector)) return false;
            } else if (conflict(note.handOrderTrackStops)) return false;
        }
    }
    return true;
}
} // namespace miacode::hand_order
