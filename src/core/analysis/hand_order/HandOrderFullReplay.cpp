#include "core/analysis/hand_order/HandOrderFullReplay.h"

#include "common/TaskCancellation.h"
#include "core/analysis/MuriConfig.h"
#include "core/analysis/MuriSlideReferenceData.h"
#include "core/chart/SlideReferenceData.h"
#include <QJsonArray>
#include <QLineF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
using Causes = QVector<BasicCause>;
using PadStates = QMap<QString, Causes>;
using Tick = qint64;
Tick firstTick(double second) { return Tick(std::ceil(second * FullReplayProfile::ticksPerSecond - epsilon)); }
Tick afterTick(double second) { return Tick(std::floor(second * FullReplayProfile::ticksPerSecond + epsilon)) + 1; }
double secondAt(Tick tick) { return tick / FullReplayProfile::ticksPerSecond; }
bool safeSecond(double second) { return std::isfinite(second) && std::abs(second) <= 1e10; }
int handMask(const Causes& causes) { int mask = 0; for (const auto& cause : causes) mask |= cause.handMask; return mask; }
QString buttonChannel(int lane) { return QStringLiteral("Button%1").arg(lane); }
QString sourceKey(int token, int branch, const QString& kind, int mask) {
    return QStringLiteral("%1|%2|%3|%4").arg(token).arg(branch).arg(kind).arg(mask);
}

struct Budget {
    qint64* remaining;
    FullReplay* result;
    bool use(qint64 units = 1) {
        task::CancellationScope::checkpoint();
        if (*remaining < units) {
            result->workUsed += qMax(qint64(0), *remaining);
            *remaining = -1; result->exhausted = true; result->valid = false;
            result->error = QStringLiteral("resource_limit"); return false;
        }
        *remaining -= units; result->workUsed += units; return true;
    }
};

struct Change { int action = 0; QString channel; bool on = false; };
struct Events : QMap<Tick, QVector<Change>> {
    qint64 entryCount = 0, limit = FullReplayProfile::maximumEventEntries;
    bool reserveEntry(Budget& budget) {
        if (!budget.use()) return false;
        if (entryCount >= limit) {
            budget.result->valid = false; budget.result->exhausted = true;
            budget.result->error = QStringLiteral("event_memory_limit"); return false;
        }
        ++entryCount; return true;
    }
    bool point(Tick tick, Budget& budget) {
        if (contains(tick)) return true;
        if (!reserveEntry(budget)) return false;
        insert(tick, {}); return true;
    }
    bool change(Tick tick, const Change& change, Budget& budget) {
        if (!point(tick, budget) || !reserveEntry(budget)) return false;
        (*this)[tick].append(change); return true;
    }
};
struct Interval { Tick first = 0, last = -1; };

// Every ContactAction is piecewise linear. Its circle/pad intersections are
// solved analytically, then projected onto the profile's grid. Thus a stationary
// contact lasting years costs two events and cannot invent a boundary DOWN.
bool addScreenIntervals(const ChartInputAction& input, int actionIndex, Events& events, Budget& budget)
{
    const auto& action = input.contact;
    QVector<double> knots{action.start};
    for (double time : action.times) {
        if (!budget.use()) return false;
        if (time > action.start && time < action.end) knots.append(time);
    }
    knots.append(action.end);
    const Tick actionFirst = firstTick(action.start), actionLast = firstTick(action.end) - 1;
    QStringList pads{QStringLiteral("C")};
    for (const QChar ring : {QLatin1Char('A'), QLatin1Char('B'), QLatin1Char('D'), QLatin1Char('E')})
        for (int lane = 1; lane <= 8; ++lane) pads.append(QStringLiteral("%1%2").arg(ring).arg(lane));
    for (const auto& pad : pads) {
        if (!budget.use()) return false;
        QVector<Interval> intervals;
        const auto center = touchPadPoint(pad);
        const double radius = muri::padRadius(pad) / 240.0 + action.radius;
        for (int i = 1; i < knots.size(); ++i) {
            if (!budget.use()) return false;
            const double start = knots.at(i - 1), end = knots.at(i);
            const auto p = contactPosition(action, start) - center;
            const auto d = contactPosition(action, end) - center - p;
            const double a = QPointF::dotProduct(d, d);
            const double b = 2 * QPointF::dotProduct(p, d);
            const double c = QPointF::dotProduct(p, p) - radius * radius;
            double enter = 0, leave = 1;
            if (a <= 1e-24) {
                if (c > epsilon) continue;
            } else {
                double disc = b * b - 4 * a * c;
                if (disc < -epsilon) continue;
                disc = std::sqrt(qMax(0.0, disc));
                enter = qMax(0.0, (-b - disc) / (2 * a));
                leave = qMin(1.0, (-b + disc) / (2 * a));
                if (leave < enter - epsilon) continue;
            }
            const Tick first = qMax(actionFirst, firstTick(start + (end - start) * enter));
            const Tick last = qMin(actionLast, afterTick(start + (end - start) * leave) - 1);
            if (last < first) continue;
            if (!intervals.isEmpty() && first <= intervals.last().last + 1) intervals.last().last = qMax(last, intervals.last().last);
            else intervals.append({first, last});
        }
        for (const auto& interval : intervals) {
            if (!budget.use(2)) return false;
            if (!events.change(interval.first, {actionIndex, pad, true}, budget)
                || !events.change(interval.last + 1, {actionIndex, pad, false}, budget)) return false;
        }
    }
    return true;
}

