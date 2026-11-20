#include "core/chart/transform/BasicHandOrderLayout.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/TouchPadAdjacency.h"
#include "core/chart/model/HandOrderRhythm.h"
#include "core/chart/transform/HandOrderPatternRules.h"

#include <cmath>
#include <limits>
#include <algorithm>
#include <QHash>
#include <QLineF>

namespace miacode::chart_transform {
namespace {
constexpr double kCentreTolerance = 1e-6;
constexpr double kTouchCanvasCentre = 270.0;
constexpr double kFastRepeatedTapGapSeconds = chart::handOrderFastRepeatGapSeconds;
constexpr double kWindingPairGapSeconds = 0.4;
constexpr double kTouchSweepGapSeconds = 0.18;
constexpr double kMinimumOneStrokeJoinSeconds = 1.0 / 180.0;
constexpr double kMaximumOneStrokeJoinSeconds = 0.15;
constexpr double kStarHitWindowSeconds = 27.0 / 180.0;

double oneStrokeJoinSeconds(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    if (!std::isfinite(first.tempoBpm) || !std::isfinite(second.tempoBpm)
        || first.tempoBpm <= 0.0 || second.tempoBpm <= 0.0
        || first.subdivision <= 0 || second.subdivision <= 0) return kMinimumOneStrokeJoinSeconds;
    const double firstGrid = 240.0 / (first.tempoBpm * first.subdivision);
    const double secondGrid = 240.0 / (second.tempoBpm * second.subdivision);
    return qBound(kMinimumOneStrokeJoinSeconds, 0.5 * qMin(firstGrid, secondGrid),
        kMaximumOneStrokeJoinSeconds);
}

HandOrderHand laneHand(int lane)
{
    if (lane < 1 || lane > 8) return HandOrderHand::Unspecified;
    return lane <= 4 ? HandOrderHand::Right : HandOrderHand::Left;
}

QString glyph(HandOrderHand hand)
{
    switch (hand) {
    case HandOrderHand::Left: return QStringLiteral("l");
    case HandOrderHand::Right: return QStringLiteral("r");
    case HandOrderHand::Either: return QStringLiteral("m");
    default: return {};
    }
}

QPointF lanePoint(int lane)
{
    if (lane < 1 || lane > 8) return {};
    const double angle = (lane - 0.5) * 3.14159265358979323846 / 4.0;
    return {std::sin(angle), -std::cos(angle)};
}

QPointF headPoint(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("touch") || marker.type == QStringLiteral("touch_hold"))
        return (marker.touchPoint - QPointF(kTouchCanvasCentre, kTouchCanvasCentre)) / 240.0;
    return lanePoint(marker.lane);
}

bool shallowUpperLowerReach(const QPointF& a, const QPointF& b)
{
    return a.x() * b.x() < 0.0 && std::abs(a.x()) > 0.25 && std::abs(b.x()) > 0.25
        && qMax(std::abs(a.x()), std::abs(b.x())) < 0.45
        && std::abs(a.y() - b.y()) > 1.5;
}

bool divergentSlideTracks(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    if (first.endLane != second.endLane) return true;
    const int segments = qMin(first.slideSegmentPoints.size(), second.slideSegmentPoints.size());
    for (int segment = 0; segment < segments; ++segment) {
        const auto& a = first.slideSegmentPoints.at(segment);
        const auto& b = second.slideSegmentPoints.at(segment);
        if (a.size() < 2 || b.size() < 2) continue;
        const auto sample = [](const QVector<QPointF>& points, double fraction) {
            const double index = fraction * (points.size() - 1);
            const int before = int(std::floor(index));
            const int after = qMin(before + 1, int(points.size()) - 1);
            const double blend = index - before;
            return points.at(before) * (1.0 - blend) + points.at(after) * blend;
        };
        for (double fraction : {0.25, 0.5, 0.75}) {
            const QPointF aPoint = sample(a, fraction);
            const QPointF bPoint = sample(b, fraction);
            if (QLineF(aPoint, bPoint).length() > 0.32 * 240.0) return true;
        }
    }
    return false;
}

struct LayoutHead {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
    QPointF point;
    HandOrderHand hand = HandOrderHand::Unspecified;
    bool fixed = false;
    int patternHand = -1;
    bool patternNeedsBothFree = false;
    bool gestureDefault = false;
    int touchPairWith = -1;
    int followSlideTail = -1;
    int interactionGroup = -1, interactionStep = -1;
    bool structuralPhase = false;
    bool preferStructuralStart = false;
    int windingPrevious = -1;
    int sweepPrevious = -1;
    int reboundPrevious = -1;
    int relaxedReboundPrevious = -1;
    int alternateSweepPrevious = -1;
    int interactionPrevious = -1;
    bool windingEach = false;
    bool windingPair = false;
    bool configuredHead = false;
};

struct LayoutSustain {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
    int head = -1;
    double end = 0.0;
    HandOrderHand hand = HandOrderHand::Unspecified;
    bool fixed = false;
    int patternHand = -1;
    int oneStrokePrevious = -1, oneStrokeNext = -1;
    bool splitSharedStar = false;
    bool keySweepSplit = false;
    bool keySweepKeep = false;
    bool tapLaneReserved = false;
};
struct LayoutWifiOccupancy {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
};
struct SharedHeadUnit {
    int key = -1;
    QVector<int> members;
};

int handIndex(HandOrderHand hand)
{
    return hand == HandOrderHand::Left ? 0 : hand == HandOrderHand::Right ? 1 : -1;
}

QVector<int> orderedHeads(const QVector<LayoutHead>& heads)
{
    QVector<int> order;
    order.reserve(heads.size());
    for (int i = 0; i < heads.size(); ++i) order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& left = heads.at(a);
        const auto& right = heads.at(b);
        if (left.marker->second != right.marker->second) return left.marker->second < right.marker->second;
        return left.marker->parseOrder < right.marker->parseOrder;
    });
    return order;
}

bool simpleLayoutSuffix(const QString& suffix, const TimelineNoteMarker& marker, int markerCount)
{
    if (suffix.isEmpty()) return true;
    if (marker.type == QStringLiteral("slide")) {
        for (const auto head : {'l', 'r'}) {
            if (markerCount == 1)
                for (const auto track : {'l', 'r'})
                    if (suffix == QStringLiteral("~[head=%1;track=0:%2]").arg(QChar(head)).arg(QChar(track)))
                        return true;
            if (markerCount == 2)
                for (const auto first : {'l', 'r'})
                    for (const auto second : {'l', 'r'})
                        if (suffix == QStringLiteral("~[head=%1;branch0=0:%2;branch1=0:%3]")
                            .arg(QChar(head)).arg(QChar(first)).arg(QChar(second))) return true;
        }
        return false;
    }
    if (markerCount != 1) return false;
    return suffix == QStringLiteral("~l") || suffix == QStringLiteral("~r");
}

bool fastTapGap(const LayoutHead& first, const LayoutHead& second)
{
    if (!chart::handOrderIsButtonStrike(*first.marker) || !chart::handOrderIsButtonStrike(*second.marker)) return false;
    const double gap = second.marker->second - first.marker->second;
    const double pseudoEach = chart::handOrderPseudoEachTolerance(*first.marker, *second.marker);
    return gap >= pseudoEach - 1e-9 && gap <= kFastRepeatedTapGapSeconds + 1e-9;
}

int adjacentLaneStep(int first, int second)
{
    if (first < 1 || first > 8 || second < 1 || second > 8) return 0;
    if (second == first % 8 + 1) return 1;
    if (second == (first + 6) % 8 + 1) return -1;
    return 0;
}

int touchLaneDistance(const QString& first, const QString& second)
{
    if (first == QStringLiteral("C") || second == QStringLiteral("C")) return 0;
    if (first.size() != 2 || second.size() != 2) return 8;
    const int distance = std::abs(first.at(1).digitValue() - second.at(1).digitValue());
    return qMin(distance, 8 - distance);
}

bool nearbyTouches(const LayoutHead& first, const LayoutHead& second)
{
    const QString& a = first.marker->touchPad;
    const QString& b = second.marker->touchPad;
    return first.marker->type == QStringLiteral("touch")
        && second.marker->type == QStringLiteral("touch")
        && chart::touchPadsCanSweep(a, b)
        && touchLaneDistance(a, b) <= 2;
}

bool regularHeadStep(const LayoutHead& first, const LayoutHead& second, bool requireFast)
{
    return chart::handOrderRegularInputStep(*first.marker, *second.marker, requireFast);
}
bool regularTapStep(const LayoutHead& first, const LayoutHead& second, bool requireFast)
{
    return first.marker->type == QStringLiteral("tap") && second.marker->type == QStringLiteral("tap")
        && regularHeadStep(first, second, requireFast);
}
bool playableStar(const LayoutHead& head)
{
    return chart::handOrderHasStarStrike(*head.marker);
}
int relaxedEighthSteps(const LayoutHead& first, const LayoutHead& second)
{
    if (first.marker->tempoBpm <= 0) return 0;
    const double gap = second.marker->second - first.marker->second;
    if (gap <= 1e-9 || gap > chart::handOrderReturnDelaySeconds + 1e-9) return 0;
    const double units = gap * first.marker->tempoBpm / 30.0;
    const int rounded = qRound(units);
    return (rounded == 1 || rounded == 2) && std::abs(units - rounded) <= 0.1 + 1e-9 ? rounded : 0;
}

