#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "common/TaskCancellation.h"
#include <QLineF>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
// A bounded path proposal tolerance, not an enlarged contact radius. Every
// resulting centre is replayed with the ordinary 15/240 finite contact circle.
constexpr double proposalReach = 0.20;
constexpr double buttonScreenReach = 0.32;
bool spend(qint64& work, qint64 count = 1)
{
    task::CancellationScope::checkpoint(); work -= count; return work >= 0;
}
bool enclosingCentre(const QVector<QPointF>& points, QPointF* centre, qint64& work)
{
    if (points.isEmpty()) return false;
    double best = std::numeric_limits<double>::infinity();
    const auto test = [&](QPointF candidate) {
        if (!spend(work, points.size())) return;
        double radius = 0;
        for (const auto& point : points) radius = qMax(radius, QLineF(candidate, point).length());
        if (radius < best) { best = radius; *centre = candidate; }
    };
    for (int i = 0; i < points.size(); ++i) {
        test(points.at(i));
        for (int j = i + 1; j < points.size(); ++j) {
            test((points.at(i) + points.at(j)) / 2);
            for (int k = j + 1; k < points.size(); ++k) {
                if (!spend(work)) return false;
                const auto a = points.at(i), b = points.at(j), c = points.at(k);
                const double d = 2 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
                if (std::abs(d) < epsilon) continue;
                const double aa = QPointF::dotProduct(a, a), bb = QPointF::dotProduct(b, b), cc = QPointF::dotProduct(c, c);
                test({(aa * (b.y() - c.y()) + bb * (c.y() - a.y()) + cc * (a.y() - b.y())) / d,
                    (aa * (c.x() - b.x()) + bb * (a.x() - c.x()) + cc * (b.x() - a.x())) / d});
            }
        }
    }
    return work >= 0 && best <= proposalReach + epsilon;
}
bool projectedForearmsCross(const QPointF& left, const QPointF& right)
{
    const QLineF leftArm(QPointF(-0.55, 1.5), left);
    const QLineF rightArm(QPointF(0.55, 1.5), right);
    QPointF crossing;
    return QLineF(left, right).length() > 0.35
        && leftArm.intersects(rightArm, &crossing) == QLineF::BoundedIntersection
        && QLineF(crossing, left).length() > 0.08
        && QLineF(crossing, right).length() > 0.08;
}
}

void compactContactAction(ContactAction& action)
{
    if (action.points.size() != action.times.size() || action.points.size() < 3) return;
    QVector<QPointF> points; QVector<double> times;
    points.reserve(action.points.size()); times.reserve(action.times.size());
    for (int i = 0; i < action.points.size(); ++i) {
        task::CancellationScope::checkpoint();
        while (points.size() >= 2) {
            const int before = points.size() - 2;
            const double duration = action.times.at(i) - times.at(before);
            if (duration <= epsilon) break;
            const double progress = (times.last() - times.at(before)) / duration;
            const auto expected = points.at(before) + (action.points.at(i) - points.at(before)) * progress;
            if (QLineF(points.last(), expected).length() > 1e-12) break;
            points.removeLast(); times.removeLast();
        }
        points.append(action.points.at(i)); times.append(action.times.at(i));
    }
    action.points = std::move(points); action.times = std::move(times);
}

bool resolveHandOrderActions(const QVector<ScheduledAction>& logical,
    QVector<ScheduledAction>* physical, qint64& work)
{
    *physical = logical;
    for (int hand = 0; hand < 2; ++hand) {
        QVector<int> ids; QVector<double> times;
        for (int i = 0; i < logical.size(); ++i) if (logical.at(i).action.hand == hand) {
            ids.append(i); times.append(logical.at(i).action.start); times.append(logical.at(i).action.end);
            times += logical.at(i).action.times;
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < epsilon; }), times.end());
        QVector<QPointF> centres; centres.reserve(times.size());
        for (double second : times) {
            QVector<QPointF> screens, buttons;
            int buttonLane = 0;
            bool pinned = false; QPointF pinnedCentre;
            for (int id : ids) {
                if (!spend(work)) return false;
                const auto& scheduled = logical.at(id); const auto& action = scheduled.action;
                if (second < action.start - epsilon || second > action.end + epsilon) continue;
                if (scheduled.button) {
                    if (buttonLane && buttonLane != scheduled.lane) return false;
                    buttonLane = scheduled.lane; buttons.append(contactPosition(action, second));
                } else {
                    const auto point = contactPosition(action, second);
                    screens.append(point);
                }
                if (scheduled.committed) {
                    const auto point = contactPosition(action, second);
                    if (pinned && QLineF(pinnedCentre, point).length() > 1e-6) return false;
                    pinned = true; pinnedCentre = point;
                }
            }
            QPointF centre;
            if (!screens.isEmpty()) {
                if (pinned) {
                    centre = pinnedCentre;
                    for (const auto& point : screens)
                        if (QLineF(point, centre).length() > proposalReach + epsilon) return false;
                } else if (!enclosingCentre(screens, &centre, work)) return false;
                for (const auto& point : buttons)
                    if (QLineF(centre, point).length() > buttonScreenReach + epsilon) return false;
            } else if (!buttons.isEmpty()) centre = pinned ? pinnedCentre : buttons.first();
            centres.append(centre);
        }
        for (int id : ids) {
            auto& action = (*physical)[id].action;
            action.points.clear(); action.times.clear();
            for (int i = 0; i < times.size(); ++i) {
                if (!spend(work)) return false;
                if (times.at(i) < action.start - epsilon || times.at(i) > action.end + epsilon) continue;
                action.times.append(times.at(i)); action.points.append(centres.at(i));
            }
            compactContactAction(action);
        }
    }
    return work >= 0;
}

