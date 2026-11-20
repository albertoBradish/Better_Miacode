#include "core/chart/transform/HandOrderInteraction.h"

#include "common/TaskCancellation.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderRhythm.h"

#include <QLineF>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace miacode::hand_order {
namespace {
using Hand = HandOrderHand;
constexpr double epsilon = 1e-7;
constexpr double contextSeconds = 0.5;
constexpr double minimumStep = 0.025;
constexpr double sameKeyContinuationSeconds = 0.1;
constexpr double repetitionTolerance = 0.015;
constexpr double nearbyRepetitionSeconds = 4.0;

int ringDistance(int a, int b)
{
    const int distance = std::abs(a - b);
    return qMin(distance, 8 - distance);
}

int sweepDirection(int a, int b)
{
    const int step = (b - a + 8) % 8;
    if (step == 1 || step == 2) return 1;
    if (step == 6 || step == 7) return -1;
    return 0;
}

bool buttonHead(const TimelineNoteMarker* note)
{
    return note && !note->isMine && note->lane >= 1 && note->lane <= 8
        && (chart::handOrderIsButtonStrike(*note) || note->type == "hold");
}

bool shortHead(const TimelineNoteMarker* note)
{
    return buttonHead(note) && (chart::handOrderIsButtonStrike(*note)
        || note->endSecond - note->second <= qMin(0.4, 2 * chart::handOrderGridSeconds(*note)) + epsilon);
}

bool atStep(double gap, double step)
{
    return std::abs(gap - step) <= 0.1 * step + epsilon;
}

bool coherentRailContinuation(const QVector<const TimelineNoteMarker*>& stream, int boundary, double gap)
{
    const auto closeRail = [&](int i) {
        const auto* first = stream.value(i - 2);
        const auto* middle = stream.value(i - 1);
        const auto* last = stream.value(i);
        return shortHead(first) && shortHead(middle) && shortHead(last)
            && ringDistance(first->lane, last->lane) <= 1
            && atStep(middle->second - first->second, gap) && atStep(last->second - middle->second, gap);
    };
    return closeRail(boundary) && closeRail(boundary + 1);
}

QVector<int> interactionShape(const InteractionSteps& steps, bool mirror)
{
    QVector<int> shape;
    for (const auto& step : steps) shape.append((mirror ? 9 - step.note->lane : step.note->lane) * 2 + int(step.opposite));
    return shape;
}

double tripleStep(const QVector<const TimelineNoteMarker*>& stream, int first)
{
    if (first + 2 >= stream.size()) return 0;
    const auto* a = stream.at(first);
    const auto* b = stream.at(first + 1);
    const auto* c = stream.at(first + 2);
    if (!shortHead(a) || !shortHead(b) || !buttonHead(c)
        || a->lane == b->lane || a->lane == c->lane || b->lane == c->lane) return 0;
    const double step = b->second - a->second;
    if (step < minimumStep - epsilon || step > chart::handOrderMaximumInteractionGapSeconds + epsilon
        || !atStep(c->second - b->second, step)) return 0;
    return step;
}

bool relatedCells(const QVector<const TimelineNoteMarker*>& stream, int first, int next, double step)
{
    if (!atStep(tripleStep(stream, next), step) || !shortHead(stream.at(first + 2))) return false;
    const double gap = stream.at(next)->second - stream.at(first + 2)->second;
    if (gap < 0.9 * step || gap > qMax(3.3 * step, contextSeconds)) return false;
    const int a = stream.at(first)->lane, b = stream.at(first + 1)->lane, c = stream.at(first + 2)->lane;
    const int direction = sweepDirection(a, b);
    if (direction && direction == sweepDirection(b, c)) return false;
    bool same = true, reflected = true;
    for (int hit = 0; hit < 3; ++hit) {
        const int lane = stream.at(first + hit)->lane, other = stream.at(next + hit)->lane;
        same &= lane == other;
        reflected &= lane + other == 9;
    }
    return same || reflected;
}

void appendAlternating(InteractionSteps& steps, const TimelineNoteMarker* note)
{
    steps.append({note, steps.size() % 2 != 0});
}

int handIndex(Hand hand) { return hand == Hand::Left ? 0 : hand == Hand::Right ? 1 : -1; }
Hand indexedHand(int hand) { return hand == 0 ? Hand::Left : Hand::Right; }

QPointF lanePoint(int lane)
{
    const double angle = (lane - 0.5) * std::numbers::pi / 4.0;
    return {std::sin(angle), -std::cos(angle)};
}

QPointF headPoint(const TimelineNoteMarker& note)
{
    return note.type.startsWith("touch") ? (note.touchPoint - QPointF(270, 270)) / 240.0 : lanePoint(note.lane);
}

struct Contact {
    double second = 0;
    QPointF point;
};

struct Phase {
    double cost = std::numeric_limits<double>::infinity();
    std::array<QVector<Contact>, 2> contacts;
};

bool trackUsesHand(const TimelineNoteMarker& track, double from, double through, Hand hand)
{
    if (track.trackMine || through < track.slideTraceSecond - epsilon || from > track.endSecond + epsilon) return false;
    const double duration = track.endSecond - track.slideTraceSecond;
    const auto occupied = [&](const QVector<HandOrderTrackStop>& stops) {
        for (int i = 0; i < stops.size(); ++i) {
            if (stops.at(i).hand != hand) continue;
            const double start = track.slideTraceSecond + stops.at(i).progress * duration;
            const double end = i + 1 < stops.size()
                ? track.slideTraceSecond + stops.at(i + 1).progress * duration : track.endSecond;
            if (start <= through + epsilon && end > from + epsilon) return true;
        }
        return false;
    };
    if (track.handOrderWifiMode == HandOrderWifiMode::Split)
        return std::any_of(track.handOrderWifiSectorStops.cbegin(), track.handOrderWifiSectorStops.cend(), occupied);
    return occupied(track.handOrderTrackStops);
}

double transition(const Contact& from, const Contact& to)
{
    const double gap = to.second - from.second;
    if (gap < -epsilon || gap >= contextSeconds) return 0;
    const double urgency = 1 - qMax(0.0, gap) / contextSeconds;
    return 1.3 * urgency * QLineF(from.point, to.point).length();
}

bool sameRhythm(const InteractionSteps& a, const InteractionSteps& b)
{
    for (int i = 0; i < a.size(); ++i) {
        const auto& x = *a.at(i).note;
        const auto& y = *b.at(i).note;
        if ((isLockingHold(x) != isLockingHold(y))
            || std::abs((x.second - a.first().note->second) - (y.second - b.first().note->second)) > repetitionTolerance)
            return false;
        if (isLockingHold(x) && std::abs((x.endSecond - x.second) - (y.endSecond - y.second)) > repetitionTolerance)
            return false;
    }
    return true;
}

} // namespace

