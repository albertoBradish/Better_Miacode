#include "core/scene/PreviewProgressStatsCache.h"

#include "core/chart/ChartScoringEvents.h"

#include <algorithm>

namespace {

enum class StatsCategory {
    Tap,
    Hold,
    Slide,
    Touch,
    Break,
};

struct ObjectEventDelta {
    double judgeSecond = 0.0;
    StatsCategory category = StatsCategory::Tap;
};

struct HudEventDelta {
    double judgeSecond = 0.0;
    StatsCategory category = StatsCategory::Tap;
    double finaleCurrentDelta = 0.0;
    double deluxeBaseCurrentDelta = 0.0;
    int deluxeBreakCurrentDelta = 0;
};

template <typename PrefixT>
qsizetype lastPrefixIndexAt(const QVector<PrefixT>& prefixes, double second)
{
    if (prefixes.isEmpty()) {
        return -1;
    }

    qsizetype left = 0;
    qsizetype right = prefixes.size();
    const double target = second + 1e-6;
    while (left < right) {
        const qsizetype middle = left + (right - left) / 2;
        if (prefixes[middle].judgeSecond <= target) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }
    return left - 1;
}

void incrementObjectTotal(
    miacode::preview::scene::PreviewObjectStatsSnapshot* totals,
    StatsCategory category
)
{
    Q_ASSERT(totals != nullptr);

    switch (category) {
    case StatsCategory::Tap:
        ++totals->tapTotal;
        break;
    case StatsCategory::Hold:
        ++totals->holdTotal;
        break;
    case StatsCategory::Slide:
        ++totals->slideTotal;
        break;
    case StatsCategory::Touch:
        ++totals->touchTotal;
        break;
    case StatsCategory::Break:
        ++totals->breakTotal;
        break;
    }
    ++totals->totalCount;
}

void applyObjectPlayedDelta(
    miacode::preview::scene::PreviewObjectStatsSnapshot* snapshot,
    StatsCategory category
)
{
    Q_ASSERT(snapshot != nullptr);

    switch (category) {
    case StatsCategory::Tap:
        ++snapshot->tapPlayed;
        break;
    case StatsCategory::Hold:
        ++snapshot->holdPlayed;
        break;
    case StatsCategory::Slide:
        ++snapshot->slidePlayed;
        break;
    case StatsCategory::Touch:
        ++snapshot->touchPlayed;
        break;
    case StatsCategory::Break:
        ++snapshot->breakPlayed;
        break;
    }
    ++snapshot->totalPlayed;
}

void applyHudPlayedDelta(
    miacode::preview::scene::PreviewHudStats* stats,
    StatsCategory category
)
{
    Q_ASSERT(stats != nullptr);

    switch (category) {
    case StatsCategory::Tap:
        ++stats->tapPlayed;
        break;
    case StatsCategory::Hold:
        ++stats->holdPlayed;
        break;
    case StatsCategory::Slide:
        ++stats->slidePlayed;
        break;
    case StatsCategory::Touch:
        ++stats->touchPlayed;
        break;
    case StatsCategory::Break:
        ++stats->breakPlayed;
        break;
    }
}

}  // namespace

