#include "core/analysis/hand_order/HandOrderConflictReview.h"

#include "common/TaskCancellation.h"
#include "core/analysis/hand_order/HandPlayContacts.h"
#include "core/analysis/hand_order/HandPlayPose.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace miacode::hand_order {
namespace {
constexpr double ticksPerSecond = FullReplayProfile::ticksPerSecond;
constexpr double epsilon = 1e-8;

qint64 firstTick(double second)
{
    return qint64(std::ceil(second * ticksPerSecond - epsilon));
}

PlayContactKind inputKind(const ChartInputAction& input)
{
    if (input.contact.kind == "hold" || input.contact.kind == "touch_hold") return PlayContactKind::Hold;
    if (input.contact.kind == "slide" || input.contact.kind == "wifi") return PlayContactKind::Trace;
    return PlayContactKind::Strike;
}

bool affectedJudgement(const FullReplayJudgement& before, const FullReplayJudgement& after)
{
    const auto responsibility = [](const auto& causes) {
        int hands = 0;
        for (const auto& cause : causes) hands |= cause.handMask;
        return hands;
    };
    return (before.meetsTarget && !after.meetsTarget) || (before.headHighQuality && !after.headHighQuality)
        || (before.trackHighQuality && !after.trackHighQuality)
        || (before.singleHandCompletionMask & ~after.singleHandCompletionMask)
        || (before.headHighQuality && responsibility(before.headCauses) != responsibility(after.headCauses))
        || (before.highQuality && before.handMask && before.handMask != after.handMask);
}

ChartInputAction slice(const ChartInputAction& input, double start, double end)
{
    auto out = input;
    auto& path = out.contact;
    path.start = start;
    path.end = end;
    if (input.button) return out;
    path.times = {start};
    path.points = {contactPosition(input.contact, start)};
    const auto& original = input.contact;
    for (auto it = std::upper_bound(original.times.cbegin(), original.times.cend(), start);
        it != original.times.cend() && *it < end; ++it) {
        path.times.append(*it);
        path.points.append(original.points.at(it - original.times.cbegin()));
    }
    path.times.append(end);
    path.points.append(contactPosition(original, end));
    compactContactAction(path);
    return out;
}
}

