#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/MuriConfig.h"
#include "core/analysis/MuriAnalyzerInternal.h"
#include "common/TaskCancellation.h"
#include <QLineF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
qint64 tickAt(double second) { return qint64(std::ceil(second * TouchJudgeProfile::ticksPerSecond - epsilon)); }
double secondAt(qint64 tick) { return tick / TouchJudgeProfile::ticksPerSecond; }
}
QPointF touchPadPoint(const QString& pad)
{
    return (muri::padCenter(pad) - QPointF(muri::kLogicalCanvasCenter, muri::kLogicalCanvasCenter)) / 240.0;
}
QPointF contactPosition(const ContactAction& action, double second)
{
    if (action.points.isEmpty()) return {};
    if (action.times.size() != action.points.size() || action.times.size() == 1) return action.points.first();
    const auto at = std::lower_bound(action.times.begin(), action.times.end(), second);
    if (at == action.times.begin()) return action.points.first();
    if (at == action.times.end()) return action.points.last();
    const int i = int(at - action.times.begin());
    const double dt = action.times.at(i) - action.times.at(i - 1);
    const double progress = dt > 0 ? qBound(0.0, (second - action.times.at(i - 1)) / dt, 1.0) : 1.0;
    return action.points.at(i - 1) * (1 - progress) + action.points.at(i) * progress;
}
bool touchPadsAdjacent(const QString& a, const QString& b)
{
    return muri::detail::touchPadsAreAdjacent(a.toUpper(), b.toUpper());
}
QVector<QVector<int>> buildHandOrderTouchGroups(const QVector<TouchTarget>& targets)
{
    QVector<QVector<int>> groups;
    QVector<bool> visited(targets.size(), false);
    for (int i = 0; i < targets.size(); ++i) {
        task::CancellationScope::checkpoint();
        if (visited.at(i) || targets.at(i).hold) continue;
        QVector<int> component{i}; visited[i] = true;
        for (int cursor = 0; cursor < component.size(); ++cursor) {
            task::CancellationScope::checkpoint();
            const auto& a = targets.at(component.at(cursor));
            for (int j = 0; j < targets.size(); ++j) {
                task::CancellationScope::checkpoint();
                const auto& b = targets.at(j);
                if (visited.at(j) || b.hold || a.eachGroupId < 0 || a.eachGroupId != b.eachGroupId) continue;
                if (std::abs(a.second - b.second) < epsilon && touchPadsAdjacent(a.pad, b.pad)) {
                    visited[j] = true; component.append(j);
                }
            }
        }
        groups.append(component);
    }
    return groups;
}
TouchReplay replayHandOrderTouches(const QVector<TouchTarget>& targets, const QVector<ContactAction>& contacts,
    double throughSecond, qint64* workRemaining)
{
    TouchReplay result;
    result.judgements.resize(targets.size());
    QSet<QString> pads;
    QSet<qint64> ticks;
    qint64 localBudget = 2000000;
    if (!workRemaining) workRemaining = &localBudget;
    const auto consume = [&](qint64 units = 1) {
        task::CancellationScope::checkpoint();
        *workRemaining -= units;
        if (*workRemaining >= 0) return true;
        result.exhausted = true; return false;
    };
    if (std::isnan(throughSecond) || throughSecond == -std::numeric_limits<double>::infinity()
        || !consume(qint64(targets.size()) * targets.size() + targets.size())) {
        result.exhausted = true; return result;
    }
    result.groups = buildHandOrderTouchGroups(targets);
    for (const auto& target : targets) {
        if (!std::isfinite(target.second) || std::abs(target.second) > 1e10 || !muri::padTokenIsValid(target.pad)) {
            result.exhausted = true; return result;
        }
        pads.insert(target.pad);
        // Expiry is a real failed judgement and can count toward the rule's
        // threshold. High-quality validation happens after the replay.
        const qint64 first = tickAt(target.second - TouchJudgeProfile::availableSeconds) - 1;
        const qint64 last = tickAt(qMin(target.second + TouchJudgeProfile::availableSeconds, throughSecond)) + 1;
        for (qint64 tick = first; tick <= last; ++tick) {
            if (!consume()) return result;
            ticks.insert(tick);
        }
    }
    for (const auto& action : contacts) {
        if (!consume()) return result;
        if (!std::isfinite(action.start) || !std::isfinite(action.end)
            || std::abs(action.start) > 1e10 || std::abs(action.end) > 1e10 || action.end < action.start
            || action.hand < 0 || action.hand > 1 || !std::isfinite(action.radius)
            || action.radius < 0 || action.radius > 0.5 || action.points.isEmpty()
            || action.points.size() != action.times.size()) { result.exhausted = true; return result; }
        for (int i = 0; i < action.times.size(); ++i) {
            if (!consume()) return result;
            if (!std::isfinite(action.times.at(i)) || !std::isfinite(action.points.at(i).x()) || !std::isfinite(action.points.at(i).y())
                || (i > 0 && action.times.at(i) < action.times.at(i - 1))) { result.exhausted = true; return result; }
        }
    }
    QList<qint64> orderedTicks = ticks.values(); std::sort(orderedTicks.begin(), orderedTicks.end());
    QVector<int> order(targets.size());
    for (int i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (targets.at(a).second != targets.at(b).second) return targets.at(a).second < targets.at(b).second;
        return targets.at(a).parseOrder < targets.at(b).parseOrder;
    });
    QSet<QString> previousOn;
    qint64 previousTick = std::numeric_limits<qint64>::min();
    const auto coverageAt = [&](double now) {
        QMap<QString, QVector<BasicCause>> on;
        for (const auto& action : contacts) {
            if (!consume()) return on;
            if (now < action.start - epsilon || now >= action.end - epsilon) continue;
            const auto point = contactPosition(action, now);
            for (const auto& pad : pads) {
                if (!consume()) return on;
                const double radius = muri::padRadius(pad) / 240.0 + action.radius;
                if (QLineF(point, touchPadPoint(pad)).length() > radius + epsilon) continue;
                BasicCause cause;
                cause.tokenStart = action.tokenStart; cause.branch = action.branch; cause.kind = action.kind;
                cause.pad = pad; cause.second = now; cause.handMask = 1 << action.hand;
                const double first = action.kind == "slide" ? action.motionStart : action.start;
                const double last = action.kind == "slide" ? action.motionEnd : action.end;
                cause.progress = last > first ? qBound(0.0, (now - first) / (last - first), 1.0) : 0;
                on[pad].append(cause);
            }
        }
        return on;
    };
    for (qint64 tick : orderedTicks) {
        if (!consume()) return result;
        const double now = secondAt(tick);
        if (now > throughSecond + epsilon) break;
        if (previousTick == std::numeric_limits<qint64>::min() || tick != previousTick + 1) {
            previousOn.clear(); const auto before = coverageAt(secondAt(tick - 1));
            for (auto it = before.cbegin(); it != before.cend(); ++it) previousOn.insert(it.key());
        }
        previousTick = tick;
        const auto on = coverageAt(now);
        if (*workRemaining < 0) { result.exhausted = true; return result; }
        // One total-state OFF -> ON creates one consumable DOWN per pad. A
        // contributor change while ON never creates another event.
        for (auto it = on.cbegin(); it != on.cend(); ++it) {
            if (previousOn.contains(it.key())) continue;
            for (int index : order) {
                if (!consume()) return result;
                const auto& target = targets.at(index);
                auto& judged = result.judgements[index];
                if (judged.judged || target.pad != it.key()
                    || now < target.second - TouchJudgeProfile::availableSeconds - epsilon
                    || now > target.second + TouchJudgeProfile::availableSeconds + epsilon) continue;
                judged.judged = true; judged.second = now; judged.causes = it.value();
                judged.margin = TouchJudgeProfile::criticalSeconds - std::abs(now - target.second);
                judged.highQuality = judged.margin >= -epsilon;
                judged.quality = judged.highQuality ? "critical" : "outside_target";
                bool slide = false;
                for (const auto& cause : judged.causes) { judged.handMask |= cause.handMask; slide |= cause.kind == "slide"; }
                judged.kind = slide ? "slide_cover" : "direct_touch";
                break;
            }
        }
        for (int index : order) {
            if (!consume()) return result;
            auto& judged = result.judgements[index];
            if (!judged.judged && now > targets.at(index).second + TouchJudgeProfile::availableSeconds + epsilon) {
                judged.judged = true; judged.second = now; judged.quality = "late_bad"; judged.kind = "expired";
            }
        }
        // Same-tick DOWNs are processed as a batch. Carry responsibility is the
        // union of all direct judgements by this tick, never the last event's hand.
        for (const auto& group : result.groups) {
            if (!consume()) return result;
            int count = 0, mask = 0; QVector<BasicCause> causes; bool quality = true;
            for (int index : group) {
                if (!consume()) return result;
                const auto& judged = result.judgements.at(index);
                if (!judged.judged || judged.kind == "group_carry") continue;
                ++count; mask |= judged.handMask; quality &= judged.highQuality;
                causes += judged.causes;
            }
            if (count < int(std::ceil(0.51 * group.size()))) continue;
            for (int index : group) {
                if (!consume()) return result;
                auto& judged = result.judgements[index];
                if (judged.judged) continue;
                judged.judged = true; judged.second = now; judged.kind = "group_carry";
                judged.margin = TouchJudgeProfile::criticalSeconds - std::abs(now - targets.at(index).second);
                judged.highQuality = quality && judged.margin >= -epsilon;
                judged.quality = judged.highQuality ? "critical" : "outside_target";
                judged.handMask = mask; judged.causes = causes;
            }
        }
        previousOn.clear(); for (auto it = on.cbegin(); it != on.cend(); ++it) previousOn.insert(it.key());
    }
    return result;
}
} // namespace miacode::hand_order