void identifyLocalPatterns(QVector<LayoutHead>* heads, QVector<LayoutSustain>* sustains,
    const QVector<bool>& canRewrite, const QVector<bool>& selectedTokens,
    int sweepThreshold, QVector<bool>* rewriteTokens)
{
    const double maximumSweepGap = chart::handOrderSweepGapSeconds(sweepThreshold);
    const auto order = orderedHeads(*heads);
    QVector<QVector<int>> headsByToken(canRewrite.size());
    QVector<QVector<int>> sustainsByToken(canRewrite.size());
    QVector<QVector<int>> sustainsByHead(heads->size());
    for (int i = 0; i < heads->size(); ++i) headsByToken[heads->at(i).token].append(i);
    for (int i = 0; i < sustains->size(); ++i) {
        const auto& sustain = sustains->at(i);
        sustainsByToken[sustain.token].append(i);
        if (sustain.head >= 0) sustainsByHead[sustain.head].append(i);
    }
    QVector<double> firstHeadAfterTail(sustains->size(), std::numeric_limits<double>::infinity());
    for (int index = 0; index < sustains->size(); ++index) {
        const double tailSecond = sustains->at(index).end;
        const auto next = std::lower_bound(order.cbegin(), order.cend(), tailSecond - 1e-9,
            [&](int headIndex, double second) { return heads->at(headIndex).marker->second < second; });
        if (next != order.cend()) firstHeadAfterTail[index] = heads->at(*next).marker->second;
    }
    const auto eligible = [&](int first, int last) {
        for (int i = first; i < last; ++i)
            if (!canRewrite.value(heads->at(order.at(i)).token)) return false;
        return true;
    };
    const auto allowRecalculation = [&](int token) {
        if (rewriteTokens->at(token)) return;
        (*rewriteTokens)[token] = true;
        for (int index : headsByToken.at(token)) {
            auto& head = (*heads)[index];
            head.fixed = false;
            head.hand = HandOrderHand::Unspecified;
        }
        for (int index : sustainsByToken.at(token)) {
            auto& sustain = (*sustains)[index];
            sustain.fixed = false;
            sustain.hand = HandOrderHand::Unspecified;
        }
    };
    const auto splitGestureTrack = [&](int headIndex, int headHand, bool leavesHead) {
        const auto& ids = sustainsByHead.at(headIndex);
        if (ids.isEmpty()) return;
        const auto& first = *sustains->at(ids.first()).marker;
        for (int id : ids) {
            const auto& sustain = sustains->at(id);
            if (sustain.marker->type != QStringLiteral("slide") || sustain.oneStrokePrevious >= 0
                || sustain.oneStrokeNext >= 0 || divergentSlideTracks(first, *sustain.marker)
                || std::abs(sustain.marker->slideTraceSecond - first.slideTraceSecond) > 1e-9
                || std::abs(sustain.marker->endSecond - first.endSecond) > 1e-9
                || sustain.marker->slideSegmentShootSeconds != first.slideSegmentShootSeconds) return;
        }
        bool keepsHead = false;
        if (!leavesHead) for (const auto& candidate : *heads)
            keepsHead |= candidate.token != heads->at(headIndex).token
                && candidate.marker->lane == first.lane
                && (candidate.marker->type == QStringLiteral("tap") || playableStar(candidate))
                && std::abs(candidate.marker->second - first.slideTraceSecond) < 1e-8;
        for (int id : ids) {
            auto& sustain = (*sustains)[id];
            sustain.patternHand = keepsHead ? headHand : 1 - headHand;
            sustain.splitSharedStar = sustain.keySweepSplit = !keepsHead;
            sustain.keySweepKeep = keepsHead;
        }
    };

    // Repeated opposite anchors with three intervening adjacent taps form a
    // winding path. The moving hand arrives at the other anchor, so the next
    // block starts with the opposite hand rather than snapping back by side.
    QVector<QVector<int>> moments;
    for (int index : order) {
        if (moments.isEmpty() || std::abs(heads->at(moments.last().first()).marker->second
                - heads->at(index).marker->second) > 1e-9) moments.append(QVector<int>{});
        moments.last().append(index);
    }
    const auto windingPath = [&](int moment, int lane) {
        QVector<int> path;
        if (moment + 4 >= moments.size()) return path;
        const int opposite = (lane + 3) % 8 + 1;
        int start = -1, finish = -1;
        for (int boundary : {moment, moment + 4}) {
            const auto& pair = moments.at(boundary);
            if (pair.size() != 2) return QVector<int>{};
            const auto& a = *heads->at(pair.at(0)).marker;
            const auto& b = *heads->at(pair.at(1)).marker;
            if (a.type != "tap" || b.type != "tap" || a.eachGroupId < 0
                || a.eachGroupId != b.eachGroupId
                || !((a.lane == lane && b.lane == opposite) || (a.lane == opposite && b.lane == lane)))
                return QVector<int>{};
            for (int index : pair) {
                if (boundary == moment && heads->at(index).marker->lane == lane) start = index;
                if (boundary == moment + 4 && heads->at(index).marker->lane == opposite) finish = index;
            }
        }
        path.append(start);
        for (int offset = 1; offset <= 3; ++offset) {
            if (moments.at(moment + offset).size() != 1) return QVector<int>{};
            const int index = moments.at(moment + offset).first();
            if (heads->at(index).marker->type != "tap") return QVector<int>{};
            path.append(index);
        }
        path.append(finish);
        const int direction = adjacentLaneStep(heads->at(start).marker->lane, heads->at(path.at(1)).marker->lane);
        const double interval = heads->at(path.at(1)).marker->second - heads->at(start).marker->second;
        if (!direction || interval <= 1e-9 || interval > 0.18) return QVector<int>{};
        for (int i = 1; i < path.size(); ++i) {
            const auto& a = *heads->at(path.at(i - 1)).marker;
            const auto& b = *heads->at(path.at(i)).marker;
            const double gap = b.second - a.second;
            if (adjacentLaneStep(a.lane, b.lane) != direction
                || gap < interval * 0.9 - 1e-9 || gap > interval * 1.1 + 1e-9) return QVector<int>{};
        }
        return path;
    };
    for (int moment = 0; moment + 8 < moments.size();) {
        bool recognized = false;
        if (moments.at(moment).size() == 2) for (int start : moments.at(moment)) {
            const int lane = heads->at(start).marker->lane;
            QVector<QVector<int>> paths;
            for (int boundary = moment; boundary + 4 < moments.size(); boundary += 4) {
                const auto path = windingPath(boundary, lane);
                if (path.isEmpty()) break;
                if (!paths.isEmpty()) {
                    const auto& previous = paths.constLast();
                    const double interval = heads->at(path.at(1)).marker->second - heads->at(path.first()).marker->second;
                    const double prior = heads->at(previous.at(1)).marker->second - heads->at(previous.first()).marker->second;
                    if (heads->at(path.at(1)).marker->lane != heads->at(previous.at(1)).marker->lane
                        || interval < prior * 0.9 - 1e-9 || interval > prior * 1.1 + 1e-9) break;
                }
                paths.append(path);
            }
            if (paths.size() < 2) continue;
            const int locked = handIndex(heads->at(start).hand);
            const int phase = locked >= 0 ? locked : handIndex(laneHand(lane));
            const auto prefer = [&](int index, int hand) {
                auto& head = (*heads)[index];
                if (canRewrite.at(head.token) && selectedTokens.at(head.token)) allowRecalculation(head.token);
                head.patternHand = hand;
                head.patternNeedsBothFree = false;
            };
            for (int block = 0; block < paths.size(); ++block) {
                const auto& path = paths.at(block);
                const int hand = phase ^ (block % 2);
                for (int i = 0; i < path.size(); ++i) {
                    prefer(path.at(i), hand);
                    if (i > 0) (*heads)[path.at(i)].windingPrevious = path.at(i - 1);
                }
                for (int boundary : {moment + 4 * block, moment + 4 * (block + 1)})
                    for (int index : moments.at(boundary)) {
                        (*heads)[index].windingEach = true;
                        const bool moving = boundary == moment + 4 * block
                            ? heads->at(index).marker->lane == lane : heads->at(index).marker->lane != lane;
                        prefer(index, moving ? hand : 1 - hand);
                    }
            }
            moment += 4 * paths.size() + 1;
            recognized = true;
            break;
        }
        if (!recognized) ++moment;
    }

    // A real two-key Each on opposite sides must not retain two crossed
    // simple labels from an earlier pass unless its surrounding phrase
    // gives evidence for a fitted shallow reach.
    for (int first = 0; first + 1 < order.size(); ++first) {
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        if (a.marker->type != QStringLiteral("tap") || b.marker->type != QStringLiteral("tap")
            || a.windingEach || b.windingEach
            || a.marker->eachGroupId < 0 || a.marker->eachGroupId != b.marker->eachGroupId
            || std::abs(a.marker->second - b.marker->second) > 1e-9
            || (first > 0 && std::abs(heads->at(order.at(first - 1)).marker->second
                - a.marker->second) < 1e-9)
            || (first + 2 < order.size() && std::abs(heads->at(order.at(first + 2)).marker->second
                - a.marker->second) < 1e-9)
            || !canRewrite.at(a.token) || !canRewrite.at(b.token)
            || !selectedTokens.at(a.token) || !selectedTokens.at(b.token)) continue;
        const double xA = a.point.x(), xB = b.point.x();
        const bool crossed = (xA < -0.25 && xB > 0.25
            && handIndex(a.hand) == 1 && handIndex(b.hand) == 0)
            || (xB < -0.25 && xA > 0.25
                && handIndex(b.hand) == 1 && handIndex(a.hand) == 0);
        const auto sameAsPair = [&](int at) {
            if (at < 0 || at + 1 >= order.size()) return false;
            const auto& x = *heads->at(order.at(at)).marker;
            const auto& y = *heads->at(order.at(at + 1)).marker;
            const double gap = std::abs(x.second - a.marker->second);
            return std::abs(x.second - y.second) < 1e-9 && gap > 1e-9 && gap <= 0.4
                && ((x.lane == a.marker->lane && y.lane == b.marker->lane)
                    || (y.lane == a.marker->lane && x.lane == b.marker->lane));
        };
        const bool guidedReach = shallowUpperLowerReach(a.point, b.point)
            && (sameAsPair(first - 2) || sameAsPair(first + 2));
        if (crossed && !guidedReach) {
            allowRecalculation(a.token);
            allowRecalculation(b.token);
        }
    }

    // A repeated, vertically separated shallow Each can carry the entering
    // hand without a mid-phrase crossover. Stronger sweep rules still apply.
    for (int moment = 1; moment + 1 < moments.size(); ++moment) {
        const auto& entry = moments.at(moment - 1);
        const auto& pair = moments.at(moment);
        const auto& repeated = moments.at(moment + 1);
        if (entry.size() != 1 || pair.size() != 2 || repeated.size() != 2) continue;
        auto& a = (*heads)[pair.first()]; auto& b = (*heads)[pair.last()];
        auto& x = (*heads)[repeated.first()]; auto& y = (*heads)[repeated.last()];
        const bool samePair = (a.marker->lane == x.marker->lane && b.marker->lane == y.marker->lane)
            || (a.marker->lane == y.marker->lane && b.marker->lane == x.marker->lane);
        if (!shallowUpperLowerReach(a.point, b.point) || !samePair
            || a.fixed || b.fixed || x.fixed || y.fixed) continue;
        const auto& before = heads->at(entry.first());
        if (!chart::handOrderIsButtonStrike(*before.marker) || !chart::handOrderRegularInputStep(*before.marker, *a.marker)
            || !chart::handOrderRegularInputStep(*a.marker, *x.marker)) continue;
        const bool followsA = adjacentLaneStep(before.marker->lane, a.marker->lane) != 0;
        const bool followsB = adjacentLaneStep(before.marker->lane, b.marker->lane) != 0;
        if (followsA == followsB) continue;
        const auto& near = followsA ? a : b;
        const int inheritedHand = handIndex(laneHand(before.marker->lane));
        if (inheritedHand < 0 || inheritedHand == handIndex(laneHand(near.marker->lane))) continue;
        const int firstHand = followsA ? inheritedHand : 1 - inheritedHand;
        for (int index : {pair.first(), pair.last(), repeated.first(), repeated.last()}) {
            auto& head = (*heads)[index];
            if (head.patternHand >= 0 || !selectedTokens.at(head.token) || !canRewrite.at(head.token)) continue;
            allowRecalculation(head.token);
            head.patternHand = head.marker->lane == a.marker->lane ? firstHand : 1 - firstHand;
            head.configuredHead = true;
        }
    }

    // Two simultaneous tracks leaving the same playable star need separate
    // tracking hands once their paths diverge. The one button head is shared.
    for (int first = 0; first < sustains->size(); ++first) {
        const auto& a = sustains->at(first);
        if (a.marker->type != QStringLiteral("slide") || a.head < 0
            || !selectedTokens.at(a.token) || !canRewrite.at(a.token)) continue;
        QVector<int> paired{first};
        for (int second = 0; second < sustains->size(); ++second) {
            if (second == first) continue;
            const auto& b = sustains->at(second);
            if (b.marker->type != QStringLiteral("slide") || b.head < 0
                || !selectedTokens.at(b.token) || !canRewrite.at(b.token)
                || b.marker->lane != a.marker->lane
                || std::abs(b.marker->second - a.marker->second) > 1e-9
                || std::abs(b.marker->slideTraceSecond - a.marker->slideTraceSecond) > 1e-9
                || !divergentSlideTracks(*a.marker, *b.marker)
                || qMin(a.end, b.end) - qMax(a.marker->slideTraceSecond,
                    b.marker->slideTraceSecond) < 0.05) continue;
            paired.append(second);
        }
        if (paired.size() != 2 || paired.at(1) < first) continue;
        const auto& b = sustains->at(paired.at(1));
        const double firstX = lanePoint(a.marker->endLane).x();
        const double secondX = lanePoint(b.marker->endLane).x();
        const int firstHand = firstX < secondX ? 0 : 1;
        allowRecalculation(a.token);
        allowRecalculation(b.token);
        (*sustains)[first].patternHand = firstHand;
        (*sustains)[first].splitSharedStar = true;
        (*sustains)[paired.at(1)].patternHand = 1 - firstHand;
        (*sustains)[paired.at(1)].splitSharedStar = true;
    }

    // A tap on a visible slide star is the same button press. Its old simple
    // label may be repaired, while the slide's own manual head stays intact.
    for (int tapIndex = 0; tapIndex < heads->size(); ++tapIndex) {
        const auto& tap = heads->at(tapIndex);
        if (tap.marker->type != QStringLiteral("tap") || !canRewrite.at(tap.token)
            || !selectedTokens.at(tap.token)) continue;
        bool sharesStar = false;
        for (const auto& slide : *heads) {
            sharesStar |= slide.marker->type == QStringLiteral("slide") && slide.marker->hasHeadStar
                && slide.marker->lane == tap.marker->lane
                && std::abs(slide.marker->second - tap.marker->second) < 1e-9;
        }
        if (sharesStar) allowRecalculation(tap.token);
    }

    // The first button after a slide tail may carry an old simple label from
    // an earlier layout. Recompute it when it is adjacent and on the track
    // hand's natural side; only a short gap actually binds it to that hand.
    for (auto& head : *heads) {
        if ((head.marker->type != QStringLiteral("tap") && head.marker->type != QStringLiteral("hold"))
            || !canRewrite.at(head.token) || !selectedTokens.at(head.token)) continue;
        int preceding = -1;
        double latestEnd = -std::numeric_limits<double>::infinity();
        for (int index = 0; index < sustains->size(); ++index) {
            const auto& sustain = sustains->at(index);
            if (sustain.marker->type != QStringLiteral("slide") || sustain.token == head.token
                || sustain.marker->slideSegmentPoints.isEmpty()) continue;
            const double gap = head.marker->second - sustain.end;
            if (gap < -1e-9) continue;
            const int distance = std::abs(sustain.marker->endLane - head.marker->lane);
            if (qMin(distance, 8 - distance) != 1) continue;
            const auto predicted = sustain.fixed && !sustain.marker->handOrderTrackStops.isEmpty()
                ? sustain.marker->handOrderTrackStops.constLast().hand
                : basicLayoutTrackHand(*sustain.marker);
            if (handIndex(predicted) != handIndex(laneHand(head.marker->lane))) continue;
            if (head.marker->second > firstHeadAfterTail.at(index) + 1e-9) continue;
            if (sustain.end > latestEnd + 1e-9) {
                preceding = index;
                latestEnd = sustain.end;
            } else if (std::abs(sustain.end - latestEnd) <= 1e-9) preceding = -1;
        }
        if (preceding >= 0) {
            const int token = head.token;
            allowRecalculation(token);
            if (head.marker->second - sustains->at(preceding).end
                <= oneStrokeJoinSeconds(*sustains->at(preceding).marker, *head.marker) + 1e-9)
                head.followSlideTail = preceding;
        }
    }

    // A new slide may show its star while the previous trace is still moving.
    // Join the moving tracks, but leave each playable star as its own task.
    QVector<int> strokePrevious(sustains->size(), -1);
    QVector<int> successorCount(sustains->size(), 0);
    for (int nextIndex = 0; nextIndex < sustains->size(); ++nextIndex) {
        const auto& next = sustains->at(nextIndex);
        if (next.marker->type != QStringLiteral("slide") || !canRewrite.at(next.token)
            || !selectedTokens.at(next.token) || next.marker->slideSegmentPoints.isEmpty()) continue;
        int previousIndex = -1;
        for (int candidateIndex = 0; candidateIndex < sustains->size(); ++candidateIndex) {
            const auto& previous = sustains->at(candidateIndex);
            if (candidateIndex == nextIndex || previous.marker->type != QStringLiteral("slide")
                || previous.token == next.token || !canRewrite.at(previous.token)
                || !selectedTokens.at(previous.token) || previous.marker->slideSegmentPoints.isEmpty()
                || previous.marker->second > next.marker->second + 1e-9
                || previous.marker->slideTraceSecond >= next.marker->slideTraceSecond - 1e-9) continue;
            const double gap = next.marker->slideTraceSecond - previous.end;
            const bool paused = previous.marker->endLane == next.marker->lane && gap >= 0
                && gap <= qMin(0.6,60.0/qMax(previous.marker->tempoBpm,next.marker->tempoBpm)) + 1e-9;
            if (!paused && std::abs(gap) > oneStrokeJoinSeconds(*previous.marker, *next.marker) + 1e-9) continue;
            const int distance = std::abs(previous.marker->endLane - next.marker->lane);
            if (qMin(distance, 8 - distance) > 1) continue;
            if (next.marker->hasHeadStar && previous.marker->endLane == next.marker->lane
                && std::abs(previous.end - next.marker->second) <= kStarHitWindowSeconds) continue;
            if (previousIndex >= 0) { previousIndex = -1; break; }
            previousIndex = candidateIndex;
        }
        if (previousIndex >= 0) {
            strokePrevious[nextIndex] = previousIndex;
            ++successorCount[previousIndex];
        }
    }
    for (int nextIndex = 0; nextIndex < sustains->size(); ++nextIndex) {
        const int previousIndex = strokePrevious.at(nextIndex);
        if (previousIndex < 0 || successorCount.at(previousIndex) != 1) continue;
        const auto& next = sustains->at(nextIndex);
        allowRecalculation((*sustains)[previousIndex].token);
        allowRecalculation(next.token);
        (*sustains)[previousIndex].oneStrokeNext = nextIndex;
        (*sustains)[nextIndex].oneStrokePrevious = previousIndex;
        (*sustains)[nextIndex].hand = HandOrderHand::Unspecified;
    }

    // A complete ring is one continuous sweep. Keep the returning start key
    // on that hand and reserve the other for an opposite key in the final each.
    for (int first = 0; first + 8 < order.size();) {
        const auto& start = heads->at(order.at(first));
        const int startLane = start.marker->lane;
        const int direction = adjacentLaneStep(startLane, heads->at(order.at(first + 1)).marker->lane);
        bool circle = direction != 0 && eligible(first, first + 8);
        for (int offset = 1; circle && offset <= 7; ++offset) {
            const auto& previous = heads->at(order.at(first + offset - 1));
            const auto& current = heads->at(order.at(first + offset));
            const int expectedLane = (startLane - 1 + direction * offset + 80) % 8 + 1;
            circle = current.patternHand < 0 && current.marker->lane == expectedLane
                && fastTapGap(previous, current);
        }
        if (!circle || start.patternHand >= 0) { ++first; continue; }
        const auto& eighth = heads->at(order.at(first + 7));
        int returnIndex = first + 8;
        int partnerIndex = -1;
        const auto isReturn = [&](int index) {
            const auto& candidate = heads->at(order.at(index));
            return candidate.patternHand < 0 && candidate.marker->lane == startLane
                && canRewrite.at(candidate.token) && fastTapGap(eighth, candidate);
        };
        if (!isReturn(returnIndex)) {
            if (returnIndex + 1 >= order.size() || !isReturn(returnIndex + 1)
                || std::abs(heads->at(order.at(returnIndex)).marker->second
                    - heads->at(order.at(returnIndex + 1)).marker->second) >= 1e-9) {
                ++first;
                continue;
            }
            partnerIndex = returnIndex;
            ++returnIndex;
        } else if (returnIndex + 1 < order.size()
            && std::abs(heads->at(order.at(returnIndex + 1)).marker->second
                - heads->at(order.at(returnIndex)).marker->second) < 1e-9) {
            partnerIndex = returnIndex + 1;
        }
        const auto& returning = heads->at(order.at(returnIndex));
        if (partnerIndex >= 0) {
            const auto& partner = heads->at(order.at(partnerIndex));
            const int oppositeLane = (startLane + 3) % 8 + 1;
            if (partner.marker->type != QStringLiteral("tap") || partner.marker->lane != oppositeLane
                || partner.marker->eachGroupId < 0 || partner.marker->eachGroupId != returning.marker->eachGroupId
                || !canRewrite.at(partner.token)) { ++first; continue; }
        }
        const int last = qMax(returnIndex, partnerIndex);
        if (last + 1 < order.size()
            && std::abs(heads->at(order.at(last + 1)).marker->second - returning.marker->second) < 1e-9) {
            ++first;
            continue;
        }
        const int sweepHand = handIndex(laneHand(startLane));
        for (int i = first; i < first + 8; ++i) {
            auto& head = (*heads)[order.at(i)];
            allowRecalculation(head.token);
            head.patternHand = sweepHand;
            head.patternNeedsBothFree = true;
        }
        auto& returnHead = (*heads)[order.at(returnIndex)];
        allowRecalculation(returnHead.token);
        returnHead.patternHand = sweepHand;
        returnHead.patternNeedsBothFree = true;
        if (partnerIndex >= 0) {
            auto& partner = (*heads)[order.at(partnerIndex)];
            allowRecalculation(partner.token);
            partner.patternHand = 1 - sweepHand;
        }
        first = last + 1;
    }

    // Winding rebounds keep each fast pair on one hand even when the jump to
    // the next lane contains an empty subdivision.
    const auto fastPairAt = [&](int first) {
        if (first + 1 >= order.size() || !eligible(first, first + 2)) return false;
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        return a.patternHand < 0 && b.patternHand < 0
            && a.marker->lane == b.marker->lane && fastTapGap(a, b);
    };
    for (int first = 0; first < order.size();) {
        int last = first;
        int step = 0;
        while (fastPairAt(last)) {
            if (last > first) {
                const int previousLane = heads->at(order.at(last - 2)).marker->lane;
                const int lane = heads->at(order.at(last)).marker->lane;
                const int nextStep = adjacentLaneStep(previousLane, lane);
                const double jump = heads->at(order.at(last)).marker->second
                    - heads->at(order.at(last - 1)).marker->second;
                if (!nextStep || (step && step != nextStep)
                    || jump <= 1e-9 || jump > kWindingPairGapSeconds + 1e-9) break;
                step = nextStep;
            }
            last += 2;
        }
        if (last - first >= 6) {
            const int initial = handIndex(laneHand(heads->at(order.at(first)).marker->lane));
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.patternHand = initial ^ ((i - first) / 2 % 2);
                head.patternNeedsBothFree = true;
                head.windingPair = true;
            }
            first = last;
        } else ++first;
    }

    // A fast directional key sweep stays on the starting hand. A back-and-
    // forth interaction reverses direction and is handled separately below.
    for (int first = 0; first < order.size();) {
        // Same-side four-key sweeps and complete rings remain gestures just
        // below the partial-sweep threshold, rather than isolated taps.
        const bool sameSide = first + 3 < order.size()
            && (heads->at(order.at(first)).marker->lane <= 4)
                == (heads->at(order.at(first + 3)).marker->lane <= 4);
        const int gestureSize = sameSide ? 4 : 8;
        bool coherentGesture = first + gestureSize - 1 < order.size();
        const int circleStep = coherentGesture ? adjacentLaneStep(heads->at(order.at(first)).marker->lane,
            heads->at(order.at(first + 1)).marker->lane) : 0;
        coherentGesture &= circleStep != 0;
        for (int i = first + 1; coherentGesture && i < first + gestureSize; ++i) {
            const auto& a = heads->at(order.at(i - 1));
            const auto& b = heads->at(order.at(i));
            const bool firstStar = i == first + 1 && playableStar(a) && b.marker->type == QStringLiteral("tap");
            coherentGesture = (regularTapStep(a, b, false) || (firstStar && regularHeadStep(a, b, false)))
                && b.marker->second - a.marker->second <= 0.13 + 1e-9
                && adjacentLaneStep(a.marker->lane, b.marker->lane) == circleStep;
        }
        const double maximumGap = coherentGesture ? qMax(0.13, maximumSweepGap) : maximumSweepGap;
        int last = first + 1;
        int direction = 0;
        while (last < order.size()) {
            const auto& previous = heads->at(order.at(last - 1));
            const auto& current = heads->at(order.at(last));
            const int step = adjacentLaneStep(previous.marker->lane, current.marker->lane);
            const bool starEnding = last == first + 2
                && ((current.marker->type == QStringLiteral("slide") && current.marker->hasHeadStar)
                    || current.marker->type == QStringLiteral("hold"));
            const bool starBeginning = last == first + 1 && playableStar(previous)
                && current.marker->type == QStringLiteral("tap");
            const double gap = current.marker->second - previous.marker->second;
            const bool regularStar = ((starEnding && previous.marker->type == QStringLiteral("tap"))
                || starBeginning) && regularHeadStep(previous, current, false);
            if (!step || (direction && step != direction) || previous.patternHand >= 0
                || current.patternHand >= 0
                || gap > maximumGap + 1e-9
                || (!regularTapStep(previous, current, false) && !regularStar)) break;
            direction = step;
            ++last;
        }
        const bool completeRing = last - first >= 8;
        const bool fastLong = last - first >= 4;
        bool shortSweep = last - first == 3
            && heads->at(order.at(first)).marker->subdivision > 0
            && heads->at(order.at(first + 1)).marker->subdivision > 0
            && heads->at(order.at(first + 2)).marker->subdivision > 0;
        if (shortSweep && first > 0 && last < order.size()) {
            const int middleLane = heads->at(order.at(first + 1)).marker->lane;
            shortSweep = heads->at(order.at(first - 1)).marker->lane != middleLane
                || heads->at(order.at(last)).marker->lane != middleLane;
        }
        bool shortPair = last - first == 2;
        int pairHand = -1;
        if (shortPair) {
            const auto& a = heads->at(order.at(first));
            const auto& b = heads->at(order.at(first + 1));
            const double gap = b.marker->second - a.marker->second;
            const double pseudoEach = chart::handOrderPseudoEachTolerance(*a.marker, *b.marker);
            pairHand = handIndex(laneHand(a.marker->lane));
            if (pairHand != handIndex(laneHand(b.marker->lane))) {
                bool occupied[2] = {false, false};
                for (const auto& sustain : *sustains) {
                    if (sustain.marker->second > a.marker->second + 1e-9
                        || sustain.end <= b.marker->second + 1e-9) continue;
                    const int hand = handIndex(sustain.hand != HandOrderHand::Unspecified
                        ? sustain.hand : basicLayoutTrackHand(*sustain.marker));
                    if (hand >= 0) occupied[hand] = true;
                }
                if (occupied[0] != occupied[1]) pairHand = occupied[0] ? 1 : 0;
                else shortPair = false;
            }
            shortPair = shortPair && gap >= pseudoEach - 1e-9
                && (first == 0 || heads->at(order.at(first - 1)).marker->lane != b.marker->lane)
                && (last == order.size() || heads->at(order.at(last)).marker->lane != a.marker->lane);
        }
        if ((completeRing || fastLong || shortSweep || shortPair)
            && eligible(first, last)) {
            const int hand = shortPair ? pairHand
                : handIndex(laneHand(heads->at(order.at(first)).marker->lane));
            const int tailHand = handIndex(laneHand(heads->at(order.at(last - 1)).marker->lane));
            const double firstSecond = heads->at(order.at(first)).marker->second;
            bool reservedHand = std::any_of(sustains->cbegin(), sustains->cend(), [&](const auto& sustain) {
                // A pending trace already needs a hand prepared for its entry.
                // A Hold ending exactly at the first tap no longer reserves it.
                return sustain.marker->second <= firstSecond + 1e-9 && sustain.end > firstSecond + 1e-9;
            });
            const auto& starting = heads->at(order.at(first));
            if (starting.marker->eachGroupId >= 0)
                reservedHand |= std::any_of(heads->cbegin(), heads->cend(), [&](const auto& other) {
                    return other.marker->parseOrder != starting.marker->parseOrder
                        && other.marker->eachGroupId == starting.marker->eachGroupId
                        && std::abs(other.marker->second - firstSecond) < 1e-9;
                });
            const bool splitFour = last - first == 4 && hand != tailHand && !reservedHand;
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.patternHand = splitFour && i >= first + 2 ? tailHand : hand;
                head.patternNeedsBothFree = false;
                if (i > first && !(splitFour && i == first + 2))
                    head.sweepPrevious = order.at(i - 1);
            }
            if (playableStar(starting) && last - first >= 3) splitGestureTrack(order.at(first), hand, true);
            if (shortSweep) {
                const int finalIndex = order.at(last - 1);
                const auto& finalHead = heads->at(finalIndex);
                if (finalHead.marker->type == QStringLiteral("slide")) splitGestureTrack(finalIndex, hand, false);
                if (last < order.size()) {
                    const int nextIndex = order.at(last);
                    auto& next = (*heads)[nextIndex];
                    const auto& previous = heads->at(finalIndex);
                    const double grid = 240.0 / (previous.marker->tempoBpm * previous.marker->subdivision);
                    const double gap = next.marker->second - previous.marker->second;
                    if (next.marker->type == QStringLiteral("slide") && next.marker->hasHeadStar
                        && next.patternHand < 0 && next.marker->lane != previous.marker->lane
                        && gap >= grid * 0.9 - 1e-9 && gap <= grid * 1.1 + 1e-9
                        && eligible(last, last + 1)) {
                        allowRecalculation(next.token);
                        next.patternHand = 1 - hand;
                        for (int sustainIndex : sustainsByHead.at(nextIndex))
                            (*sustains)[sustainIndex].patternHand = 1 - hand;
                    }
                }
            }
            first = last;
        } else ++first;
    }

    // A-B-A-C at an eighth-note cadence is a small axis, not a three-key
    // sweep beginning at B. Identify it before relaxed directional gestures.
    for (int first = 0; first + 3 < order.size(); ++first) {
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        const auto& c = heads->at(order.at(first + 2));
        const auto& d = heads->at(order.at(first + 3));
        const int step = adjacentLaneStep(a.marker->lane, b.marker->lane);
        if (!step || a.marker->lane != c.marker->lane
            || adjacentLaneStep(a.marker->lane, d.marker->lane) != -step || !eligible(first, first + 4)) continue;
        bool axis = true;
        for (int i = first; i < first + 4; ++i) {
            const auto& head = heads->at(order.at(i));
            axis &= head.marker->type == QStringLiteral("tap") && !head.marker->isEach && head.patternHand < 0;
            if (i > first) axis &= relaxedEighthSteps(heads->at(order.at(i - 1)), head) == 1;
        }
        if (!axis) continue;
        const int hand = handIndex(laneHand(a.marker->lane));
        for (int i = first; i < first + 4; ++i) {
            auto& head = (*heads)[order.at(i)];
            allowRecalculation(head.token); head.patternHand = hand ^ ((i - first) % 2);
            if (i > first) head.interactionPrevious = order.at(i - 1);
        }
    }

    // An isolated eighth-note neighbour excursion mildly prefers the
    // returning key's hand, without forcing the neighbour onto that hand.
    for (int first = 0; first + 2 < order.size(); ++first) {
        const auto& a = heads->at(order.at(first));
        if (a.marker->type != QStringLiteral("tap") || a.marker->isEach) continue;
        constexpr int offset = 2;
        if (first + offset >= order.size() || !eligible(first, first + offset + 1)) continue;
        auto& returning = (*heads)[order.at(first + offset)];
        bool rebound = returning.marker->type == QStringLiteral("tap") && !returning.marker->isEach
            && returning.marker->lane == a.marker->lane;
        for (int i = first + 1; rebound && i <= first + offset; ++i) {
            const auto& head = heads->at(order.at(i));
            rebound &= head.marker->type == QStringLiteral("tap") && !head.marker->isEach
                && relaxedEighthSteps(heads->at(order.at(i - 1)), head) > 0;
        }
        rebound &= adjacentLaneStep(a.marker->lane, heads->at(order.at(first + 1)).marker->lane) != 0;
        const int repeatedLane = heads->at(order.at(first + 1)).marker->lane;
        if (first > 0 && heads->at(order.at(first - 1)).marker->lane == repeatedLane
            && relaxedEighthSteps(heads->at(order.at(first - 1)), a)) rebound = false;
        if (first + offset + 1 < order.size()
            && heads->at(order.at(first + offset + 1)).marker->lane == repeatedLane
            && relaxedEighthSteps(returning, heads->at(order.at(first + offset + 1)))) rebound = false;
        if (!rebound) continue;
        allowRecalculation(returning.token); returning.relaxedReboundPrevious = order.at(first);
    }

    // A free hand can keep an eighth-note gesture through a star while
    // the other hand finishes an incoming trace at the gesture entrance.
    for (int first = 0; first + 2 < order.size();) {
        const auto& start = heads->at(order.at(first));
        int last = first + 1, direction = 0;
        bool eighth = false;
        while (last < order.size() && last - first < 8) {
            const auto& a = heads->at(order.at(last - 1));
            const auto& b = heads->at(order.at(last));
            const int step = adjacentLaneStep(a.marker->lane, b.marker->lane), units = relaxedEighthSteps(a, b);
            if (!step || (direction && step != direction) || !units || a.patternHand >= 0 || b.patternHand >= 0
                || a.marker->isEach || b.marker->isEach
                || (a.marker->type != QStringLiteral("tap") && !playableStar(a))
                || (b.marker->type != QStringLiteral("tap") && !playableStar(b))) break;
            direction = step; eighth |= units == 1; ++last;
        }
        bool occupiedStarEntry = false;
        for (int i = first + 1; i + 1 < last; ++i) {
            const auto& head = heads->at(order.at(i));
            if (!playableStar(head)) continue;
            for (const auto& sustain : *sustains)
                occupiedStarEntry |= sustain.head != order.at(i)
                    && sustain.marker->type == QStringLiteral("slide")
                    && sustain.marker->endLane == start.marker->lane
                    && sustain.marker->slideTraceSecond <= start.marker->second + 1e-9
                    && sustain.end > start.marker->second + 1e-9;
        }
        if (last - first < 3 || last - first > 4 || !eighth || !occupiedStarEntry
            || start.marker->type != QStringLiteral("tap") || !eligible(first, last)) { ++first; continue; }
        const int hand = handIndex(laneHand(start.marker->lane));
        for (int i = first; i < last; ++i) {
            auto& head = (*heads)[order.at(i)];
            allowRecalculation(head.token); head.patternHand = hand;
            if (i > first) head.sweepPrevious = order.at(i - 1);
            if (playableStar(head) && i + 1 < last) splitGestureTrack(order.at(i), hand, true);
        }
        first = last;
    }

    // Opposed sweeps returning to a shallow entrance use two whole gestures.
    // This preserves 5,4,3,2 -> 5*,6,7,8 instead of splitting the first sweep.
    for (int first = 0; first + 7 < order.size(); ++first) {
        const auto& entrance = heads->at(order.at(first));
        const auto& star = heads->at(order.at(first + 4));
        if (!playableStar(star) || star.marker->lane != entrance.marker->lane
            || std::abs(entrance.point.x()) > 0.45 || !eligible(first, first + 8)) continue;
        const int direction = adjacentLaneStep(entrance.marker->lane, heads->at(order.at(first + 1)).marker->lane);
        bool paired = direction != 0;
        int rightCount[2] = {};
        for (int i = 0; paired && i < 8; ++i) {
            const auto& head = heads->at(order.at(first + i));
            paired &= !head.marker->isEach && (i == 4 || head.marker->type == QStringLiteral("tap"));
            rightCount[i / 4] += head.marker->lane <= 4;
            if (i > 0) paired &= regularHeadStep(heads->at(order.at(first + i - 1)), head, false)
                && head.marker->second - heads->at(order.at(first + i - 1)).marker->second <= 0.13 + 1e-9;
            if (i > 0 && i != 4) paired &= adjacentLaneStep(heads->at(order.at(first + i - 1)).marker->lane,
                head.marker->lane) == (i < 4 ? direction : -direction);
        }
        if (!paired || rightCount[0] == 2 || rightCount[1] == 2 || (rightCount[0] > 2) == (rightCount[1] > 2)) continue;
        const int firstHand = rightCount[0] > 2 ? 1 : 0;
        for (int i = 0; i < 8; ++i) {
            auto& head = (*heads)[order.at(first + i)];
            allowRecalculation(head.token); head.patternHand = i < 4 ? firstHand : 1 - firstHand;
            head.gestureDefault = true;
            head.configuredHead = true;
            if (i > 0 && i != 4) head.sweepPrevious = order.at(first + i - 1);
        }
        (*heads)[order.at(first + 4)].alternateSweepPrevious = order.at(first);
        splitGestureTrack(order.at(first + 4), 1 - firstHand, true);
    }

    // Returning immediately to the beginning of the same three-key sweep
    // uses the other hand instead of making the first hand jump backward.
    for (int first = 0; first + 5 < order.size();) {
        bool repeated = eligible(first, first + 6);
        for (int i = 0; repeated && i < 3; ++i) {
            const auto& a = heads->at(order.at(first + i));
            const auto& b = heads->at(order.at(first + i + 3));
            repeated = a.marker->type == QStringLiteral("tap")
                && b.marker->type == QStringLiteral("tap")
                && !a.marker->isEach && !b.marker->isEach
                && a.marker->lane == b.marker->lane
                && a.interactionGroup < 0 && b.interactionGroup < 0;
        }
        const auto& firstStart = heads->at(order.at(first));
        const auto& firstMiddle = heads->at(order.at(first + 1));
        const auto& firstEnd = heads->at(order.at(first + 2));
        const auto& secondStart = heads->at(order.at(first + 3));
        const int direction = adjacentLaneStep(firstStart.marker->lane, firstMiddle.marker->lane);
        const double step = firstMiddle.marker->second - firstStart.marker->second;
        const double returnGap = secondStart.marker->second - firstEnd.marker->second;
        repeated = repeated && direction != 0 && step > 0.0
            && adjacentLaneStep(firstMiddle.marker->lane, firstEnd.marker->lane) == direction
            && regularTapStep(firstStart, firstMiddle, false)
            && step <= maximumSweepGap + 1e-9
            && returnGap > 1.5 * step && returnGap <= qMin(4.5 * step, 0.18) + 1e-9;
        for (int i : {2, 4, 5})
            if (repeated)
                repeated = regularTapStep(heads->at(order.at(first + i - 1)),
                    heads->at(order.at(first + i)), false);
        if (!repeated) { ++first; continue; }
        const int firstHand = firstStart.patternHand >= 0 ? firstStart.patternHand
            : handIndex(laneHand(firstStart.marker->lane));
        for (int i = 3; i < 6; ++i) {
            auto& head = (*heads)[order.at(first + i)];
            allowRecalculation(head.token);
            head.patternHand = 1 - firstHand;
        }
        (*heads)[order.at(first + 3)].alternateSweepPrevious = order.at(first);
        first += 6;
    }

    // In a rapid paired sweep the hands may cross the screen centre at the
    // final Each. Continue each adjacent path instead of resetting by side.
    for (int moment = 0; moment + 2 < moments.size(); ++moment) {
        const auto& firstPair = moments.at(moment);
        const auto& secondPair = moments.at(moment + 1);
        const auto& thirdPair = moments.at(moment + 2);
        if (firstPair.size() != 2 || secondPair.size() != 2 || thirdPair.size() != 2) continue;
        const auto pairValid = [&](const QVector<int>& pair) {
            const auto& a = heads->at(pair.at(0));
            const auto& b = heads->at(pair.at(1));
            return a.marker->type == QStringLiteral("tap")
                && b.marker->type == QStringLiteral("tap")
                && a.marker->eachGroupId >= 0
                && a.marker->eachGroupId == b.marker->eachGroupId
                && a.marker->subdivision > 0 && b.marker->subdivision > 0;
        };
        if (!pairValid(firstPair) || !pairValid(secondPair) || !pairValid(thirdPair)) continue;
        const auto bySide = [&](const QVector<int>& pair, int hand) {
            for (int index : pair)
                if (handIndex(laneHand(heads->at(index).marker->lane)) == hand) return index;
            return -1;
        };
        const int firstLeft = bySide(firstPair, 0), firstRight = bySide(firstPair, 1);
        const int secondLeft = bySide(secondPair, 0), secondRight = bySide(secondPair, 1);
        const int thirdLeft = bySide(thirdPair, 0), thirdRight = bySide(thirdPair, 1);
        if (firstLeft < 0 || firstRight < 0 || secondLeft < 0 || secondRight < 0
            || thirdLeft < 0 || thirdRight < 0) continue;
        const auto& anchor = *heads->at(firstRight).marker;
        const double grid = 240.0 / (anchor.tempoBpm * anchor.subdivision);
        const double firstGap = heads->at(secondRight).marker->second - anchor.second;
        const double secondGap = heads->at(thirdRight).marker->second
            - heads->at(secondRight).marker->second;
        if (firstGap < grid * 0.9 - 1e-9 || firstGap > grid * 1.1 + 1e-9
            || secondGap < grid * 0.9 - 1e-9 || secondGap > grid * 1.1 + 1e-9
            || firstGap > maximumSweepGap + 1e-9
            || !adjacentLaneStep(heads->at(firstRight).marker->lane,
                heads->at(secondRight).marker->lane)
            || !adjacentLaneStep(heads->at(firstLeft).marker->lane,
                heads->at(secondLeft).marker->lane)
            || !adjacentLaneStep(heads->at(secondRight).marker->lane,
                heads->at(thirdLeft).marker->lane)
            || !adjacentLaneStep(heads->at(secondLeft).marker->lane,
                heads->at(thirdRight).marker->lane)
            || adjacentLaneStep(heads->at(secondRight).marker->lane,
                heads->at(thirdRight).marker->lane)
            || adjacentLaneStep(heads->at(secondLeft).marker->lane,
                heads->at(thirdLeft).marker->lane)
            || !canRewrite.at(heads->at(thirdLeft).token)
            || !canRewrite.at(heads->at(thirdRight).token)) continue;
        auto& leftFinish = (*heads)[thirdRight];
        auto& rightFinish = (*heads)[thirdLeft];
        allowRecalculation(leftFinish.token);
        allowRecalculation(rightFinish.token);
        leftFinish.patternHand = 0;
        rightFinish.patternHand = 1;
        leftFinish.sweepPrevious = secondLeft;
        rightFinish.sweepPrevious = secondRight;
        if (moment + 3 < moments.size() && moments.at(moment + 3).size() == 1) {
            const int followingIndex = moments.at(moment + 3).first();
            auto& following = (*heads)[followingIndex];
            const double delay = following.marker->second - leftFinish.marker->second;
            const bool followsLeft = adjacentLaneStep(leftFinish.marker->lane,
                following.marker->lane) != 0;
            const bool followsRight = adjacentLaneStep(rightFinish.marker->lane,
                following.marker->lane) != 0;
            if (following.marker->type == QStringLiteral("tap")
                && delay > 0.0 && delay <= qMin(8.0 * grid, 0.45) + 1e-9
                && followsLeft != followsRight
                && canRewrite.at(following.token) && selectedTokens.at(following.token)) {
                allowRecalculation(following.token);
                following.patternHand = followsLeft ? 0 : 1;
                following.reboundPrevious = followsLeft ? thirdRight : thirdLeft;
            }
        }
        moment += 2;
    }

    for (int first = 0; first < order.size();) {
        int last = first + 1;
        if (last >= order.size()) break;
        const int firstLane = heads->at(order.at(first)).marker->lane;
        const int secondLane = heads->at(order.at(last)).marker->lane;
        if (!adjacentLaneStep(firstLane, secondLane)) { ++first; continue; }
        while (last < order.size()) {
            const auto& previous = heads->at(order.at(last - 1));
            const auto& current = heads->at(order.at(last));
            const int expected = (last - first) % 2 ? secondLane : firstLane;
            if (previous.patternHand >= 0 || current.patternHand >= 0
                || current.marker->lane != expected || !regularTapStep(previous, current, true)) break;
            ++last;
        }
        bool fiveNoteAxisOverlap = false;
        if (last - first == 3 && first + 4 < order.size()) {
            const int centre = heads->at(order.at(first)).marker->lane;
            const int step = adjacentLaneStep(centre, heads->at(order.at(first + 1)).marker->lane);
            fiveNoteAxisOverlap = heads->at(order.at(first + 4)).marker->lane == centre
                && adjacentLaneStep(centre, heads->at(order.at(first + 3)).marker->lane) == -step
                && regularTapStep(heads->at(order.at(first + 2)), heads->at(order.at(first + 3)), true)
                && regularTapStep(heads->at(order.at(first + 3)), heads->at(order.at(first + 4)), true);
        }
        if (last - first == 3 && first >= 2) {
            const int centre = heads->at(order.at(first)).marker->lane;
            const int step = adjacentLaneStep(centre, heads->at(order.at(first + 1)).marker->lane);
            fiveNoteAxisOverlap |= heads->at(order.at(first - 2)).marker->lane == centre
                && adjacentLaneStep(centre, heads->at(order.at(first - 1)).marker->lane) == -step
                && regularTapStep(heads->at(order.at(first - 2)), heads->at(order.at(first - 1)), true)
                && regularTapStep(heads->at(order.at(first - 1)), heads->at(order.at(first)), true);
        }
        if (last - first >= 3 && !fiveNoteAxisOverlap && eligible(first, last)) {
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.interactionGroup = first;
                head.interactionStep = i - first;
            }
            first = last;
        } else ++first;
    }

    // After an Each, an empty subdivision gives its same-key hand time to
    // strike again. Revisit stale simple labels without touching rapid jacks.
    for (int i = 0; i + 2 < order.size(); ++i) {
        const auto& a = heads->at(order.at(i));
        const auto& b = heads->at(order.at(i + 1));
        auto& next = (*heads)[order.at(i + 2)];
        if (a.marker->type != QStringLiteral("tap") || b.marker->type != QStringLiteral("tap")
            || next.marker->type != QStringLiteral("tap") || next.marker->isEach
            || a.marker->eachGroupId < 0 || a.marker->eachGroupId != b.marker->eachGroupId
            || std::abs(a.marker->second - b.marker->second) > 1e-9
            || next.marker->second <= a.marker->second + 1e-9
            || next.patternHand >= 0 || !canRewrite.at(next.token)
            || !selectedTokens.at(next.token)) continue;
        const double grid = 240.0 / (a.marker->tempoBpm * a.marker->subdivision);
        const double gap = next.marker->second - a.marker->second;
        const bool relaxed = gap > kFastRepeatedTapGapSeconds + 1e-9
            && gap >= grid * 0.9 - 1e-9 && gap <= grid * 1.1 + 1e-9;
        const bool delayed = gap >= grid * 1.9 - 1e-9 && gap <= grid * 3.1 + 1e-9;
        if (!relaxed && !delayed) continue;
        const int source = a.marker->lane == next.marker->lane ? order.at(i)
            : b.marker->lane == next.marker->lane ? order.at(i + 1) : -1;
        if (source < 0) continue;
        allowRecalculation(next.token);
        next.reboundPrevious = source;
        const int known = handIndex(heads->at(source).hand);
        if (known >= 0) next.patternHand = known;
    }
    for (int moment = 1; moment < moments.size(); ++moment) {
        const auto& previous = moments.at(moment - 1);
        const auto& current = moments.at(moment);
        if (previous.size() != 1 || current.size() != 2) continue;
        if (!canRewrite.at(heads->at(current.at(0)).token)
            || !canRewrite.at(heads->at(current.at(1)).token)
            || !selectedTokens.at(heads->at(current.at(0)).token)
            || !selectedTokens.at(heads->at(current.at(1)).token)) continue;
        const int source = previous.constFirst();
        const auto& anchor = heads->at(source);
        if (anchor.marker->type != QStringLiteral("tap") || anchor.marker->isEach
            || anchor.reboundPrevious < 0) continue;
        for (int index : current) {
            auto& head = (*heads)[index];
            if (head.marker->type != QStringLiteral("tap")
                || !head.marker->isEach
                || head.marker->lane != anchor.marker->lane
                || head.patternHand >= 0 || !canRewrite.at(head.token)
                || !selectedTokens.at(head.token)) continue;
            const double grid = 240.0 / (anchor.marker->tempoBpm * anchor.marker->subdivision);
            const double gap = head.marker->second - anchor.marker->second;
            if (gap <= kFastRepeatedTapGapSeconds + 1e-9
                || gap < grid * 0.9 - 1e-9 || gap > grid * 1.1 + 1e-9) continue;
            allowRecalculation(heads->at(current.at(0)).token);
            allowRecalculation(heads->at(current.at(1)).token);
            head.reboundPrevious = source;
        }
    }

    // A-B-A-C-A is a five-note axis with opposite neighbours. The centre
    // repeats on one hand while the other hand takes both outer notes.
    for (int first = 0; first + 4 < order.size(); ++first) {
        const int centreLane = heads->at(order.at(first)).marker->lane;
        const int firstStep = adjacentLaneStep(centreLane, heads->at(order.at(first + 1)).marker->lane);
        if (!firstStep || heads->at(order.at(first + 2)).marker->lane != centreLane
            || heads->at(order.at(first + 4)).marker->lane != centreLane
            || adjacentLaneStep(centreLane, heads->at(order.at(first + 3)).marker->lane) != -firstStep
            || !eligible(first, first + 5)) continue;
        bool regular = true;
        for (int i = first + 1; i < first + 5; ++i)
            regular &= heads->at(order.at(i)).interactionGroup < 0
                && heads->at(order.at(i - 1)).interactionGroup < 0
                && regularTapStep(heads->at(order.at(i - 1)), heads->at(order.at(i)), true);
        if (!regular) continue;
        for (int i = first; i < first + 5; ++i) {
            auto& head = (*heads)[order.at(i)];
            allowRecalculation(head.token);
            head.interactionGroup = first;
            head.interactionStep = i - first;
        }
        first += 4;
    }

    // A visible star followed by three relaxed taps on its own key uses the
    // tapping hand on that key and the other hand on the overlapping trace.
    for (int sustainIndex = 0; sustainIndex < sustains->size(); ++sustainIndex) {
        auto& sustain = (*sustains)[sustainIndex];
        if (sustain.marker->type != QStringLiteral("slide") || sustain.head < 0
            || !sustain.marker->hasHeadStar || sustain.oneStrokePrevious >= 0
            || sustain.oneStrokeNext >= 0 || !canRewrite.at(sustain.token)) continue;
        const auto& star = heads->at(sustain.head);
        if (star.patternHand >= 0) continue;
        const auto start = std::find(order.cbegin(), order.cend(), sustain.head);
        if (start == order.cend()) continue;
        QVector<int> taps;
        double previousSecond = star.marker->second;
        double interval = 0.0;
        const TimelineNoteMarker* previousMarker = star.marker;
        for (auto it = start + 1; it != order.cend(); ++it) {
            const auto& head = heads->at(*it);
            if (head.marker->type != QStringLiteral("tap")
                || head.marker->lane != star.marker->lane
                || head.marker->isEach || head.patternHand >= 0
                || !canRewrite.at(head.token)) break;
            const double gap = head.marker->second - previousSecond;
            const double grid = 240.0 / (previousMarker->tempoBpm * previousMarker->subdivision);
            const int grids = int(std::round(gap / grid));
            if (gap <= kFastRepeatedTapGapSeconds + 1e-9 || gap > 0.32
                || grids < 1 || grids > 4
                || std::abs(gap - grids * grid) > grids * grid * 0.1 + 1e-9
                || (interval > 0.0 && std::abs(gap - interval) > interval * 0.1 + 1e-9)
                || head.marker->second > sustain.end + 1.1 * gap + 1e-9) break;
            taps.append(*it);
            interval = gap;
            previousSecond = head.marker->second;
            previousMarker = head.marker;
        }
        if (taps.size() < 3) continue;
        const int tappingHand = handIndex(laneHand(star.marker->lane));
        allowRecalculation(sustain.token);
        sustain.patternHand = 1 - tappingHand;
        sustain.splitSharedStar = true;
        sustain.tapLaneReserved = true;
        for (int i = 0; i < taps.size(); ++i) {
            auto& tap = (*heads)[taps.at(i)];
            allowRecalculation(tap.token);
            tap.patternHand = tappingHand;
            if (i > 0) tap.reboundPrevious = taps.at(i - 1);
        }
    }

    // A coordinated launch keeps the star, launch tap and trace on one hand;
    // the other hand takes the adjacent tap. A handoff remains an alternative
    // in the structural choices rather than the layout default.
    for (int sustainIndex = 0; sustainIndex < sustains->size(); ++sustainIndex) {
        auto& sustain = (*sustains)[sustainIndex];
        if (sustain.marker->type != QStringLiteral("slide") || sustain.head < 0
            || !sustain.marker->hasHeadStar || sustain.oneStrokePrevious >= 0
            || sustain.oneStrokeNext >= 0 || !canRewrite.at(sustain.token)
            || sustain.patternHand >= 0) continue;
        const int lane = sustain.marker->lane;
        const int travel = std::abs(sustain.marker->endLane - lane);
        if ((lane != 1 && lane != 8) || (lane == 1 && sustain.marker->endLane < 5)
            || (lane == 8 && sustain.marker->endLane > 4)
            || qMin(travel, 8 - travel) < 3) continue;
        const int innerLane = lane == 1 ? 2 : 7;
        int startingTap = -1, neighbour = -1;
        for (int index = 0; index < heads->size(); ++index) {
            const auto& head = heads->at(index);
            if (head.marker->type != QStringLiteral("tap") || !canRewrite.at(head.token)
                || std::abs(head.marker->second - sustain.marker->slideTraceSecond) > 1e-6) continue;
            if (head.marker->lane == lane) {
                if (startingTap >= 0) { startingTap = -2; break; }
                startingTap = index;
            } else if (head.marker->lane == innerLane) {
                if (neighbour >= 0) { neighbour = -2; break; }
                neighbour = index;
            }
        }
        if (startingTap < 0 || neighbour < 0
            || heads->at(startingTap).patternHand >= 0
            || heads->at(neighbour).patternHand >= 0) continue;
        const int strikingHand = heads->at(sustain.head).point.x() < heads->at(neighbour).point.x() ? 0 : 1;
        allowRecalculation(sustain.token);
        allowRecalculation(heads->at(startingTap).token);
        allowRecalculation(heads->at(neighbour).token);
        sustain.patternHand = strikingHand;
        sustain.tapLaneReserved = true;
        (*heads)[sustain.head].patternHand = strikingHand;
        (*heads)[startingTap].patternHand = strikingHand;
        (*heads)[neighbour].patternHand = 1 - strikingHand;
    }

    // Other fast same-lane runs alternate individual taps when both hands are free.
    for (int first = 0; first < order.size();) {
        const auto& initial = heads->at(order.at(first));
        if (!chart::handOrderIsButtonStrike(*initial.marker) || initial.patternHand >= 0) { ++first; continue; }
        int last = first + 1;
        while (last < order.size() && heads->at(order.at(last)).patternHand < 0
            && chart::handOrderIsButtonStrike(*heads->at(order.at(last)).marker)
            && chart::handOrderFastRepeatStep(*heads->at(order.at(last - 1)).marker, *heads->at(order.at(last)).marker)) ++last;
        for (int start = first; start < last;) {
            int finish = start + 1;
            const int lane = heads->at(order.at(start)).marker->lane;
            while (finish < last && heads->at(order.at(finish)).marker->lane == lane) ++finish;
            if (finish - start >= 3 && eligible(start, finish)) {
                const int initialHand = handIndex(laneHand(lane));
                for (int i = start; i < finish; ++i) {
                    auto& head = (*heads)[order.at(i)];
                    allowRecalculation(head.token);
                    head.patternHand = initialHand ^ ((i - start) % 2);
                    head.patternNeedsBothFree = true;
                }
            }
            start = finish;
        }
        first = last;
    }

    // Repeated pairs at a relaxed cadence have no rebound pressure that
    // justifies swapping hands inside either pair.
    for (int first = 0; first + 3 < order.size();) {
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        const auto& c = heads->at(order.at(first + 2));
        const auto& d = heads->at(order.at(first + 3));
        const int firstLane = a.marker->lane;
        const int secondLane = c.marker->lane;
        const bool pairShape = a.marker->type == QStringLiteral("tap")
            && b.marker->type == QStringLiteral("tap")
            && c.marker->type == QStringLiteral("tap")
            && d.marker->type == QStringLiteral("tap")
            && firstLane == b.marker->lane && secondLane == d.marker->lane
            && !a.windingPair && !b.windingPair && !c.windingPair && !d.windingPair
            && adjacentLaneStep(firstLane, secondLane) != 0;
        const auto relaxedGap = [](const LayoutHead& left, const LayoutHead& right) {
            const double gap = right.marker->second - left.marker->second;
            // Short paired bursts at 125ms remain one relaxed rebound gesture;
            // 62.5ms jacks do not. Idle gaps beyond the return delay end it.
            return gap >= 0.12 - 1e-9 && gap <= chart::handOrderReturnDelaySeconds + 1e-9;
        };
        if (pairShape && eligible(first, first + 4)
            && relaxedGap(a, b) && relaxedGap(b, c) && relaxedGap(c, d)) {
            for (int i = first; i < first + 4; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.patternHand = handIndex(laneHand(i < first + 2 ? firstLane : secondLane));
                head.patternNeedsBothFree = true;
                if (i == first + 1 || i == first + 3) head.relaxedReboundPrevious = order.at(i - 1);
            }
            first += 4;
        } else ++first;
    }

    for (int moment = 1; moment < moments.size(); ++moment) {
        const auto& previous = moments.at(moment - 1);
        const auto& current = moments.at(moment);
        if (previous.size() != 1 || current.size() != 1) continue;
        const int anchor = previous.first();
        auto& target = (*heads)[current.first()];
        if (!canRewrite.at(target.token) || !selectedTokens.at(target.token) || target.patternHand >= 0
            || target.interactionGroup >= 0 || target.followSlideTail >= 0 || target.reboundPrevious >= 0
            || target.sweepPrevious >= 0 || target.windingPrevious >= 0
            || !chart::handOrderSameKeyRebound(*heads->at(anchor).marker, *target.marker)) continue;
        allowRecalculation(target.token);
        target.relaxedReboundPrevious = anchor;
    }

    // Adjacent E sensors are intentionally not a simultaneous Touchgroup,
    // yet a timed E-ring run is one continuous hand sweep.
    for (int first = 0; first + 2 < order.size();) {
        int last = first + 1;
        while (last < order.size()) {
            const auto& previous = heads->at(order.at(last - 1));
            const auto& current = heads->at(order.at(last));
            if (!nearbyTouches(previous, current) || previous.patternHand >= 0
                || current.patternHand >= 0 || previous.marker->tempoBpm <= 0
                || previous.marker->subdivision <= 0) break;
            const double grid = 240.0 / (previous.marker->tempoBpm * previous.marker->subdivision);
            const double gap = current.marker->second - previous.marker->second;
            if (gap < grid * 0.9 - 1e-9 || gap > qMin(grid * 1.1, kTouchSweepGapSeconds) + 1e-9) break;
            ++last;
        }
        if (last - first >= 3 && eligible(first, last)) {
            const auto& start = heads->at(order.at(first));
            int hand = handIndex(basicLayoutHandForPoint(start.point));
            if (hand < 0) hand = handIndex(basicLayoutHandForPoint(heads->at(order.at(last - 1)).point));
            if (hand >= 0)
                for (int i = first; i < last; ++i) {
                    auto& head = (*heads)[order.at(i)];
                    allowRecalculation(head.token);
                    head.patternHand = hand;
                }
            first = last;
        } else ++first;
    }

    for (int i = 0; i + 1 < order.size(); ++i) {
        auto& first = (*heads)[order.at(i)];
        auto& second = (*heads)[order.at(i + 1)];
        if (!nearbyTouches(first, second) || !eligible(i, i + 2)) continue;
        const double gap = second.marker->second - first.marker->second;
        const double pseudoEach = chart::handOrderPseudoEachTolerance(*first.marker, *second.marker);
        if (gap < -1e-9) continue;
        if (gap < 1e-9 && chart::touchPadsAreAdjacent(first.marker->touchPad, second.marker->touchPad)
            && first.marker->eachGroupId >= 0
            && first.marker->eachGroupId == second.marker->eachGroupId) {
            if (first.patternHand >= 0 && second.patternHand >= 0
                && first.patternHand != second.patternHand) continue;
            allowRecalculation(first.token);
            allowRecalculation(second.token);
            if (first.patternHand < 0) first.patternHand = second.patternHand;
            if (second.patternHand < 0) second.patternHand = first.patternHand;
            first.touchPairWith = order.at(i + 1);
            second.touchPairWith = order.at(i);
            ++i;
        } else if (first.patternHand < 0 && second.patternHand < 0
            && gap >= pseudoEach - 1e-9 && gap <= kTouchSweepGapSeconds + 1e-9) {
            const auto preferred = basicLayoutHandForPoint((first.point + second.point) / 2.0);
            const int hand = handIndex(preferred);
            if (hand < 0) continue;
            allowRecalculation(first.token);
            allowRecalculation(second.token);
            first.patternHand = hand;
            second.patternHand = hand;
            ++i;
        }
    }

    // Overlapping stars whose heads alternate across the screen reserve the
    // corresponding hand for the whole track, including old simple labels.
    for (int first = 0; first < order.size();) {
        const auto& initial = heads->at(order.at(first));
        if (initial.marker->type != QStringLiteral("slide") || !initial.marker->hasHeadStar
            || std::any_of(sustainsByHead.at(order.at(first)).cbegin(), sustainsByHead.at(order.at(first)).cend(),
                [&](int index) { return sustains->at(index).oneStrokePrevious >= 0 || sustains->at(index).oneStrokeNext >= 0; })) {
            ++first;
            continue;
        }
        int last = first + 1;
        while (last < order.size()) {
            const auto& previous = heads->at(order.at(last - 1));
            const auto& next = heads->at(order.at(last));
            if (next.marker->type != QStringLiteral("slide") || !next.marker->hasHeadStar
                || std::any_of(sustainsByHead.at(order.at(last)).cbegin(), sustainsByHead.at(order.at(last)).cend(),
                    [&](int index) { return sustains->at(index).oneStrokePrevious >= 0 || sustains->at(index).oneStrokeNext >= 0; })
                || previous.marker->endSecond < next.marker->second - 1e-9
                || next.marker->second <= previous.marker->second + 1e-9
                || handIndex(laneHand(previous.marker->lane)) == handIndex(laneHand(next.marker->lane))) break;
            ++last;
        }
        if (last - first >= 3 && eligible(first, last)) {
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                const int hand = handIndex(laneHand(head.marker->lane));
                allowRecalculation(head.token);
                head.patternHand = hand;
                for (int index : sustainsByHead.at(order.at(i))) (*sustains)[index].patternHand = hand;
            }
        }
        first = last;
    }
}