HandOrderConflictReview reviewPhysicalConflicts(const QVector<TimelineNoteMarker>& markers,
    const BasicReplayEvidence& evidence, const PlayProfile& profile, bool wifiNeedC, qint64& work,
    const HandOrderProgressCallback& progress)
{
    HandOrderConflictReview result;
    if (!evidence.replay.valid || evidence.replay.exhausted) { result.complete = false; return result; }
    const auto& inputs = evidence.inputs;
    QSet<int> holds;
    // Button Holds are represented by the ordinary button channel in the
    // solver. Their exact source identity is restored by the compiled plan.
    for (const auto& marker : markers) if (marker.type == "hold" || marker.type == "touch_hold")
        holds.insert(marker.parseOrder);
    struct Event { qint64 tick; int index; bool down; };
    QVector<Event> events;
    for (int i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs.at(i);
        const auto from = firstTick(input.contact.start), to = firstTick(input.contact.end);
        if (to <= from) continue;
        events.append({from, i, true});
        events.append({to, i, false});
    }
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    using TickRange = QPair<qint64,qint64>;
    QVector<QVector<TickRange>> blocked(inputs.size());
    std::array<std::set<int>, 2> active;
    PlayBodyOffset body;
    qint64 lastTick = events.isEmpty() ? 0 : events.first().tick - 1;
    bool collisionActive = false;
    for (int at = 0; at < events.size();) {
        task::CancellationScope::checkpoint();
        if (progress) progress({HandOrderProgress::ConflictReview, at, int(events.size() + 1)});
        const qint64 start = events.at(at).tick;
        do {
            const auto& event = events.at(at++);
            auto& hand = active[inputs.at(event.index).contact.hand];
            if (event.down) hand.insert(event.index);
            else hand.erase(event.index);
        } while (at < events.size() && events.at(at).tick == start);
        if (at == events.size()) break;
        if (active[0].empty() || active[1].empty()) {
            collisionActive = false;
            continue;
        }
        const qint64 end = events.at(at).tick;
        for (qint64 tick = start; tick < end; ++tick) {
            task::CancellationScope::checkpoint();
            work -= 8 + qint64(active[0].size()) + qint64(active[1].size());
            if (work < 0) { result.complete = false; return result; }
            const double time = tick / ticksPerSecond;
            std::array<PlayHandMotion, 2> hands;
            bool hasSustainedInput = false;
            for (int hand = 0; hand < 2; ++hand) {
                auto& motion = hands[hand];
                motion.touching = true;
                motion.contact = contactPosition(inputs.at(*active[hand].begin()).contact, time);
                for (int index : active[hand]) {
                    const auto& input = inputs.at(index);
                    const auto kind = holds.contains(input.sourceParseOrder) ? PlayContactKind::Hold : inputKind(input);
                    if (kind == PlayContactKind::Hold || (kind == PlayContactKind::Trace && motion.kind != PlayContactKind::Hold))
                        motion.kind = kind;
                    hasSustainedInput |= kind != PlayContactKind::Strike;
                    motion.palmRadius = qMax(motion.palmRadius, input.contact.radius);
                    if (input.button) motion.buttonLane = input.lane;
                    else if (!motion.screen) {
                        motion.screen = true;
                        motion.contact = contactPosition(input.contact, time);
                    }
                    motion.source = input.contact.tokenStart;
                }
            }
            // Pure strike crossings remain the search's Each check. Sustained
            // contacts may make the planned hold or path physically unavoidable.
            if (!hasSustainedInput) { collisionActive = false; continue; }
            const auto target = preferredBodyOffset(hands, profile);
            const double gap = (tick - lastTick) / ticksPerSecond;
            if (gap > 0.5) body = {};
            const double rise = gap * profile.verticalSpeedCmPerSecond;
            const double step = gap * profile.foreAftSpeedCmPerSecond;
            body.verticalCm += qBound(-rise, target.verticalCm - body.verticalCm, rise);
            body.distanceCm += qBound(-step, target.distanceCm - body.distanceCm, step);
            lastTick = tick;
            const auto pose = resolvePlayArms(hands, 0.08, profile, body.verticalCm, body.distanceCm);
            if (pose.interference != PlayInterference::Collision) { collisionActive = false; continue; }
            if (!collisionActive) ++result.intervals;
            collisionActive = true;
            for (int hand = 0; hand < 2; ++hand) {
                if (playArmCanPress(pose, hands, hand)) continue;
                for (int index : active[hand]) {
                    auto& ranges = blocked[index];
                    if (!ranges.isEmpty() && ranges.last().second == tick) ranges.last().second = tick + 1;
                    else ranges.append({tick, tick + 1});
                }
            }
        }
    }
    if (result.intervals == 0) return result;
    QVector<ChartInputAction> checked;
    for (int i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs.at(i);
        double from = input.contact.start;
        for (const auto& interval : blocked.at(i)) {
            const double until = qMin(input.contact.end, interval.first / ticksPerSecond);
            if (until > from + epsilon) checked.append(slice(input, from, until));
            from = qMax(from, interval.second / ticksPerSecond);
        }
        if (input.contact.end > from + epsilon)
            checked.append(blocked.at(i).isEmpty() ? input : slice(input, from, input.contact.end));
    }
    FullReplayOptions options;
    options.wifiNeedC = wifiNeedC;
    if (progress) progress({HandOrderProgress::ConflictReview, int(events.size()), int(events.size() + 1)});
    // Compare both streams under the same judgement settings.
    const auto configuredBaseline = wifiNeedC ? replayHandOrderChart(markers, inputs, options, &work) : FullReplay{};
    const auto& baseline = wifiNeedC ? configuredBaseline : evidence.replay;
    if (!baseline.valid || baseline.exhausted) { result.complete = false; return result; }
    const auto replay = replayHandOrderChart(markers, checked, options, &work);
    result.complete = replay.valid && !replay.exhausted && replay.judgements.size() == baseline.judgements.size();
    if (!result.complete) return result;
    for (int i = 0; i < replay.judgements.size(); ++i) {
        const auto& before = baseline.judgements.at(i);
        const auto& after = replay.judgements.at(i);
        if (!affectedJudgement(before, after)) continue;
        result.regressedOrders.insert(before.parseOrder);
        result.affectedJudgements.insert(before.parseOrder, after);
        for (const auto& cause : before.causes) result.affectedSources.insert(cause.tokenStart);
        for (const auto& cause : before.headCauses) result.affectedSources.insert(cause.tokenStart);
    }
    const auto regressed = result.affectedSources;
    // The retained companion arm may have caused every suppression while
    // none of its own notes lost judgement.
    for (int i = 0; !result.regressedOrders.isEmpty() && i < blocked.size(); ++i) {
        for (const auto& range : blocked.at(i)) {
            if (!regressed.contains(inputs.at(i).contact.tokenStart)) continue;
            const double begin = range.first / ticksPerSecond, end = range.second / ticksPerSecond;
            for (const auto& input : inputs) {
                task::CancellationScope::checkpoint();
                if (--work < 0) { result.complete = false; return result; }
                if (input.contact.hand != inputs.at(i).contact.hand && input.contact.start < end && input.contact.end > begin)
                    result.affectedSources.insert(input.contact.tokenStart);
            }
        }
    }
    return result;
}

} // namespace miacode::hand_order
