#pragma once
#include "core/chart/model/TimelineData.h"
#include <QSet>
#include <algorithm>

namespace miacode::chart {
enum class ScoringKind { Tap, Hold, Star, Slide, Touch, TouchHold };
struct ScoringEvent {
    double second = 0.0;
    ScoringKind kind = ScoringKind::Tap;
    bool isBreak = false;
    bool isEx = false;
    bool isFirework = false;
    bool exTailCanDegrade = false; // DX holds longer than the 18-frame head/tail grace.
    int baseScore() const {
        if (isBreak) return 2500;
        if (kind == ScoringKind::Slide) return 1500;
        if (kind == ScoringKind::Hold || kind == ScoringKind::TouchHold) return 1000;
        return 500;
    }
};
struct ScoringEvents {
    QVector<ScoringEvent> events;
    bool hasUnofficialExtensions = false;
    int mineCount = 0;
};
inline ScoringEvents buildScoringEvents(const QVector<TimelineNoteMarker>& markers)
{
    ScoringEvents result;
    QSet<QString> heads;
    for (const auto& marker : markers) {
        const QString type = marker.type.toLower();
        if (marker.isMine || marker.headMine || marker.trackMine)
            result.hasUnofficialExtensions = true;
        const bool slide = type == QLatin1String("slide") || type == QLatin1String("wifi");
        if (slide) {
            if (marker.hasHeadStar) {
                const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                    .arg(marker.second, 0, 'g', 17).arg(marker.lane)
                    .arg(marker.sourceLine).arg(marker.sourceCol).arg(marker.eachGroupId);
                if (!heads.contains(key)) {
                    heads.insert(key);
                    if (marker.headMine) ++result.mineCount;
                    else result.events.append({marker.second, marker.slideHeadUsesTapMaterial
                        ? ScoringKind::Tap : ScoringKind::Star, marker.headBreak, marker.headEx, false});
                }
            }
            if (marker.trackMine) ++result.mineCount;
            else result.events.append({marker.endSecond > marker.second ? marker.endSecond
                : (marker.slideTraceSecond > marker.second ? marker.slideTraceSecond : marker.second),
                ScoringKind::Slide, marker.trackBreak, false, false});
            continue;
        }
        if (marker.isMine) { ++result.mineCount; continue; }
        ScoringKind kind;
        if (type == QLatin1String("tap")) kind = marker.tapUsesStarMaterial ? ScoringKind::Star : ScoringKind::Tap;
        else if (type == QLatin1String("hold")) kind = ScoringKind::Hold;
        else if (type == QLatin1String("touch")) kind = ScoringKind::Touch;
        else if (type == QLatin1String("touch_hold")) kind = ScoringKind::TouchHold;
        else { result.hasUnofficialExtensions = true; continue; }
        const bool hold = kind == ScoringKind::Hold || kind == ScoringKind::TouchHold;
        result.events.append({hold ? std::max(marker.second, marker.endSecond) : marker.second,
            kind, marker.isBreak, marker.isEx, marker.isFirework,
            kind == ScoringKind::Hold && marker.endSecond - marker.second > 0.3});
    }
    std::stable_sort(result.events.begin(), result.events.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
    return result;
}
inline int scoringEventsInPastSecond(const QVector<ScoringEvent>& events, double second)
{
    const auto upper = [&](double value) {
        return std::upper_bound(events.cbegin(), events.cend(), value,
            [](double time, const ScoringEvent& event) { return time < event.second; });
    };
    return static_cast<int>(upper(second) - upper(second - 1.0));
}
} // namespace miacode::chart