PhysicalCrossingEffort physicalHandCrossingEffort(const QVector<ScheduledAction>& physical,
    const QSet<int>& holdSources, const QSet<int>& sweepSources, qint64& work)
{
    PhysicalCrossingEffort effort;
    if (!spend(work, physical.size() + 1)) return {0, 0, false};
    if (std::none_of(physical.cbegin(), physical.cend(), [&](const auto& action) {
        return action.motion || holdSources.contains(action.action.tokenStart);
    })) return effort;
    QVector<double> first(physical.size(), std::numeric_limits<double>::infinity());
    QVector<double> last(physical.size(), -std::numeric_limits<double>::infinity());
    // Only overlapping opposite-hand contacts need geometric sampling. A
    // lone long trace must not consume this budget for all of its knots.
    for (int i = 0; i < physical.size(); ++i) for (int j = i + 1; j < physical.size(); ++j) {
        const auto& a = physical.at(i); const auto& b = physical.at(j);
        if (a.action.hand == b.action.hand || (a.committed && b.committed)
            || (!a.motion && !b.motion && !holdSources.contains(a.action.tokenStart)
                && !holdSources.contains(b.action.tokenStart))) continue;
        const double begin = qMax(a.action.start, b.action.start), end = qMin(a.action.end, b.action.end);
        if (end <= begin + epsilon) continue;
        if (!spend(work)) return {0, 0, false};
        first[i] = qMin(first.at(i), begin); last[i] = qMax(last.at(i), end);
        first[j] = qMin(first.at(j), begin); last[j] = qMax(last.at(j), end);
    }
    QVector<double> times;
    QVector<const ScheduledAction*> ordered;
    for (int index = 0; index < physical.size(); ++index) {
        if (last.at(index) <= first.at(index)) continue;
        const auto& scheduled = physical.at(index);
        if (!spend(work, scheduled.action.times.size() + 2)) return {0, 0, false};
        times.append(first.at(index)); times.append(last.at(index));
        ordered.append(&scheduled);
        for (double time : scheduled.action.times)
            if (time > first.at(index) && time < last.at(index)) times.append(time);
        if (holdSources.contains(scheduled.action.tokenStart)
            && sweepSources.contains(scheduled.action.tokenStart)
            && scheduled.action.start + 0.05 > first.at(index)
            && scheduled.action.start + 0.05 < last.at(index))
            times.append(scheduled.action.start + 0.05);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) {
        return std::abs(a - b) < epsilon;
    }), times.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->action.start < b->action.start;
    });
    QVector<const ScheduledAction*> active;
    int next = 0;
    bool previousFlowingConflict = false;
    for (int i = 1; i < times.size(); ++i) {
        const double first = times.at(i - 1);
        while (next < ordered.size() && ordered.at(next)->action.start <= first + epsilon)
            active.append(ordered.at(next++));
        active.erase(std::remove_if(active.begin(), active.end(), [&](const auto* scheduled) {
            return scheduled->action.end <= first + epsilon;
        }), active.end());
        if (!spend(work, active.size() + 1)) return {0, 0, false};
        const double middle = (times.at(i - 1) + times.at(i)) / 2;
        const ScheduledAction* hands[2] = {};
        bool motion[2] = {}, sweep[2] = {}, proposed = false, sustained = false;
        for (const auto* selected : active) {
            const auto& scheduled = *selected;
            const auto& action = scheduled.action;
            if (middle < action.start || middle >= action.end) continue;
            if (action.hand < 0 || action.hand > 1) return {0, 0, false};
            if (!hands[action.hand]) hands[action.hand] = &scheduled;
            proposed |= !scheduled.committed;
            sustained |= scheduled.motion || holdSources.contains(action.tokenStart);
            motion[action.hand] |= scheduled.motion;
            sweep[action.hand] |= sweepSources.contains(action.tokenStart)
                && (!holdSources.contains(action.tokenStart) || middle <= action.start + 0.05);
        }
        bool conflict = false;
        if (hands[0] && hands[1] && proposed && sustained
            && projectedForearmsCross(contactPosition(hands[0]->action, middle),
                contactPosition(hands[1]->action, middle))) {
            // An isolated tap or stationary anchor can pass over/under an
            // arm. Only two flowing gestures have a topological conflict.
            bool flowing[2] = {};
            for (int hand = 0; hand < 2; ++hand)
                flowing[hand] = sweep[hand] || (motion[hand]
                    && QLineF(contactPosition(hands[hand]->action, times.at(i - 1)),
                        contactPosition(hands[hand]->action, times.at(i))).length() > 1e-6);
            conflict = flowing[0] && flowing[1];
            if (!conflict) effort.stationarySeconds += times.at(i) - times.at(i - 1);
        }
        if (conflict && !previousFlowingConflict) ++effort.flowingConflicts;
        previousFlowingConflict = conflict;
    }
    return effort;
}

double physicalHandTransferCharge(const QVector<PhysicalHandTransfer>& transfers)
{
    QMap<QString, double> charges;
    for (const auto& transfer : transfers) {
        task::CancellationScope::checkpoint();
        const QString key = QStringLiteral("%1/%2/%3/%4/%5")
            .arg(qRound64(transfer.second * 1e7)).arg(qRound64(transfer.position.x() * 1e7))
            .arg(qRound64(transfer.position.y() * 1e7)).arg(transfer.beforeHand).arg(transfer.afterHand);
        const int k = transfer.ordinal;
        const double marginal = 0.6 + 1.8 * (qMax(0, k - 1) * qMax(0, k - 1) - qMax(0, k - 2) * qMax(0, k - 2));
        charges[key] = qMax(charges.value(key), marginal);
    }
    double charge = 0; for (double value : charges) charge += value;
    return charge;
}
} // namespace miacode::hand_order