HandOrderHand handAt(const LayoutSustain& sustain, double second)
{
    if (!sustain.fixed || sustain.marker->handOrderTrackStops.size() <= 1) return sustain.hand;
    const auto& marker = *sustain.marker;
    const double duration = marker.endSecond - marker.slideTraceSecond;
    if (duration <= 0.0) return sustain.hand;
    const double progress = qBound(0.0, (second - marker.slideTraceSecond) / duration, 1.0);
    HandOrderHand hand = sustain.hand;
    for (const auto& stop : marker.handOrderTrackStops) {
        if (stop.progress > progress + 1e-9) break;
        hand = stop.hand;
    }
    return hand;
}

HandOrderHand trackHandAt(const QVector<HandOrderTrackStop>& stops, double progress,
                         HandOrderHand fallback)
{
    HandOrderHand hand = fallback;
    for (const auto& stop : stops) {
        if (stop.progress > progress + 1e-9) break;
        hand = stop.hand;
    }
    return hand;
}

bool wifiOccupiesHand(const TimelineNoteMarker& marker, int hand, double second)
{
    if (second < marker.slideTraceSecond - 1e-9
        || second > marker.endSecond + 1e-9) return false;
    const double duration = marker.endSecond - marker.slideTraceSecond;
    const double progress = duration > 0.0
        ? qBound(0.0, (second - marker.slideTraceSecond) / duration, 1.0) : 0.0;
    const auto occupied = [hand](HandOrderHand selected) {
        return selected == HandOrderHand::Either || handIndex(selected) == hand;
    };
    if (marker.handOrderWifiMode == HandOrderWifiMode::Split
        && marker.handOrderWifiSectorStops.size() == 2) {
        for (int sector = 0; sector < 2; ++sector)
            if (occupied(trackHandAt(marker.handOrderWifiSectorStops.at(sector), progress,
                    basicLayoutWifiSectorHand(marker, sector)))) return true;
        return false;
    }
    if (!marker.handOrderTrackStops.isEmpty())
        return occupied(trackHandAt(marker.handOrderTrackStops, progress,
            marker.handOrderHead));
    // A new Wifi receives two sector hands in the basic layout.
    return true;
}