QVector<InteractionSteps> findTriangleInteractions(const QVector<const TimelineNoteMarker*>& stream)
{
    QVector<InteractionSteps> result;
    for (int first = 0; first + 2 < stream.size();) {
        task::CancellationScope::checkpoint();
        const double step = tripleStep(stream, first);
        if (step <= 0) { ++first; continue; }
        if (relatedCells(stream, first, first + 3, step)) {
            InteractionSteps steps;
            int cell = first;
            for (;;) {
                for (int i = 0; i < 3; ++i) appendAlternating(steps, stream.at(cell + i));
                const int next = cell + 3;
                if (!relatedCells(stream, cell, next, step)) break;
                task::CancellationScope::checkpoint();
                if (!atStep(stream.at(next)->second - stream.at(next - 1)->second, step)) {
                    result.append(std::move(steps));
                    steps.clear();
                }
                cell = next;
            }
            first = cell + 3;
            // A final incomplete repetition still belongs to the current phase.
            if (shortHead(stream.at(first - 1))) for (int i = 0; i < 2 && first < stream.size(); ++i) {
                const auto* note = stream.at(first);
                if (!shortHead(note) || note->lane != stream.at(cell + i)->lane
                    || !atStep(note->second - stream.at(first - 1)->second, step)) break;
                appendAlternating(steps, note);
                ++first;
            }
            result.append(std::move(steps));
            continue;
        }

        const auto& a = *stream.at(first);
        const auto& b = *stream.at(first + 1);
        const auto& c = *stream.at(first + 2);
        const auto* previous = first > 0 ? stream.at(first - 1) : nullptr;
        const auto* next = stream.value(first + 3);
        const bool startsHere = !shortHead(previous) || previous->lane == a.lane
            || !atStep(a.second - previous->second, step);
        const bool endsHere = !shortHead(&c) || !shortHead(next) || next->lane == c.lane
            || !atStep(next->second - c.second, step);
        const bool continuesOnHold = next && isLockingHold(*next) && next->lane == c.lane
            && atStep(next->second - c.second, step);
        const bool fold = (startsHere || continuesOnHold) && endsHere
            && chart::handOrderIsButtonStrike(a) && chart::handOrderIsButtonStrike(b)
            && a.tempoBpm > 0 && step <= 60.0 / a.tempoBpm / 4 * 1.1 + epsilon
            && ringDistance(a.lane, b.lane) == 1 && ringDistance(a.lane, c.lane) == 1;
        if (!fold) { ++first; continue; }
        InteractionSteps steps{{&a, false}, {&b, true}, {&c, false}};
        first += 3;
        // The first strike of a following same-key Hold or relaxed jack can
        // keep the hand that reached the key at the end of the fold.
        if (chart::handOrderIsButtonStrike(c) && buttonHead(next) && next->lane == c.lane
            && atStep(next->second - c.second, step)
            && (next->type == "hold" || step >= sameKeyContinuationSeconds - epsilon)) {
            steps.append({next, false});
            ++first;
        }
        result.append(std::move(steps));
    }
    return result;
}