namespace miacode::preview::scene {

void PreviewProgressStatsCache::rebuild(const QVector<TimelineNoteMarker>& markers)
{
    totals_ = PreviewObjectStatsSnapshot();
    objectEventPrefixes_.clear();
    hudEventPrefixes_.clear();
    finaleBaseTotal_ = 0.0;
    deluxeBaseTotal_ = 0.0;
    deluxeBreakTotal_ = 0;

    QVector<ObjectEventDelta> objectEvents;
    QVector<HudEventDelta> hudEvents;
    objectEvents.reserve(markers.size() * 2);
    hudEvents.reserve(markers.size() * 2);

    const auto addObjectEvent = [this, &objectEvents](double judgeSecond, StatsCategory category) {
        incrementObjectTotal(&totals_, category);
        objectEvents.append(ObjectEventDelta{judgeSecond, category});
    };

    const auto addHudNormalEvent = [this, &hudEvents](
                                        double judgeSecond,
                                        StatsCategory category,
                                        int finaleScore,
                                        int deluxeValue
                                    ) {
        finaleBaseTotal_ += static_cast<double>(finaleScore);
        deluxeBaseTotal_ += static_cast<double>(deluxeValue);
        hudEvents.append(HudEventDelta{
            judgeSecond,
            category,
            static_cast<double>(finaleScore),
            static_cast<double>(deluxeValue),
            0
        });
    };

    const auto addHudBreakEvent = [this, &hudEvents](double judgeSecond) {
        finaleBaseTotal_ += 2500.0;
        deluxeBaseTotal_ += 5.0;
        ++deluxeBreakTotal_;
        hudEvents.append(HudEventDelta{
            judgeSecond,
            StatsCategory::Break,
            2600.0,
            5.0,
            1
        });
    };

    scoringEvents_ = miacode::chart::buildScoringEvents(markers).events;
    for (const auto& event : scoringEvents_) {
        if (event.isBreak) {
            addObjectEvent(event.second, StatsCategory::Break);
            addHudBreakEvent(event.second);
            continue;
        }
        StatsCategory category = StatsCategory::Tap;
        using miacode::chart::ScoringKind;
        switch (event.kind) {
        case ScoringKind::Hold: case ScoringKind::TouchHold: category = StatsCategory::Hold; break;
        case ScoringKind::Slide: category = StatsCategory::Slide; break;
        case ScoringKind::Touch: category = StatsCategory::Touch; break;
        default: break;
        }
        addObjectEvent(event.second, category);
        addHudNormalEvent(event.second, category, event.baseScore(), event.baseScore() / 500);
    }

    auto sortByJudgeSecond = [](const auto& a, const auto& b) {
        if (!qFuzzyCompare(a.judgeSecond + 1.0, b.judgeSecond + 1.0)) {
            return a.judgeSecond < b.judgeSecond;
        }
        return false;
    };
    std::stable_sort(objectEvents.begin(), objectEvents.end(), sortByJudgeSecond);
    std::stable_sort(hudEvents.begin(), hudEvents.end(), sortByJudgeSecond);

    objectEventPrefixes_.reserve(objectEvents.size());
    PreviewObjectStatsSnapshot objectRunning;
    for (const ObjectEventDelta& event : objectEvents) {
        applyObjectPlayedDelta(&objectRunning, event.category);
        objectEventPrefixes_.append(ObjectEventPrefix{
            event.judgeSecond,
            objectRunning.tapPlayed,
            objectRunning.holdPlayed,
            objectRunning.slidePlayed,
            objectRunning.touchPlayed,
            objectRunning.breakPlayed,
            objectRunning.totalPlayed
        });
    }

    hudEventPrefixes_.reserve(hudEvents.size());
    PreviewHudStats hudRunning;
    double finaleCurrent = 0.0;
    double deluxeBaseCurrent = 0.0;
    int deluxeBreakCurrent = 0;
    for (const HudEventDelta& event : hudEvents) {
        applyHudPlayedDelta(&hudRunning, event.category);
        finaleCurrent += event.finaleCurrentDelta;
        deluxeBaseCurrent += event.deluxeBaseCurrentDelta;
        deluxeBreakCurrent += event.deluxeBreakCurrentDelta;
        hudEventPrefixes_.append(HudEventPrefix{
            event.judgeSecond,
            hudRunning.tapPlayed,
            hudRunning.holdPlayed,
            hudRunning.slidePlayed,
            hudRunning.touchPlayed,
            hudRunning.breakPlayed,
            finaleCurrent,
            deluxeBaseCurrent,
            deluxeBreakCurrent
        });
    }
}

bool PreviewProgressStatsCache::isEmpty() const
{
    return totals_.totalCount <= 0
        && objectEventPrefixes_.isEmpty()
        && hudEventPrefixes_.isEmpty()
        && qFuzzyIsNull(finaleBaseTotal_)
        && qFuzzyIsNull(deluxeBaseTotal_)
        && deluxeBreakTotal_ <= 0;
}

PreviewObjectStatsSnapshot PreviewProgressStatsCache::snapshotAt(double second) const
{
    PreviewObjectStatsSnapshot snapshot = totals_;
    const qsizetype index = lastPrefixIndexAt(objectEventPrefixes_, second);
    if (index < 0) {
        snapshot.tapPlayed = 0;
        snapshot.holdPlayed = 0;
        snapshot.slidePlayed = 0;
        snapshot.touchPlayed = 0;
        snapshot.breakPlayed = 0;
        snapshot.totalPlayed = 0;
        return snapshot;
    }

    const ObjectEventPrefix& prefix = objectEventPrefixes_.at(index);
    snapshot.tapPlayed = prefix.tapPlayed;
    snapshot.holdPlayed = prefix.holdPlayed;
    snapshot.slidePlayed = prefix.slidePlayed;
    snapshot.touchPlayed = prefix.touchPlayed;
    snapshot.breakPlayed = prefix.breakPlayed;
    snapshot.totalPlayed = prefix.totalPlayed;
    return snapshot;
}

PreviewHudStats PreviewProgressStatsCache::hudStatsAt(double second) const
{
    PreviewHudStats stats;
    stats.tapTotal = totals_.tapTotal;
    stats.holdTotal = totals_.holdTotal;
    stats.slideTotal = totals_.slideTotal;
    stats.touchTotal = totals_.touchTotal;
    stats.breakTotal = totals_.breakTotal;
    stats.totalNotes = totals_.totalCount;
    stats.dxScoreMax = totals_.totalCount * 3;
    stats.deluxeBreakTotal = deluxeBreakTotal_;
    const qsizetype index = lastPrefixIndexAt(hudEventPrefixes_, second);
    if (index < 0) {
        return stats;
    }

    const HudEventPrefix& prefix = hudEventPrefixes_.at(index);
    stats.tapPlayed = prefix.tapPlayed;
    stats.holdPlayed = prefix.holdPlayed;
    stats.slidePlayed = prefix.slidePlayed;
    stats.touchPlayed = prefix.touchPlayed;
    stats.breakPlayed = prefix.breakPlayed;
    stats.tapTotal = totals_.tapTotal;
    stats.holdTotal = totals_.holdTotal;
    stats.slideTotal = totals_.slideTotal;
    stats.touchTotal = totals_.touchTotal;
    stats.breakTotal = totals_.breakTotal;
    stats.combo = prefix.tapPlayed + prefix.holdPlayed + prefix.slidePlayed + prefix.touchPlayed + prefix.breakPlayed;
    stats.totalNotes = totals_.totalCount;
    stats.dxScore = stats.combo * 3;
    stats.dxScoreMax = totals_.totalCount * 3;
    stats.finaleRate = finaleBaseTotal_ > 0.0
        ? (prefix.finaleCurrent / finaleBaseTotal_) * 100.0
        : 0.0;
    stats.deluxeRate = (deluxeBaseTotal_ > 0.0
        ? (prefix.deluxeBaseCurrent / deluxeBaseTotal_) * 100.0
        : 0.0)
        + (deluxeBreakTotal_ > 0
            ? (static_cast<double>(prefix.deluxeBreakCurrent) / static_cast<double>(deluxeBreakTotal_))
            : 0.0);
    stats.deluxeBreakCurrent = prefix.deluxeBreakCurrent;
    stats.deluxeBreakTotal = deluxeBreakTotal_;
    return stats;
}

}  // namespace miacode::preview::scene