bool screenInputsConsistent(const QVector<ChartInputAction>& actions, Budget& budget)
{
    QVector<int> order;
    for (int i = 0; i < actions.size(); ++i) if (!actions.at(i).button) order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return actions.at(a).contact.start < actions.at(b).contact.start; });
    for (int i = 0; i < order.size(); ++i) {
        const auto& a = actions.at(order.at(i)).contact;
        for (int j = i + 1; j < order.size(); ++j) {
            if (!budget.use()) return false;
            const auto& b = actions.at(order.at(j)).contact;
            if (b.start >= a.end - epsilon) break;
            if (a.hand != b.hand) continue;
            const double first = qMax(a.start, b.start), last = qMin(a.end, b.end);
            if (last <= first + epsilon) continue;
            QVector<double> samples{first, last};
            for (const auto* action : {&a, &b}) for (double time : action->times) {
                if (!budget.use()) return false;
                if (time > first && time < last) samples.append(time);
            }
            // Both paths are linear between this union of their knots.
            for (double time : samples) {
                if (!budget.use()) return false;
                if (QLineF(contactPosition(a, time), contactPosition(b, time)).length() > epsilon) return false;
            }
        }
    }
    return true;
}

struct HoldBody {
    double observedSeconds = 0, rawHeldSeconds = 0;
    double gapSeconds = 0, pendingGapSeconds = 0, maxIgnoredGapSeconds = 0;
};

struct Simple {
    QVector<int> owners;
    QString pad, channel;
    double second = 0, end = 0, critical = 0, available = 0;
    bool touch = false, hold = false, head = false, judged = false, completed = false, highQuality = false;
    bool headHighQuality = false;
    double judgeSecond = 0, margin = 0;
    double bodyFirst = 0, bodyLast = 0;
    HoldBody body;
    std::array<HoldBody, 2> singleHandBodies;
    int singleHandCompletionMask = 0;
    QString kind, reason;
    Causes causes, bodyCauses;
};

struct Track {
    int owner = -1;
    double available = 0, end = 0, critical = 0, tolerance = 0;
    bool wifi = false, isL = false, specialL = false, needC = false, passedC = false;
    bool done = false, highQuality = false, failed = false;
    double judgeSecond = 0, margin = 0, cSecond = std::numeric_limits<double>::quiet_NaN();
    QVector<QVector<QStringList>> queues;
    QVector<QVector<FullReplayArea>> hits;
    QVector<int> progress, segmentStarts, segmentEnds;
    QVector<QString> pressing;
    Causes causes;
};

bool buildTrack(const TimelineNoteMarker& marker, int owner, Track& track, const FullReplayOptions& options, Budget& budget)
{
    const auto& root = slide_reference::root();
    track.owner = owner; track.wifi = marker.type == QLatin1String("wifi");
    track.available = marker.second - FullReplayProfile::slideLeadingSeconds;
    track.end = marker.endSecond;
    double lastAreaDuration = 0;
    if (track.wifi) {
        const auto entry = root.value(QStringLiteral("wifi")).toObject().value(marker.slideTrackKey).toObject();
        track.queues = muri::detail::loadTriPadAreaSequence(entry.value(QStringLiteral("tri_judge_sequence")).toArray());
        if (track.queues.size() != 3 || track.queues.first().isEmpty()) return false;
        const int count = track.queues.first().size();
        for (const auto& queue : track.queues) if (queue.size() != count) return false;
        if (!std::isfinite(marker.wifiCriticalProportion) || marker.wifiCriticalProportion < 0 || marker.wifiCriticalProportion > 1) return false;
        lastAreaDuration = (1 - marker.wifiCriticalProportion) * qMax(0.0, marker.endSecond - marker.slideTraceSecond);
        track.needC = options.wifiNeedC; track.passedC = !track.needC;
    } else {
        QVector<QStringList> queue;
        for (int segment = 0; segment < marker.slideSegmentKeys.size(); ++segment) {
            if (!budget.use()) return false;
            const auto entry = root.value(QStringLiteral("slides")).toObject().value(marker.slideSegmentKeys.at(segment)).toObject();
            const auto next = muri::detail::loadPadAreaSequence(entry.value(QStringLiteral("judge_sequence")).toArray());
            if (next.isEmpty()) return false;
            const bool shared = !queue.isEmpty() && queue.last() == next.first();
            track.segmentStarts.append(shared ? queue.size() - 1 : queue.size());
            for (int area = shared ? 1 : 0; area < next.size(); ++area) {
                if (!budget.use()) return false;
                queue.append(next.at(area));
            }
            track.segmentEnds.append(queue.size() - 1);
            if (marker.slideSegmentKeys.size() == 1) {
                track.isL = entry.value(QStringLiteral("is_l")).toBool();
                track.specialL = entry.value(QStringLiteral("is_special_l")).toBool();
            }
        }
        if (queue.isEmpty()) return false;
        track.queues = {queue};
        const int last = qMin(marker.slideSegmentDurations.size(), marker.slideSegmentCriticalProportions.size()) - 1;
        if (last >= 0) {
            const double proportion = marker.slideSegmentCriticalProportions.at(last);
            const double duration = marker.slideSegmentDurations.at(last);
            if (!safeSecond(duration) || duration < 0 || !std::isfinite(proportion) || proportion < 0 || proportion > 1) return false;
            lastAreaDuration = (1 - proportion) * duration;
        }
    }
    for (const auto& queue : track.queues) {
        if (!budget.use(queue.size())) return false;
        for (const auto& area : queue) {
            if (area.isEmpty()) return false;
            for (const auto& pad : area) if (!muri::padTokenIsValid(pad)) return false;
        }
        track.hits.append(QVector<FullReplayArea>(queue.size()));
        track.progress.append(0); track.pressing.append(QString());
    }
    track.critical = marker.endSecond - lastAreaDuration;
    track.tolerance = qMin(FullReplayProfile::slideAvailableSeconds,
        FullReplayProfile::slideCriticalSeconds + lastAreaDuration / 4.0);
    return true;
}