// This is a local scheduling constraint, not a judgement or playability proof.
void assignLocalHands(QVector<LayoutHead>* heads, QVector<LayoutSustain>* sustains,
    const QVector<LayoutWifiOccupancy>& wifiTracks, double returnSpeed)
{
    const auto order = orderedHeads(*heads);
    QPointF position[2] = {chart::handOrderNaturalRestPoint(0), chart::handOrderNaturalRestPoint(1)};
    double positionTime[2] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    QVector<bool> ended(sustains->size(), false);
    const auto busy = [&](int hand, double second, int token) {
        for (const auto& wifi : wifiTracks)
            if (wifi.token != token && wifiOccupiesHand(*wifi.marker, hand, second))
                return true;
        for (const auto& sustain : *sustains) {
            if (sustain.token == token || sustain.marker->second > second + 1e-9
                || sustain.end < second - 1e-9) continue;
            if (sustain.marker->type == QStringLiteral("slide")
                && second < sustain.marker->slideTraceSecond - 1e-9
                && std::any_of(heads->cbegin(),heads->cend(),[&](const auto& head) {
                    return head.token == token && head.gestureDefault;
                })) continue;
            if (sustain.marker->type == QStringLiteral("slide")
                && std::abs(sustain.end - second) <= 1e-9) {
                bool adjacentRelease = false;
                for (const auto& head : *heads)
                    adjacentRelease |= head.token == token
                        && (head.marker->type == QStringLiteral("tap")
                            || head.marker->type == QStringLiteral("hold"))
                        && std::abs(head.marker->second - second) <= 1e-9
                        && adjacentLaneStep(sustain.marker->endLane, head.marker->lane) != 0;
                if (adjacentRelease) continue;
            }
            if (sustain.marker->type == "slide" && std::abs(sustain.marker->slideTraceSecond-second) <= 1e-9) {
                bool launch = false;
                for (const auto& head : *heads) if (head.token == token && std::abs(head.marker->second-second) <= 1e-9) {
                    const auto& m = *head.marker;
                    launch |= (m.type == "tap" || (m.type == "slide" && m.hasHeadStar)) && m.lane == sustain.marker->lane;
                    launch |= m.type == "touch" && m.touchPad == "A" + QString::number(sustain.marker->lane);
                }
                if (launch) continue;
            }
            if (handIndex(handAt(sustain, second)) == hand) return true;
        }
        return false;
    };
    const auto trackBusyDuring = [&](int hand, int targetIndex) {
        const auto& target = sustains->at(targetIndex);
        const double targetStart = target.marker->type == QStringLiteral("slide")
            ? target.marker->slideTraceSecond : target.marker->second;
        for (int otherIndex = 0; otherIndex < sustains->size(); ++otherIndex) {
            if (otherIndex == targetIndex) continue;
            const auto& other = sustains->at(otherIndex);
            if (target.oneStrokePrevious == otherIndex
                || target.oneStrokeNext == otherIndex
                || other.oneStrokePrevious == targetIndex
                || other.oneStrokeNext == targetIndex) continue;
            if (target.marker->type == QStringLiteral("slide")
                && other.marker->type == QStringLiteral("slide")
                && std::abs(target.marker->slideTraceSecond
                    - other.marker->slideTraceSecond) < 1e-9
                && std::abs(target.end - other.end) < 1e-9
                && !divergentSlideTracks(*target.marker, *other.marker)) continue;
            const double otherStart = other.marker->type == QStringLiteral("slide")
                ? other.marker->slideTraceSecond : other.marker->second;
            const double first = qMax(targetStart, otherStart);
            const double last = qMin(target.end, other.end);
            if (last - first <= 1e-9) continue;
            QVector<double> boundaries{first, last};
            if (other.fixed && other.marker->type == QStringLiteral("slide"))
                for (const auto& stop : other.marker->handOrderTrackStops) {
                    const double at = otherStart + stop.progress * (other.end - otherStart);
                    if (at > first + 1e-9 && at < last - 1e-9)
                        boundaries.append(at);
                }
            std::sort(boundaries.begin(), boundaries.end());
            for (int i = 1; i < boundaries.size(); ++i) {
                const double middle = (boundaries.at(i - 1) + boundaries.at(i)) / 2.0;
                if (handIndex(handAt(other, middle)) == hand) return true;
            }
        }
        for (const auto& wifi : wifiTracks) {
            const auto& marker = *wifi.marker;
            const double first = qMax(targetStart, marker.slideTraceSecond);
            const double last = qMin(target.end, marker.endSecond);
            if (last - first <= 1e-9) continue;
            QVector<double> boundaries{first, last};
            const auto addStops = [&](const QVector<HandOrderTrackStop>& stops) {
                for (const auto& stop : stops) {
                    const double at = marker.slideTraceSecond
                        + stop.progress * (marker.endSecond - marker.slideTraceSecond);
                    if (at > first + 1e-9 && at < last - 1e-9)
                        boundaries.append(at);
                }
            };
            addStops(marker.handOrderTrackStops);
            for (const auto& sector : marker.handOrderWifiSectorStops) addStops(sector);
            std::sort(boundaries.begin(), boundaries.end());
            for (int i = 1; i < boundaries.size(); ++i)
                if (wifiOccupiesHand(marker, hand,
                    (boundaries.at(i - 1) + boundaries.at(i)) / 2.0)) return true;
        }
        return false;
    };
    const auto headBodyBusy = [&](int index, int hand) {
        for (int sustainIndex = 0; sustainIndex < sustains->size(); ++sustainIndex) {
            const auto& sustain = sustains->at(sustainIndex);
            if (sustain.head != index || sustain.fixed
                || (sustain.marker->type != QStringLiteral("hold")
                    && sustain.marker->type != QStringLiteral("touch_hold"))) continue;
            if (trackBusyDuring(hand, sustainIndex)) return true;
        }
        return false;
    };
    const auto headUnavailable = [&](int index, int hand) {
        const auto& head = heads->at(index);
        return busy(hand, head.marker->second, head.token)
            || headBodyBusy(index, hand);
    };
    const auto carryOneStrokeHand = [&](int firstIndex) {
        int previousIndex = firstIndex;
        while (previousIndex >= 0 && sustains->at(previousIndex).oneStrokeNext >= 0) {
            const int nextIndex = sustains->at(previousIndex).oneStrokeNext;
            auto& next = (*sustains)[nextIndex];
            const int hand = handIndex(sustains->at(previousIndex).hand);
            if (hand < 0 || next.fixed || handIndex(next.hand) >= 0) break;
            bool occupied = false;
            for (int otherIndex = 0; otherIndex < sustains->size(); ++otherIndex) {
                if (otherIndex == previousIndex || otherIndex == nextIndex) continue;
                const auto& other = sustains->at(otherIndex);
                occupied |= other.marker->second <= next.marker->slideTraceSecond + 1e-9
                    && other.end > next.marker->slideTraceSecond + 1e-9
                    && handIndex(handAt(other, next.marker->slideTraceSecond)) == hand;
            }
            if (occupied || trackBusyDuring(hand, nextIndex)) break;
            next.hand = sustains->at(previousIndex).hand;
            previousIndex = nextIndex;
        }
    };
    QVector<int> headlessOrder;
    for (int index = 0; index < sustains->size(); ++index)
        if (sustains->at(index).head < 0 && !sustains->at(index).fixed
            && sustains->at(index).oneStrokePrevious < 0)
            headlessOrder.append(index);
    std::stable_sort(headlessOrder.begin(), headlessOrder.end(), [&](int a, int b) {
        return sustains->at(a).marker->slideTraceSecond
            < sustains->at(b).marker->slideTraceSecond;
    });
    int nextHeadless = 0;
    const auto assignHeadlessThrough = [&](double second) {
        while (nextHeadless < headlessOrder.size()) {
            const int index = headlessOrder.at(nextHeadless);
            auto& sustain = (*sustains)[index];
            if (sustain.marker->slideTraceSecond > second + 1e-9) break;
            ++nextHeadless;
            const int preferred = handIndex(basicLayoutTrackHand(*sustain.marker));
            if (preferred < 0) continue;
            for (int candidate : {preferred, 1 - preferred})
                if (!trackBusyDuring(candidate, index)) {
                    sustain.hand = candidate == 0 ? HandOrderHand::Left : HandOrderHand::Right;
                    break;
                }
            if (sustain.oneStrokeNext >= 0 && handIndex(sustain.hand) >= 0)
                carryOneStrokeHand(index);
        }
    };
    const auto headCost = [&](int index, int hand) {
        const auto& head = heads->at(index);
        const auto preferred = basicLayoutHeadHand(*head.marker);
        const double sideCost = handIndex(preferred) >= 0 && handIndex(preferred) != hand ? 0.25 : 0.0;
        const QPointF origin = chart::handOrderRestedPoint(position[hand], positionTime[hand],
            head.marker->second, hand, returnSpeed);
        return QLineF(origin, head.point).length() + sideCost
            + (headUnavailable(index, hand) ? 100.0 : 0.0);
    };
    const auto commitHead = [&](int index, int hand) {
        auto& head = (*heads)[index];
        if (!head.fixed) head.hand = hand < 0 ? HandOrderHand::Either
            : hand == 0 ? HandOrderHand::Left : HandOrderHand::Right;
        const int actual = handIndex(head.hand);
        if (actual >= 0) {
            position[actual] = head.point;
            positionTime[actual] = head.marker->second;
        }
    };
    for (int first = 0; first < order.size();) {
        const auto& anchor = heads->at(order.at(first));
        assignHeadlessThrough(anchor.marker->second);
        for (int index = 0; index < sustains->size(); ++index) {
            const auto& sustain = sustains->at(index);
            if (ended.at(index) || sustain.end > anchor.marker->second + 1e-9) continue;
            ended[index] = true;
            const int hand = handIndex(handAt(sustain, sustain.end));
            if (hand < 0 || sustain.end < positionTime[hand]) continue;
            position[hand] = sustain.marker->type == QStringLiteral("slide")
                ? lanePoint(sustain.marker->endLane) : headPoint(*sustain.marker);
            positionTime[hand] = sustain.end;
        }
        int last = first + 1;
        while (last < order.size()) {
            const auto& next = heads->at(order.at(last));
            const double threshold = chart::handOrderPseudoEachTolerance(*anchor.marker, *next.marker);
            if (next.marker->second - anchor.marker->second >= threshold - 1e-9) break;
            ++last;
        }
        if (anchor.interactionGroup >= 0 && anchor.interactionStep == 0) {
            QVector<int> members;
            for (int index : order)
                if (heads->at(index).interactionGroup == anchor.interactionGroup) members.append(index);
            const int preferred = anchor.structuralPhase ? anchor.patternHand : handIndex(laneHand(anchor.marker->lane));
            int selectedPhase = -1;
            double best = std::numeric_limits<double>::infinity();
            for (int phase : {preferred, 1 - preferred}) {
                QPointF projectedPosition[2] = {position[0], position[1]};
                double projectedTime[2] = {positionTime[0], positionTime[1]};
                double cost = 0.0;
                bool valid = true;
                for (int index : members) {
                    const auto& head = heads->at(index);
                    const int hand = phase ^ (head.interactionStep % 2);
                    const double second = head.marker->second;
                    if (headUnavailable(index, hand)) { valid = false; break; }
                    const QPointF from = chart::handOrderRestedPoint(projectedPosition[hand], projectedTime[hand],
                        second, hand, returnSpeed);
                    const double distance = QLineF(from, head.point).length();
                    const double gap = second - projectedTime[hand];
                    if (std::isfinite(gap) && gap <= 1e-9 && distance > 1e-6) { valid = false; break; }
                    const double travelSeconds = std::isfinite(gap) ? qMax(0.05, gap) : 1.0;
                    cost += distance + 0.05 * distance * distance / travelSeconds;
                    if (handIndex(basicLayoutHeadHand(*head.marker)) != hand) cost += 0.25;
                    projectedPosition[hand] = head.point;
                    projectedTime[hand] = second;
                }
                // Start from the structural default when both complete phases
                // fit. The final review has both neighbouring configurations.
                if (valid && anchor.preferStructuralStart) { selectedPhase = phase; break; }
                if (valid && cost < best) { best = cost; selectedPhase = phase; }
            }
            if (selectedPhase >= 0)
                for (int index : members) {
                    auto& head = (*heads)[index];
                    head.patternHand = selectedPhase ^ (head.interactionStep % 2);
                }
            else if (anchor.structuralPhase)
                for (int index : members) {
                    auto& head = (*heads)[index];
                    head.patternHand = -1;
                    head.gestureDefault = false;
                }
        }
        for (int i = first; i < last; ++i) {
            auto& head = (*heads)[order.at(i)];
            if (head.fixed || head.followSlideTail < 0) continue;
            const auto& previous = sustains->at(head.followSlideTail);
            head.patternHand = handIndex(handAt(previous, previous.end));
        }
        for (int i = first; i < last; ++i) {
            auto& head = (*heads)[order.at(i)];
            const int previous = head.reboundPrevious >= 0 ? head.reboundPrevious : head.relaxedReboundPrevious;
            if (head.fixed || previous < 0) continue;
            const int previousHand = handIndex(heads->at(previous).hand);
            if (previousHand >= 0 && !headUnavailable(order.at(i), previousHand)
                && (head.reboundPrevious >= 0 || head.patternHand < 0)) head.patternHand = previousHand;
        }
        QVector<SharedHeadUnit> headUnits;
        for (int i = first; i < last; ++i) {
            const auto& head = heads->at(order.at(i));
            int key = i;
            if (head.marker->type == QStringLiteral("tap")
                || (head.marker->type == QStringLiteral("slide") && head.marker->hasHeadStar)) {
                for (int starIndex = first; starIndex < last; ++starIndex) {
                    const auto& star = heads->at(order.at(starIndex));
                    if (star.marker->type == QStringLiteral("slide") && star.marker->hasHeadStar
                        && star.marker->lane == head.marker->lane
                        && std::abs(star.marker->second - head.marker->second) < 1e-9) {
                        key = starIndex;
                        break;
                    }
                }
            }
            auto unit = std::find_if(headUnits.begin(), headUnits.end(),
                [key](const SharedHeadUnit& candidate) { return candidate.key == key; });
            if (unit == headUnits.end()) headUnits.append(SharedHeadUnit{key, {order.at(i)}});
            else unit->members.append(order.at(i));
        }
        const bool hasSharedHeads = std::any_of(headUnits.cbegin(), headUnits.cend(),
            [](const SharedHeadUnit& unit) { return unit.members.size() > 1; });
        if (hasSharedHeads && headUnits.size() <= 2) {
            const auto representative = [&](const SharedHeadUnit& unit) -> const LayoutHead& {
                for (int index : unit.members) if (heads->at(index).marker->type == QStringLiteral("slide"))
                    return heads->at(index);
                return heads->at(unit.members.constFirst());
            };
            const auto unitAllowed = [&](const SharedHeadUnit& unit, int hand) {
                const auto& representativeHead = representative(unit);
                for (int index : unit.members) {
                    const auto& head = heads->at(index);
                    if ((head.fixed && handIndex(head.hand) != hand)
                        || (!head.fixed && head.patternHand >= 0 && head.patternHand != hand)
                        || (!head.fixed && headBodyBusy(index, hand))) return false;
                    if (!head.fixed)
                        for (const auto& wifi : wifiTracks)
                            if (wifi.token != head.token
                                && wifiOccupiesHand(*wifi.marker, hand, head.marker->second))
                                return false;
                }
                for (const auto& sustain : *sustains) {
                    bool ownHead = false;
                    for (int index : unit.members) ownHead |= sustain.token == heads->at(index).token;
                    if (ownHead || sustain.marker->second > representativeHead.marker->second + 1e-9
                        || sustain.end < representativeHead.marker->second - 1e-9) continue;
                    if (handIndex(handAt(sustain, representativeHead.marker->second)) == hand) return false;
                }
                return true;
            };
            const auto unitCost = [&](const SharedHeadUnit& unit, int hand) {
                const auto& head = representative(unit);
                const QPointF origin = chart::handOrderRestedPoint(position[hand], positionTime[hand],
                    head.marker->second, hand, returnSpeed);
                return QLineF(origin, head.point).length()
                    + (handIndex(basicLayoutHeadHand(*head.marker)) != hand ? 0.25 : 0.0);
            };
            int firstHand = -1;
            double best = std::numeric_limits<double>::infinity();
            for (int candidate = 0; candidate < 2; ++candidate) {
                if (!unitAllowed(headUnits.at(0), candidate)
                    || (headUnits.size() == 2 && !unitAllowed(headUnits.at(1), 1 - candidate))) continue;
                double cost = unitCost(headUnits.at(0), candidate);
                if (headUnits.size() == 2) {
                    cost += unitCost(headUnits.at(1), 1 - candidate);
                    const auto& a = representative(headUnits.at(0));
                    const auto& b = representative(headUnits.at(1));
                    if ((a.point.x() < b.point.x() && candidate != 0)
                        || (a.point.x() > b.point.x() && candidate != 1)) cost += 2.0;
                }
                if (cost < best) { best = cost; firstHand = candidate; }
            }
            if (firstHand >= 0)
                for (int unitIndex = 0; unitIndex < headUnits.size(); ++unitIndex)
                    for (int index : headUnits.at(unitIndex).members)
                        commitHead(index, unitIndex == 0 ? firstHand : 1 - firstHand);
        } else if (last - first == 2) {
            auto& a = (*heads)[order.at(first)];
            auto& b = (*heads)[order.at(first + 1)];
            if (a.touchPairWith == order.at(first + 1) && b.touchPairWith == order.at(first)) {
                double best = std::numeric_limits<double>::infinity();
                int sharedHand = -1;
                for (int candidate = 0; candidate < 2; ++candidate) {
                    if (headUnavailable(order.at(first), candidate)
                        || headUnavailable(order.at(first + 1), candidate)) continue;
                    const double cost = headCost(order.at(first), candidate)
                        + headCost(order.at(first + 1), candidate);
                    if (cost < best) { best = cost; sharedHand = candidate; }
                }
                if (sharedHand >= 0) {
                    commitHead(order.at(first), sharedHand);
                    commitHead(order.at(first + 1), sharedHand);
                }
                first = last;
                continue;
            }
            double best = std::numeric_limits<double>::infinity();
            int firstHand = -1;
            for (int candidate = 0; candidate < 2; ++candidate) {
                if (a.fixed && handIndex(a.hand) >= 0 && handIndex(a.hand) != candidate) continue;
                if (b.fixed && handIndex(b.hand) >= 0 && handIndex(b.hand) == candidate) continue;
                if ((!a.fixed && a.patternHand >= 0 && a.patternHand != candidate)
                    || (!b.fixed && b.patternHand >= 0 && b.patternHand != 1 - candidate)) continue;
                if ((!a.fixed && headUnavailable(order.at(first), candidate))
                    || (!b.fixed && headUnavailable(order.at(first + 1), 1 - candidate))) continue;
                double cost = headCost(order.at(first), candidate)
                    + headCost(order.at(first + 1), 1 - candidate);
                if ((a.point.x() < b.point.x() && candidate != 0)
                    || (a.point.x() > b.point.x() && candidate != 1)) cost += 2.0;
                if (cost < best) { best = cost; firstHand = candidate; }
            }
            if (firstHand >= 0) {
                commitHead(order.at(first), firstHand);
                commitHead(order.at(first + 1), 1 - firstHand);
            }
        } else if (last - first == 1) {
            const int index = order.at(first);
            const auto& head = heads->at(index);
            const int fixed = handIndex(head.hand);
            const auto preferred = basicLayoutHeadHand(*head.marker);
            if (!head.fixed && headUnavailable(index, 0)
                && headUnavailable(index, 1)) {
                // No legal hand is available; the token stays unassigned.
            } else if (!head.fixed && head.patternHand >= 0
                && !headUnavailable(index, head.patternHand)
                && (!head.patternNeedsBothFree
                    || !headUnavailable(index, 1 - head.patternHand)))
                commitHead(index, head.patternHand);
            else if (!head.fixed && preferred == HandOrderHand::Either
                && !headUnavailable(index, 0)
                && !headUnavailable(index, 1)) commitHead(index, -1);
            else if (!head.fixed && handIndex(preferred) >= 0
                && head.marker->second > chart::handOrderFullyReturnedTime(position[0], positionTime[0], 0, returnSpeed)
                && head.marker->second > chart::handOrderFullyReturnedTime(position[1], positionTime[1], 1, returnSpeed)
                && !headUnavailable(index, handIndex(preferred)))
                commitHead(index, handIndex(preferred));
            else commitHead(index, fixed >= 0 ? fixed
                : headCost(index, 0) <= headCost(index, 1) ? 0 : 1);
        } else {
            // More than two independent heads cannot be assigned to two hands.
            for (int i = first; i < last; ++i) {
                const int index = order.at(i);
                if (heads->at(index).fixed) commitHead(index, handIndex(heads->at(index).hand));
            }
        }
        for (int sustainIndex = 0; sustainIndex < sustains->size(); ++sustainIndex) {
            auto& sustain = (*sustains)[sustainIndex];
            if (sustain.head < 0) continue;
            const auto& head = heads->at(sustain.head);
            if (head.marker->second < anchor.marker->second - 1e-9
                || head.marker->second >= heads->at(order.at(last - 1)).marker->second + 1e-9) continue;
            if (sustain.fixed || sustain.hand != HandOrderHand::Unspecified) continue;
            if (sustain.marker->type == QStringLiteral("hold")
                || sustain.marker->type == QStringLiteral("touch_hold")) {
                sustain.hand = head.hand;
                continue;
            }
            if (sustain.patternHand >= 0 && (sustain.splitSharedStar
                || handIndex(head.hand) == sustain.patternHand)
                && !busy(sustain.patternHand, head.marker->second, sustain.token)
                && !trackBusyDuring(sustain.patternHand, sustainIndex)) {
                sustain.hand = sustain.patternHand == 0 ? HandOrderHand::Left : HandOrderHand::Right;
                continue;
            }
            const auto preferred = basicLayoutTrackHand(*sustain.marker);
            double best = std::numeric_limits<double>::infinity();
            for (int candidate = 0; candidate < 2; ++candidate) {
                if (trackBusyDuring(candidate, sustainIndex)) continue;
                double cost = handIndex(preferred) >= 0 && handIndex(preferred) != candidate ? 1.2 : 0.0;
                if (handIndex(head.hand) >= 0 && handIndex(head.hand) != candidate) cost += 0.8;
                if (busy(candidate, head.marker->second, sustain.token)) cost += 100.0;
                for (const auto& future : *heads) {
                    if (future.token == sustain.token || future.marker->second <= head.marker->second + 1e-9
                        || future.marker->second > sustain.end + 1e-9) continue;
                    const int futureHand = future.patternHand >= 0 ? future.patternHand
                        : handIndex(future.fixed ? future.hand : basicLayoutHeadHand(*future.marker));
                    if (futureHand == candidate) cost += future.patternHand >= 0 ? 4.0 : 1.0;
                }
                if (cost < best) { best = cost; sustain.hand = candidate == 0 ? HandOrderHand::Left : HandOrderHand::Right; }
            }
        }
        for (int index = 0; index < sustains->size(); ++index)
            if (sustains->at(index).oneStrokeNext >= 0
                && handIndex(sustains->at(sustains->at(index).oneStrokeNext).hand) < 0
                && handIndex(sustains->at(index).hand) >= 0) carryOneStrokeHand(index);
        first = last;
    }
    assignHeadlessThrough(std::numeric_limits<double>::infinity());
}
} // namespace