QVector<InteractionSteps> findTrillInteractions(const QVector<const TimelineNoteMarker*>& stream)
{
    QVector<InteractionSteps> result;
    for (int first = 0; first + 2 < stream.size();) {
        task::CancellationScope::checkpoint();
        const auto* a = stream.at(first);
        const auto* b = stream.at(first + 1);
        if (!shortHead(a) || !shortHead(b) || a->lane == b->lane) { ++first; continue; }
        const double gap = b->second - a->second;
        if (gap < minimumStep || gap > chart::handOrderMaximumInteractionGapSeconds + epsilon
            || !chart::handOrderRegularInputStep(*a, *b, true)) { ++first; continue; }
        int last = first + 2;
        while (last < stream.size() && shortHead(stream.at(last))
            && stream.at(last)->lane == stream.at(last - 2)->lane
            && atStep(stream.at(last)->second - stream.at(last - 1)->second, gap)) ++last;
        const auto* next = stream.value(last);
        const auto* tail = stream.value(last + 1);
        const auto extendsAxis = [&](const TimelineNoteMarker* outer, const TimelineNoteMarker* centre) {
            return shortHead(outer) && shortHead(centre) && centre->lane == a->lane && outer->lane != b->lane
                && ringDistance(a->lane, b->lane) == 1 && ringDistance(a->lane, outer->lane) == 1
                && atStep(std::abs(centre->second - outer->second), gap);
        };
        if (last - first < (ringDistance(a->lane, b->lane) == 1 ? 3 : 4)
            || coherentRailContinuation(stream, first, gap) || coherentRailContinuation(stream, last, gap)
            || (last - first == 3 && ((extendsAxis(next, tail) && atStep(next->second - stream.at(last - 1)->second, gap))
                || (extendsAxis(stream.value(first - 1), stream.value(first - 2))
                    && atStep(a->second - stream.at(first - 1)->second, gap))))) {
            ++first;
            continue;
        }
        InteractionSteps group;
        for (int i = first; i < last; ++i) appendAlternating(group, stream.at(i));
        result.append(std::move(group));
        first = last;
    }
    return result;
}