bool canSkip(const Track& track, int lane)
{
    const int area = track.progress.at(lane), count = track.queues.at(lane).size();
    if (area >= count - 1) return false;
    if (track.wifi || !track.pressing.at(lane).isEmpty()) return true;
    if (track.segmentEnds.size() == 1) {
        if (track.specialL) return area != 1 && area != 3;
        if (track.isL) return area != 1;
    }
    return count >= 4 || area != count - 2;
}

void recordArea(Track& track, int lane, int area, const QString& pad, double now, const Causes& causes, bool skipped = false)
{
    auto& hit = track.hits[lane][area];
    if (hit.hit) return;
    hit.hit = true; hit.skipped = skipped; hit.second = now; hit.pad = pad; hit.causes = causes;
    track.causes += causes;
}

bool advanceOnce(Track& track, int lane, double now, const PadStates& on, const PadStates& up, Budget& budget)
{
    const int index = track.progress.at(lane), count = track.queues.at(lane).size();
    if (index >= count || !budget.use()) return false;
    auto& pressing = track.pressing[lane];
    if (pressing.isEmpty()) {
        for (const auto& pad : track.queues.at(lane).at(index)) {
            if (!budget.use()) return false;
            if (!on.contains(pad)) continue;
            pressing = pad; recordArea(track, lane, index, pad, now, on.value(pad));
            if (index == count - 1) ++track.progress[lane];
            return true;
        }
    } else if (!on.contains(pressing)) {
        pressing.clear(); ++track.progress[lane]; return true;
    }
    if (canSkip(track, lane)) {
        for (const auto& pad : track.queues.at(lane).at(index + 1)) {
            if (!budget.use()) return false;
            if (!on.contains(pad) && !up.contains(pad)) continue;
            const auto causes = on.contains(pad) ? on.value(pad) : up.value(pad);
            if (!track.hits.at(lane).at(index).hit) recordArea(track, lane, index, pad, now, causes, true);
            pressing = pad; ++track.progress[lane];
            recordArea(track, lane, index + 1, pad, now, causes);
            if (index + 1 == count - 1) ++track.progress[lane];
            return true;
        }
    }
    return false;
}

void advanceTrack(Track& track, double now, const PadStates& on, const PadStates& up, Budget& budget)
{
    if (track.done || track.failed || now + epsilon < track.available) return;
    bool finished = true;
    for (int lane = 0; lane < track.queues.size(); ++lane) {
        while (track.progress.at(lane) < track.queues.at(lane).size()) {
            if (!budget.use() || !advanceOnce(track, lane, now, on, up, budget)) break;
        }
        finished &= track.progress.at(lane) >= track.queues.at(lane).size();
    }
    if (track.wifi && !track.passedC && track.progress.value(1) > 0 && up.contains(QStringLiteral("C"))) {
        track.passedC = true; track.cSecond = now; track.causes += up.value(QStringLiteral("C"));
    }
    if (finished && (!track.wifi || track.passedC)) {
        track.done = true; track.judgeSecond = now;
        track.margin = track.tolerance - std::abs(now - track.critical);
        if (!track.wifi) track.margin = qMax(track.margin,
            FullReplayProfile::slideCriticalSeconds - std::abs(now - track.critical + FullReplayProfile::slideDeltaShiftSeconds));
        track.highQuality = track.margin >= -epsilon;
    } else if (now > track.end + FullReplayProfile::slideAvailableSeconds + epsilon) {
        track.failed = true; track.judgeSecond = now;
    }
}

QString headKey(const TimelineNoteMarker& marker)
{
    const QString scope = marker.sameHeadSlide ? QStringLiteral("same") : QString::number(marker.sourceCol);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7").arg(marker.second, 0, 'g', 17).arg(marker.sourceLine)
        .arg(scope).arg(marker.lane).arg(marker.headBreak).arg(marker.headEx).arg(marker.headEach);
}

double holdReleasedSeconds(const HoldBody& body)
{
    return body.gapSeconds + (body.pendingGapSeconds + epsilon >= FullReplayProfile::holdReleaseDebounceSeconds
        ? body.pendingGapSeconds : 0.0);
}

void appendDistinctCauses(Causes& target, const Causes& incoming, Budget& budget)
{
    for (const auto& cause : incoming) {
        if (!budget.use()) return;
        bool exists = false;
        for (const auto& old : target) {
            if (!budget.use()) return;
            if (old.tokenStart == cause.tokenStart && old.branch == cause.branch && old.kind == cause.kind
                && old.pad == cause.pad && old.handMask == cause.handMask) { exists = true; break; }
        }
        if (!exists) target.append(cause);
    }
}