HandOrderHand basicLayoutHandForPoint(const QPointF& point)
{
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return HandOrderHand::Unspecified;
    if (point.x() < -kCentreTolerance) return HandOrderHand::Left;
    if (point.x() > kCentreTolerance) return HandOrderHand::Right;
    return HandOrderHand::Either;
}

HandOrderHand basicLayoutHeadHand(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("touch") || marker.type == QStringLiteral("touch_hold")) {
        // The parser's Touch points use a legacy 540-square canvas origin.
        return basicLayoutHandForPoint(marker.touchPoint - QPointF(kTouchCanvasCentre, kTouchCanvasCentre));
    }
    return laneHand(marker.lane);
}

QPointF basicLayoutTrackStartPoint(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("slide") && !marker.slideSegmentPoints.isEmpty()
        && !marker.slideSegmentPoints.constFirst().isEmpty()) return marker.slideSegmentPoints.constFirst().constFirst();
    if (marker.type == QStringLiteral("wifi") && !marker.wifiLanePoints.isEmpty()
        && !marker.wifiLanePoints.constFirst().isEmpty()) return marker.wifiLanePoints.constFirst().constFirst();
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    return {invalid, invalid};
}

HandOrderHand basicLayoutTrackHand(const TimelineNoteMarker& marker)
{
    return basicLayoutHandForPoint(basicLayoutTrackStartPoint(marker));
}