HandOrderHand interactionHand(HandOrderHand first, bool opposite)
{
    if (!opposite || !concreteHoldHand(first)) return first;
    return first == Hand::Left ? Hand::Right : Hand::Left;
}

QVector<const TimelineNoteMarker*> sustainedInteractionContext(const QVector<TimelineNoteMarker>& chart)
{
    QVector<const TimelineNoteMarker*> sustained;
    for (const auto& note : chart)
        if ((isLockingHold(note) && !note.isMine)
            || ((note.type == "slide" || note.type == "wifi") && !note.trackMine)) sustained.append(&note);
    return sustained;
}

bool interactionPhaseFits(const InteractionSteps& steps, Hand first,
    const QVector<const TimelineNoteMarker*>& sustainedContext)
{
    QHash<int, Hand> proposed;
    QVector<const TimelineNoteMarker*> internalHolds;
    for (const auto& step : steps) {
        proposed.insert(step.note->parseOrder, interactionHand(first, step.opposite));
        if (isLockingHold(*step.note)) internalHolds.append(step.note);
    }
    for (const auto* original : internalHolds) {
        auto hold = *original;
        hold.handOrderHead = proposed.value(hold.parseOrder);
        for (const auto& step : steps)
            if (holdLocksHead(hold, *step.note, proposed.value(step.note->parseOrder))) return false;
        for (const auto* fixed : sustainedContext)
            if (!proposed.contains(fixed->parseOrder) && isLockingHold(*fixed)
                && holdLocksHead(hold, *fixed, fixed->handOrderHead)) return false;
    }
    for (const auto& step : steps) {
        const auto& note = *step.note;
        const Hand hand = proposed.value(note.parseOrder);
        for (const auto* fixed : sustainedContext) {
            if (fixed->parseOrder == note.parseOrder || fixed->isMine) continue;
            if (!proposed.contains(fixed->parseOrder) && holdLocksHead(*fixed, note, hand)) return false;
            if (fixed->type == "slide" || fixed->type == "wifi") {
                const bool sameKeyLaunch = !isLockingHold(note) && note.lane == fixed->lane
                    && std::abs(note.second - fixed->slideTraceSecond) <= epsilon;
                if (!sameKeyLaunch && trackUsesHand(*fixed, note.second,
                    isLockingHold(note) ? note.endSecond : note.second, hand)) return false;
            }
        }
    }
    return true;
}

bool interactionPhaseFitsCombined(const InteractionSteps& steps, Hand first,
    const QVector<TimelineNoteMarker>& chart, const QVector<const TimelineNoteMarker*>& sustainedContext)
{
    if (!interactionPhaseFits(steps, first, sustainedContext)) return false;
    for (const auto& step : steps) {
        if (!isLockingHold(*step.note)) continue;
        auto hold = *step.note;
        hold.handOrderHead = interactionHand(first, step.opposite);
        for (const auto& note : chart)
            if (!note.isMine && !note.headMine && holdLocksHead(hold, note, note.handOrderHead)) return false;
    }
    return true;
}