void observeHoldBody(HoldBody& body, double duration, bool pressed, bool realHead)
{
    body.observedSeconds += duration;
    if (pressed) {
        body.rawHeldSeconds += duration;
        if (body.pendingGapSeconds + epsilon >= FullReplayProfile::holdReleaseDebounceSeconds)
            body.gapSeconds += body.pendingGapSeconds;
        else body.maxIgnoredGapSeconds = qMax(body.maxIgnoredGapSeconds, body.pendingGapSeconds);
        body.pendingGapSeconds = 0;
    } else if (!realHead && body.rawHeldSeconds <= epsilon) {
        // Debounce preserves an existing press. It cannot turn the initial OFF
        // of an entirely untouched note into a held body or a non-Miss result.
        body.gapSeconds += duration;
    } else body.pendingGapSeconds += duration;
}

void observeHold(Simple& simple, double first, double last, const PadStates& on, Budget& budget)
{
    if (!simple.hold || simple.completed) return;
    const double begin = qMax(first, simple.bodyFirst), end = qMin(last, simple.bodyLast);
    if (end <= begin) return;
    // Body shadows read existing physical ON contributors. They never filter
    // and replay DOWNs, which could manufacture a new head after removing a hand.
    const QString button = simple.touch ? QString() : buttonChannel(simple.pad.mid(1).toInt());
    int pressedHands = 0, headHands = 0;
    for (const auto& cause : simple.causes) { if (!budget.use()) return; headHands |= cause.handMask; }
    for (const auto& channel : {simple.pad, button}) for (const auto& cause : on.value(channel)) {
        if (!budget.use()) return;
        pressedHands |= cause.handMask;
    }
    observeHoldBody(simple.body, end - begin, pressedHands != 0, headHands != 0);
    for (int hand = 0; hand < 2; ++hand) {
        if (!budget.use()) return;
        observeHoldBody(simple.singleHandBodies[hand], end - begin, (pressedHands & (1 << hand)) != 0, (headHands & (1 << hand)) != 0);
    }
    if (pressedHands) {
        if (on.contains(simple.pad)) appendDistinctCauses(simple.bodyCauses, on.value(simple.pad), budget);
        if (on.contains(button)) appendDistinctCauses(simple.bodyCauses, on.value(button), budget);
    }
}

void finishSimple(Simple& simple, double now)
{
    if (!simple.judged && now > simple.second + simple.available + epsilon) {
        simple.judged = true; simple.judgeSecond = now;
        simple.reason = QStringLiteral("no_down_in_window"); simple.kind = QStringLiteral("expired");
    }
    if (!simple.hold || !simple.judged || simple.completed || now + epsilon < simple.end) return;
    const double duration = qMax(0.0, simple.bodyLast - simple.bodyFirst);
    const double released = holdReleasedSeconds(simple.body);
    const double ratio = duration > epsilon ? qBound(0.0, (duration - released) / duration, 1.0) : 0;
    // A missed head can still obtain a non-Miss result from the held body.
    // The source's table grants Late Good once at least 5% of it is held.
    simple.completed = !simple.causes.isEmpty() || (duration > epsilon && ratio + epsilon >= .05);
    if (simple.completed && simple.causes.isEmpty()) simple.kind = QStringLiteral("hold_body");
    simple.highQuality = simple.headHighQuality && released <= epsilon;
    if (!simple.completed) simple.reason = QStringLiteral("no_down_in_window");
    else if (!simple.headHighQuality) simple.reason = QStringLiteral("hold_head_outside_target");
    else if (!simple.highQuality) simple.reason = QStringLiteral("hold_contact_gap");
    else simple.reason = QStringLiteral("completed");
    simple.singleHandCompletionMask = 0;
    if (simple.highQuality) for (int hand = 0; hand < 2; ++hand)
        if ((handMask(simple.causes) & (1 << hand)) && holdReleasedSeconds(simple.singleHandBodies[hand]) <= epsilon)
            simple.singleHandCompletionMask |= 1 << hand;
    if (simple.highQuality && duration > epsilon) {
        const double gap = qMax(simple.body.maxIgnoredGapSeconds, simple.body.pendingGapSeconds);
        simple.margin = qMin(simple.margin, FullReplayProfile::holdReleaseDebounceSeconds - gap);
    }
}

} // namespace