HandOrderHand basicLayoutWifiSectorHand(const TimelineNoteMarker& marker, int sector)
{
    if (sector < 0 || sector > 1 || marker.type != QStringLiteral("wifi") || marker.wifiLanePoints.size() < 3
        || marker.wifiLanePoints.at(0).isEmpty() || marker.wifiLanePoints.at(2).isEmpty()) return HandOrderHand::Unspecified;
    const auto first = marker.wifiLanePoints.at(0).constLast();
    const auto second = marker.wifiLanePoints.at(2).constLast();
    if (!std::isfinite(first.x()) || !std::isfinite(first.y())
        || !std::isfinite(second.x()) || !std::isfinite(second.y())) return HandOrderHand::Unspecified;
    // A Wifi basis always uses two hands, including fans on one half-screen.
    // Native sector order is the deterministic tie-break for equal x.
    const int leftSector = first.x() <= second.x() ? 0 : 1;
    return sector == leftSector ? HandOrderHand::Left : HandOrderHand::Right;
}

BasicHandOrderLayoutPlan basicHandOrderLayoutInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata, double returnSpeed,
    int sweepThreshold, bool colorUnresolved, const QVector<hand_order::PatternSuggestion>* recognizedPatterns)
{
    BasicHandOrderLayoutPlan result;
    auto& plan = result.edit;
    const int first = qBound(0, qMin(begin, end), int(text.size()));
    const int last = qBound(first, qMax(begin, end), int(text.size()));
    begin = first == last ? 0 : first;
    end = first == last ? int(text.size()) : last;
    plan.replacement = text.mid(begin, end - begin);
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    if (!parsed.errors.isEmpty()) {
        plan.diagnostics = parsed.errors;
        return result;
    }
    QVector<const TimelineNoteMarker*> byOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byOrder.size()) byOrder[marker.parseOrder] = &marker;
    QVector<int> lineStarts{0};
    for (int index = 0; index < text.size(); ++index)
        if (text.at(index) == QLatin1Char('\n')) lineStarts.append(index + 1);
    QVector<LayoutHead> heads;
    QVector<LayoutSustain> sustains;
    QVector<LayoutWifiOccupancy> wifiTracks;
    QVector<bool> canRewrite(parsed.sourceTokens.size(), false), canRewriteStarHead(parsed.sourceTokens.size(), false);
    QVector<bool> selectedTokens(parsed.sourceTokens.size(), false);
    QVector<bool> rewriteTokens(parsed.sourceTokens.size(), false);
    QHash<int, int> headByOrder;
    QHash<int, int> sustainByOrder;
    for (int tokenIndex = 0; tokenIndex < parsed.sourceTokens.size(); ++tokenIndex) {
        const auto& source = parsed.sourceTokens.at(tokenIndex);
        if (!source.nativeValid || source.markerCount == 0 || source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const auto parts = hand_order::splitToken(text.mid(start, source.length));
        const bool fixed = !parts.suffix.isEmpty();
        const bool completeTarget = start >= begin && start + source.length <= end;
        selectedTokens[tokenIndex] = completeTarget;
        const auto* firstMarker = byOrder.value(source.firstParseOrder);
        bool annotationValid = true;
        for (const auto& issue : parsed.annotationErrors)
            if (issue.line == source.line && issue.col >= source.col && issue.col < source.col + source.length)
                annotationValid = false;
        canRewrite[tokenIndex] = firstMarker && annotationValid && (!fixed || completeTarget)
            && simpleLayoutSuffix(parts.suffix, *firstMarker, source.markerCount);
        canRewriteStarHead[tokenIndex] = firstMarker && annotationValid && completeTarget
            && chart::handOrderHasStarStrike(*firstMarker);
        int sharedSlideHead = -1;
        for (int index = 0; index < source.markerCount; ++index) {
            const auto* marker = byOrder.value(source.firstParseOrder + index);
            if (!marker || marker->isMine || marker->headMine || marker->trackMine) continue;
            const bool slide = marker->type == QStringLiteral("slide") || marker->type == QStringLiteral("wifi");
            int headIndex = -1;
            if (!slide || chart::handOrderHasStarStrike(*marker)) {
                if (!slide || sharedSlideHead < 0) {
                    headIndex = heads.size();
                    heads.append({marker, tokenIndex, headPoint(*marker),
                        fixed ? marker->handOrderHead : HandOrderHand::Unspecified, fixed});
                    if (slide) sharedSlideHead = headIndex;
                } else headIndex = sharedSlideHead;
                headByOrder.insert(marker->parseOrder, headIndex);
            }
            if (slide && marker->type == QStringLiteral("slide")) {
                sustainByOrder.insert(marker->parseOrder, sustains.size());
                sustains.append({marker, tokenIndex, headIndex, marker->endSecond,
                    fixed && !marker->handOrderTrackStops.isEmpty()
                        ? marker->handOrderTrackStops.constFirst().hand
                        : HandOrderHand::Unspecified, fixed});
            } else if (marker->type == QStringLiteral("hold") || marker->type == QStringLiteral("touch_hold")) {
                sustainByOrder.insert(marker->parseOrder, sustains.size());
                sustains.append({marker, tokenIndex, headIndex, marker->endSecond,
                    fixed ? marker->handOrderHead : HandOrderHand::Unspecified, fixed});
            } else if (marker->type == QStringLiteral("wifi")) {
                wifiTracks.append({marker, tokenIndex});
            }
        }
    }
    identifyLocalPatterns(&heads, &sustains, canRewrite, selectedTokens,
        sweepThreshold, &rewriteTokens);
    // Structural defaults are shared with the candidate pane. Keep their
    // gesture relationships in inference, rather than only colouring a preview.
    const auto detected = recognizedPatterns ? QVector<hand_order::PatternSuggestion>{}
        : hand_order::recognizeHandOrderPatterns(text, parsed, begin, end);
    const auto& patterns = recognizedPatterns ? *recognizedPatterns : detected;
    QSet<int> phasedHeads;
    for (const auto& pattern : patterns) {
        if (!pattern.definite || pattern.preferred < 0) continue;
        const bool interaction = pattern.interactionPhase != hand_order::InteractionPhaseMode::None;
        const bool specialized = interaction || pattern.registryId == "T17" || pattern.registryId == "T24"
            || pattern.registryId == "N01" || pattern.registryId == "U10"
            || pattern.registryId == "S07" || pattern.registryId == "S01" || recognizedPatterns;
        if (!specialized) continue;
        const auto& preferred = pattern.variants.at(pattern.preferred);
        auto markers = preferred.markers;
        std::stable_sort(markers.begin(),markers.end(),[](const auto& a,const auto& b) {
            return a.second != b.second ? a.second < b.second : a.parseOrder < b.parseOrder;
        });
        int previous = -1; HandOrderHand previousHand = HandOrderHand::Unspecified;
        if (!recognizedPatterns && interaction && pattern.registryId != "T06"
            && std::any_of(markers.cbegin(), markers.cend(), [&](const auto& marker) {
                const int index = headByOrder.value(marker.parseOrder, -1);
                if (index < 0) return false;
                const auto& head = heads.at(index);
                return head.windingEach || head.windingPrevious >= 0 || head.followSlideTail >= 0;
            })) continue;
        if (interaction && std::any_of(markers.cbegin(), markers.cend(), [&](const auto& marker) {
                const int index = headByOrder.value(marker.parseOrder, -1);
                if (index < 0) return true;
                const auto& head = heads.at(index);
                return (!canRewrite.at(head.token) && !canRewriteStarHead.at(head.token))
                    || !selectedTokens.at(head.token);
            })) continue;
        int interactionGroup = -1;
        int previousHead = -1;
        for (const auto& marker : markers) {
            const int index = headByOrder.value(marker.parseOrder,-1);
            if (index >= 0) {
                auto& head = heads[index];
                if (interaction && index == previousHead) continue;
                previousHead = index;
                const bool headOnly = interaction && canRewriteStarHead.at(head.token);
                if ((canRewrite.value(head.token) || headOnly) && selectedTokens.value(head.token)) {
                    rewriteTokens[head.token] = true; head.fixed = false; head.hand = HandOrderHand::Unspecified;
                    head.patternHand = handIndex(marker.handOrderHead); head.patternNeedsBothFree = false; head.gestureDefault = true;
                    head.sweepPrevious = head.windingPrevious = head.reboundPrevious = head.relaxedReboundPrevious = -1;
                    head.alternateSweepPrevious = head.interactionPrevious = -1; head.interactionGroup = -1;
                    if (interaction) {
                        if (interactionGroup < 0) interactionGroup = heads.size() + index;
                        head.structuralPhase = true;
                        head.preferStructuralStart = pattern.interactionPhase == hand_order::InteractionPhaseMode::Triangle;
                        head.interactionGroup = interactionGroup;
                        head.interactionStep = marker.handOrderHead != markers.first().handOrderHead ? 1 : previous < 0 ? 0 : 2;
                        phasedHeads.insert(index);
                    }
                    if (previous >= 0 && interaction) {
                        result.phaseLinks.append({heads.at(previous).marker->parseOrder, marker.parseOrder,
                            previousHand != marker.handOrderHead});
                    }
                    if (previous >= 0 && (!interaction && (pattern.registryId == "T17" || pattern.registryId == "T24"))) {
                        if (previousHand == marker.handOrderHead) head.reboundPrevious = previous;
                        else head.interactionPrevious = previous;
                    }
                    if (recognizedPatterns && previous >= 0 && !interaction
                        && (preferred.choiceKey == "same_hand" || preferred.choiceKey == "paginate")) {
                        const auto& prior = heads.at(previous);
                        if (pattern.registryId.startsWith('T')
                            && chart::handOrderIsButtonStrike(*prior.marker) && chart::handOrderIsButtonStrike(marker)
                            && previousHand == marker.handOrderHead && marker.second - prior.marker->second > 0.025
                            && prior.marker->lane != marker.lane)
                            head.sweepPrevious = previous;
                        else if (chart::handOrderSameKeyRebound(*prior.marker, marker))
                            head.relaxedReboundPrevious = previous;
                    }
                    if (!interaction) result.gestureHeads.insert(marker.parseOrder);
                }
                previous = index; previousHand = marker.handOrderHead;
            }
            const int sustain = sustainByOrder.value(marker.parseOrder,-1);
            if (!interaction && (!recognizedPatterns || pattern.prioritizesTrack)
                && sustain >= 0 && marker.type == "slide" && !marker.handOrderTrackStops.isEmpty()) {
                auto& track = sustains[sustain];
                if (canRewrite.value(track.token) && selectedTokens.value(track.token)) {
                    rewriteTokens[track.token] = true; track.fixed = false; track.hand = HandOrderHand::Unspecified;
                    track.patternHand = handIndex(marker.handOrderTrackStops.first().hand);
                    track.splitSharedStar = true; result.gestureTracks.insert(marker.parseOrder);
                }
            }
        }
    }
    QHash<int, int> previousInPhase;
    for (int index : orderedHeads(heads)) {
        const auto& head = heads.at(index);
        if (head.interactionGroup < 0 || phasedHeads.contains(index)) continue;
        const int previous = previousInPhase.value(head.interactionGroup, -1);
        if (previous >= 0) result.phaseLinks.append({heads.at(previous).marker->parseOrder, head.marker->parseOrder,
            (heads.at(previous).interactionStep % 2) != (head.interactionStep % 2)});
        previousInPhase.insert(head.interactionGroup, index);
    }
    for (const auto& head : heads) {
        if (head.configuredHead) result.configuredHeads.insert(head.marker->parseOrder);
        if (head.windingPrevious >= 0)
            result.windingLinks.append({heads.at(head.windingPrevious).marker->parseOrder, head.marker->parseOrder});
        if (head.sweepPrevious >= 0)
            result.sweepLinks.append({heads.at(head.sweepPrevious).marker->parseOrder, head.marker->parseOrder});
        if (head.interactionPrevious >= 0)
            result.interactionLinks.append({heads.at(head.interactionPrevious).marker->parseOrder, head.marker->parseOrder});
        if (head.reboundPrevious >= 0)
            result.reboundLinks.append({heads.at(head.reboundPrevious).marker->parseOrder,
                head.marker->parseOrder});
        if (head.relaxedReboundPrevious >= 0)
            result.relaxedReboundLinks.append({heads.at(head.relaxedReboundPrevious).marker->parseOrder,
                head.marker->parseOrder});
        if (head.alternateSweepPrevious >= 0)
            result.alternatingSweepLinks.append({
                heads.at(head.alternateSweepPrevious).marker->parseOrder,
                head.marker->parseOrder});
        if (head.windingEach) result.windingEachHeads.insert(head.marker->parseOrder);
    }
    for (const auto& sustain : sustains)
        if (sustain.oneStrokeNext >= 0)
            result.oneStrokeLinks.append({sustain.marker->parseOrder,
                sustains.at(sustain.oneStrokeNext).marker->parseOrder});
    for (const auto& sustain : sustains)
        if (sustain.tapLaneReserved)
            result.tapLaneReservedTracks.insert(sustain.marker->parseOrder);
    for (const auto& sustain : sustains)
        if (sustain.keySweepSplit)
            result.keySweepSplitTracks.insert(sustain.marker->parseOrder);
    for (const auto& sustain : sustains)
        if (sustain.keySweepKeep)
            result.keySweepKeepTracks.insert(sustain.marker->parseOrder);
    if (!std::isfinite(returnSpeed)) returnSpeed = chart::handOrderDefaultReturnSpeed;
    assignLocalHands(&heads, &sustains, wifiTracks, qBound(chart::handOrderMinReturnSpeed, returnSpeed,
        chart::handOrderMaxReturnSpeed));
    int delta = 0;
    for (int tokenIndex = 0; tokenIndex < parsed.sourceTokens.size(); ++tokenIndex) {
        const auto& source = parsed.sourceTokens.at(tokenIndex);
        if (source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        if (start >= end || finish <= begin || source.markerCount == 0) continue;
        if (start < begin || finish > end) { ++plan.partial; continue; }
        const QString original = text.mid(start, source.length);
        const auto parts = hand_order::splitToken(original);
        if (!parts.suffix.isEmpty() && !rewriteTokens.at(tokenIndex)) {
            ++result.preserved;
            for (const auto& issue : parsed.annotationErrors)
                if (issue.line == source.line && issue.col >= source.col && issue.col < source.col + source.length)
                    plan.diagnostics.append(issue);
            continue;
        }
        if (!source.nativeValid) { ++plan.unsupported; continue; }
        QString replacement;
        bool usedDefaultColor = false;
        const auto* firstMarker = byOrder.value(source.firstParseOrder);
        if (!firstMarker) { ++plan.unsupported; continue; }
        const bool slide = firstMarker->type == QStringLiteral("slide") || firstMarker->type == QStringLiteral("wifi");
        if (slide) {
            HandOrderTokenEdit token;
            token.baseToken = parts.baseToken;
            const int headIndex = headByOrder.value(firstMarker->parseOrder, -1);
            token.head = headIndex >= 0 ? heads.at(headIndex).hand : HandOrderHand::Unspecified;
            if (firstMarker->hasHeadStar && token.head == HandOrderHand::Unspecified) {
                if (!colorUnresolved) { ++plan.unsupported; continue; }
                token.head = basicLayoutHeadHand(*firstMarker);
                usedDefaultColor = true;
                if (token.head == HandOrderHand::Unspecified) { ++plan.unsupported; continue; }
            }
            bool supported = true;
            for (int index = 0; index < source.markerCount; ++index) {
                const auto* marker = byOrder.value(source.firstParseOrder + index);
                if (!marker || marker->slideBranchIndex != token.tracks.size()) { supported = false; break; }
                token.hasHead |= marker->hasHeadStar;
                const bool wifi = marker->type == QStringLiteral("wifi");
                if (!wifi && marker->type != QStringLiteral("slide")) { supported = false; break; }
                token.wifiBranches.append(wifi);
                token.wifiModes.append(wifi ? HandOrderWifiMode::Split : HandOrderWifiMode::Legacy);
                if (wifi && !marker->handOrderTrackStops.isEmpty()) {
                    token.wifiModes.last() = marker->handOrderWifiMode;
                    token.tracks.append(marker->handOrderTrackStops);
                    token.wifiSectors.append(marker->handOrderWifiSectorStops);
                } else if (wifi && !marker->handOrderWifiSectorStops.isEmpty()) {
                    token.wifiModes.last() = marker->handOrderWifiMode;
                    token.tracks.append(QVector<HandOrderTrackStop>{});
                    token.wifiSectors.append(marker->handOrderWifiSectorStops);
                } else if (wifi) {
                    token.tracks.append(QVector<HandOrderTrackStop>{});
                    token.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{
                        QVector<HandOrderTrackStop>{{0.0, basicLayoutWifiSectorHand(*marker, 0)}},
                        QVector<HandOrderTrackStop>{{0.0, basicLayoutWifiSectorHand(*marker, 1)}}});
                } else {
                    const int sustainIndex = sustainByOrder.value(marker->parseOrder, -1);
                    if (sustainIndex >= 0 && sustains.at(sustainIndex).fixed
                        && !marker->handOrderTrackStops.isEmpty()) {
                        token.tracks.append(marker->handOrderTrackStops);
                        token.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
                        continue;
                    }
                    auto assigned = sustainIndex >= 0 ? sustains.at(sustainIndex).hand : HandOrderHand::Unspecified;
                    if (assigned == HandOrderHand::Unspecified && colorUnresolved) {
                        assigned = basicLayoutTrackHand(*marker);
                        usedDefaultColor = true;
                    }
                    if (assigned == HandOrderHand::Unspecified) { supported = false; break; }
                    token.tracks.append(QVector<HandOrderTrackStop>{{0.0, assigned}});
                    token.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
                }
            }
            const QString suffix = supported ? handOrderSuffix(token) : QString();
            if (suffix.isEmpty()) { ++plan.unsupported; continue; }
            replacement = parts.baseToken + suffix;
        } else {
            bool cluster = source.markerCount > 1 && parts.baseToken.size() == source.markerCount;
            for (const auto ch : parts.baseToken) cluster &= ch >= QLatin1Char('1') && ch <= QLatin1Char('8');
            if (cluster) {
                QStringList notes;
                for (int index = 0; index < source.markerCount; ++index) {
                    const auto* marker = byOrder.value(source.firstParseOrder + index);
                    const int headIndex = marker ? headByOrder.value(marker->parseOrder, -1) : -1;
                    if (headIndex < 0) break;
                    auto hand = heads.at(headIndex).hand;
                    if (hand == HandOrderHand::Unspecified && colorUnresolved && marker) {
                        hand = basicLayoutHeadHand(*marker);
                        usedDefaultColor = true;
                    }
                    if (hand == HandOrderHand::Unspecified) break;
                    notes.append(QString(parts.baseToken.at(index)) + QLatin1Char('~') + glyph(hand));
                }
                if (notes.size() != source.markerCount) { ++plan.unsupported; continue; }
                replacement = notes.join(QLatin1Char('/'));
            } else {
                const int headIndex = headByOrder.value(firstMarker->parseOrder, -1);
                auto hand = headIndex >= 0 ? heads.at(headIndex).hand : HandOrderHand::Unspecified;
                if (hand == HandOrderHand::Unspecified && colorUnresolved) {
                    hand = basicLayoutHeadHand(*firstMarker);
                    usedDefaultColor = true;
                }
                if (source.markerCount != 1 || hand == HandOrderHand::Unspecified) { ++plan.unsupported; continue; }
                replacement = parts.baseToken + QLatin1Char('~') + glyph(hand);
            }
        }
        if (replacement != original) {
            plan.replacement.replace(start - begin + delta, source.length, replacement);
            plan.patches.append({start, finish, replacement});
            delta += replacement.size() - source.length;
            ++plan.changed;
            if (usedDefaultColor) {
                ++result.unresolvedColored;
            }
        } else if (!parts.suffix.isEmpty()) ++result.preserved;
    }
    const auto checked = SimaiParser::validateSyntax(text.left(begin) + plan.replacement + text.mid(end), metadata);
    if (!checked.errors.isEmpty() || checked.annotationErrors.size() > parsed.annotationErrors.size()) {
        plan.replacement = text.mid(begin, end - begin);
        plan.patches.clear();
        plan.changed = 0;
        result.oneStrokeLinks.clear(); result.gestureHeads.clear(); result.gestureTracks.clear();
        result.windingLinks.clear(); result.windingEachHeads.clear();
        result.sweepLinks.clear(); result.reboundLinks.clear(); result.interactionLinks.clear();
        result.relaxedReboundLinks.clear();
        result.keySweepSplitTracks.clear();
        result.keySweepKeepTracks.clear();
        result.alternatingSweepLinks.clear();
        result.tapLaneReservedTracks.clear();
        result.unresolvedColored = 0;
        plan.diagnostics += checked.errors;
        plan.diagnostics += checked.annotationErrors;
    }
    return result;
}
} // namespace miacode::chart_transform