QVector<HandOrderHand> reviewInteractionPhases(const QVector<InteractionPhases>& groups,
    const QVector<TimelineNoteMarker>& context, double returnSpeed)
{
    if (groups.isEmpty()) return {};
    QSet<int> groupOrders;
    for (const auto& group : groups) for (const auto& step : group.steps) groupOrders.insert(step.note->parseOrder);
    if (!std::isfinite(returnSpeed)) returnSpeed = chart::handOrderDefaultReturnSpeed;
    returnSpeed = qBound(chart::handOrderMinReturnSpeed, returnSpeed, chart::handOrderMaxReturnSpeed);

    std::array<QVector<Contact>, 2> fixed;
    for (const auto& note : context) {
        if (groupOrders.contains(note.parseOrder) || note.isMine || note.headMine) continue;
        const bool slide = note.type == "slide" || note.type == "wifi";
        const int hand = handIndex(note.handOrderHead);
        if (hand >= 0 && (!slide || (note.hasHeadStar && note.slideBranchIndex == 0))) {
            const QPointF point = headPoint(note);
            fixed[hand].append({note.second, point});
            if (isLockingHold(note)) fixed[hand].append({note.endSecond, point});
        }
        if (slide && !note.trackMine && !note.handOrderTrackStops.isEmpty()
            && note.handOrderWifiMode != HandOrderWifiMode::Split) {
            const int tailHand = handIndex(note.handOrderTrackStops.last().hand);
            if (tailHand >= 0) fixed[tailHand].append({note.endSecond, lanePoint(note.endLane)});
        }
    }
    for (auto& contacts : fixed) std::stable_sort(contacts.begin(), contacts.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });

    QVector<std::array<Phase, 2>> phases;
    for (const auto& group : groups) {
        task::CancellationScope::checkpoint();
        std::array<Phase, 2> options;
        for (int first = 0; first < 2; ++first) {
            if (first == 0 ? !group.allowLeft : !group.allowRight) continue;
            auto& option = options[first];
            double posture = 0;
            for (const auto& step : group.steps) {
                const int hand = first ^ int(step.opposite);
                const auto& note = *step.note;
                const QPointF point = headPoint(note);
                posture += qMax(0.0, hand == 0 ? point.x() : -point.x());
                option.contacts[hand].append({note.second, point});
                if (isLockingHold(note)) option.contacts[hand].append({note.endSecond, point});
            }
            option.cost = 1.5 * posture / std::sqrt(double(group.steps.size()));
            if (concreteHoldHand(group.preferred) && indexedHand(first) != group.preferred) option.cost += 0.12;
            for (int hand = 0; hand < 2; ++hand) {
                auto& hits = option.contacts[hand];
                if (hits.isEmpty()) continue;
                std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
                const auto& known = fixed[hand];
                const auto before = std::lower_bound(known.cbegin(), known.cend(), hits.first().second - epsilon,
                    [](const auto& hit, double second) { return hit.second < second; });
                const bool recent = before != known.cbegin()
                    && hits.first().second - (before - 1)->second < contextSeconds;
                if (recent) {
                    auto origin = *(before - 1);
                    origin.point = chart::handOrderRestedPoint(origin.point, origin.second,
                        hits.first().second, hand, returnSpeed);
                    option.cost += transition(origin, hits.first());
                } else {
                    option.cost += 0.18 * QLineF(chart::handOrderNaturalRestPoint(hand), hits.first().point).length();
                }
                const auto after = std::lower_bound(before, known.cend(), hits.last().second - epsilon,
                    [](const auto& hit, double second) { return hit.second < second; });
                if (after != known.cend()) option.cost += transition(hits.last(), *after);
            }
        }
        phases.append(std::move(options));
    }
    // Exact nearby repeats and reflections still provide evidence when an
    // unrelated configuration occurs between them.
    QVector<int> relationships(groups.size(), 0);
    QHash<QVector<int>, int> latestShape;
    for (int i = 0; i < groups.size(); ++i) {
        const auto& steps = groups.at(i).steps;
        const auto shape = interactionShape(steps, false);
        const int same = latestShape.value(shape, -1);
        const int reflected = latestShape.value(interactionShape(steps, true), -1);
        const int previous = qMax(same, reflected);
        if (previous >= 0 && std::isfinite(qMin(phases.at(previous)[0].cost, phases.at(previous)[1].cost))) {
            const auto& previousSteps = groups.at(previous).steps;
            if (steps.first().note->second - previousSteps.last().note->second <= nearbyRepetitionSeconds
                && sameRhythm(previousSteps, steps)) {
                const int relationship = previous == reflected ? -1 : 1;
                if (previous == i - 1) relationships[i] = relationship;
                else {
                    const auto& options = phases.at(previous);
                    const int first = options[0].cost < options[1].cost ? 0 : 1;
                    const int expected = relationship < 0 ? 1 - first : first;
                    if (options[1 - first].cost - options[first].cost > 0.35)
                        phases[i][1 - expected].cost += 0.8;
                }
            }
        }
        latestShape.insert(shape, i);
    }
    QVector<Hand> selected(groups.size(), Hand::Unspecified);
    QVector<std::array<int, 2>> predecessors(groups.size(), {-1, -1});
    QVector<bool> blocked(groups.size(), false);
    for (int i = 1; i < groups.size(); ++i) {
        const auto& previous = phases.at(i - 1);
        if (!std::isfinite(previous[0].cost) && !std::isfinite(previous[1].cost)) continue;
        const auto& left = groups.at(i - 1).steps;
        const auto& right = groups.at(i).steps;
        const double nextSecond = right.first().note->second;
        // A continued Hold can outlast this group. A neighbouring choice must
        // still be compatible with the hand that was actually chosen for it.
        std::array<QVector<TimelineNoteMarker>, 2> crossingHolds;
        for (const auto& step : left) {
            if (!isLockingHold(*step.note) || step.note->endSecond < nextSecond - epsilon) continue;
            for (int hand = 0; hand < 2; ++hand) {
                auto hold = *step.note;
                hold.handOrderHead = indexedHand(hand ^ int(step.opposite));
                crossingHolds[hand].append(std::move(hold));
            }
        }
        const int relationship = relationships.at(i);
        bool linked = false, hasCurrent = false;
        for (int hand = 0; hand < 2; ++hand) {
            auto& current = phases[i][hand];
            if (!std::isfinite(current.cost)) continue;
            hasCurrent = true;
            double best = std::numeric_limits<double>::infinity();
            for (int prior = 0; prior < 2; ++prior) {
                if (!std::isfinite(previous[prior].cost)) continue;
                bool conflict = false;
                for (const auto& hold : crossingHolds[prior]) for (const auto& next : right)
                    conflict |= holdLocksHead(hold, *next.note, indexedHand(hand ^ int(next.opposite)));
                if (conflict) continue;
                double cost = previous[prior].cost;
                if (relationship && (prior == hand) != (relationship > 0)) cost += 1.35;
                for (int h = 0; h < 2; ++h) {
                    const auto& from = previous[prior].contacts[h];
                    const auto& to = current.contacts[h];
                    if (from.isEmpty() || to.isEmpty()) continue;
                    const auto& known = fixed[h];
                    const auto between = std::lower_bound(known.cbegin(), known.cend(), from.last().second,
                        [](const auto& hit, double second) { return hit.second < second; });
                    if (between == known.cend() || between->second >= to.first().second - epsilon)
                        cost += transition(from.last(), to.first());
                }
                if (cost < best - epsilon) { best = cost; predecessors[i][hand] = prior; }
            }
            linked |= std::isfinite(best);
            current.cost += best;
        }
        if (hasCurrent && !linked) blocked[i - 1] = blocked[i] = true;
    }
    int hand = -1;
    for (int i = groups.size() - 1; i >= 0; --i) {
        if (hand < 0) {
            const auto& options = phases.at(i);
            hand = options[0].cost < options[1].cost - epsilon ? 0 : 1;
            if (!std::isfinite(options[hand].cost)) { hand = -1; continue; }
        }
        selected[i] = indexedHand(hand);
        hand = predecessors.at(i)[hand];
    }
    for (int i = 0; i < blocked.size(); ++i)
        if (blocked.at(i)) selected[i] = Hand::Unspecified;
    return selected;
}

} // namespace miacode::hand_order