FullReplay replayHandOrderChart(const QVector<TimelineNoteMarker>& markers,
    const QVector<ChartInputAction>& actions, const FullReplayOptions& options, qint64* workRemaining)
{
    FullReplay result;
    result.judgements.resize(markers.size());
    qint64 localRemaining = 30000000;
    if (!workRemaining) workRemaining = &localRemaining;
    Budget budget{workRemaining, &result};
    if (options.profileRevision != fullReplayProfileRevision || std::isnan(options.throughSecond)
        || options.throughSecond == -std::numeric_limits<double>::infinity()
        || (std::isfinite(options.throughSecond) && !safeSecond(options.throughSecond))
        || options.maxEventEntries <= 0 || options.maxEventEntries > FullReplayProfile::maximumEventEntries) {
        result.valid = false; result.error = QStringLiteral("invalid_profile_or_horizon"); return result;
    }
    Events events;
    events.limit = options.maxEventEntries;
    QVector<Simple> simpleNotes;
    QVector<Track> tracks;
    QVector<TouchTarget> touchTargets;
    QVector<int> touchSimpleIndices;
    QMap<QString, int> heads;
    QMap<QString, QSet<int>> sourceOrders;
    for (int index = 0; index < markers.size(); ++index) {
        if (!budget.use()) return result;
        const auto& marker = markers.at(index);
        auto& judgement = result.judgements[index];
        judgement.markerIndex = index; judgement.parseOrder = marker.parseOrder;
        judgement.kind = marker.type; judgement.reason = QStringLiteral("pending"); judgement.quality = QStringLiteral("pending");
        const bool slide = marker.type == QLatin1String("slide") || marker.type == QLatin1String("wifi");
        if (!safeSecond(marker.second) || !safeSecond(marker.endSecond)
            || (slide && (!safeSecond(marker.slideTraceSecond) || marker.endSecond < marker.slideTraceSecond))) {
            result.valid = false; result.error = QStringLiteral("invalid_marker_time"); return result;
        }
        if (marker.isMine || marker.headMine || marker.trackMine) {
            judgement.supported = false; judgement.reason = QStringLiteral("mine_unsupported"); continue;
        }
        if (slide) {
            Track track;
            if (!buildTrack(marker, index, track, options, budget)) {
                if (result.exhausted) return result;
                judgement.supported = false; judgement.reason = QStringLiteral("judge_geometry_unsupported"); continue;
            }
            if (!events.point(firstTick(track.available), budget)
                || !events.point(afterTick(track.end + FullReplayProfile::slideAvailableSeconds), budget)) return result;
            tracks.append(track);
            judgement.headRequired = marker.hasHeadStar;
            if (!marker.hasHeadStar) continue;
        } else if (marker.type != QLatin1String("tap") && marker.type != QLatin1String("hold")
            && marker.type != QLatin1String("touch") && marker.type != QLatin1String("touch_hold")) {
            judgement.supported = false; judgement.reason = QStringLiteral("note_type_unsupported"); continue;
        }
        const bool touch = marker.type == QLatin1String("touch") || marker.type == QLatin1String("touch_hold");
        if ((!touch && (marker.lane < 1 || marker.lane > 8)) || (touch && !muri::padTokenIsValid(marker.touchPad))) {
            judgement.supported = false; judgement.reason = QStringLiteral("invalid_input_channel"); continue;
        }
        if (slide && heads.contains(headKey(marker))) {
            simpleNotes[heads.value(headKey(marker))].owners.append(index); continue;
        }
        Simple simple;
        simple.owners = {index}; simple.second = marker.second; simple.touch = touch; simple.head = slide;
        simple.hold = marker.type == QLatin1String("hold") || marker.type == QLatin1String("touch_hold");
        simple.end = simple.hold ? qMax(marker.second, marker.endSecond) : marker.second;
        simple.pad = touch ? marker.touchPad.toUpper() : QStringLiteral("A%1").arg(marker.lane);
        simple.critical = touch ? FullReplayProfile::touchCriticalSeconds : FullReplayProfile::tapCriticalSeconds;
        simple.available = touch ? FullReplayProfile::touchAvailableSeconds : FullReplayProfile::tapAvailableSeconds;
        if (!touch && (slide ? marker.headEx : marker.isEx)) simple.critical = simple.available;
        if (simple.hold) {
            simple.bodyFirst = marker.second + (touch ? FullReplayProfile::touchHoldHeadIgnoreSeconds : FullReplayProfile::holdHeadIgnoreSeconds);
            simple.bodyLast = qMax(simple.bodyFirst, marker.endSecond - FullReplayProfile::holdTailIgnoreSeconds);
            if (!events.point(firstTick(simple.bodyFirst), budget) || !events.point(firstTick(simple.bodyLast), budget)) return result;
            judgement.headRequired = true;
        }
        if (!events.point(firstTick(simple.second - simple.available), budget)
            || !events.point(afterTick(simple.second + simple.available), budget)) return result;
        if (simple.hold && !events.point(firstTick(simple.end), budget)) return result;
        if (touch) {
            touchTargets.append({index, marker.parseOrder, marker.eachGroupId, simple.pad, simple.second, simple.hold, -1});
            touchSimpleIndices.append(simpleNotes.size());
        }
        if (slide) heads.insert(headKey(marker), simpleNotes.size());
        simpleNotes.append(simple);
    }
    if (!budget.use(qint64(touchTargets.size()) * touchTargets.size() + touchTargets.size())) return result;
    const auto touchGroups = buildHandOrderTouchGroups(touchTargets);
    for (const auto& group : touchGroups) {
        QVector<int> markerIndices;
        for (int index : group) markerIndices.append(touchTargets.at(index).tokenStart);
        result.touchGroups.append(markerIndices);
    }
    for (int index = 0; index < actions.size(); ++index) {
        if (!budget.use()) return result;
        const auto& input = actions.at(index); const auto& action = input.contact;
        if (!safeSecond(action.start) || !safeSecond(action.end) || action.end < action.start
            || action.hand < 0 || action.hand > 1 || !safeSecond(action.motionStart) || !safeSecond(action.motionEnd)
            || !std::isfinite(action.radius) || action.radius < 0 || action.radius > FullReplayProfile::maximumPalmRadius + epsilon
            || (!input.button && (action.points.isEmpty() || action.times.size() != action.points.size()))
            || (input.button && (input.lane < 1 || input.lane > 8))) {
            result.valid = false; result.error = QStringLiteral("invalid_input_action"); return result;
        }
        if (input.sourceParseOrder >= 0)
            sourceOrders[sourceKey(action.tokenStart, action.branch, action.kind, 1 << action.hand)].insert(input.sourceParseOrder);
        for (int i = 0; i < action.times.size(); ++i) {
            if (!budget.use()) return result;
            if (!safeSecond(action.times.at(i)) || i >= action.points.size()
                || !std::isfinite(action.points.at(i).x()) || !std::isfinite(action.points.at(i).y())
                || std::abs(action.points.at(i).x()) > 1e6 || std::abs(action.points.at(i).y()) > 1e6
                || (i > 0 && action.times.at(i) <= action.times.at(i - 1))) {
                result.valid = false; result.error = QStringLiteral("invalid_action_path"); return result;
            }
        }
        if (action.end <= action.start) continue;
        if (input.button) {
            if (!events.change(firstTick(action.start), {index, buttonChannel(input.lane), true}, budget)
                || !events.change(firstTick(action.end), {index, buttonChannel(input.lane), false}, budget)) return result;
        } else if (!addScreenIntervals(input, index, events, budget)) return result;
    }
    if (!screenInputsConsistent(actions, budget)) {
        result.valid = false;
        if (!result.exhausted) result.error = QStringLiteral("inconsistent_same_hand_screen_contacts");
        return result;
    }
    // Expiry is a single event, independent of chart duration and empty rests.
    // Finite prefixes stop before it and retain the entire accumulated queue.
    const double horizon = options.throughSecond;
    if (std::isfinite(horizon) && !events.point(afterTick(horizon), budget)) return result;
    QVector<int> order(simpleNotes.size());
    for (int i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& left = simpleNotes.at(a); const auto& right = simpleNotes.at(b);
        if (left.second != right.second) return left.second < right.second;
        return markers.at(left.owners.first()).parseOrder < markers.at(right.owners.first()).parseOrder;
    });
    // Keep the original consumption order, but visit only notes whose window
    // has opened. Finished notes still remain in the returned audit; removing
    // them from these queues never creates inputs or removes a native consumer.
    QVector<int> simpleActivation(order.size()), trackActivation(tracks.size());
    for (int rank = 0; rank < order.size(); ++rank) simpleActivation[rank] = rank;
    std::stable_sort(simpleActivation.begin(), simpleActivation.end(), [&](int a, int b) {
        const auto& left = simpleNotes.at(order.at(a)); const auto& right = simpleNotes.at(order.at(b));
        return left.second - left.available < right.second - right.available;
    });
    for (int index = 0; index < tracks.size(); ++index) trackActivation[index] = index;
    std::stable_sort(trackActivation.begin(), trackActivation.end(), [&](int a, int b) { return tracks.at(a).available < tracks.at(b).available; });
    QVector<int> simpleGroup(simpleNotes.size(), -1);
    for (int group = 0; group < touchGroups.size(); ++group) for (int child : touchGroups.at(group)) {
        if (!budget.use()) return result;
        simpleGroup[touchSimpleIndices.at(child)] = group;
    }
    if (!budget.use(simpleActivation.size() + trackActivation.size() + simpleGroup.size())) return result;
    std::set<int> activeSimples, activeHolds, activeGroups, activeTracks;
    int nextSimple = 0, nextTrack = 0;
    QMap<QString, QSet<int>> contributors;
    PadStates previousOn;
    double previousSecond = 0;
    bool havePrevious = false;
    const auto causesFor = [&](const QString& channel, const QSet<int>& indices, double now) {
        Causes causes; auto ordered = indices.values(); std::sort(ordered.begin(), ordered.end());
        for (int index : ordered) {
            if (!budget.use()) break;
            const auto& action = actions.at(index).contact;
            BasicCause cause; cause.tokenStart = action.tokenStart; cause.branch = action.branch;
            cause.kind = action.kind; cause.pad = channel; cause.second = now; cause.handMask = 1 << action.hand;
            const bool moving = action.kind == QLatin1String("slide") || action.kind == QLatin1String("wifi");
            const double first = moving ? action.motionStart : action.start, last = moving ? action.motionEnd : action.end;
            cause.progress = last > first ? qBound(0.0, (now - first) / (last - first), 1.0) : 0;
            causes.append(cause);
        }
        return causes;
    };
    for (auto eventIt = events.cbegin(); eventIt != events.cend(); ++eventIt) {
        if (!budget.use()) return result;
        const double now = secondAt(eventIt.key());
        if (now > horizon + epsilon) break;
        while (nextSimple < simpleActivation.size()) {
            const int rank = simpleActivation.at(nextSimple), index = order.at(rank);
            const auto& simple = simpleNotes.at(index);
            if (now < simple.second - simple.available - epsilon) break;
            if (!budget.use()) return result;
            ++nextSimple; activeSimples.insert(rank);
            if (simple.hold) activeHolds.insert(rank);
            if (simpleGroup.at(index) >= 0) activeGroups.insert(simpleGroup.at(index));
        }
        while (nextTrack < trackActivation.size() && now + epsilon >= tracks.at(trackActivation.at(nextTrack)).available) {
            if (!budget.use()) return result;
            activeTracks.insert(trackActivation.at(nextTrack++));
        }
        if (havePrevious) for (int rank : activeHolds) {
            if (!budget.use()) return result;
            observeHold(simpleNotes[order.at(rank)], previousSecond, now, previousOn, budget);
        }
        const PadStates before = previousOn;
        const auto previousContributors = contributors;
        for (const auto& change : eventIt.value()) {
            if (!budget.use()) return result;
            if (change.on) contributors[change.channel].insert(change.action);
            else contributors[change.channel].remove(change.action);
        }
        PadStates on, up;
        for (auto it = contributors.cbegin(); it != contributors.cend(); ++it) {
            if (!budget.use()) return result;
            if (!it.value().isEmpty()) on.insert(it.key(), causesFor(it.key(), it.value(), now));
        }
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            if (!budget.use()) return result;
            if (!on.contains(it.key())) {
                auto causes = causesFor(it.key(), previousContributors.value(it.key()), now);
                up.insert(it.key(), causes);
            }
        }
        // A total OFF->ON transition is consumable once per independent input
        // channel. Slides read sustained screen state and never consume DOWNs.
        auto channels = on.keys();
        std::stable_sort(channels.begin(), channels.end(), [](const QString& a, const QString& b) {
            const bool buttonA = a.startsWith(QLatin1String("Button")), buttonB = b.startsWith(QLatin1String("Button"));
            if (buttonA != buttonB) return buttonA;
            return a < b;
        });
        for (const auto& channel : channels) {
            if (!budget.use()) return result;
            if (before.contains(channel)) continue;
            for (int rank : activeSimples) {
                if (!budget.use()) return result;
                auto& simple = simpleNotes[order.at(rank)];
                if (simple.judged || now < simple.second - simple.available - epsilon
                    || now > simple.second + simple.available + epsilon) continue;
                const bool matches = channel == simple.pad || (!simple.touch
                    && channel == buttonChannel(markers.at(simple.owners.first()).lane));
                if (!matches) continue;
                simple.judged = true; simple.completed = !simple.hold; simple.judgeSecond = now;
                simple.margin = simple.critical - std::abs(now - simple.second); simple.highQuality = simple.margin >= -epsilon;
                simple.headHighQuality = simple.highQuality;
                simple.causes = on.value(channel); simple.channel = channel;
                simple.kind = simple.head ? QStringLiteral("star_head") : QStringLiteral("direct_input");
                simple.reason = simple.highQuality ? QStringLiteral("completed") : QStringLiteral("outside_target_window");
                break;
            }
        }
        for (auto it = activeSimples.begin(); it != activeSimples.end();) {
            if (!budget.use()) return result;
            auto& simple = simpleNotes[order.at(*it)]; finishSimple(simple, now);
            if (simple.judged && (!simple.hold || now + epsilon >= simple.end)) {
                activeHolds.erase(*it); it = activeSimples.erase(it);
            } else ++it;
        }
        for (auto it = activeGroups.begin(); it != activeGroups.end();) {
            if (!budget.use()) return result;
            const auto& group = touchGroups.at(*it);
            int count = 0; bool quality = true; Causes causes;
            bool allJudged = true;
            for (int child : group) {
                if (!budget.use()) return result;
                const auto& simple = simpleNotes.at(touchSimpleIndices.at(child));
                allJudged &= simple.judged;
                if (!simple.judged || simple.kind == QLatin1String("group_carry")) continue;
                ++count; quality &= simple.highQuality; causes += simple.causes;
            }
            if (allJudged) { it = activeGroups.erase(it); continue; }
            if (count < int(std::ceil(0.51 * group.size()))) { ++it; continue; }
            for (int child : group) {
                if (!budget.use()) return result;
                auto& simple = simpleNotes[touchSimpleIndices.at(child)];
                if (simple.judged) continue;
                simple.judged = simple.completed = true; simple.judgeSecond = now; simple.causes = causes;
                simple.margin = simple.critical - std::abs(now - simple.second);
                simple.highQuality = quality && simple.margin >= -epsilon;
                simple.kind = QStringLiteral("group_carry");
                simple.reason = simple.highQuality ? QStringLiteral("completed") : QStringLiteral("group_outside_target");
            }
            it = activeGroups.erase(it);
        }
        for (auto it = activeTracks.begin(); it != activeTracks.end();) {
            if (!budget.use()) return result;
            auto& track = tracks[*it]; advanceTrack(track, now, on, up, budget);
            if (track.done || track.failed) it = activeTracks.erase(it); else ++it;
        }
        if (result.exhausted) return result;
        previousOn = on; previousSecond = now; havePrevious = true;
    }
    // Complete a finite prefix's hold integral up to the exact requested time.
    if (std::isfinite(horizon) && havePrevious && horizon > previousSecond) for (int rank : activeHolds) {
        if (!budget.use()) return result;
        observeHold(simpleNotes[order.at(rank)], previousSecond, horizon, previousOn, budget);
    }
    for (const auto& simple : simpleNotes) for (int index : simple.owners) {
        if (!budget.use()) return result;
        auto& judgement = result.judgements[index];
        if (simple.head) {
            judgement.headJudged = simple.judged; judgement.headCompleted = simple.completed;
            judgement.headHighQuality = simple.highQuality && simple.completed;
            judgement.headSecond = simple.judgeSecond; judgement.headMargin = simple.margin;
            judgement.headHitDelta = simple.judgeSecond - simple.second; judgement.headCauses = simple.causes;
        } else {
            judgement.judged = simple.judged && (!simple.hold || (havePrevious && previousSecond + epsilon >= simple.end));
            judgement.completed = simple.completed; judgement.highQuality = simple.highQuality && simple.completed;
            judgement.second = simple.judgeSecond; judgement.margin = simple.margin;
            judgement.hitDelta = simple.judgeSecond - simple.second;
            judgement.causes = simple.causes;
            if (simple.hold) appendDistinctCauses(judgement.causes, simple.bodyCauses, budget);
            judgement.handMask = handMask(judgement.causes);
            judgement.inputChannel = simple.channel; judgement.kind = simple.kind;
            judgement.reason = judgement.judged ? simple.reason : QStringLiteral("pending");
            judgement.quality = !judgement.judged ? QStringLiteral("pending") : (judgement.highQuality ? QStringLiteral("critical")
                : (!simple.completed ? QStringLiteral("miss") : QStringLiteral("bad")));
            judgement.releasedSeconds = holdReleasedSeconds(simple.body);
            judgement.heldSeconds = qMax(0.0, simple.body.observedSeconds - judgement.releasedSeconds);
            judgement.rawHeldSeconds = simple.body.rawHeldSeconds;
            judgement.singleHandCompletionMask = simple.singleHandCompletionMask;
            judgement.requiredHoldSeconds = qMax(0.0, simple.bodyLast - simple.bodyFirst);
            judgement.heldRatio = judgement.requiredHoldSeconds > epsilon
                ? qBound(0.0, judgement.heldSeconds / judgement.requiredHoldSeconds, 1.0) : (simple.completed ? 1.0 : 0.0);
            if (simple.hold) {
                judgement.headJudged = simple.judged; judgement.headCompleted = !simple.causes.isEmpty();
                judgement.headHighQuality = simple.headHighQuality; judgement.headSecond = simple.judgeSecond;
                judgement.headMargin = simple.critical - std::abs(simple.judgeSecond - simple.second);
                judgement.headHitDelta = simple.judgeSecond - simple.second; judgement.headCauses = simple.causes;
            }
        }
    }
    for (const auto& track : tracks) {
        if (!budget.use()) return result;
        auto& judgement = result.judgements[track.owner];
        judgement.trackCompleted = track.done; judgement.trackHighQuality = track.highQuality;
        judgement.judged = (track.done || track.failed) && (!judgement.headRequired || judgement.headJudged);
        judgement.completed = track.done && (!judgement.headRequired || judgement.headCompleted);
        judgement.highQuality = track.highQuality && (!judgement.headRequired || judgement.headHighQuality);
        judgement.second = track.judgeSecond; judgement.margin = track.margin;
        judgement.hitDelta = track.judgeSecond - track.critical;
        judgement.causes = track.causes; judgement.handMask = handMask(track.causes);
        if (track.wifi) {
            judgement.wifiAreas = track.hits; judgement.wifiProgress = track.progress; judgement.wifiPadCSecond = track.cSecond;
        } else {
            judgement.areas = track.hits.value(0); judgement.segmentStartAreas = track.segmentStarts; judgement.segmentEndAreas = track.segmentEnds;
            for (int area : track.segmentEnds) {
                const auto hit = track.hits.value(0).value(area);
                judgement.segmentCompletedSeconds.append(hit.hit ? hit.second : std::numeric_limits<double>::quiet_NaN());
            }
        }
        if (!judgement.judged) judgement.reason = QStringLiteral("pending");
        else if (judgement.headRequired && !judgement.headCompleted) judgement.reason = QStringLiteral("star_head_miss");
        else if (judgement.headRequired && !judgement.headHighQuality) judgement.reason = QStringLiteral("star_head_outside_target");
        else if (track.failed) judgement.reason = track.wifi && !track.passedC ? QStringLiteral("wifi_c_not_released") : QStringLiteral("incomplete_judge_sequence");
        else judgement.reason = track.highQuality ? QStringLiteral("completed") : QStringLiteral("slide_outside_target_window");
        judgement.quality = !judgement.judged ? QStringLiteral("pending") : (judgement.highQuality ? QStringLiteral("critical")
            : (judgement.completed ? QStringLiteral("bad") : QStringLiteral("miss")));
        if (judgement.headRequired) judgement.margin = qMin(judgement.margin, judgement.headMargin);
    }
    result.allCompleted = result.meetsTarget = true;
    for (auto& judgement : result.judgements) {
        if (!budget.use()) return result;
        judgement.meetsTarget = judgement.supported && judgement.completed && (!options.requireHighQuality || judgement.highQuality);
        judgement.sideEffect = judgement.judged && !judgement.highQuality && !judgement.causes.isEmpty();
        for (bool head : {false, true}) {
            const auto& causes = head ? judgement.headCauses : judgement.causes;
            QSet<int> orders;
            for (const auto& cause : causes) {
                if (!budget.use()) return result;
                const auto source = sourceOrders.value(sourceKey(cause.tokenStart, cause.branch, cause.kind, cause.handMask));
                for (int order : source) {
                    if (!budget.use()) return result;
                    orders.insert(order); judgement.sideEffect |= order != judgement.parseOrder;
                }
            }
            auto sorted = orders.values(); std::sort(sorted.begin(), sorted.end());
            if (head) judgement.headSourceParseOrders = sorted;
            else judgement.sourceParseOrders = sorted;
        }
        result.allCompleted &= judgement.supported && judgement.completed; result.meetsTarget &= judgement.meetsTarget;
    }
    return result;
}

} // namespace miacode::hand_order
