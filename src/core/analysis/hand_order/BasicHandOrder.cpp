#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/analysis/hand_order/BasicHandOrderWifi.h"
#include "core/analysis/hand_order/HandOrderHoldCandidates.h"
#include "core/chart/SlideReferenceData.h"
#include "core/chart/model/TouchPadAdjacency.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderRhythm.h"
#include <QJsonArray>
#include <QJsonObject>
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/analysis/MuriConfig.h"
#include "common/TaskCancellation.h"
#include <QLineF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <tuple>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
constexpr double referenceRadius = 240.0;
constexpr double handoffOverlap = 2.0 / TouchJudgeProfile::ticksPerSecond;
constexpr double minimumStableSeconds = 0.1;
constexpr double maximumSlideInputOffset = 0.15;
constexpr double maximumDiagnosticSlideInputOffset = 0.2;
struct Primitive {
    ContactAction action;
    int token = -1, lane = 0, lock = -1, basis = -1, layoutBasis = -1;
    bool gestureDefault = false;
    bool configuredHead = false;
    int interactionPrevious = -1;
    int phasePrevious = -1;
    bool phaseChangesHand = true;
    int crossPeer = -1;
    int longitudinalPrevious = -1;
    int holdRelayPrevious = -1;
    int sweepPrevious = -1;
    int reboundPrevious = -1;
    int relaxedReboundPrevious = -1;
    int sameKeyPrevious = -1;
    int alternateSweepPrevious = -1;
    bool flexibleSweepEntry = false;
    bool button = true;
    TimelineNoteMarker marker;
};
struct Track {
    int token = -1, branch = 0;
    int parseOrder = -1, oneStrokePrevious = -1, headPrimitive = -1;
    int headLane = 0;
    double headSecond = 0;
    bool hasHeadStar = false;
    bool sweepSplit = false;
    bool sweepKeep = false;
    bool tapLaneReserved = false;
    int basis = -1, layoutBasis = -1;
    bool gestureDefault = false;
    ContactAction action;
    QVector<HandOrderTrackStop> locks;
    QVector<double> boundaries;
    bool wifi = false, legacyAnnotated = false;
    int sector = -1, peer = -1;
    HandOrderWifiMode wifiMode = HandOrderWifiMode::Legacy;
    QVector<ContactAction> singleActions;
    QVector<HandOrderTrackStop> singleLocks;
    double judgeAvailable = 0;
};
struct Token {
    int start = 0, end = 0, line = 1, col = 1, dependencyUnit = -1;
    QString base, suffix, issue;
    QString layoutFallback;
    bool target = false, partial = false, slide = false, protectedText = false;
    double beginTime = 0, endTime = 0;
    double nominal = 0;
    QVector<int> primitives, tracks, touches;
    QVector<TimelineNoteMarker> markers;
};
using Scheduled = ScheduledAction;
struct Decision {
    enum Kind { Slide, Fixed, Touches } kind = Fixed;
    int index = -1;
    QVector<int> members;
    double second = 0, end = 0;
    int lane = 0;
};
struct SearchConstraint {
    int primitive = -1, track = -1, touch = -1, hand = -1;
    double progress = 0;
};
struct PlayerPathState {
    PlayerEffortState effort;
    QPointF point;
    double free = -std::numeric_limits<double>::infinity();
    int source = -1;
    bool known = false, screen = false;
};
struct State {
    int inputPreference = 0;
    double returnSpeed = chart::handOrderDefaultReturnSpeed;
    PlayerProfile player;
    std::array<PlayerPathState, 2> effortOrigins, effortEnds;
    QHash<int, int> sweepPreviousSources;
    QSet<int> sweepSources;
    QSet<int> holdSources;
    QHash<int, QPair<double, double>> holdBodyRanges;
    QHash<int, TimelineNoteMarker> holdMarkers;
    QSet<int> proposedHeads;
    bool ordinaryOnly = false;
    double scoreFrom = -std::numeric_limits<double>::infinity();
    double focusSecond = 0;
    int focusOrdinal = 0;
    QSet<int> focusSources;
    struct ButtonHand { double free = -1e9; QPointF point; int lane = 0; bool known = false; int ordinal = -1; double pressed = 0; };
    std::array<ButtonHand, 2> buttonHands;
    std::array<ButtonHand, 2> origins;
    QVector<Scheduled> actions;
    QVector<Scheduled> physical;
    QVector<bool> wifiSingle;
    FullReplay fullReplay;
    QVector<int> primitiveHands;
    QVector<QVector<HandOrderTrackStop>> tracks;
    QVector<BasicHandoffEvidence> transferEvidence;
    double cost = 0, peak = 0, focusPeak = 0, risk = 0;
    double playerCost = 0, peakTapRate = 0, peakLoad = 0;
    bool adjustedSlideTiming = false;
    int ergonomicBreaks = 0;
    int interactionBreaks = 0;
    int sameKeyBreaks = 0;
    int longitudinalBreaks = 0;
    TouchReplay replay;
    QVector<int> responsibilityTouches;
};
struct Search {
    bool found = false, truncated = false, exhausted = false, sideEffect = false;
    State state;
    QMap<QString, qint64> rejectionCounts;
    QVector<BasicRejectionEvidence> rejectionSamples;
};
bool spend(qint64& remaining, qint64 units = 1)
{
    task::CancellationScope::checkpoint(); remaining -= units; return remaining >= 0;
}
int fixedHand(HandOrderHand hand) { return hand == HandOrderHand::Left ? 0 : hand == HandOrderHand::Right ? 1 : -1; }
HandOrderHand namedHand(int hand) { return hand == 0 ? HandOrderHand::Left : hand == 1 ? HandOrderHand::Right : HandOrderHand::Unspecified; }
int maskHand(int mask) { return mask == 1 ? 0 : mask == 2 ? 1 : -1; }
QChar glyph(int hand) { return hand == 0 ? QLatin1Char('l') : QLatin1Char('r'); }
int peakTier(double peak) { return int(std::floor(qMin(peak, 100000.0) / 4.0)); }
int switches(const QVector<HandOrderTrackStop>& stops) { return qMax(0, int(stops.size()) - 1); }
bool better(const State& a, const State& b)
{
    if (a.interactionBreaks != b.interactionBreaks) return a.interactionBreaks < b.interactionBreaks;
    if (a.ergonomicBreaks != b.ergonomicBreaks)
        return a.ergonomicBreaks < b.ergonomicBreaks;
    if (a.longitudinalBreaks != b.longitudinalBreaks) return a.longitudinalBreaks < b.longitudinalBreaks;
    if (a.sameKeyBreaks != b.sameKeyBreaks) return a.sameKeyBreaks < b.sameKeyBreaks;
    const auto expressible = [](const State& state) {
        int count = 0;
        for (int id : state.responsibilityTouches) if (id < state.replay.judgements.size()) {
            const auto& judged = state.replay.judgements[id];
            count += judged.judged && judged.highQuality && maskHand(judged.handMask) >= 0 && judged.stableResponsibility;
        }
        return count;
    };
    const int expressibleA = expressible(a), expressibleB = expressible(b);
    if (expressibleA != expressibleB) return expressibleA > expressibleB;
    const int riskA = int(std::ceil(a.risk / 0.025 - epsilon)), riskB = int(std::ceil(b.risk / 0.025 - epsilon));
    if (riskA != riskB) return riskA < riskB;
    // Speed and fatigue are soft player-relative costs. A fixed peak-speed
    // tier must not override the configured ability or technique preference.
    if (std::abs(a.risk - b.risk) > epsilon) return a.risk < b.risk;
    if (std::abs(a.cost - b.cost) > epsilon) return a.cost < b.cost;
    return std::lexicographical_compare(a.primitiveHands.begin(), a.primitiveHands.end(), b.primitiveHands.begin(), b.primitiveHands.end());
}
void scoreHeadContinuity(State& state, const Primitive& head, int hand)
{
    if (head.configuredHead && head.layoutBasis >= 0 && hand != head.layoutBasis) ++state.interactionBreaks;
    const int phase = state.primitiveHands.value(head.phasePrevious, -1);
    if (phase >= 0 && (phase != hand) != head.phaseChangesHand) ++state.interactionBreaks;
    const int sameKey = state.primitiveHands.value(head.sameKeyPrevious, -1);
    if (sameKey >= 0 && sameKey != hand) ++state.sameKeyBreaks;
    if (state.primitiveHands.value(head.longitudinalPrevious, -1) == hand) ++state.longitudinalBreaks;
}
QJsonArray rejectedCauses(const QVector<BasicCause>& causes)
{
    QJsonArray result;
    for (const auto& cause : causes) result.append(QJsonObject{{"token_start", cause.tokenStart}, {"branch", cause.branch},
        {"kind", cause.kind}, {"pad", cause.pad}, {"second", cause.second}, {"progress", cause.progress}, {"hand_mask", cause.handMask}});
    return result;
}
QJsonObject rejectedJudgement(const FullReplayJudgement& full)
{
    QJsonArray areas;
    for (const auto& area : full.areas) areas.append(QJsonObject{{"pad", area.pad}, {"hit", area.hit}, {"skipped", area.skipped},
        {"second", area.second}, {"causes", rejectedCauses(area.causes)}});
    return {{"parse_order", full.parseOrder}, {"marker_index", full.markerIndex}, {"kind", full.kind}, {"quality", full.quality},
        {"reason", full.reason}, {"supported", full.supported}, {"judged", full.judged}, {"completed", full.completed},
        {"high_quality", full.highQuality}, {"meets_target", full.meetsTarget}, {"hand_mask", full.handMask},
        {"second", full.second}, {"margin", full.margin}, {"input_channel", full.inputChannel},
        {"head_required", full.headRequired}, {"head_judged", full.headJudged}, {"head_completed", full.headCompleted},
        {"head_high_quality", full.headHighQuality}, {"head_second", full.headSecond}, {"head_margin", full.headMargin},
        {"track_completed", full.trackCompleted}, {"track_high_quality", full.trackHighQuality},
        {"held_seconds", full.heldSeconds}, {"required_hold_seconds", full.requiredHoldSeconds},
        {"held_ratio", full.heldRatio}, {"released_seconds", full.releasedSeconds},
        {"single_hand_completion_mask", full.singleHandCompletionMask},
        {"causes", rejectedCauses(full.causes)}, {"head_causes", rejectedCauses(full.headCauses)}, {"areas", areas}};
}
void captureRejected(Search& search, const QString& category, const FullReplayJudgement* judgement = nullptr)
{
    ++search.rejectionCounts[category];
    if (judgement) {
        ++search.rejectionCounts[QStringLiteral("replay:%1:%2:%3").arg(judgement->kind, judgement->quality, judgement->reason)];
        if (search.rejectionSamples.size() < 3) search.rejectionSamples.append({category, 0, rejectedJudgement(*judgement)});
    }
}
QPointF lanePoint(int lane)
{
    const double angle = qDegreesToRadians(muri::kPadAngleBaseDegrees + (lane - 1) * muri::kPadAngleStepDegrees);
    return {std::cos(angle), std::sin(angle)};
}
double headPostureCost(const State& state, const Primitive& primitive, int hand)
{
    if (!primitive.button || hand < 0) return 0;
    const auto point = state.inputPreference == 1
        ? touchPadPoint(QStringLiteral("A%1").arg(primitive.lane)) : lanePoint(primitive.lane);
    const double weight = chart::handOrderIsButtonStrike(primitive.marker) && primitive.action.start > state.focusSecond + epsilon
        ? basicOrdinaryAssociationWeight(primitive.action.start - state.focusSecond,
            qMax(0, primitive.token - state.focusOrdinal - 1)) : 1.0;
    // Reaching one key past the centre (L:1/4, R:8/5) is routine. Deep
    // lateral reaches retain the full charge; travel is scored separately.
    const double across = qMax(0.0, hand == 0 ? point.x() : -point.x());
    const double discount = 0.8 * qBound(0.0, (0.65 - across) / 0.2, 1.0);
    const double posture = across * (1.0 - discount);
    return weight * state.player.postureWeight * posture;
}
double headLayoutCost(const Primitive& primitive, int hand)
{
    if (primitive.layoutBasis < 0 || hand == primitive.layoutBasis) return 0;
    return primitive.gestureDefault ? 12.0 : primitive.button && std::abs(lanePoint(primitive.lane).x()) < 0.45 ? 0.15 : 0.75;
}
bool extremeCrossedEach(const Primitive& a, int handA, const Primitive& b, int handB)
{
    if (a.configuredHead && b.configuredHead && a.layoutBasis == handA && b.layoutBasis == handB) return false;
    const int distance = std::abs(a.lane - b.lane);
    if (qMin(distance, 8 - distance) < 2) return false;
    const double xA = lanePoint(a.lane).x(), xB = lanePoint(b.lane).x();
    // A shallow reach at 1/4/5/8 is valid even in an opposite Each.
    return (xA < -0.25 && xB > 0.25 && qMax(-xA, xB) > 0.65 && handA == 1 && handB == 0)
        || (xB < -0.25 && xA > 0.25 && qMax(-xB, xA) > 0.65 && handB == 1 && handA == 0);
}

bool oneStrokeEntryDetour(const State& state, const QVector<Track>& tracks,
                         double second, const QPointF& point, int hand)
{
    for (int id = 0; id < tracks.size(); ++id) {
        const auto& track = tracks.at(id);
        if (track.wifi || !track.hasHeadStar || track.oneStrokePrevious >= 0
            || track.headPrimitive < 0 || state.primitiveHands.value(track.headPrimitive, -1) != hand
            || state.tracks.value(id).isEmpty()
            || fixedHand(state.tracks.at(id).first().hand) != hand
            || second <= track.headSecond + epsilon || second >= track.action.start - epsilon
            || track.action.start - second > 0.25 + epsilon) continue;
        const bool chain = std::any_of(tracks.cbegin(), tracks.cend(), [&](const auto& next) {
                return next.oneStrokePrevious == id;
            });
        if (!chain) continue;
        const auto entry = track.action.points.first();
        const auto outward = point - lanePoint(track.headLane), returning = entry - point;
        const double travel = std::hypot(outward.x(), outward.y())
            * std::hypot(returning.x(), returning.y());
        if (travel > 0.25 && QPointF::dotProduct(outward, returning) < -0.5 * travel)
            return true;
    }
    return false;
}
bool continuingCrossedSweep(const Primitive& a, int handA, const Primitive& b, int handB,
                            const QVector<Primitive>& primitives, const QVector<int>& hands)
{
    if (a.sweepPrevious < 0 || b.sweepPrevious < 0) return false;
    const auto& previousA = primitives.at(a.sweepPrevious);
    const auto& previousB = primitives.at(b.sweepPrevious);
    const auto adjacent = [](int first, int second) {
        return second == first % 8 + 1 || second == (first + 6) % 8 + 1;
    };
    return hands.value(a.sweepPrevious, -1) == handA
        && hands.value(b.sweepPrevious, -1) == handB
        && adjacent(previousA.lane, a.lane) && adjacent(previousB.lane, b.lane);
}
ContactAction slice(const ContactAction& source, double first, double last, int hand)
{
    ContactAction action = source;
    action.start = first; action.end = last; action.hand = hand;
    action.points = {contactPosition(source, first)}; action.times = {first};
    for (int i = 0; i < source.times.size(); ++i) if (source.times.at(i) > first + epsilon && source.times.at(i) < last - epsilon) {
        action.points.append(source.points.at(i)); action.times.append(source.times.at(i));
    }
    action.points.append(contactPosition(source, last)); action.times.append(last);
    return action;
}
QVector<ContactAction> contacts(const State& state)
{
    QVector<ContactAction> result;
    for (const auto& action : state.physical) if (!action.button) result.append(action.action);
    return result;
}

bool possibleSlideConsumer(const Track& track, const TimelineNoteMarker& marker, qint64& work, double inputOffsetLimit)
{
    const bool touch = marker.type == "touch" || marker.type == "touch_hold";
    const bool head = marker.type == "tap" || marker.type == "hold"
        || ((marker.type == "slide" || marker.type == "wifi") && marker.hasHeadStar);
    if ((!touch && !head) || marker.second < track.headSecond - epsilon) return false;
    const double available = touch ? FullReplayProfile::touchAvailableSeconds : FullReplayProfile::tapAvailableSeconds;
    const double offset = track.wifi ? 0.0 : inputOffsetLimit;
    const double earliest = marker.second - available - offset;
    const double latest = marker.second + available + offset;
    if (track.action.end < earliest || track.action.start > latest) return false;
    const QString pad = touch ? marker.touchPad : QStringLiteral("A%1").arg(marker.lane);
    const QPointF centre = touchPadPoint(pad);
    const double radius = track.action.radius + muri::padRadius(pad) / referenceRadius;
    for (int i = 1; i < track.action.times.size(); ++i) {
        if (!spend(work)) return false;
        const double begin = qMax(earliest, track.action.times.at(i - 1));
        const double end = qMin(latest, track.action.times.at(i));
        if (end < begin) continue;
        const QPointF first = contactPosition(track.action, begin), last = contactPosition(track.action, end);
        const QPointF direction = last - first;
        const double squared = QPointF::dotProduct(direction, direction);
        const double progress = squared > epsilon ? qBound(0.0, QPointF::dotProduct(centre - first, direction) / squared, 1.0) : 0.0;
        if (QLineF(centre, first + progress * direction).length() <= radius + epsilon) return true;
    }
    return false;
}

void setPlayerActionKinds(State& state, const QVector<Primitive>& primitives,
                          const QVector<TouchTarget>& touches, int sweepThreshold)
{
    const double maximumSweepGap = chart::handOrderSweepGapSeconds(sweepThreshold);
    QHash<int, double> sourceSeconds;
    for (const auto& primitive : primitives) {
        sourceSeconds.insert(primitive.action.tokenStart, primitive.action.start);
        if (primitive.sweepPrevious >= 0)
            state.sweepPreviousSources.insert(primitive.action.tokenStart,
                primitives.at(primitive.sweepPrevious).action.tokenStart);
        if (primitive.marker.type == "hold" || primitive.marker.type == "touch_hold") {
            state.holdSources.insert(primitive.action.tokenStart);
            state.holdMarkers.insert(primitive.action.tokenStart, primitive.marker);
            if (holdHasRequiredBody(primitive.marker)) {
                const double headIgnore = primitive.marker.type == QStringLiteral("touch_hold")
                    ? FullReplayProfile::touchHoldHeadIgnoreSeconds : FullReplayProfile::holdHeadIgnoreSeconds;
                state.holdBodyRanges.insert(primitive.action.tokenStart,
                    {primitive.marker.second + headIgnore,
                        primitive.marker.endSecond - FullReplayProfile::holdTailIgnoreSeconds});
            }
        }
    }
    for (const auto& touch : touches) {
        sourceSeconds.insert(touch.tokenStart, touch.second);
        if (touch.sweepPrevious >= 0)
            state.sweepPreviousSources.insert(touch.tokenStart, touches.at(touch.sweepPrevious).tokenStart);
    }
    // Physical sweep effort follows elapsed time, not the spelling of the
    // rhythmic grid: {48} with three commas is still a regular 16th-note sweep.
    // Rim buttons crossed by one continuous wrist stroke are movement work,
    // not repeated lifts at the configured single-key tapping rate.
    QVector<int> taps;
    for (int i = 0; i < primitives.size(); ++i)
        if (primitives.at(i).marker.type == "tap") taps.append(i);
    std::stable_sort(taps.begin(), taps.end(), [&](int a, int b) {
        return primitives.at(a).action.start < primitives.at(b).action.start;
    });
    const auto direction = [&](int a, int b) {
        const int step = (primitives.at(b).lane - primitives.at(a).lane + 8) % 8;
        return step == 1 ? 1 : step == 7 ? -1 : 0;
    };
    for (int first = 0; first + 3 < taps.size();) {
        const int step = direction(taps.at(first), taps.at(first + 1));
        const double interval = primitives.at(taps.at(first + 1)).action.start
            - primitives.at(taps.at(first)).action.start;
        int last = first + 1;
        while (step && interval > epsilon && interval <= maximumSweepGap + epsilon
            && last < taps.size()) {
            const auto& a = primitives.at(taps.at(last - 1));
            const auto& b = primitives.at(taps.at(last));
            const double gap = b.action.start - a.action.start;
            if (direction(taps.at(last - 1), taps.at(last)) != step
                || gap < interval * 0.9 || gap > interval * 1.1
                || a.interactionPrevious >= 0 || b.interactionPrevious >= 0) break;
            ++last;
        }
        if (last - first >= 4) {
            for (int i = first + 1; i < last; ++i)
                state.sweepPreviousSources.insert(primitives.at(taps.at(i)).action.tokenStart,
                    primitives.at(taps.at(i - 1)).action.tokenStart);
            first = last;
        } else ++first;
    }
    for (auto it = state.sweepPreviousSources.cbegin(); it != state.sweepPreviousSources.cend(); ++it) {
        const double gap = sourceSeconds.value(it.key(), -1e10) - sourceSeconds.value(it.value(), 1e10);
        // Relaxed eighth-note gestures keep a hand but are not continuous
        // glissandi for the stricter moving-forearm topology check.
        if (gap <= epsilon || gap > qMax(0.13, maximumSweepGap) + epsilon) continue;
        state.sweepSources.insert(it.key());
        state.sweepSources.insert(it.value());
    }
}

// Recompute from a fixed window-entry history, never from the last candidate's
// tail. Logical screen paths already share one resolved physical centre.
bool scorePlayerEffort(State& state, qint64& work,
                       double through = std::numeric_limits<double>::infinity())
{
    state.playerCost = state.peakTapRate = state.peakLoad = 0;
    state.effortEnds = state.effortOrigins;
    for (int hand = 0; hand < 2; ++hand) {
        auto& path = state.effortEnds[hand];
        QVector<const Scheduled*> ordered;
        QVector<double> times;
        if (!spend(work, state.physical.size() + 1)) return false;
        for (const auto& action : state.physical) if (action.action.hand == hand) {
            const double begin = qMax(action.action.start, state.scoreFrom);
            const double end = qMin(action.action.end, through);
            if (end <= begin + epsilon) continue;
            if (!spend(work, action.action.times.size() + 2)) return false;
            ordered.append(&action); times.append(begin); times.append(end);
            for (double time : action.action.times) if (time > begin && time < end) times.append(time);
            const auto body = state.holdBodyRanges.constFind(action.action.tokenStart);
            if (body != state.holdBodyRanges.cend()) for (double at : {body->first, body->second})
                if (at > begin && at < end) times.append(at);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->action.start < b->action.start;
        });
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) {
            return std::abs(a - b) < epsilon;
        }), times.end());
        QVector<const Scheduled*> active;
        int next = 0;
        const auto add = [&](const PlayerEffortDelta& delta, bool charged) {
            if (!delta.valid) return false;
            if (charged) {
                state.playerCost += delta.cost;
                state.peakTapRate = qMax(state.peakTapRate, delta.peakTapRate);
                state.peakLoad = qMax(state.peakLoad, delta.peakLoad);
            }
            return true;
        };
        for (int i = 0; i + 1 < times.size(); ++i) {
            if (!spend(work, active.size() + 1)) return false;
            const double first = times.at(i), last = times.at(i + 1);
            while (next < ordered.size() && ordered.at(next)->action.start <= first + epsilon)
                active.append(ordered.at(next++));
            active.erase(std::remove_if(active.begin(), active.end(), [&](const auto* action) {
                return action->action.end <= first + epsilon;
            }), active.end());
            if (active.isEmpty()) continue;
            const auto* selected = active.first();
            const auto from = contactPosition(selected->action, first);
            const auto to = contactPosition(selected->action, last);
            bool charged = false, screen = false, sustained = false, requiredHold = false, committedHold = false;
            bool buttonDown = false, screenDown = false, chargedDown = false;
            for (const auto* action : active) {
                charged |= !action->committed;
                screen |= !action->button;
                sustained |= state.holdSources.contains(action->action.tokenStart);
                const auto body = state.holdBodyRanges.constFind(action->action.tokenStart);
                const bool holding = body != state.holdBodyRanges.cend()
                    && (first + last) / 2 >= body->first && (first + last) / 2 <= body->second;
                if (action->committed) committedHold |= holding;
                else requiredHold |= holding;
                if (std::abs(action->action.start - first) < epsilon && !action->motion) {
                    buttonDown |= action->button;
                    screenDown |= !action->button;
                    chargedDown |= !action->committed;
                }
            }
            const bool joinedScreen = path.known && path.screen && screen && first <= path.free + epsilon;
            const bool joinedSweep = path.known && path.screen == screen
                && state.sweepPreviousSources.value(selected->action.tokenStart, -2) == path.source
                && first - path.free <= 0.35;
            if (!path.known || first > path.free + epsilon) {
                const QPointF rested = path.known
                    ? chart::handOrderRestedPoint(path.point, path.free, first, hand, state.returnSpeed)
                    : chart::handOrderNaturalRestPoint(hand);
                const double begin = path.known ? path.free : first - 1.0;
                // Idle recovery is lazy. The approach ending at this window's
                // first note has not already been included in committed history.
                if (!add(playerMovement(path.effort, begin, first,
                        QLineF(rested, from).length(), state.player), charged)) return false;
            }
            if (((buttonDown && !joinedSweep) || (screenDown && !joinedScreen && !joinedSweep))
                && first > path.effort.lastStrike + epsilon)
                if (!add(playerStrike(path.effort, first, state.player), chargedDown)) return false;
            if (!add(playerMovement(path.effort, first, last,
                    QLineF(from, to).length(), state.player, sustained), charged)) return false;
            if (charged && requiredHold && !committedHold) {
                // A shallow strike is brief; maintaining the same contralateral
                // pose through a required Hold body has time-dependent effort.
                const double x0 = hand == 0 ? from.x() : -from.x();
                const double x1 = hand == 0 ? to.x() : -to.x();
                double area = 0.0;
                if (x0 >= 0.0 && x1 >= 0.0) area = (x0 + x1) / 2;
                else if (qMax(x0, x1) > 0.0)
                    area = qMax(x0, x1) * qMax(x0, x1) / (2 * std::abs(x1 - x0));
                state.playerCost += state.player.postureWeight * area * (last - first);
            }
            path.point = to; path.free = last; path.known = true; path.screen = screen;
            path.source = selected->action.tokenStart;
        }
    }
    return true;
}
// Score and replay one realizable centre path per physical hand.
bool score(State& state, const QVector<Track>& tracks, const QVector<Primitive>& primitives,
           const QVector<TouchTarget>& touches,
           qint64& work, bool* sideEffect = nullptr)
{
    Q_UNUSED(sideEffect);
    state.cost = state.peak = state.focusPeak = 0;
    state.adjustedSlideTiming = false;
    state.ergonomicBreaks = state.interactionBreaks = state.sameKeyBreaks = state.longitudinalBreaks = 0;
    for (const auto& held : state.actions) {
        if (!state.holdMarkers.contains(held.action.tokenStart)) continue;
        auto hold = state.holdMarkers.value(held.action.tokenStart);
        hold.handOrderHead = namedHand(held.action.hand);
        if (held.action.end < hold.endSecond - epsilon) return false;
        for (int id : state.proposedHeads) {
            if (!spend(work)) return false;
            if (holdLocksHead(hold, primitives.at(id).marker, namedHand(state.primitiveHands.value(id, -1)))) return false;
        }
        const auto anchor = contactPosition(held.action, hold.second);
        for (const auto& other : state.actions) {
            if (!spend(work)) return false;
            if (other.action.tokenStart == held.action.tokenStart || other.action.hand != held.action.hand) continue;
            const double first = qMax(hold.second, other.action.start), last = qMin(hold.endSecond, other.action.end);
            if (first > last + epsilon) continue;
            if (QLineF(anchor, contactPosition(other.action, first)).length() > 1e-6
                || QLineF(anchor, contactPosition(other.action, last)).length() > 1e-6) return false;
            for (double second : other.action.times) if (second > first && second < last
                && QLineF(anchor, contactPosition(other.action, second)).length() > 1e-6) return false;
        }
    }
    if (!resolveHandOrderActions(state.actions, &state.physical, work)) return false;
    for (const auto& action : state.actions) {
        if (action.committed) continue;
        if (state.inputPreference == 1 && action.button) state.cost += 0.12;
        if (state.inputPreference == 0 && action.action.kind == "screen_button") state.cost += 0.12;
    }
    for (int id = 0; id < primitives.size(); ++id) {
        const auto& primitive = primitives.at(id);
        const int assigned = state.primitiveHands.value(id, -1);
        if (assigned < 0) continue;
        if (state.proposedHeads.contains(id)) state.cost += headPostureCost(state, primitive, assigned);
        if (primitive.marker.type == QStringLiteral("tap")
            && oneStrokeEntryDetour(state, tracks, primitive.action.start,
                primitive.action.points.first(), assigned)) {
            ++state.ergonomicBreaks;
            state.cost += 12.0;
        }
        state.cost += headLayoutCost(primitive, assigned);
        scoreHeadContinuity(state, primitive, assigned);
        if (primitive.crossPeer >= 0 && state.primitiveHands.value(primitive.crossPeer, -1) >= 0
            && extremeCrossedEach(primitive, assigned, primitives.at(primitive.crossPeer),
                state.primitiveHands.at(primitive.crossPeer))
            && !continuingCrossedSweep(primitive, assigned, primitives.at(primitive.crossPeer),
                state.primitiveHands.at(primitive.crossPeer), primitives, state.primitiveHands)) return false;
        if (primitive.holdRelayPrevious >= 0
            && assigned == state.primitiveHands.value(primitive.holdRelayPrevious, -1)) return false;
        for (int previous : {primitive.interactionPrevious, primitive.longitudinalPrevious})
            if (previous >= 0 && assigned == state.primitiveHands.value(previous, -1))
                state.cost += 3.0;
        if (primitive.sweepPrevious >= 0
            && state.primitiveHands.value(primitive.sweepPrevious, -1) >= 0
            && assigned != state.primitiveHands.value(primitive.sweepPrevious)) {
            state.cost += primitive.flexibleSweepEntry ? 0.35 : 12.0;
            if (!primitive.flexibleSweepEntry) ++state.ergonomicBreaks;
        }
        const int rebound = primitive.reboundPrevious >= 0 ? primitive.reboundPrevious : primitive.relaxedReboundPrevious;
        if (rebound >= 0 && state.primitiveHands.value(rebound, -1) >= 0
            && assigned != state.primitiveHands.value(rebound)) {
            state.cost += primitive.reboundPrevious >= 0 ? 12.0 : 3.0;
            if (primitive.reboundPrevious >= 0) ++state.ergonomicBreaks;
        }
        if (primitive.alternateSweepPrevious >= 0
            && state.primitiveHands.value(primitive.alternateSweepPrevious, -1) >= 0
            && assigned == state.primitiveHands.value(primitive.alternateSweepPrevious)) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
    }
    QHash<int, int> directTouchHands;
    for (const auto& action : state.actions)
        if (action.action.kind == QStringLiteral("touch") && action.touchParseOrder >= 0)
            directTouchHands.insert(action.touchParseOrder, action.action.hand);
    for (int index = 0; index < touches.size(); ++index) {
        const auto& touch = touches.at(index);
        const int directHand = directTouchHands.value(touch.parseOrder, -1);
        if (!touch.hold && directHand >= 0 && oneStrokeEntryDetour(state, tracks,
                touch.second, touchPadPoint(touch.pad), directHand)) {
            ++state.ergonomicBreaks;
            state.cost += 12.0;
        }
        if (touch.layoutBasis >= 0 && directHand >= 0 && directHand != touch.layoutBasis)
            state.cost += touch.gestureDefault ? 12.0 : 0.75;
        if (touch.sweepPrevious < 0) continue;
        const int currentHand = directTouchHands.value(touch.parseOrder, -1);
        const int previousHand = directTouchHands.value(touches.at(touch.sweepPrevious).parseOrder, -1);
        if (currentHand >= 0 && previousHand >= 0 && currentHand != previousHand) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
    }
    for (int first = 0; first < tracks.size(); ++first) {
        const auto& a = tracks.at(first);
        if (a.oneStrokePrevious >= 0
            && !state.tracks.value(first).isEmpty()
            && !state.tracks.value(a.oneStrokePrevious).isEmpty()
            && state.tracks.at(first).constFirst().hand
                != state.tracks.at(a.oneStrokePrevious).constLast().hand) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
        if (a.layoutBasis >= 0 && !state.tracks.value(first).isEmpty()
            && fixedHand(state.tracks.at(first).constFirst().hand) != a.layoutBasis)
            state.cost += a.gestureDefault ? 12.0 : 0.75;
        if (a.sweepSplit && a.headPrimitive >= 0 && !state.tracks.value(first).isEmpty()
            && state.primitiveHands.value(a.headPrimitive, -1) >= 0
            && fixedHand(state.tracks.at(first).constFirst().hand) == state.primitiveHands.at(a.headPrimitive)) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
        if (a.sweepKeep && a.headPrimitive >= 0 && !state.tracks.value(first).isEmpty()
            && state.primitiveHands.value(a.headPrimitive, -1) >= 0
            && fixedHand(state.tracks.at(first).constFirst().hand) != state.primitiveHands.at(a.headPrimitive)) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
        if (a.tapLaneReserved && a.layoutBasis >= 0 && !state.tracks.value(first).isEmpty()
            && fixedHand(state.tracks.at(first).constFirst().hand) != a.layoutBasis) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
        if (a.wifi) {
            if (a.sector == 0 && a.hasHeadStar && !a.legacyAnnotated
                && a.wifiMode != HandOrderWifiMode::Single
                && state.wifiSingle.value(first, false)) {
                state.cost += 12.0;
                ++state.ergonomicBreaks;
            }
            continue;
        }
        if (!a.hasHeadStar || state.tracks.value(first).isEmpty()) continue;
        for (int second = first + 1; second < tracks.size(); ++second) {
            const auto& b = tracks.at(second);
            if (b.wifi || !b.hasHeadStar || state.tracks.value(second).isEmpty()
                || a.headLane != b.headLane
                || std::abs(a.headSecond - b.headSecond) > epsilon
                || std::abs(a.action.start - b.action.start) > epsilon) continue;
            const double end = qMin(a.action.end, b.action.end);
            if (end - a.action.start < 0.05) continue;
            bool divergent = false;
            for (double fraction : {0.25, 0.5, 0.75}) {
                const double at = a.action.start + (end - a.action.start) * fraction;
                divergent |= QLineF(contactPosition(a.action, at),
                    contactPosition(b.action, at)).length() > 0.32;
            }
            if (divergent && state.tracks.at(first).constFirst().hand
                == state.tracks.at(second).constFirst().hand) {
                state.cost += 12.0;
                ++state.ergonomicBreaks;
            }
            if (divergent && state.tracks.at(first).constFirst().hand
                != state.tracks.at(second).constFirst().hand) {
                double separation = 0;
                for (double fraction : {0.2, 0.4, 0.6, 0.8}) {
                    const double at = a.action.start + (end - a.action.start) * fraction;
                    separation += contactPosition(a.action, at).x() - contactPosition(b.action, at).x();
                }
                const bool aLeft = state.tracks.at(first).constFirst().hand == HandOrderHand::Left;
                if ((aLeft ? separation : -separation) > 0.4) {
                    state.cost += 12.0;
                    ++state.ergonomicBreaks;
                }
            }
        }
    }
    const auto crossings = physicalHandCrossingEffort(state.physical, state.holdSources, state.sweepSources, work);
    if (!crossings.valid) return false;
    state.ergonomicBreaks += crossings.flowingConflicts;
    state.cost += 12.0 * crossings.flowingConflicts + 1.5 * crossings.stationarySeconds;
    for (int hand = 0; hand < 2; ++hand) {
        QVector<const Scheduled*> active;
        for (const auto& action : state.physical) if (action.action.hand == hand) active.append(&action);
        std::stable_sort(active.begin(), active.end(), [](const auto* a, const auto* b) { return a->action.start < b->action.start; });
        QVector<double> times;
        for (const auto* action : active) { times.append(action->action.start); times.append(action->action.end); times += action->action.times; }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < epsilon; }), times.end());
        const auto& origin = state.origins[hand];
        bool known = origin.known, wasActive = false;
        int previousOrdinal = origin.ordinal;
        double pressed = origin.pressed;
        double free = origin.free; QPointF position = known ? origin.point : chart::handOrderNaturalRestPoint(hand);
        for (int i = 1; i < times.size(); ++i) {
            if (!spend(work, active.size() + 1)) return false;
            const double rawFirst = times.at(i - 1), last = times.at(i);
            if (last <= state.scoreFrom + epsilon) continue;
            const double first = qMax(rawFirst, state.scoreFrom), middle = (first + last) / 2;
            const Scheduled* selected = nullptr;
            bool focused = false;
            for (const auto* action : active) if (middle >= action->action.start && middle < action->action.end)
                focused |= state.focusSources.contains(action->action.tokenStart);
            for (const auto* action : active) if (middle >= action->action.start && middle < action->action.end) { selected = action; break; }
            if (!selected) { wasActive = false; continue; }
            const auto from = contactPosition(selected->action, first), to = contactPosition(selected->action, last);
            if (rawFirst <= state.scoreFrom + epsilon && selected->committed && selected->action.start <= first + epsilon) {
                // Historical contacts remain in replay to preserve DOWN/head
                // consumption, but the moving hand enters scoring at its actual
                // horizon position, not at an invented new press or rest pose.
                position = from; free = first; known = wasActive = true;
                pressed = selected->action.start; previousOrdinal = selected->ordinal;
            }
            if (!wasActive) {
                const QPointF rested = known ? chart::handOrderRestedPoint(position, free, first,
                    hand, state.returnSpeed) : position;
                const double distance = QLineF(rested, from).length(), gap = known ? first - free : 1.0;
                if (known && gap <= epsilon && distance > 1e-6) return false;
                const double prep = gap > epsilon ? gap : 1.0;
                const double weight = selected->ordinary && known ? basicOrdinaryAssociationWeight(first - pressed,
                    qMax(0, selected->ordinal - previousOrdinal - 1)) : 1.0;
                const double velocityWeight = selected->ordinary ? 0.01 : 0.1;
                if (!selected->committed) state.cost += selected->softWeight * weight * (distance + velocityWeight * distance * distance / prep);
                state.peak = qMax(state.peak, distance / prep);
                if (focused) state.focusPeak = qMax(state.focusPeak, distance / prep);
                if (!selected->committed && !selected->motion && selected->lane == 0) state.cost += 0.16 * qMax(0.0, hand == 0 ? from.x() : -from.x());
                pressed = first; previousOrdinal = selected->ordinal;
            } else if (QLineF(position, from).length() > 1e-6) return false;
            const double dt = last - first, distance = QLineF(from, to).length(), speed = distance / dt;
            if (!selected->committed) state.cost += distance + 0.1 * speed * speed * dt;
            if (selected->motion && !selected->committed) state.cost += 0.16 * qMax(0.0, hand == 0 ? (from.x() + to.x()) / 2 : -(from.x() + to.x()) / 2) * dt;
            state.peak = qMax(state.peak, speed);
            if (focused) state.focusPeak = qMax(state.focusPeak, speed);
            position = to; free = last; known = wasActive = true;
        }
    }
    for (int i = 0; i < state.actions.size(); ++i) if (state.actions.at(i).button)
        for (int j = i + 1; j < state.actions.size(); ++j) if (state.actions.at(j).button && state.actions.at(j).lane == state.actions.at(i).lane) {
            if (!spend(work)) return false;
            const auto& a = state.actions.at(i).action; const auto& b = state.actions.at(j).action;
            if (qMax(a.start, b.start) < qMin(a.end, b.end) - epsilon) return false;
        }
    QVector<PhysicalHandTransfer> transfers;
    for (int id = 0; id < tracks.size(); ++id) {
        const auto& stops = state.tracks.at(id); const auto& track = tracks.at(id);
        bool proposed = false;
        for (const auto& action : state.actions) proposed |= !action.committed && action.motion && action.action.tokenStart == track.action.tokenStart;
        double timeOffset = 0.0;
        for (const auto& physical : state.physical)
            if (physical.motion && physical.action.tokenStart == track.action.tokenStart
                && physical.action.branch == track.branch) {
                timeOffset = physical.action.motionStart - track.action.motionStart;
                break;
            }
        state.adjustedSlideTiming |= std::abs(timeOffset) > epsilon;
        if (!proposed) continue;
        state.cost += 0.4 * std::abs(timeOffset);
        for (int i = 1; i < stops.size(); ++i) {
            const double nominal = track.action.start + stops.at(i).progress * (track.action.end - track.action.start);
            const double second = nominal + timeOffset;
            const auto proposal = contactPosition(track.action, nominal); auto point = proposal; double nearest = std::numeric_limits<double>::infinity();
            for (const auto& physical : state.physical) if (physical.motion && physical.action.tokenStart == track.action.tokenStart
                && physical.action.branch == track.branch && second >= physical.action.start - epsilon && second <= physical.action.end + epsilon) {
                const auto actual = contactPosition(physical.action, second);
                const double distance = QLineF(proposal, actual).length();
                if (distance < nearest) { nearest = distance; point = actual; }
            }
            transfers.append({second, point, fixedHand(stops.at(i - 1).hand), fixedHand(stops.at(i).hand), i});
        }
    }
    state.cost += physicalHandTransferCharge(transfers) * (1.0 + 0.35 * state.player.techniquePreference);
    if (!scorePlayerEffort(state, work)) return false;
    state.cost += state.playerCost;
    return true;
}
void addTrack(State& state, const Track& track, const QVector<HandOrderTrackStop>& stops, double timeOffset = 0.0)
{
    auto action = track.action;
    action.start += timeOffset; action.end += timeOffset;
    action.motionStart += timeOffset; action.motionEnd += timeOffset;
    for (double& second : action.times) second += timeOffset;
    for (int i = 0; i < stops.size(); ++i) {
        const double duration = action.end - action.start;
        const double first = action.start + stops.at(i).progress * duration;
        const double last = i + 1 < stops.size() ? action.start + stops.at(i + 1).progress * duration : action.end;
        const double occupiedFirst = i > 0 ? qMax(action.start, first - handoffOverlap / 2) : first;
        const double occupiedLast = i + 1 < stops.size() ? qMin(action.end, last + handoffOverlap / 2) : last;
        state.actions.append({slice(action, occupiedFirst, occupiedLast, fixedHand(stops.at(i).hand)), false, true, 0});
    }
}
QVector<QVector<HandOrderTrackStop>> trackOptions(const Track& track, int cap, qint64& work)
{
    QVector<QVector<HandOrderTrackStop>> options;
    const double duration = track.action.end - track.action.start;
    if (!track.locks.isEmpty()) {
        std::function<void(int, QVector<HandOrderTrackStop>)> visit = [&](int index, QVector<HandOrderTrackStop> stops) {
            if (!spend(work)) return;
            if (index == track.locks.size()) {
                if (options.size() >= 512) { work = -1; return; }
                options.append(stops); return;
            }
            const auto& lock = track.locks.at(index);
            for (int hand = 0; hand < 2; ++hand) if (fixedHand(lock.hand) < 0 || fixedHand(lock.hand) == hand) {
                auto candidate = stops;
                if (candidate.isEmpty() || candidate.last().hand != namedHand(hand)) candidate.append({lock.progress, namedHand(hand)});
                visit(index + 1, candidate);
            }
        };
        visit(0, {}); return options;
    }
    std::function<void(int, QVector<HandOrderTrackStop>)> visit = [&](int next, QVector<HandOrderTrackStop> stops) {
        if (!spend(work)) return; options.append(stops);
        if (switches(stops) >= cap) return;
        for (int i = next; i < track.boundaries.size(); ++i) {
            const double progress = track.boundaries.at(i);
            if ((progress - stops.last().progress) * duration < minimumStableSeconds - epsilon
                || (1.0 - progress) * duration < minimumStableSeconds - epsilon) continue;
            auto candidate = stops; candidate.append({progress, namedHand(1 - fixedHand(stops.last().hand))}); visit(i + 1, candidate);
        }
    };
    const int basis = track.basis >= 0 ? track.basis : 0;
    visit(0, {{0, namedHand(basis)}}); visit(0, {{0, namedHand(1 - basis)}}); return options;
}
bool locksSatisfied(const QVector<TouchTarget>& targets, const TouchReplay& replay, double through, bool requireAll)
{
    if (replay.exhausted) return false;
    for (int i = 0; i < targets.size(); ++i) {
        const auto& target = targets.at(i); const auto& judgement = replay.judgements.at(i);
        if (target.second + TouchJudgeProfile::availableSeconds > through + epsilon && !requireAll) continue;
        if (!judgement.judged || !judgement.highQuality) return false;
        if (!target.hold && target.lock >= 0 && judgement.handMask != (1 << target.lock)) return false;
        // Hold head/body responsibility is checked by the complete replay's
        // per-hand sufficient mask. A source ordering or redundant slide
        // contributor cannot decide the sustained owner's identity here.

    }
    return true;
}
Search search(const QVector<Decision>& decisions, const QVector<Primitive>& primitives, const QVector<Track>& tracks,
    const QVector<TouchTarget>& originalTouches, const QVector<TimelineNoteMarker>& markers, const BasicParameters& p, const QVector<int>& caps, qint64& work,
    const SearchConstraint& constraint = {}, const State* seed = nullptr, int breadth = 2,
    const HandOrderProgressCallback& progressCallback = {})
{
    if (progressCallback) progressCallback({HandOrderProgress::Decisions, 0, int(decisions.size())});
    Search result; State initial = seed ? *seed : State{};
    initial.fullReplay = {}; initial.replay = {}; initial.risk = 0; initial.inputPreference = p.inputPreference;
    initial.returnSpeed = p.returnSpeed;
    initial.player = p.player;
    if (!seed) setPlayerActionKinds(initial, primitives, originalTouches, p.sweepThreshold);
    auto touches = originalTouches;
    initial.responsibilityTouches.clear();
    for (int id = 0; id < touches.size(); ++id) if (!touches[id].hold) initial.responsibilityTouches.append(id);
    if (constraint.touch >= 0) {
        if (constraint.touch >= touches.size() || (touches.at(constraint.touch).lock >= 0
            && touches.at(constraint.touch).lock != constraint.hand)) return result;
        touches[constraint.touch].lock = constraint.hand;
    }
    const auto trackMatches = [&](const QVector<HandOrderTrackStop>& stops, int id) {
        if (constraint.track != id) return true;
        int hand = -1;
        for (const auto& stop : stops) if (stop.progress <= constraint.progress + epsilon) hand = fixedHand(stop.hand);
        return hand == constraint.hand;
    };
    const auto scored = [&](State& candidate) {
        const bool valid = score(candidate, tracks, primitives, touches, work, &result.sideEffect);
        if (!valid && p.captureRejectedCandidates)
            captureRejected(result, work < 0 ? QStringLiteral("score_work_exhausted") : QStringLiteral("physical_score_rejected"));
        return valid;
    };
    initial.buttonHands = initial.origins;
    for (int hand = 0; hand < 2; ++hand) if (!initial.buttonHands[hand].known)
        initial.buttonHands[hand].point = chart::handOrderNaturalRestPoint(hand);
    for (const auto& action : initial.actions) {
        auto& hand = initial.buttonHands[action.action.hand];
        if (action.action.end >= hand.free) hand = {action.action.end, contactPosition(action.action, action.action.end),
            action.lane, true, action.ordinal, action.action.start};
    }
    initial.ordinaryOnly = touches.isEmpty() && std::all_of(decisions.begin(), decisions.end(), [&](const Decision& d) {
        return d.kind == Decision::Fixed && primitives.at(d.index).marker.type == "tap";
    }) && std::all_of(initial.actions.begin(), initial.actions.end(), [](const Scheduled& action) { return action.ordinary; });
    bool buttonOnly = p.inputPreference != 2 && initial.ordinaryOnly && breadth == 0;
    if (buttonOnly) for (int i = 0; i + 2 < decisions.size(); ++i)
        if (std::abs(decisions.at(i + 2).second - decisions.at(i).second) < epsilon) buttonOnly = false;
    if (buttonOnly) for (int i = 0; i < decisions.size(); ++i) for (int j = i + 1; j < decisions.size(); ++j) {
        const auto& a = primitives.at(decisions.at(i).index); const auto& b = primitives.at(decisions.at(j).index);
        if (a.lane == b.lane && qMax(a.action.start, b.action.start) < qMin(a.action.end, b.action.end) - epsilon)
            buttonOnly = false;
    }
    if (buttonOnly) for (const auto& scheduled : initial.actions) for (const auto& d : decisions) {
        const auto& primitive = primitives.at(d.index);
        if (scheduled.lane == primitive.lane && qMax(scheduled.action.start, primitive.action.start)
            < qMin(scheduled.action.end, primitive.action.end) - epsilon) buttonOnly = false;
    }
    if (!seed) {
        initial.primitiveHands.fill(-1, primitives.size()); initial.tracks.resize(tracks.size()); initial.wifiSingle.fill(false, tracks.size());
    }
    if (buttonOnly) {
        initial.cost = initial.peak = initial.focusPeak = 0;
        initial.playerCost = initial.peakTapRate = initial.peakLoad = 0;
        initial.ergonomicBreaks = initial.interactionBreaks = initial.sameKeyBreaks = initial.longitudinalBreaks = 0;
    }
    else if (!scored(initial)) { result.exhausted = work < 0; return result; }
    QVector<State> beam{initial};
    // Locked sustained actions are known constraints on the whole dependency
    // unit. Rank partial paths against their real button/screen templates
    // before pruning, so cheap constant-hand prefixes cannot erase a required
    // later transfer. This is a bounded forecast, never an autoplay input.
    const auto rankWithFutureHolds = [&](State& state) {
        if (buttonOnly) return true;
        QVector<State> forecasts{state};
        for (const auto& decision : decisions) {
            if (!spend(work)) return false;
            if (decision.kind != Decision::Fixed || state.primitiveHands.at(decision.index) >= 0) continue;
            const auto& primitive = primitives.at(decision.index);
            if (primitive.lock < 0 || (primitive.marker.type != "hold" && primitive.marker.type != "touch_hold")) continue;
            QVector<Scheduled> templates;
            auto action = primitive.action; action.hand = primitive.lock;
            templates.append({action, primitive.button, false, primitive.lane});
            if (primitive.button) {
                action.kind = QStringLiteral("screen_button");
                action.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                templates.append({action, false, false, primitive.lane});
            }
            if (breadth == 0 && templates.size() > 1) templates = {templates.at(p.inputPreference == 1 ? 1 : 0)};
            QVector<State> next;
            for (const auto& forecast : forecasts) for (const auto& physical : templates) {
                State candidate = forecast; candidate.actions.append(physical);
                candidate.primitiveHands[decision.index] = primitive.lock;
                candidate.proposedHeads.insert(decision.index);
                if (scored(candidate)) next.append(std::move(candidate));
            }
            if (next.isEmpty()) return false;
            std::stable_sort(next.begin(), next.end(), better);
            if (next.size() > 4) { next.resize(4); result.truncated = true; }
            forecasts = std::move(next);
        }
        if (!forecasts.isEmpty()) {
            state.cost = forecasts.first().cost;
            state.peak = forecasts.first().peak;
            state.ergonomicBreaks = forecasts.first().ergonomicBreaks;
            state.interactionBreaks = forecasts.first().interactionBreaks;
            state.sameKeyBreaks = forecasts.first().sameKeyBreaks;
            state.longitudinalBreaks = forecasts.first().longitudinalBreaks;
            state.playerCost = forecasts.first().playerCost;
            state.peakTapRate = forecasts.first().peakTapRate;
            state.peakLoad = forecasts.first().peakLoad;
        }
        return work >= 0;
    };
    for (int event = 0; event < decisions.size(); ++event) {
        const auto& decision = decisions.at(event);
        int fixedFinish = event + 1;
        if (decision.kind == Decision::Fixed) while (fixedFinish < decisions.size()
            && decisions.at(fixedFinish).kind == Decision::Fixed
            && std::abs(decisions.at(fixedFinish).second - decision.second) < epsilon) ++fixedFinish;
        task::CancellationScope::check(); QVector<State> next;
        const auto options = decision.kind == Decision::Slide ? trackOptions(tracks.at(decision.index), tracks.at(decision.index).wifi ? qMin(caps.at(decision.index), 1) : caps.at(decision.index), work)
            : QVector<QVector<HandOrderTrackStop>>{};
        for (const auto& state : beam) {
            if (!spend(work)) { result.exhausted = true; return result; }
            if (decision.kind == Decision::Fixed) {
                std::function<void(int, State)> assign = [&](int index, State candidate) {
                    if (!spend(work)) return;
                    if (index == fixedFinish) { if (rankWithFutureHolds(candidate)) next.append(std::move(candidate)); return; }
                    const int primitiveId = decisions.at(index).index; const auto& primitive = primitives.at(primitiveId);
                    int inheritedMask = 0; bool inheritedQuality = false;
                    if (!buttonOnly && primitive.button && !candidate.physical.isEmpty()) {
                        QVector<ChartInputAction> inputs;
                        for (const auto& scheduled : candidate.physical) inputs.append({scheduled.action, scheduled.button, scheduled.lane});
                        FullReplayOptions prefix; prefix.throughSecond = primitive.action.start + FullReplayProfile::tapCriticalSeconds;
                        auto head = primitive.marker;
                        if (head.type == "slide" || head.type == "wifi") { head.type = QStringLiteral("tap"); head.isEx = head.headEx; }
                        if (head.type == "hold") head.type = QStringLiteral("tap");
                        const auto existing = replayHandOrderChart({head}, inputs, prefix, &work);
                        if (existing.exhausted) { result.exhausted = true; return; }
                        if (!existing.judgements.isEmpty()) {
                            const auto& judged = existing.judgements.first();
                            for (const auto& cause : (judged.headRequired ? judged.headCauses : judged.causes)) inheritedMask |= cause.handMask;
                            inheritedQuality = judged.headRequired ? judged.headHighQuality : judged.highQuality;
                        }
                    }
                    for (int choiceIndex = 0; choiceIndex < 2; ++choiceIndex) {
                        const int hand = primitive.basis >= 0 ? (choiceIndex == 0 ? primitive.basis : 1 - primitive.basis) : choiceIndex;
                        if ((primitive.lock >= 0 && primitive.lock != hand) || (primitiveId == constraint.primitive && hand != constraint.hand)) continue;
                        auto action = primitive.action; action.hand = hand;
                        QVector<Scheduled> choices{{action, primitive.button, false, primitive.lane}};
                        if (primitive.button && p.inputPreference == 1) {
                            choices[0].button = false; choices[0].action.kind = QStringLiteral("screen_button");
                            choices[0].action.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                        }
                        if (!buttonOnly && primitive.button) {
                            auto screen = action; screen.kind = QStringLiteral("screen_button");
                            screen.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                            if (breadth >= 1) choices.append(p.inputPreference == 1 ? Scheduled{action, true, false, primitive.lane}
                                : Scheduled{screen, false, false, primitive.lane});
                            if (inheritedQuality && inheritedMask == (1 << hand)
                                && primitive.marker.type != "hold" && primitive.marker.type != "touch_hold") {
                                State inherited = candidate; inherited.primitiveHands[primitiveId] = hand;
                                inherited.proposedHeads.insert(primitiveId);
                                if (scored(inherited)) assign(index + 1, std::move(inherited));
                            }
                        }
                        bool nearContact = !touches.isEmpty() || fixedFinish - event > 2;
                        for (const auto& scheduled : candidate.physical) nearContact |= scheduled.action.start <= action.end + FullReplayProfile::tapCriticalSeconds
                            && scheduled.action.end >= action.start - FullReplayProfile::tapCriticalSeconds;
                        if (breadth >= 2 && !buttonOnly && nearContact && primitive.button && primitive.marker.type != "hold") {
                            const auto nominal = choices;
                            for (double delta : {-FullReplayProfile::tapCriticalSeconds, -1.0 / FullReplayProfile::ticksPerSecond,
                                    1.0 / FullReplayProfile::ticksPerSecond, FullReplayProfile::tapCriticalSeconds}) {
                                for (auto shifted : nominal) {
                                    shifted.action.start += delta; shifted.action.end += delta;
                                    shifted.action.times = {shifted.action.start}; choices.append(shifted);
                                }
                            }
                        }
                        for (const auto& choice : choices) {
                        State branch = candidate; action = choice.action;
                        auto scheduled = choice; scheduled.ordinary = chart::handOrderIsButtonStrike(primitive.marker); scheduled.ordinal = primitive.token;
                        if (scheduled.ordinary && action.start > branch.focusSecond + epsilon)
                            scheduled.softWeight = basicOrdinaryAssociationWeight(action.start - branch.focusSecond,
                                qMax(0, primitive.token - branch.focusOrdinal - 1));
                        branch.actions.append(scheduled); branch.primitiveHands[primitiveId] = hand;
                        branch.proposedHeads.insert(primitiveId);
                        bool valid = primitive.crossPeer < 0
                            || branch.primitiveHands.at(primitive.crossPeer) < 0
                            || !extremeCrossedEach(primitive, hand,
                                primitives.at(primitive.crossPeer),
                                branch.primitiveHands.at(primitive.crossPeer))
                            || continuingCrossedSweep(primitive, hand,
                                primitives.at(primitive.crossPeer),
                                branch.primitiveHands.at(primitive.crossPeer),
                                primitives, branch.primitiveHands);
                        if (valid && buttonOnly) {
                            const auto& previous = candidate.buttonHands[hand]; const auto& other = candidate.buttonHands[1 - hand];
                            const QPointF rested = chart::handOrderRestedPoint(previous.point, previous.free,
                                action.start, hand, p.returnSpeed);
                            const double distance = QLineF(rested, action.points.first()).length();
                            const double gap = previous.known ? action.start - previous.free : 1.0;
                            valid = previous.free <= action.start + epsilon
                                && !(other.free > action.start + epsilon && other.lane == primitive.lane)
                                && !(previous.known && gap <= epsilon && distance > 1e-6);
                            if (valid) {
                                const double prep = gap > epsilon ? gap : 1.0;
                                const double weight = previous.known ? basicOrdinaryAssociationWeight(action.start - previous.pressed,
                                    qMax(0, primitive.token - previous.ordinal - 1)) : 1.0;
                                branch.cost += scheduled.softWeight * weight * (distance + 0.01 * distance * distance / prep)
                                    + headPostureCost(branch, primitive, hand);
                                branch.cost += headLayoutCost(primitive, hand);
                                scoreHeadContinuity(branch, primitive, hand);
                                for (int previousId : {primitive.interactionPrevious,
                                                       primitive.longitudinalPrevious})
                                    if (previousId >= 0 && branch.primitiveHands.at(previousId) == hand)
                                        branch.cost += 3.0;
                                if (primitive.sweepPrevious >= 0
                                    && branch.primitiveHands.at(primitive.sweepPrevious) >= 0
                                    && branch.primitiveHands.at(primitive.sweepPrevious) != hand) {
                                    branch.cost += primitive.flexibleSweepEntry ? 0.35 : 12.0;
                                    if (!primitive.flexibleSweepEntry) ++branch.ergonomicBreaks;
                                }
                                const int rebound = primitive.reboundPrevious >= 0
                                    ? primitive.reboundPrevious : primitive.relaxedReboundPrevious;
                                if (rebound >= 0 && branch.primitiveHands.at(rebound) >= 0
                                    && branch.primitiveHands.at(rebound) != hand) {
                                    branch.cost += primitive.reboundPrevious >= 0 ? 12.0 : 3.0;
                                    if (primitive.reboundPrevious >= 0) ++branch.ergonomicBreaks;
                                }
                                if (primitive.alternateSweepPrevious >= 0
                                    && branch.primitiveHands.at(primitive.alternateSweepPrevious) == hand) {
                                    branch.cost += 12.0;
                                    ++branch.ergonomicBreaks;
                                }
                                branch.peak = qMax(branch.peak, distance / prep);
                                if (branch.focusSources.contains(action.tokenStart)) branch.focusPeak = qMax(branch.focusPeak, distance / prep);
                                branch.buttonHands[hand] = {action.end, action.points.first(), primitive.lane, true, primitive.token, action.start};
                                branch.physical = branch.actions;
                                branch.cost -= branch.playerCost;
                                valid = scorePlayerEffort(branch, work);
                                branch.cost += branch.playerCost;
                            }
                        } else if (valid) valid = scored(branch);
                        if (valid) {
                            if (primitive.button) {
                                const double margin = FullReplayProfile::tapCriticalSeconds - std::abs(action.start - primitive.marker.second);
                                branch.risk += qMax(0.0, 0.025 - margin);
                            }
                            assign(index + 1, std::move(branch));
                        }
                        }
                    }
                };
                assign(event, state);
            } else if (decision.kind == Decision::Slide) {
                const auto& track = tracks.at(decision.index);
                if (!track.wifi) {
                    // These are actual input-time proposals, not shifted notes
                    // or wider judgement windows. A nominal screen DOWN can
                    // steal a later tap, so preserve small early/late gestures
                    // until the unchanged complete replay evaluates consumers.
                    QVector<double> timeOffsets{0.0};
                    if (std::isfinite(p.diagnosticSlideInputOffsetSeconds)) timeOffsets = {p.diagnosticSlideInputOffsetSeconds};
                    else if (breadth >= 1) timeOffsets += QVector<double>{
                        -2.0 / FullReplayProfile::ticksPerSecond, 2.0 / FullReplayProfile::ticksPerSecond,
                        -3.0 / FullReplayProfile::ticksPerSecond, 3.0 / FullReplayProfile::ticksPerSecond,
                        -0.05, 0.05, -0.1, 0.1, -maximumSlideInputOffset, maximumSlideInputOffset};
                    for (const auto& option : options) {
                        if (!trackMatches(option, decision.index)) continue;
                        for (double offset : timeOffsets) {
                            if (!spend(work)) break;
                            if (track.action.start + offset < track.judgeAvailable - epsilon) continue;
                            State candidate = state; candidate.tracks[decision.index] = option; addTrack(candidate, track, option, offset);
                            if (scored(candidate) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                        }
                    }
                } else {
                    const auto& peer = tracks.at(track.peer);
                    if (track.wifiMode != HandOrderWifiMode::Single && !track.legacyAnnotated) {
                        const auto peerOptions = trackOptions(peer, qMin(caps.at(track.peer), 1), work);
                        for (const auto& option : options) for (const auto& other : peerOptions) {
                            if (!spend(work)) break;
                            if (!trackMatches(option, decision.index) || !trackMatches(other, track.peer)) continue;
                            // Standard two-palm Wifi keeps the two real hands
                            // distinct; shared centre coverage is replayed.
                            bool opposite = true;
                            QVector<double> boundaries{0.0};
                            for (const auto& stop : option) boundaries.append(stop.progress);
                            for (const auto& stop : other) boundaries.append(stop.progress);
                            for (double at : boundaries) {
                                int a = 0, b = 0;
                                for (const auto& stop : option) if (stop.progress <= at + epsilon) a = fixedHand(stop.hand);
                                for (const auto& stop : other) if (stop.progress <= at + epsilon) b = fixedHand(stop.hand);
                                opposite &= a != b;
                            }
                            if (!opposite) continue;
                            State candidate = state; candidate.tracks[decision.index] = option; candidate.tracks[track.peer] = other;
                            addTrack(candidate, track, option); addTrack(candidate, peer, other);
                            if (scored(candidate) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                        }
                    }
                    if (track.wifiMode != HandOrderWifiMode::Split) {
                        QVector<QVector<HandOrderTrackStop>> singleOptions;
                        if (track.legacyAnnotated) {
                            auto legacy = track; legacy.locks = track.singleLocks;
                            singleOptions = trackOptions(legacy, caps.at(decision.index), work);
                        } else for (int hand = 0; hand < 2; ++hand) {
                            bool allowed = true;
                            for (const auto& lock : track.singleLocks) allowed &= fixedHand(lock.hand) < 0 || fixedHand(lock.hand) == hand;
                            if (allowed) singleOptions.append({{0.0, namedHand(hand)}});
                        }
                        for (const auto& stops : singleOptions) {
                            if (!trackMatches(stops, decision.index) || !trackMatches(stops, track.peer)) continue;
                            for (const auto& action : track.singleActions) {
                                State candidate = state;
                                candidate.wifiSingle[decision.index] = candidate.wifiSingle[track.peer] = true;
                                candidate.tracks[decision.index] = stops;
                                auto path = track; path.action = action;
                                addTrack(candidate, path, stops);
                                if (scored(candidate) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                            }
                        }
                    }
                }
            } else {
                QVector<State> groupBeam{state};
                const auto progress = [&](const State& candidate) {
                    int count = 0;
                    for (int id : decision.members) if (candidate.replay.judgements.at(id).highQuality) ++count;
                    return count;
                };
                for (int index : decision.members) {
                    QVector<State> layer;
                    for (auto candidate : groupBeam) {
                        if (!spend(work)) break;
                        candidate.replay = replayHandOrderTouches(touches, contacts(candidate), decision.second, &work);
                        if (candidate.replay.exhausted) break;
                        const auto& target = touches.at(index); const auto judgement = candidate.replay.judgements.at(index);
                        layer.append(candidate);
                        const bool directlyExpressible = judgement.kind != QStringLiteral("group_carry")
                            && maskHand(judgement.handMask) >= 0 && judgement.stableResponsibility;
                        const bool sufficientResponsibility = target.lock >= 0
                            ? judgement.handMask == (1 << target.lock) : directlyExpressible;
                        if (judgement.highQuality && judgement.margin >= 0.025 && sufficientResponsibility) continue;
                        QVector<double> moments{target.second,
                            target.second - TouchJudgeProfile::criticalSeconds + 3.0 / TouchJudgeProfile::ticksPerSecond,
                            target.second + TouchJudgeProfile::criticalSeconds - 3.0 / TouchJudgeProfile::ticksPerSecond};
                        for (const auto& cause : judgement.causes) moments.append(cause.second);
                        std::sort(moments.begin(), moments.end());
                        moments.erase(std::unique(moments.begin(), moments.end(), [](double a, double b) {
                            return std::abs(a - b) < epsilon;
                        }), moments.end());
                        for (double moment : moments) for (int hand = 0; hand < 2; ++hand) {
                            if (!spend(work)) break;
                            if (target.lock >= 0 && target.lock != hand) continue;
                            State branch = candidate; ContactAction press;
                            press.tokenStart = target.tokenStart; press.kind = "touch"; press.hand = hand;
                            press.start = std::ceil(moment * TouchJudgeProfile::ticksPerSecond - epsilon) / TouchJudgeProfile::ticksPerSecond;
                            press.end = press.start + p.pressSeconds; press.points = {touchPadPoint(target.pad)}; press.times = {press.start};
                            branch.actions.append({press, false, false, 0});
                            branch.actions.last().touchParseOrder = target.parseOrder;
                            if (!scored(branch)) continue;
                            branch.replay = replayHandOrderTouches(touches, contacts(branch), decision.second, &work);
                            if (!branch.replay.exhausted) layer.append(std::move(branch));
                        }
                    }
                    std::stable_sort(layer.begin(), layer.end(), [&](const State& a, const State& b) {
                        if (progress(a) != progress(b)) return progress(a) > progress(b);
                        return better(a, b);
                    });
                    if (layer.size() > p.beamWidth) {
                        if (p.captureRejectedCandidates) result.rejectionCounts[QStringLiteral("beam_pruned_states")] += layer.size() - p.beamWidth;
                        layer.resize(p.beamWidth); result.truncated = true;
                    }
                    groupBeam = std::move(layer);
                }
                for (auto& candidate : groupBeam) {
                    bool complete = !candidate.replay.exhausted;
                    for (int id : decision.members) {
                        const auto& judged = candidate.replay.judgements.at(id);
                        complete &= judged.highQuality && (touches.at(id).lock < 0 || judged.handMask == (1 << touches.at(id).lock));
                    }
                    if (complete) {
                        candidate.risk = 0;
                        for (const auto& judged : candidate.replay.judgements) if (judged.judged) candidate.risk += qMax(0.0, 0.025 - judged.margin);
                        if (rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                    }
                }
            }
        }
        if (work < 0 || result.exhausted) { result.exhausted = true; return result; }
        if (next.isEmpty()) return result;
        std::stable_sort(next.begin(), next.end(), better);
        if (next.size() > p.beamWidth) {
            if (p.captureRejectedCandidates) result.rejectionCounts[QStringLiteral("beam_pruned_states")] += next.size() - p.beamWidth;
            next.resize(p.beamWidth); result.truncated = true;
        }
        beam = std::move(next);
        if (decision.kind == Decision::Fixed) event = fixedFinish - 1;
        if (progressCallback) progressCallback({HandOrderProgress::Decisions, event + 1, int(decisions.size())});
    }
    if (progressCallback) progressCallback({HandOrderProgress::Replay});
    QVector<State> finished;
    for (auto& candidate : beam) {
        if (work < 0) { result.exhausted = result.truncated = true; break; }
        if (!buttonOnly && !scored(candidate)) continue;
        candidate.replay = replayHandOrderTouches(touches, contacts(candidate), std::numeric_limits<double>::infinity(), &work);
        if (!locksSatisfied(touches, candidate.replay, std::numeric_limits<double>::infinity(), true)) continue;
        QVector<ChartInputAction> inputs;
        const auto& physical = candidate.physical.isEmpty() ? candidate.actions : candidate.physical;
        for (const auto& action : physical) inputs.append({action.action, action.button, action.lane});
        candidate.fullReplay = replayHandOrderChart(markers, inputs, {}, &work);
        if (candidate.fullReplay.exhausted) {
            if (p.captureRejectedCandidates) captureRejected(result, QStringLiteral("full_replay_work_exhausted"));
            result.exhausted = result.truncated = true; break;
        }
        if (!candidate.fullReplay.valid || !candidate.fullReplay.meetsTarget) {
            if (p.captureRejectedCandidates) {
                captureRejected(result, QStringLiteral("full_replay_target_failed"));
                for (const auto& full : candidate.fullReplay.judgements) if (!full.meetsTarget)
                    captureRejected(result, QStringLiteral("failed_judgement"), &full);
            }
            result.sideEffect = true; continue;
        }
        bool fixedResponsibilities = true;
        for (int primitiveId = 0; primitiveId < primitives.size(); ++primitiveId) if (candidate.primitiveHands.at(primitiveId) >= 0) {
            const auto& primitive = primitives.at(primitiveId);
            for (const auto& full : candidate.fullReplay.judgements) if (full.parseOrder == primitive.marker.parseOrder) {
                const int assignedMask = 1 << candidate.primitiveHands.at(primitiveId);
                const bool hold = primitive.marker.type == "hold" || primitive.marker.type == "touch_hold";
                if (hold) fixedResponsibilities &= (full.singleHandCompletionMask & assignedMask) != 0;
                else {
                    const bool star = primitive.marker.type == "slide" || primitive.marker.type == "wifi";
                    int mask = 0; for (const auto& cause : (star ? full.headCauses : full.causes)) mask |= cause.handMask;
                    fixedResponsibilities &= mask == assignedMask;
                }
            }
        }
        if (!fixedResponsibilities) {
            if (p.captureRejectedCandidates) captureRejected(result, QStringLiteral("fixed_responsibility_rejected"));
            continue;
        }
        // The lightweight Touch replay proposes actions. The complete replay
        // is authoritative once button, hold and slide event competition is known.
        for (int i = 0; i < touches.size(); ++i) for (const auto& full : candidate.fullReplay.judgements) {
            if (full.parseOrder != touches.at(i).parseOrder) continue;
            auto& judged = candidate.replay.judgements[i];
            judged.judged = full.completed; judged.highQuality = full.highQuality;
            judged.handMask = full.handMask; judged.second = full.second; judged.margin = full.margin;
            judged.quality = full.quality; judged.causes = full.causes;
            bool slide = false; for (const auto& cause : full.causes) slide |= cause.kind == "slide" || cause.kind == "wifi";
            judged.kind = touches.at(i).hold ? QStringLiteral("touch_hold")
                : full.kind == "group_carry" ? QStringLiteral("group_carry")
                : slide ? QStringLiteral("slide_cover") : QStringLiteral("direct_touch");
        }
        if (!locksSatisfied(touches, candidate.replay, std::numeric_limits<double>::infinity(), true)) continue;
        candidate.risk = 0;
        for (const auto& judged : candidate.fullReplay.judgements)
            candidate.risk += qMax(0.0, 0.025 - judged.margin);
        finished.append(std::move(candidate));
        // Button-only costs are already exact and their event margins do not
        // depend on hand identity. Avoid replaying the same 100-tap schedule
        // for every equivalent terminal hand assignment.
        if (buttonOnly) break;
    }
    // Evaluate the same existing carry-stability rule for each real terminal
    // stream before choosing a winner. A cheap ambiguous winner must not hide
    // a direct, fully expressible critical alternative.
    if (!initial.responsibilityTouches.isEmpty()) for (auto& selected : finished) {
        for (const auto& alternate : finished) {
            if (peakTier(alternate.peak) != peakTier(selected.peak)
                || std::abs(alternate.risk - selected.risk) > epsilon
                || alternate.cost > selected.cost + 0.25) continue;
            for (int i = 0; i < touches.size(); ++i) {
                auto& judged = selected.replay.judgements[i];
                if (judged.kind != QStringLiteral("group_carry")) continue;
                if (!spend(work)) { result.exhausted = result.truncated = true; break; }
                if (judged.handMask != alternate.replay.judgements[i].handMask) judged.stableResponsibility = false;
            }
            if (work < 0) break;
        }
    }
    if (result.exhausted || work < 0) for (auto& candidate : finished) for (auto& judged : candidate.replay.judgements)
        if (judged.kind == QStringLiteral("group_carry")) judged.stableResponsibility = false;
    for (const auto& candidate : finished) if (!result.found || better(candidate, result.state)) {
        result.found = true; result.state = candidate;
    }
    result.exhausted |= work < 0; return result;
}
}

double basicOrdinaryAssociationWeight(double seconds, int intermediateNotes)
{
    return std::exp(-qMax(0.0, seconds) / 0.35) * std::pow(0.5, qMax(0, intermediateNotes));
}

double basicMotionCost(const QVector<QPointF>& points, const QVector<double>& seconds, double* peakSpeed)
{
    double cost = 0, peak = 0;
    if (points.size() != seconds.size() + 1) return std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < seconds.size(); ++i) {
        task::CancellationScope::checkpoint();
        if (!std::isfinite(seconds.at(i)) || seconds.at(i) <= 0) return std::numeric_limits<double>::quiet_NaN();
        const double distance = QLineF(points.at(i), points.at(i + 1)).length(), speed = distance / seconds.at(i);
        cost += distance + 0.1 * speed * speed * seconds.at(i); peak = qMax(peak, speed);
    }
    if (peakSpeed) *peakSpeed = peak; return cost;
}

static bool validBasicParameters(const BasicParameters& p)
{
    if (p.version != basicAlgorithmVersion || p.judgeProfileRevision != basicJudgeProfileRevision
        || p.beamWidth < 1 || p.beamWidth > 128 || p.maxTasks < 1 || p.maxTasks > 2048
        || p.counterfactualBudget < 0 || p.counterfactualBudget > 128
        || !std::isfinite(p.contextSeconds) || p.contextSeconds < 0 || p.contextSeconds > 10
        || !std::isfinite(p.pressSeconds) || p.pressSeconds <= 0 || p.pressSeconds > 1
        || !std::isfinite(p.returnSpeed) || p.returnSpeed < BasicParameters::minReturnSpeed
        || p.returnSpeed > BasicParameters::maxReturnSpeed
        || p.sweepThreshold < chart::handOrderMinSweepThreshold
        || p.sweepThreshold > chart::handOrderMaxSweepThreshold
        || !validPlayerProfile(p.player)
        || (!std::isnan(p.diagnosticSlideInputOffsetSeconds)
            && (!std::isfinite(p.diagnosticSlideInputOffsetSeconds)
                || std::abs(p.diagnosticSlideInputOffsetSeconds) > maximumDiagnosticSlideInputOffset))
        || p.maxWorkUnits < 1000 || p.maxWorkUnits > BasicParameters::maxAllowedWorkUnits || p.inputPreference < 0 || p.inputPreference > 2) {
        return false;
    }
    return true;
}

static BasicResult inferBasicHandOrderImpl(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const BasicParameters& p, const miacode::simai::SimaiTimingMetadata& metadata,
    const HandOrderProgressCallback& progress)
{
    if (progress) progress({HandOrderProgress::Preparing});
    BasicResult result;
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    if (!validBasicParameters(p)) { result.error = "invalid_parameters"; return result; }
    const double inputOffsetLimit = std::isfinite(p.diagnosticSlideInputOffsetSeconds)
        ? qMax(maximumSlideInputOffset, std::abs(p.diagnosticSlideInputOffsetSeconds)) : maximumSlideInputOffset;
    QVector<int> lines{0}; for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') lines.append(i + 1);
    QString layoutSource = text;
    int layoutEnd = end;
    if (p.overwriteExisting || p.evaluateCompleteAssignment) {
        // Recalculation must derive its layout from the native chart, not from
        // the old labels that happen to be present in the selected range.
        for (int i = parsed.sourceTokens.size() - 1; i >= 0; --i) {
            const auto& source = parsed.sourceTokens.at(i);
            const int start = lines.at(source.line - 1) + source.col - 1;
            const int finish = start + source.length;
            if (!p.evaluateCompleteAssignment && (start < begin || finish > end)) continue;
            const auto parts = splitToken(text.mid(start, source.length));
            if (parts.suffix.isEmpty()) continue;
            layoutSource.replace(start, source.length, parts.baseToken);
            if (finish <= end) layoutEnd -= source.length - parts.baseToken.size();
        }
    }
    const auto layout = chart_transform::basicHandOrderLayoutInSelection(
        layoutSource, begin, layoutEnd, metadata, p.returnSpeed, p.sweepThreshold, p.colorUnresolved,
        p.recognizedPatterns.get());
    QString layoutText = layoutSource;
    for (int i = layout.edit.patches.size() - 1; i >= 0; --i) {
        const auto& patch = layout.edit.patches.at(i);
        layoutText.replace(patch.start, patch.end - patch.start, patch.text);
    }
    const auto layoutParsed = SimaiParser::validateSyntax(layoutText, metadata);
    QVector<QString> layoutFallback(parsed.sourceTokens.size());
    QVector<const TimelineNoteMarker*> layoutByOrder;
    const bool layoutMarkersMatch = layoutParsed.errors.isEmpty() && layoutParsed.noteMarkers.size() == parsed.noteMarkers.size()
        && std::equal(parsed.noteMarkers.cbegin(), parsed.noteMarkers.cend(), layoutParsed.noteMarkers.cbegin(),
            [](const auto& before, const auto& after) {
                return before.parseOrder == after.parseOrder && before.type == after.type && before.lane == after.lane
                    && before.endLane == after.endLane && std::abs(before.second - after.second) < epsilon
                    && std::abs(before.endSecond - after.endSecond) < epsilon;
            });
    bool layoutCompatible = layoutMarkersMatch && layoutParsed.sourceTokens.size() == parsed.sourceTokens.size();
    QVector<int> layoutLines{0};
    for (int i = 0; i < layoutText.size(); ++i)
        if (layoutText.at(i) == QLatin1Char('\n')) layoutLines.append(i + 1);
    for (int i = 0; layoutCompatible && i < parsed.sourceTokens.size(); ++i) {
        const auto& source = parsed.sourceTokens.at(i);
        const auto& suggested = layoutParsed.sourceTokens.at(i);
        if (source.line < 1 || source.line > lines.size()
            || suggested.line < 1 || suggested.line > layoutLines.size()
            || source.markerCount != suggested.markerCount) { layoutCompatible = false; break; }
        const auto originalParts = splitToken(text.mid(lines.at(source.line - 1) + source.col - 1,
            source.length));
        const QString suggestedToken = layoutText.mid(
            layoutLines.at(suggested.line - 1) + suggested.col - 1, suggested.length);
        const auto suggestedParts = splitToken(suggestedToken);
        if (originalParts.baseToken != suggestedParts.baseToken) { layoutCompatible = false; break; }
        if (!suggestedParts.suffix.isEmpty()) layoutFallback[i] = suggestedToken;
    }
    if (layoutMarkersMatch) {
        layoutByOrder.fill(nullptr, parsed.noteMarkers.size());
        for (const auto& marker : layoutParsed.noteMarkers)
            if (marker.parseOrder >= 0 && marker.parseOrder < layoutByOrder.size())
                layoutByOrder[marker.parseOrder] = &marker;
    }
    if (!layoutCompatible) layoutFallback.fill({});
    QHash<int, const TimelineNoteMarker*> patternByOrder;
    if (p.recognizedPatterns) for (const auto& group : *p.recognizedPatterns) {
        if (group.preferred < 0) continue;
        for (const auto& marker : group.variants.at(group.preferred).markers)
            patternByOrder.insert(marker.parseOrder, &marker);
    }
    QVector<const TimelineNoteMarker*> byOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers) if (marker.parseOrder >= 0 && marker.parseOrder < byOrder.size()) byOrder[marker.parseOrder] = &marker;
    QVector<Token> tokens; QVector<Primitive> primitives; QVector<Track> tracks; QVector<TouchTarget> touches;
    struct Window { double first, last; }; QVector<Window> risks;
    for (const auto& source : parsed.sourceTokens) {
        task::CancellationScope::checkpoint(); Token token;
        token.start = lines.at(source.line - 1) + source.col - 1; token.end = token.start + source.length;
        token.line = source.line; token.col = source.col;
        const auto parts = splitToken(text.mid(token.start, source.length)); token.base = parts.baseToken; token.suffix = parts.suffix;
        token.protectedText = !parts.suffix.isEmpty(); token.target = token.start >= begin && token.end <= end;
        token.partial = token.start < end && token.end > begin && !token.target;
        if (p.overwriteExisting && token.target) token.protectedText = false;
        if (!source.nativeValid || source.markerCount == 0 || !parsed.errors.isEmpty()) token.issue = "invalid_native";
        for (const auto& error : parsed.annotationErrors) if (error.line == source.line && error.col >= source.col && error.col < source.col + source.length) token.issue = "invalid_annotation";
        token.beginTime = token.nominal = std::numeric_limits<double>::infinity(); token.endTime = -std::numeric_limits<double>::infinity();
        const int tokenIndex = tokens.size(); bool headCreated = false;
        token.layoutFallback = layoutFallback.value(tokenIndex);
        for (int i = 0; i < source.markerCount; ++i) {
            const auto* pointer = byOrder.value(source.firstParseOrder + i); if (!pointer) { token.issue = "invalid_native"; continue; }
            auto marker = *pointer;
            const auto* suggested = layoutByOrder.value(marker.parseOrder, nullptr);
            if (p.overwriteExisting && token.target) {
                marker.handOrderHead = HandOrderHand::Unspecified; marker.handOrderTrackStops.clear();
                marker.handOrderWifiSectorStops.clear(); marker.handOrderWifiMode = HandOrderWifiMode::Legacy;
            }
            if (!std::isfinite(marker.second) || !std::isfinite(marker.endSecond)
                || !std::isfinite(marker.slideTraceSecond) || std::abs(marker.second) > 1e10
                || std::abs(marker.endSecond) > 1e10 || std::abs(marker.slideTraceSecond) > 1e10) {
                token.issue = "resource_limit"; continue;
            }
            const bool screenTouch = marker.type == "touch" || marker.type == "touch_hold";
            token.nominal = qMin(token.nominal, marker.second);
            const double available = screenTouch ? FullReplayProfile::touchAvailableSeconds : FullReplayProfile::tapAvailableSeconds;
            token.beginTime = qMin(token.beginTime, marker.second - available);
            token.endTime = qMax(token.endTime, qMax(marker.second + p.pressSeconds, marker.endSecond));
            token.endTime = qMax(token.endTime, marker.second + available);
            if (marker.type == "slide" || marker.type == "wifi")
                token.endTime = qMax(token.endTime, marker.endSecond + FullReplayProfile::slideAvailableSeconds);
            const bool slide = marker.type == "slide" || marker.type == "wifi", touch = marker.type == "touch", touchHold = marker.type == "touch_hold";
            token.slide |= slide;
            if (marker.type != "tap" && marker.type != "hold" && !slide && !touch && !touchHold) token.issue = "unsupported_object";
            if (marker.isMine || marker.headMine || marker.trackMine) token.issue = "mine_unsupported";
            token.markers.append(marker);
            if (touch || touchHold) {
                if (!muri::padTokenIsValid(marker.touchPad)) token.issue = "geometry_unsupported";
                token.touches.append(touches.size());
                touches.append({token.start, marker.parseOrder, marker.eachGroupId, marker.touchPad,
                    marker.second, touchHold, fixedHand(marker.handOrderHead), -1,
                    marker.tempoBpm, marker.subdivision,
                    suggested ? fixedHand(suggested->handOrderHead) : -1,
                    layoutCompatible && layout.gestureHeads.contains(marker.parseOrder)});
            }
            if (!touch && (!slide || (marker.hasHeadStar && !headCreated))) {
                Primitive primitive; primitive.token = tokenIndex; primitive.lane = marker.lane; primitive.marker = marker;
                primitive.action.tokenStart = token.start; primitive.action.kind = touchHold ? "touch_hold" : "button";
                primitive.action.start = marker.second;
                primitive.action.end = (marker.type == "hold" || touchHold) ? qMax(marker.endSecond, marker.second + p.pressSeconds) : marker.second + p.pressSeconds;
                primitive.action.points = {touchHold ? touchPadPoint(marker.touchPad) : lanePoint(marker.lane)};
                primitive.action.times = {primitive.action.start}; primitive.button = !touchHold;
                primitive.lock = fixedHand(marker.handOrderHead);
                primitive.basis = fixedHand(chart_transform::basicLayoutHeadHand(marker));
                primitive.layoutBasis = suggested ? fixedHand(suggested->handOrderHead) : -1;
                primitive.gestureDefault = layoutCompatible && layout.gestureHeads.contains(marker.parseOrder);
                primitive.configuredHead = layoutMarkersMatch && layout.configuredHeads.contains(marker.parseOrder);
                if (const auto* pattern = patternByOrder.value(marker.parseOrder);
                    pattern && !isLockingHold(marker) && primitive.lock < 0) {
                    const int preferred = fixedHand(pattern->handOrderHead);
                    if (preferred >= 0) {
                        primitive.basis = preferred;
                        if (primitive.layoutBasis < 0) primitive.layoutBasis = preferred;
                    }
                }
                if (primitive.layoutBasis >= 0) primitive.basis = primitive.layoutBasis;
                token.primitives.append(primitives.size()); primitives.append(primitive); headCreated |= slide;
            }
            if (slide) {
                Track track; track.token = tokenIndex; track.branch = marker.slideBranchIndex;
                track.parseOrder = marker.parseOrder;
                track.gestureDefault = layoutCompatible && layout.gestureTracks.contains(marker.parseOrder);
                track.headLane = marker.lane; track.headSecond = marker.second;
                track.hasHeadStar = marker.hasHeadStar; track.locks = marker.handOrderTrackStops;
                track.basis = fixedHand(chart_transform::basicLayoutTrackHand(marker));
                track.layoutBasis = suggested && !suggested->handOrderTrackStops.isEmpty()
                    ? fixedHand(suggested->handOrderTrackStops.constFirst().hand) : -1;
                track.sweepSplit = layoutCompatible && layout.keySweepSplitTracks.contains(marker.parseOrder);
                track.sweepKeep = layoutCompatible && layout.keySweepKeepTracks.contains(marker.parseOrder);
                track.tapLaneReserved = layoutCompatible
                    && layout.tapLaneReservedTracks.contains(marker.parseOrder);
                if (track.layoutBasis >= 0) track.basis = track.layoutBasis;
                track.action.tokenStart = token.start; track.action.branch = track.branch; track.action.kind = "slide";
                track.action.start = marker.slideTraceSecond; track.action.end = marker.endSecond;
                track.judgeAvailable = marker.second - muri::kSlideLeadingSeconds;
                if (marker.type != "wifi" && inputOffsetLimit > maximumSlideInputOffset)
                    token.beginTime = qMin(token.beginTime, qMax(track.judgeAvailable, track.action.start - inputOffsetLimit));
                track.action.motionStart = track.action.start; track.action.motionEnd = track.action.end;
                if ((marker.type != "wifi" && marker.slideSegmentPoints.isEmpty()) || track.action.end <= track.action.start) token.issue = "geometry_unsupported";
                for (int segment = 0; marker.type != "wifi" && segment < marker.slideSegmentPoints.size(); ++segment) {
                    const auto& points = marker.slideSegmentPoints.at(segment);
                    if (points.size() < 2 || segment >= marker.slideSegmentShootSeconds.size()) { token.issue = "geometry_unsupported"; break; }
                    const double first = marker.slideSegmentShootSeconds.at(segment);
                    const double last = segment + 1 < marker.slideSegmentShootSeconds.size() ? marker.slideSegmentShootSeconds.at(segment + 1) : marker.endSecond;
                    if (last <= first || !std::isfinite(first) || !std::isfinite(last)) { token.issue = "geometry_unsupported"; break; }
                    for (int point = 0; point < points.size(); ++point) {
                        const double second = first + (last - first) * point / (points.size() - 1);
                        const auto position = points.at(point) / referenceRadius;
                        if (!std::isfinite(position.x()) || !std::isfinite(position.y())) { token.issue = "geometry_unsupported"; break; }
                        if (!track.action.times.isEmpty() && second <= track.action.times.last() + epsilon) {
                            if (QLineF(position, track.action.points.last()).length() > 1e-6) token.issue = "geometry_unsupported";
                            continue;
                        }
                        track.action.points.append(position); track.action.times.append(second);
                    }
                }
                compactContactAction(track.action);
                if (track.locks.size() > 14) token.issue = "resource_limit";
                if (marker.type == "wifi") {
                    QVector<ContactAction> sectors, single;
                    if (!buildWifiHandOrderActions(marker, &sectors, &single)) { token.issue = "geometry_unsupported"; continue; }
                    const int primary = tracks.size();
                    for (int sector = 0; sector < 2; ++sector) {
                        Track wing = track; wing.wifi = true; wing.sector = sector;
                        wing.basis = fixedHand(chart_transform::basicLayoutWifiSectorHand(marker, sector));
                        wing.layoutBasis = suggested && suggested->handOrderWifiSectorStops.size() > sector
                            && !suggested->handOrderWifiSectorStops.at(sector).isEmpty()
                            ? fixedHand(suggested->handOrderWifiSectorStops.at(sector).constFirst().hand) : -1;
                        if (wing.layoutBasis >= 0) wing.basis = wing.layoutBasis;
                        wing.peer = primary + 1 - sector; wing.wifiMode = marker.handOrderWifiMode;
                        wing.action = sectors.at(sector); wing.action.tokenStart = token.start;
                        wing.singleActions = single;
                        for (auto& action : wing.singleActions) action.tokenStart = token.start;
                        wing.singleLocks = marker.handOrderTrackStops;
                        wing.locks = marker.handOrderWifiMode == HandOrderWifiMode::Split
                            ? marker.handOrderWifiSectorStops.value(sector) : QVector<HandOrderTrackStop>{};
                        // Legacy l/r/m remains a whole-track label; an existing
                        // label constrains the corresponding real single hand.
                        wing.legacyAnnotated = marker.handOrderWifiMode == HandOrderWifiMode::Legacy && !marker.handOrderTrackStops.isEmpty();
                        token.tracks.append(tracks.size()); tracks.append(wing);
                    }
                } else { token.tracks.append(tracks.size()); tracks.append(track); }
            }
        }
        if (headCreated) for (int trackId : token.tracks) {
            tracks[trackId].hasHeadStar = true;
            tracks[trackId].headPrimitive = token.primitives.first();
        }
        if (!token.issue.isEmpty() && std::isfinite(token.beginTime)) risks.append({token.beginTime - p.contextSeconds, token.endTime + p.contextSeconds});
        tokens.append(token);
    }
    // A judgeable early release cannot make two distinct Hold buttons at the
    // same nominal tail/start an instantaneous one-hand movement.
    for (int next = 0; next < primitives.size(); ++next) {
        auto& current = primitives[next];
        if (current.marker.type != QStringLiteral("hold")) continue;
        int previous = -1;
        for (int id = 0; id < next; ++id) {
            const auto& earlier = primitives.at(id);
            if (earlier.marker.type != QStringLiteral("hold")
                || earlier.lane == current.lane
                || std::abs(earlier.marker.endSecond - current.action.start)
                    > 1.0 / FullReplayProfile::ticksPerSecond + epsilon) continue;
            if (previous >= 0) { previous = -1; break; }
            previous = id;
        }
        current.holdRelayPrevious = previous;
    }
    if (layoutCompatible) for (const auto& link : layout.oneStrokeLinks) {
        int previous = -1, next = -1;
        for (int id = 0; id < tracks.size(); ++id) if (!tracks.at(id).wifi) {
            if (tracks.at(id).parseOrder == link.first) previous = id;
            if (tracks.at(id).parseOrder == link.second) next = id;
        }
        if (previous >= 0 && next >= 0) tracks[next].oneStrokePrevious = previous;
    }
    for (int first = 0; first < primitives.size(); ++first) {
        const auto& a = primitives.at(first);
        if (!a.button || a.marker.eachGroupId < 0) continue;
        if (layoutCompatible && layout.windingEachHeads.contains(a.marker.parseOrder)) continue;
        for (int second = first + 1; second < primitives.size(); ++second) {
            const auto& b = primitives.at(second);
            if (!b.button || b.marker.eachGroupId != a.marker.eachGroupId
                || std::abs(a.action.start - b.action.start) > epsilon) continue;
            const int distance = std::abs(a.lane - b.lane);
            if (qMin(distance, 8 - distance) < 2) continue;
            const double xA = lanePoint(a.lane).x(), xB = lanePoint(b.lane).x();
            if (!((xA < -0.25 && xB > 0.25) || (xB < -0.25 && xA > 0.25))) continue;
            primitives[first].crossPeer = second;
            primitives[second].crossPeer = first;
        }
    }
    // Correct only an unmistakably reversed, simple label in a selected
    // opposite-side Each. Other existing annotations remain hard locks.
    for (int id = 0; !p.evaluateCompleteAssignment && id < primitives.size(); ++id) {
        auto& primitive = primitives[id];
        if (primitive.crossPeer < 0 || !tokens.at(primitive.token).target
            || !tokens.at(primitives.at(primitive.crossPeer).token).target) continue;
        auto& token = tokens[primitive.token];
        if (token.suffix != QStringLiteral("~l") && token.suffix != QStringLiteral("~r")) continue;
        const double x = lanePoint(primitive.lane).x();
        const double peerX = lanePoint(primitives.at(primitive.crossPeer).lane).x();
        if (qMax(std::abs(x), std::abs(peerX)) <= 0.65) continue;
        if (!((x < -0.25 && primitive.lock == 1)
            || (x > 0.25 && primitive.lock == 0))) continue;
        token.protectedText = false;
        primitive.lock = -1;
        primitive.marker.handOrderHead = HandOrderHand::Unspecified;
        for (auto& marker : token.markers) marker.handOrderHead = HandOrderHand::Unspecified;
    }
    // Three or more regular neighbouring-lane ABAB taps are normally played by
    // alternating hands. Keep this as a cost so occupied hands can still win.
    QVector<int> tapOrder;
    for (int id = 0; id < primitives.size(); ++id)
        if (chart::handOrderIsButtonStrike(primitives.at(id).marker)) tapOrder.append(id);
    std::stable_sort(tapOrder.begin(), tapOrder.end(), [&](int a, int b) {
        return primitives.at(a).action.start < primitives.at(b).action.start;
    });
    const auto regular = [&](int a, int b) {
        const auto& first = primitives.at(a).marker;
        const auto& second = primitives.at(b).marker;
        return chart::handOrderRegularInputStep(first, second, true);
    };
    const auto adjacentStep = [&](int a, int b) {
        const int first = primitives.at(a).lane, second = primitives.at(b).lane;
        if (second == first % 8 + 1) return 1;
        if (second == (first + 6) % 8 + 1) return -1;
        return 0;
    };
    for (int first = 0; first + 2 < tapOrder.size();) {
        const int a = tapOrder.at(first), b = tapOrder.at(first + 1);
        const int delta = std::abs(primitives.at(a).lane - primitives.at(b).lane);
        if (delta != 1 && delta != 7) { ++first; continue; }
        int last = first + 2;
        while (last < tapOrder.size()) {
            const int previous = tapOrder.at(last - 1), current = tapOrder.at(last);
            const int expected = primitives.at(tapOrder.at(first + (last - first) % 2)).lane;
            if (primitives.at(current).lane != expected || !regular(previous, current)) break;
            ++last;
        }
        if (last - first >= 3 && regular(a, b)) {
            for (int i = first + 1; i < last; ++i)
                primitives[tapOrder.at(i)].interactionPrevious = tapOrder.at(i - 1);
            first = last;
        } else ++first;
    }
    // A-B-A-C-A uses the centre hand on beats 1/3/5 and the other hand
    // on the two opposite neighbours. This remains a soft preference.
    for (int first = 0; first + 4 < tapOrder.size(); ++first) {
        const int centre = tapOrder.at(first);
        if (primitives.at(tapOrder.at(first + 2)).lane != primitives.at(centre).lane
            || primitives.at(tapOrder.at(first + 4)).lane != primitives.at(centre).lane
            || adjacentStep(centre, tapOrder.at(first + 1)) == 0
            || adjacentStep(centre, tapOrder.at(first + 1))
                != -adjacentStep(centre, tapOrder.at(first + 3))) continue;
        bool regularRun = true;
        for (int i = first + 1; i <= first + 4; ++i)
            regularRun &= regular(tapOrder.at(i - 1), tapOrder.at(i));
        if (!regularRun) continue;
        for (int i = first + 1; i <= first + 4; ++i)
            primitives[tapOrder.at(i)].interactionPrevious = tapOrder.at(i - 1);
    }
    // Three or more rapid identical keys need alternating DOWN actions.
    // Two-key pairs remain available to the winding and rebound patterns.
    for (int first = 0; first < tapOrder.size();) {
        int last = first + 1;
        while (last < tapOrder.size()
            && primitives.at(tapOrder.at(last)).lane == primitives.at(tapOrder.at(first)).lane
            && regular(tapOrder.at(last - 1), tapOrder.at(last))) ++last;
        const auto& start = primitives.at(tapOrder.at(first)).marker;
        const bool fastRun = last - first >= 3 && chart::handOrderLongitudinalStep(start,
            primitives.at(tapOrder.at(first + 1)).marker);
        const bool sustainedRun = last - first >= 5 && !chart::handOrderIsEighthOrSlower(
            primitives.at(tapOrder.at(first + 1)).marker.second - start.second, start);
        if (fastRun || sustainedRun)
            for (int i = first + 1; i < last; ++i)
                primitives[tapOrder.at(i)].longitudinalPrevious = tapOrder.at(i - 1);
        first = last;
    }
    // A rapid monotonic key sweep keeps one hand through the centre and the
    // 8/1 seam; charging each handoff avoids a last-moment return jump.
    for (int first = 0; first + 3 < tapOrder.size();) {
        const int direction = adjacentStep(tapOrder.at(first), tapOrder.at(first + 1));
        const bool sameSide = first + 3 < tapOrder.size()
            && (primitives.at(tapOrder.at(first)).lane <= 4)
                == (primitives.at(tapOrder.at(first + 3)).lane <= 4);
        const int gestureSize = sameSide ? 4 : 8;
        bool coherentGesture = direction && first + gestureSize - 1 < tapOrder.size();
        for (int i = first + 1; coherentGesture && i < first + gestureSize; ++i) {
            const auto& a = primitives.at(tapOrder.at(i - 1)).marker;
            const auto& b = primitives.at(tapOrder.at(i)).marker;
            const double grid = a.tempoBpm > 0 && a.subdivision > 0 ? 240.0 / (a.tempoBpm * a.subdivision) : 0;
            const double gap = b.second - a.second;
            coherentGesture = (i == first + 1 || !chart::handOrderHasStarStrike(a))
                && (i == first + gestureSize - 1 || !chart::handOrderHasStarStrike(b))
                && grid > 0 && gap >= grid * 0.9 - epsilon && gap <= grid * 1.1 + epsilon
                && gap <= 0.13 + epsilon && adjacentStep(tapOrder.at(i - 1), tapOrder.at(i)) == direction;
        }
        const double maximumGap = coherentGesture ? qMax(0.13, chart::handOrderSweepGapSeconds(p.sweepThreshold))
                                              : chart::handOrderSweepGapSeconds(p.sweepThreshold);
        int last = first + 1;
        while (direction && last < tapOrder.size() && last - first < 8) {
            const auto& previous = primitives.at(tapOrder.at(last - 1)).marker;
            const auto& current = primitives.at(tapOrder.at(last)).marker;
            const double gap = current.second - previous.second;
            const double grid = previous.tempoBpm > 0 && previous.subdivision > 0
                ? 240.0 / (previous.tempoBpm * previous.subdivision) : 0.0;
            if (previous.subdivision <= 0 || current.subdivision <= 0
                || grid <= 0.0 || gap < grid * 0.9 - epsilon || gap > grid * 1.1 + epsilon
                || gap > maximumGap + epsilon
                || (last > first + 1 && chart::handOrderHasStarStrike(previous))
                || adjacentStep(tapOrder.at(last - 1), tapOrder.at(last)) != direction) break;
            ++last;
        }
        const bool completeRing = last - first == 8;
        const bool fastRun = last - first >= 4;
        if (completeRing || fastRun) {
            const double firstSecond = primitives.at(tapOrder.at(first)).action.start;
            bool reservedHand = std::any_of(tracks.cbegin(), tracks.cend(), [&](const auto& track) {
                return track.headSecond <= firstSecond + epsilon && track.action.end > firstSecond + epsilon;
            });
            reservedHand |= std::any_of(primitives.cbegin(), primitives.cend(), [&](const auto& primitive) {
                return (primitive.marker.type == "hold" || primitive.marker.type == "touch_hold")
                    && primitive.marker.second <= firstSecond + epsilon
                    && primitive.marker.endSecond > firstSecond + epsilon;
            });
            const auto& starting = primitives.at(tapOrder.at(first)).marker;
            if (starting.eachGroupId >= 0)
                reservedHand |= std::any_of(primitives.cbegin(), primitives.cend(), [&](const auto& other) {
                    return other.marker.parseOrder != starting.parseOrder
                        && other.marker.eachGroupId == starting.eachGroupId
                        && std::abs(other.marker.second - firstSecond) < epsilon;
                });
            const bool splitFour = last - first == 4
                && (primitives.at(tapOrder.at(first)).lane <= 4)
                    != (primitives.at(tapOrder.at(last - 1)).lane <= 4)
                && !reservedHand;
            for (int i = first + 1; i < last; ++i) {
                primitives[tapOrder.at(i)].sweepPrevious = tapOrder.at(i - 1);
                // Short cross-side runs may use two contiguous two-key chunks.
                // Same-side runs and complete rings retain their flowing hand.
                primitives[tapOrder.at(i)].flexibleSweepEntry = splitFour && i == first + 2;
            }
            first = last;
        } else ++first;
    }
    if (layoutMarkersMatch) {
        QHash<int, int> byOrder;
        for (int i = 0; i < primitives.size(); ++i) byOrder.insert(primitives.at(i).marker.parseOrder, i);
        for (const auto& link : layout.interactionLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous >= 0 && next >= 0) primitives[next].alternateSweepPrevious = previous;
        }
        for (const auto& link : layout.phaseLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous < 0 || next < 0) continue;
            primitives[next].phasePrevious = previous;
            primitives[next].phaseChangesHand = link.changesHand;
            primitives[next].interactionPrevious = -1;
        }
        for (const auto& link : layout.windingLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous < 0 || next < 0) continue;
            primitives[next].sweepPrevious = previous;
            primitives[next].flexibleSweepEntry = false;
        }
        for (const auto& link : layout.sweepLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous < 0 || next < 0) continue;
            primitives[next].sweepPrevious = previous;
            primitives[next].flexibleSweepEntry = false;
        }
        for (const auto& link : layout.reboundLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous >= 0 && next >= 0) primitives[next].reboundPrevious = previous;
        }
        for (const auto& link : layout.relaxedReboundLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous >= 0 && next >= 0) primitives[next].relaxedReboundPrevious = previous;
        }
        for (const auto& link : layout.alternatingSweepLinks) {
            const int previous = byOrder.value(link.first, -1), next = byOrder.value(link.second, -1);
            if (previous >= 0 && next >= 0) primitives[next].alternateSweepPrevious = previous;
        }
    }
    QVector<int> strikes;
    for (int id = 0; id < primitives.size(); ++id) strikes.append(id);
    std::stable_sort(strikes.begin(), strikes.end(), [&](int a, int b) {
        return primitives.at(a).marker.second < primitives.at(b).marker.second;
    });
    for (int first = 0, previous = 0; first < strikes.size();) {
        int endOfMoment = first + 1;
        while (endOfMoment < strikes.size() && std::abs(primitives.at(strikes.at(endOfMoment)).marker.second
            - primitives.at(strikes.at(first)).marker.second) < epsilon) ++endOfMoment;
        if (endOfMoment - first == 1 && first - previous == 1) {
            const int previousKey = strikes.at(previous);
            auto& target = primitives[strikes.at(first)];
            if (target.phasePrevious < 0 && target.longitudinalPrevious < 0
                && chart::handOrderSameKeyRebound(primitives.at(previousKey).marker, target.marker))
                target.sameKeyPrevious = previousKey;
        }
        previous = first;
        first = endOfMoment;
    }
    QVector<int> touchOrder;
    for (int id = 0; id < touches.size(); ++id)
        if (!touches.at(id).hold) touchOrder.append(id);
    std::stable_sort(touchOrder.begin(), touchOrder.end(), [&](int a, int b) {
        return touches.at(a).second < touches.at(b).second;
    });
    const auto adjacentTouch = [&](int a, int b) {
        const auto& first = touches.at(a);
        const auto& second = touches.at(b);
        if (!chart::touchPadsCanSweep(first.pad, second.pad)
            || first.tempoBpm <= 0 || first.subdivision <= 0) return false;
        const double grid = 240.0 / (first.tempoBpm * first.subdivision);
        const double gap = second.second - first.second;
        return gap >= grid * 0.9 - epsilon && gap <= qMin(grid * 1.1, 0.18) + epsilon;
    };
    for (int first = 0; first + 2 < touchOrder.size();) {
        int last = first + 1;
        while (last < touchOrder.size()
            && adjacentTouch(touchOrder.at(last - 1), touchOrder.at(last))) ++last;
        if (last - first >= 3) {
            for (int i = first + 1; i < last; ++i)
                touches[touchOrder.at(i)].sweepPrevious = touchOrder.at(i - 1);
            first = last;
        } else ++first;
    }
    for (auto& track : tracks) {
        const double duration = track.action.end - track.action.start; if (duration <= 0) continue;
        track.boundaries = {0.25, 0.5, 0.75};
        for (const auto& primitive : primitives) for (double second : {primitive.action.start - handoffOverlap, primitive.action.end + handoffOverlap}) {
            const double progress = (second - track.action.start) / duration;
            if (progress > 0.02 && progress < 0.98) track.boundaries.append(progress);
        }
        std::sort(track.boundaries.begin(), track.boundaries.end());
        track.boundaries.erase(std::unique(track.boundaries.begin(), track.boundaries.end(), [&](double a, double b) { return (b - a) * duration < 0.025; }), track.boundaries.end());
        if (track.boundaries.size() > 8) {
            QVector<double> bounded;
            for (int i = 0; i < 8; ++i) bounded.append(track.boundaries.at(i * (track.boundaries.size() - 1) / 7));
            track.boundaries = bounded;
        }
    }
    // Ordinary proximity is a non-transitive lookaround, never a component edge.
    for (auto& token : tokens) if (token.issue.isEmpty()) for (const auto& risk : risks)
        if (token.beginTime <= risk.last && token.endTime >= risk.first) { token.issue = "unsupported_context"; break; }
    const auto sustained = [&](int id) {
        if (!tokens.at(id).tracks.isEmpty()) return true;
        for (int primitive : tokens.at(id).primitives) if (primitives.at(primitive).marker.type == "hold"
            || primitives.at(primitive).marker.type == "touch_hold") return true;
        return false;
    };
    const qint64 auditReserve = qMin(p.maxWorkUnits / 10, qMax(qint64(1000), qint64(parsed.noteMarkers.size()) * 3000));
    qint64 work = p.maxWorkUnits - auditReserve;
    QVector<bool> included(tokens.size(), false);
    const double softHorizon = qMax(0.35, p.contextSeconds);
    for (int i = 0; i < tokens.size(); ++i) if (tokens.at(i).target || tokens.at(i).partial) {
        for (int j = qMax(0, i - 1); j <= qMin(int(tokens.size()) - 1, i + 2); ++j)
            if (tokens.at(j).issue.isEmpty() && std::abs(tokens.at(j).nominal - tokens.at(i).nominal) <= softHorizon) included[j] = true;
        // Import an ongoing input/queue directly affecting this target, without
        // recursively importing every object whose broad time window overlaps it.
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty() && sustained(j)
            && tokens.at(j).beginTime <= tokens.at(i).endTime && tokens.at(j).endTime >= tokens.at(i).beginTime)
            included[j] = true;
    }
    // A visible sustained assumption must respect every explicit lock it spans.
    // Such locks do not grow a new soft neighbourhood of their own.
    const auto imported = included;
    for (int i = 0; i < tokens.size(); ++i) if (imported.at(i) && sustained(i))
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty() && tokens.at(j).protectedText
            && tokens.at(j).nominal <= tokens.at(i).endTime && tokens.at(j).endTime >= tokens.at(i).beginTime)
            included[j] = true;
    // Import direct native consumers of the initially visible trajectories.
    // This is one generation only: a lookahead star does not recursively pull
    // its whole future track neighbourhood into the original request.
    QVector<QSet<int>> trackConsumers(tracks.size());
    for (int track = 0; track < tracks.size() && work >= 0; ++track) if (imported.at(tracks.at(track).token)) {
        for (int other = 0; other < tokens.size() && work >= 0; ++other) {
            if (!spend(work)) break;
            if (other == tracks.at(track).token || !tokens.at(other).issue.isEmpty()) continue;
            for (const auto& marker : tokens.at(other).markers)
                if (possibleSlideConsumer(tracks.at(track), marker, work, inputOffsetLimit)) {
                    trackConsumers[track].insert(other); included[other] = true;
                    break;
                }
        }
    }
    if (work < 0) result.workLimited = result.truncated = true;
    const auto groups = buildHandOrderTouchGroups(touches);
    for (const auto& group : groups) {
        bool any = false;
        for (int id : group) for (int i = 0; i < tokens.size(); ++i)
            if (tokens.at(i).touches.contains(id)) any |= included.at(i);
        if (any) for (int id : group) for (int i = 0; i < tokens.size(); ++i)
            if (tokens.at(i).touches.contains(id) && tokens.at(i).issue.isEmpty()) included[i] = true;
    }
    // Complete every nominally simultaneous set, including ordinary slash
    // tokens outside the one-before/two-after soft-neighbour count.
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i))
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty()
            && std::abs(tokens.at(i).nominal - tokens.at(j).nominal) < epsilon) included[j] = true;
    // A later DOWN is not free to ignore an earlier waiting head on the same
    // channel. Import those real consumers as read-only hard context, then
    // execute them through the same bounded rolling windows (never one beam).
    QVector<QSet<QString>> emitted(tokens.size());
    QMap<QString, QVector<int>> consumers;
    for (int id = 0; id < tokens.size(); ++id) for (const auto& marker : tokens.at(id).markers) {
        const bool touch = marker.type == "touch" || marker.type == "touch_hold";
        const bool head = marker.type == "tap" || marker.type == "hold"
            || ((marker.type == "slide" || marker.type == "wifi") && marker.hasHeadStar);
        if (!touch && !head) continue;
        const QString pad = touch ? marker.touchPad : QStringLiteral("A%1").arg(marker.lane);
        consumers[pad].append(id);
        if (head) {
            const QString button = QStringLiteral("Button%1").arg(marker.lane);
            consumers[button].append(id); emitted[id].insert(button);
        }
        const auto point = touchPadPoint(pad);
        for (const auto& ring : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("E")})
            for (int lane = 1; lane <= (ring == "C" ? 1 : 8); ++lane) {
                const QString reached = ring == "C" ? ring : ring + QString::number(lane);
                if (QLineF(point, touchPadPoint(reached)).length() <= TouchJudgeProfile::handRadius + muri::padRadius(reached) / referenceRadius + epsilon)
                    emitted[id].insert(reached);
            }
    }
    QVector<int> queue;
    for (int id = 0; id < tokens.size(); ++id) if (included.at(id)) queue.append(id);
    const auto includeConsumer = [&](int id) {
        if (included.at(id) || !tokens.at(id).issue.isEmpty()) return;
        included[id] = true; queue.append(id);
    };
    for (int at = 0; at < queue.size(); ++at) {
        task::CancellationScope::checkpoint(); const int id = queue.at(at);
        for (int other = 0; other < tokens.size(); ++other)
            if (std::abs(tokens.at(other).nominal - tokens.at(id).nominal) < epsilon) includeConsumer(other);
        for (const auto& group : groups) {
            bool member = false;
            for (int touch : group) member |= tokens.at(id).touches.contains(touch);
            if (member) for (int touch : group) for (int other = 0; other < tokens.size(); ++other)
                if (tokens.at(other).touches.contains(touch)) includeConsumer(other);
        }
        for (int touch : tokens.at(id).touches) {
            if (touches.at(touch).sweepPrevious >= 0)
                for (int other = 0; other < tokens.size(); ++other)
                    if (tokens.at(other).touches.contains(touches.at(touch).sweepPrevious))
                        includeConsumer(other);
            for (int next = 0; next < touches.size(); ++next)
                if (touches.at(next).sweepPrevious == touch)
                    for (int other = 0; other < tokens.size(); ++other)
                        if (tokens.at(other).touches.contains(next)) includeConsumer(other);
        }
        if (p.maxTasks >= 16) for (int primitive : tokens.at(id).primitives) {
            for (int previous : {primitives.at(primitive).holdRelayPrevious,
                                 primitives.at(primitive).sweepPrevious,
                                 primitives.at(primitive).reboundPrevious,
                                 primitives.at(primitive).alternateSweepPrevious})
                if (previous >= 0) includeConsumer(primitives.at(previous).token);
            for (int next = 0; next < primitives.size(); ++next)
                if (primitives.at(next).holdRelayPrevious == primitive
                    || primitives.at(next).sweepPrevious == primitive
                    || primitives.at(next).reboundPrevious == primitive
                    || primitives.at(next).alternateSweepPrevious == primitive)
                    includeConsumer(primitives.at(next).token);
        }
        const double early = tokens.at(id).touches.isEmpty() ? FullReplayProfile::tapCriticalSeconds : TouchJudgeProfile::criticalSeconds;
        for (const auto& channel : emitted.at(id)) for (int other : consumers.value(channel))
            if (tokens.at(other).nominal < tokens.at(id).nominal - epsilon
                && tokens.at(other).nominal + FullReplayProfile::tapAvailableSeconds >= tokens.at(id).nominal - early - epsilon) includeConsumer(other);
    }
    for (bool value : included) result.includedObjects += value;
    QVector<Decision> decisions;
    for (int i = 0; i < primitives.size(); ++i) if (included.at(primitives.at(i).token)) decisions.append({Decision::Fixed, i, {}, primitives.at(i).action.start, primitives.at(i).action.end, primitives.at(i).lane});
    for (int i = 0; i < tracks.size(); ++i) if (included.at(tracks.at(i).token) && tracks.at(i).sector != 1) decisions.append({Decision::Slide, i, {}, tracks.at(i).action.start,
        tracks.at(i).action.end + FullReplayProfile::slideAvailableSeconds, 0});
    for (const auto& group : groups) {
        QVector<int> members; double last = -1e9;
        for (int id : group) for (int i = 0; i < tokens.size(); ++i) if (included.at(i) && tokens.at(i).touches.contains(id)) {
            members.append(id); last = qMax(last, touches.at(id).second + TouchJudgeProfile::availableSeconds);
        }
        if (!members.isEmpty()) decisions.append({Decision::Touches, -1, members, last, last, 0});
    }
    std::stable_sort(decisions.begin(), decisions.end(), [](const auto& a, const auto& b) { return std::tie(a.second, a.kind, a.lane, a.end) < std::tie(b.second, b.kind, b.lane, b.end); });
    QVector<int> assigned(primitives.size(), -1); QVector<QVector<HandOrderTrackStop>> planned(tracks.size());
    QVector<bool> plannedWifiSingle(tracks.size(), false);
    QVector<FullReplayJudgement> fullJudged(parsed.noteMarkers.size());
    QVector<TouchJudgement> judged(touches.size());
    QVector<double> costs(tokens.size(), 0), peaks(tokens.size(), 0), gaps(tokens.size(), 0);
    QVector<double> playerCosts(tokens.size(), 0), tapRates(tokens.size(), 0), loads(tokens.size(), 0);
    QVector<bool> timingAdjusted(tokens.size(), false);
    QVector<int> tierGaps(tokens.size(), 0); QVector<bool> counter(tokens.size(), false), participated(tokens.size(), false);
    QVector<HandOrderHand> counterHands(tokens.size(), HandOrderHand::Unspecified);
    QVector<int> counterLanes(tokens.size(), 0);
    QVector<BasicHandoffEvidence> transferEvidence(tracks.size());
    QVector<QVector<BasicCounterfactualEvidence>> confidenceEvidence(tokens.size());
    QVector<int> parent(tokens.size());
    for (int i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto root = [&](int id) { while (parent.at(id) != id) id = parent.at(id); return id; };
    const auto join = [&](int a, int b) {
        a = root(a); b = root(b); parent[qMax(a, b)] = qMin(a, b);
    };
    const auto orderedTokens = [](const QSet<int>& values) {
        auto ordered = values.values(); std::sort(ordered.begin(), ordered.end()); return ordered;
    };
    const auto firstNominal = [&](const QSet<int>& values) {
        double first = std::numeric_limits<double>::infinity();
        for (int id : values) first = qMin(first, tokens.at(id).nominal);
        return first;
    };
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i))
        for (int j = i + 1; j < tokens.size(); ++j) if (included.at(j)
            && std::abs(tokens.at(i).nominal - tokens.at(j).nominal) < epsilon) join(i, j);
    for (const auto& group : groups) {
        int first = -1;
        for (int touch : group) for (int id = 0; id < tokens.size(); ++id)
            if (included.at(id) && tokens.at(id).touches.contains(touch)) {
                if (first >= 0) join(first, id); else first = id;
            }
    }
    for (int touch = 0; touch < touches.size(); ++touch) {
        const int previous = touches.at(touch).sweepPrevious;
        if (previous < 0) continue;
        for (int a = 0; a < tokens.size(); ++a) if (included.at(a) && tokens.at(a).touches.contains(previous))
            for (int b = 0; b < tokens.size(); ++b) if (included.at(b) && tokens.at(b).touches.contains(touch))
                join(a, b);
    }
    if (p.maxTasks >= 16) for (int primitive = 0; primitive < primitives.size(); ++primitive) {
        for (int previous : {primitives.at(primitive).holdRelayPrevious,
                             primitives.at(primitive).sweepPrevious,
                             primitives.at(primitive).reboundPrevious,
                             primitives.at(primitive).alternateSweepPrevious})
            if (previous >= 0 && included.at(primitives.at(previous).token)
                && included.at(primitives.at(primitive).token))
                join(primitives.at(previous).token, primitives.at(primitive).token);
    }
    // Prepare the first stroke with the same continuation known to later
    // strokes. Otherwise its head/tap choices are committed before the
    // returning trace is ever considered.
    for (const auto& track : tracks) if (track.oneStrokePrevious >= 0) {
        const int previous = tracks.at(track.oneStrokePrevious).token;
        if (included.at(previous) && included.at(track.token)) join(previous, track.token);
    }
    QMap<int, QSet<int>> hardGroups;
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i)) hardGroups[root(i)].insert(i);
    QVector<QSet<int>> cores;
    for (const auto& group : hardGroups) cores.append(group);
    std::stable_sort(cores.begin(), cores.end(), [&](const auto& a, const auto& b) {
        return firstNominal(a) < firstNominal(b);
    });
    QVector<int> coreForToken(tokens.size(), -1);
    for (int index = 0; index < cores.size(); ++index)
        for (int token : cores.at(index)) coreForToken[token] = index;
    State committed; committed.inputPreference = p.inputPreference; committed.returnSpeed = p.returnSpeed;
    committed.player = p.player;
    setPlayerActionKinds(committed, primitives, touches, p.sweepThreshold);
    committed.primitiveHands.fill(-1, primitives.size()); committed.tracks.resize(tracks.size()); committed.wifiSingle.fill(false, tracks.size());
    QSet<int> committedTokens;
    QHash<int, int> tokenForSource;
    for (int id = 0; id < tokens.size(); ++id) tokenForSource.insert(tokens.at(id).start, id);
    QVector<TimelineNoteMarker> committedMarkers;
    double historyThrough = -std::numeric_limits<double>::infinity();
    std::array<PlayerPathState, 2> historyEnds;
    QVector<Scheduled> pendingHistory;
    int historyActionCount = 0;
    // Optional evidence must not consume the inexpensive ordinary main route
    // that can already fit the request. Keep a conservative work floor for
    // the remaining rolling solve and its eventual combined audit.
    qint64 optionalRemaining = qMin(p.maxWorkUnits / 3,
        qMax(qint64(0), p.maxWorkUnits - qint64(result.includedObjects) * 1000));
    int counterfactualRemaining = p.counterfactualBudget;
    int dependencyUnit = 0;
    if (progress) progress({HandOrderProgress::Windows, 0, int(cores.size())});
    for (int coreIndex = 0; coreIndex < cores.size(); ++coreIndex) {
        auto core = cores.at(coreIndex);
        for (int id : committedTokens) core.remove(id);
        bool relevant = false;
        for (int id : core) relevant |= tokens.at(id).issue.isEmpty() && !committedTokens.contains(id);
        if (!relevant) {
            if (progress) progress({HandOrderProgress::Windows, coreIndex + 1, int(cores.size())});
            continue;
        }
        ++dependencyUnit; ++result.windowCount;
        task::CancellationScope::check();
        QSet<int> windowTokens = core;
        bool ordinaryCore = true, incomingPhase = false;
        for (int id : core) ordinaryCore &= !sustained(id) && tokens.at(id).touches.isEmpty();
        for (int next = coreIndex + 1; next <= qMin(coreIndex + 2, int(cores.size()) - 1); ++next) {
            bool ordinaryNext = true;
            for (int id : cores.at(next)) ordinaryNext &= !sustained(id) && tokens.at(id).touches.isEmpty();
            bool immediateJudgementCoupling = false;
            for (int id : core) for (int other : cores.at(next))
                immediateJudgementCoupling |= tokens.at(other).beginTime <= tokens.at(id).nominal + p.pressSeconds
                    && tokens.at(other).nominal >= tokens.at(id).nominal;
            if ((!ordinaryCore || ordinaryNext || immediateJudgementCoupling)
                && firstNominal(cores.at(next)) - firstNominal(core) <= softHorizon)
                windowTokens.unite(cores.at(next));
        }
        // Reach one step across a structural boundary before committing the
        // entering hand, without putting an entire dense chart in one beam.
        if (coreIndex + 1 < cores.size()) {
            for (int id : cores.at(coreIndex + 1)) for (int primitive : tokens.at(id).primitives) {
                const int previous = primitives.at(primitive).phasePrevious;
                incomingPhase |= previous >= 0 && core.contains(primitives.at(previous).token);
            }
            if (incomingPhase) windowTokens.unite(cores.at(coreIndex + 1));
        }
        // A trajectory must be ranked with the later taps its real screen
        // contact can consume. Expanding only this core's new tracks keeps
        // ordinary windows bounded and avoids recursive lookahead growth.
        for (int owner : core) for (int track : tokens.at(owner).tracks)
            for (int consumer : trackConsumers.at(track)) if (!committedTokens.contains(consumer)) {
                const int group = coreForToken.at(consumer);
                if (group >= 0) windowTokens.unite(cores.at(group));
            }
        // A moving trace must also be compared with the free hand's actual
        // tasks, even when it cannot consume their input. This is bounded
        // to new core tracks and one generation of coherent head groups.
        for (int owner : core) for (int trackId : tokens.at(owner).tracks) {
            const auto& track = tracks.at(trackId);
            for (int id = 0; id < tokens.size(); ++id) if (included.at(id)
                && !committedTokens.contains(id) && tokens.at(id).nominal >= track.headSecond - epsilon
                && tokens.at(id).nominal <= track.action.end + inputOffsetLimit + epsilon) {
                const int group = coreForToken.at(id);
                if (group >= 0) windowTokens.unite(cores.at(group));
            }
        }
        double horizon = std::numeric_limits<double>::infinity(), last = -horizon;
        for (int id : core) { horizon = qMin(horizon, tokens.at(id).beginTime); last = qMax(last, tokens.at(id).endTime); }
        int protectedTargetImports = 0, protectedContextImports = 0;
        for (int id = 0; id < tokens.size(); ++id) if (included.at(id) && !committedTokens.contains(id)
            && tokens.at(id).protectedText && tokens.at(id).beginTime <= last && tokens.at(id).endTime >= horizon
            && (!p.evaluateCompleteAssignment || !tokens.at(id).target || p.legacyFixedTargetLookahead)) {
            if (!windowTokens.contains(id)) {
                if (tokens.at(id).target) ++protectedTargetImports;
                else ++protectedContextImports;
            }
            windowTokens.insert(id);
        }
        for (int id : windowTokens) horizon = qMin(horizon, tokens.at(id).beginTime);
        State seed = committed; seed.actions.clear(); seed.physical.clear(); seed.fullReplay = {}; seed.replay = {};
        seed.cost = seed.peak = seed.risk = 0; seed.origins = {};
        seed.scoreFrom = horizon; seed.focusSources.clear();
        seed.focusSecond = -std::numeric_limits<double>::infinity(); seed.focusOrdinal = 0;
        for (int id : core) {
            seed.focusSources.insert(tokens.at(id).start);
            seed.focusSecond = qMax(seed.focusSecond, tokens.at(id).nominal); seed.focusOrdinal = qMax(seed.focusOrdinal, id);
        }
        QSet<int> retainedSources;
        for (int id : committedTokens) if (tokens.at(id).endTime >= horizon - epsilon)
            retainedSources.insert(tokens.at(id).start);
        for (const auto& action : committed.actions) if (action.action.end >= horizon - epsilon)
            retainedSources.insert(action.action.tokenStart);
        // A carried head may itself depend on a still earlier carried head.
        // Replay needs that complete consumed-input ancestry, not just one
        // generation of sources; otherwise a valid historical star reappears
        // without the DOWN that originally served it in the next window.
        QVector<int> ancestry = retainedSources.values().toVector();
        std::sort(ancestry.begin(), ancestry.end());
        for (int cursor = 0; cursor < ancestry.size(); ++cursor) {
            if (!spend(work)) break;
            const int id = tokenForSource.value(ancestry.at(cursor), -1);
            if (id < 0 || !committedTokens.contains(id)) continue;
            for (const auto& marker : tokens.at(id).markers) {
                const auto& full = fullJudged.value(marker.parseOrder);
                auto causes = full.causes; causes += full.headCauses;
                for (const auto& cause : causes) if (!retainedSources.contains(cause.tokenStart)) {
                    retainedSources.insert(cause.tokenStart); ancestry.append(cause.tokenStart);
                }
            }
        }
        for (const auto& action : committed.actions) if (action.action.end >= horizon - epsilon || retainedSources.contains(action.action.tokenStart)) {
            seed.actions.append(action);
        }
        for (const auto& action : committed.actions) if (action.action.end < horizon - epsilon) {
            auto& origin = seed.origins[action.action.hand];
            if (!origin.known || action.action.end > origin.free) origin = {action.action.end,
                contactPosition(action.action, action.action.end), action.lane, true, action.ordinal, action.action.start};
        }
        QSet<int> blockTokens = windowTokens, touchIds;
        for (int id : committedTokens) if (tokens.at(id).endTime >= horizon - epsilon) blockTokens.insert(id);
        for (const auto& action : seed.actions) for (int id : committedTokens)
            if (tokens.at(id).start == action.action.tokenStart) blockTokens.insert(id);
        QVector<Decision> block;
        const auto decisionInWindow = [&](const Decision& d) {
            if (d.kind == Decision::Fixed) return windowTokens.contains(primitives.at(d.index).token)
                && !committedTokens.contains(primitives.at(d.index).token);
            if (d.kind == Decision::Slide) return windowTokens.contains(tracks.at(d.index).token)
                && !committedTokens.contains(tracks.at(d.index).token);
            for (int touch : d.members) for (int id : windowTokens)
                if (!committedTokens.contains(id) && tokens.at(id).touches.contains(touch)) return true;
            return false;
        };
        for (const auto& decision : decisions) if (decisionInWindow(decision)) block.append(decision);
        for (int id : blockTokens) for (int touch : tokens.at(id).touches) touchIds.insert(touch);
        for (int id : core) tokens[id].dependencyUnit = dependencyUnit;
        QVector<TouchTarget> localTouches; QMap<int, int> localIndex;
        QList<int> orderedIds = touchIds.values(); std::sort(orderedIds.begin(), orderedIds.end());
        for (int id : orderedIds) {
            localIndex[id] = localTouches.size(); auto target = touches.at(id);
            for (int owner : committedTokens) if (tokens.at(owner).touches.contains(id) && !target.hold)
                target.lock = maskHand(judged.at(id).handMask);
            localTouches.append(target);
        }
        for (auto& d : block) if (d.kind == Decision::Touches) for (int& id : d.members) id = localIndex.value(id);
        for (int id : orderedTokens(blockTokens)) for (int touch : tokens.at(id).touches) if (touches.at(touch).hold && !localIndex.contains(touch)) {
            localIndex[touch] = localTouches.size(); localTouches.append(touches.at(touch));
        }
        for (auto& touch : localTouches)
            if (touch.sweepPrevious >= 0)
                touch.sweepPrevious = localIndex.value(touch.sweepPrevious, -1);
        QVector<TimelineNoteMarker> blockMarkers;
        for (int id : orderedTokens(blockTokens)) blockMarkers += tokens.at(id).markers;
        QString failure;
        const int tasks = block.size() + localTouches.size() + seed.actions.size();
        result.maxWindowTasks = qMax(result.maxWindowTasks, tasks);
        if (tasks > p.maxTasks) { failure = "task_limit"; result.taskLimited = true; }
        const qint64 beforeMain = qMax(qint64(0), work);
        const int remainingWindows = qMax(1, int(cores.size()) - coreIndex);
        const qint64 mainGrant = qMin(beforeMain, remainingWindows <= 16 ? beforeMain
            : qMax(qint64(12000), beforeMain * 4 / remainingWindows));
        qint64 mainReserved = beforeMain - mainGrant;
        work = mainGrant;
        State history = committed;
        // Only accepted physical history contributes to the next entry state;
        // the candidate's uncommitted lookahead must never accumulate fatigue.
        bool reuseHistory = std::isfinite(historyThrough) && horizon >= historyThrough - epsilon;
        for (int i = historyActionCount; reuseHistory && i < committed.actions.size(); ++i)
            reuseHistory &= committed.actions.at(i).action.start >= historyThrough - epsilon;
        if (!reuseHistory) {
            pendingHistory = committed.actions;
            historyEnds = {};
            historyThrough = -std::numeric_limits<double>::infinity();
        } else {
            for (int i = historyActionCount; i < committed.actions.size(); ++i)
                pendingHistory.append(committed.actions.at(i));
        }
        history.physical = pendingHistory;
        history.scoreFrom = historyThrough;
        history.effortOrigins = historyEnds;
        const bool historyReady = scorePlayerEffort(history, work, horizon);
        if (!historyReady && failure.isEmpty())
            failure = work < 0 ? QStringLiteral("work_limit") : QStringLiteral("player_history_invalid");
        seed.effortOrigins = history.effortEnds;
        if (historyReady) {
            historyThrough = horizon;
            historyEnds = history.effortEnds;
            historyActionCount = committed.actions.size();
            pendingHistory.erase(std::remove_if(pendingHistory.begin(), pendingHistory.end(), [&](const auto& action) {
                return action.action.end <= horizon + epsilon;
            }), pendingHistory.end());
        } else {
            historyThrough = -std::numeric_limits<double>::infinity();
        }
        bool carryResponsibility = false;
        int ordinaryTracks = 0;
        for (const auto& decision : block) carryResponsibility |= decision.kind == Decision::Touches && decision.members.size() >= 3;
        for (const auto& decision : block) ordinaryTracks += decision.kind == Decision::Slide && !tracks.at(decision.index).wifi;
        int searchAttempt = 0;
        int searchRequests = 0, searchCacheHits = 0, repeatedCompleteSearches = 0;
        qint64 repeatedCompleteWork = 0, reusedSearchWork = 0;
        struct CachedAttempt { QByteArray key; Search result; qint64 work = 0; };
        QVector<CachedAttempt> completedAttempts;
        const auto attemptKey = [&](const QVector<int>& limits, const SearchConstraint& constraint,
            bool evidence, int beamWidth, int breadth, double offset) {
            QByteArray key;
            const auto add = [&](const auto& value) { key.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
            const int size = limits.size(); add(size);
            for (int limit : limits) add(limit);
            add(constraint.primitive); add(constraint.track); add(constraint.touch); add(constraint.hand);
            add(constraint.progress); add(evidence); add(beamWidth); add(breadth); add(offset);
            return key;
        };
        const auto runSearch = [&](const QVector<int>& limits, const SearchConstraint& constraint = SearchConstraint{}, bool evidence = false) {
            Search combined;
            auto narrow = p;
            if (!evidence && !carryResponsibility) narrow.beamWidth = qMin(8, p.beamWidth);
            for (int pass = 0; pass < (narrow.beamWidth < p.beamWidth ? 2 : 1); ++pass) {
            struct TimingAttempt { int breadth; double offset; };
            const double automaticTiming = std::numeric_limits<double>::quiet_NaN();
            QVector<TimingAttempt> proposals{{0, automaticTiming}};
            // A coherent early chain can survive the original judge even when
            // independently ranked offsets lose every consistent combination.
            // Keep the nominal fast path first and use only existing offsets.
            if (ordinaryTracks >= 2 && !std::isfinite(p.diagnosticSlideInputOffsetSeconds))
                for (double offset : {-2.0 / FullReplayProfile::ticksPerSecond, -0.05, -0.1, -maximumSlideInputOffset})
                    proposals.append({0, offset});
            proposals.append({1, automaticTiming}); proposals.append({2, automaticTiming});
            for (const auto& proposal : proposals) {
                // Try alternative physical channels before spending the whole
                // window on a wider beam of the same impossible channel.
                const int attemptNumber = searchAttempt + 1;
                const auto onProgress = [&](HandOrderProgress update) {
                    update.window = coreIndex + 1; update.windows = cores.size(); update.attempt = attemptNumber;
                    if (progress) progress(update);
                };
                auto attemptParameters = pass == 0 ? narrow : p;
                if (std::isfinite(proposal.offset)) attemptParameters.diagnosticSlideInputOffsetSeconds = proposal.offset;
                task::CancellationScope::checkpoint();
                ++searchRequests;
                const auto key = attemptKey(limits, constraint, evidence, attemptParameters.beamWidth,
                    proposal.breadth, attemptParameters.diagnosticSlideInputOffsetSeconds);
                const auto cached = std::find_if(completedAttempts.cbegin(), completedAttempts.cend(),
                    [&](const auto& entry) { return entry.key == key; });
                Search attempt;
                bool reused = false;
                if (cached != completedAttempts.cend()) {
                    ++repeatedCompleteSearches; repeatedCompleteWork += cached->work;
                    if (p.reuseWindowSearch && work >= 0) {
                        task::CancellationScope::check();
                        attempt = cached->result;
                        ++searchCacheHits; reusedSearchWork += cached->work; reused = true;
                    }
                    const int index = std::distance(completedAttempts.cbegin(), cached);
                    auto recent = completedAttempts.takeAt(index);
                    if (reused) completedAttempts.append(std::move(recent));
                }
                if (!reused) {
                    const qint64 beforeAttempt = work;
                    ++searchAttempt;
                    attempt = search(block, primitives, tracks, localTouches, blockMarkers,
                        attemptParameters, limits, work, constraint, &seed, proposal.breadth,
                        progress ? HandOrderProgressCallback(onProgress) : HandOrderProgressCallback{});
                    if (!attempt.exhausted && work >= 0) {
                        if (completedAttempts.size() >= 16) completedAttempts.removeFirst();
                        // Qt value containers detach when callers later update
                        // transfer evidence or conditional Touch stability.
                        completedAttempts.append({key, p.reuseWindowSearch ? attempt : Search{}, beforeAttempt - work});
                    }
                }
                if (!reused && p.captureRejectedCandidates) {
                    for (auto it = attempt.rejectionCounts.cbegin(); it != attempt.rejectionCounts.cend(); ++it)
                        result.rejectionCounts[it.key()] += it.value();
                    for (auto sample : attempt.rejectionSamples) {
                        if (result.rejectionSamples.size() >= 12) break;
                        sample.window = coreIndex + 1; result.rejectionSamples.append(std::move(sample));
                    }
                }
                attempt.sideEffect |= combined.sideEffect;
                combined = std::move(attempt);
                if (combined.found || combined.exhausted) return combined;
            }
            }
            return combined;
        };
        QVector<int> caps(tracks.size(), 0);
        auto best = failure.isEmpty() ? runSearch(caps) : Search{};
        if (failure.isEmpty() && !best.found && !best.exhausted) {
            caps.fill(1); best = runSearch(caps);
        }
        if (failure.isEmpty() && !best.found && !best.exhausted) {
            caps.fill(3); best = runSearch(caps);
        }
        if (failure.isEmpty() && !best.found && best.exhausted) {
            const qint64 futureFloor = qint64(qMax(0, remainingWindows - 1)) * 1000;
            const qint64 extra = qMin(qMax(qint64(0), mainReserved - futureFloor),
                qMax(qint64(240000), mainGrant * 4));
            if (extra > mainGrant) {
                mainReserved -= extra; work = extra;
                best = runSearch(caps);
                if (!best.found && !best.exhausted) {
                    caps.fill(1); best = runSearch(caps);
                }
            }
        }
        bool pressureLimited = false, hasTrack = false;
        bool confidenceIterationLimited = false;
        for (const auto& decision : block) hasTrack |= decision.kind == Decision::Slide && tracks.at(decision.index).locks.isEmpty()
            && !tracks.at(decision.index).legacyAnnotated;
        if (best.found && best.state.focusPeak >= 12.0 && hasTrack && caps.value(0) == 0 && work > 0) {
            auto expanded = caps; expanded.fill(1);
            const qint64 grant = qMin(work, qMax(qint64(1000), mainGrant / 4));
            const qint64 reserved = work - grant; work = grant;
            auto improved = runSearch(expanded);
            work = qMax(qint64(0), work) + reserved;
            pressureLimited = improved.exhausted;
            result.truncated |= improved.truncated || improved.exhausted;
            if (improved.found && better(improved.state, best.state)) { best = std::move(improved); caps = expanded; }
        }
        result.truncated |= best.truncated;
        const auto admitTransfers = [&](Search candidate, const SearchConstraint& required = SearchConstraint{}, const QVector<int>* candidateCaps = nullptr) {
            auto localCaps = candidateCaps ? *candidateCaps : caps;
            bool retry = candidate.found;
            while (retry) {
                retry = false;
                candidate.state.transferEvidence.fill({}, tracks.size());
                QVector<int> trackIds;
                for (const auto& d : block) if (d.kind == Decision::Slide) {
                    trackIds.append(d.index);
                    if (tracks.at(d.index).wifi && !candidate.state.wifiSingle.at(d.index)) trackIds.append(tracks.at(d.index).peer);
                }
                for (int id : trackIds) {
                    auto& proof = candidate.state.transferEvidence[id];
                    proof.branch = tracks.at(id).branch; proof.sector = tracks.at(id).sector;
                    proof.handoffCount = switches(candidate.state.tracks.at(id));
                    if (proof.handoffCount > 0) proof.reason = tracks.at(id).locks.isEmpty()
                        ? QStringLiteral("single_handoff_occupation") : QStringLiteral("existing_handoff_constraint");
                }
                for (int id : trackIds) if (tracks.at(id).locks.isEmpty() && switches(candidate.state.tracks.at(id)) > 1) {
                    auto limited = localCaps; limited[id] = 1;
                    const auto baseline = runSearch(limited, required, true);
                    result.truncated |= baseline.truncated;
                    if (baseline.exhausted) { candidate.exhausted = true; candidate.found = false; return candidate; }
                    const double costGain = baseline.found ? baseline.state.cost - candidate.state.cost : 0;
                    const double peakGain = baseline.found ? baseline.state.peak - candidate.state.peak : 0;
                    const bool clearPeak = baseline.found && peakGain >= 4.0 && peakGain >= baseline.state.peak * 0.2;
                    const bool clearCost = baseline.found && costGain >= 2.0 && costGain >= baseline.state.cost * 0.2 && candidate.state.peak <= baseline.state.peak + epsilon;
                    const bool resolves = !baseline.found && !baseline.truncated;
                    auto& proof = candidate.state.transferEvidence[id];
                    proof.baselineFound = baseline.found;
                    proof.baselineCost = baseline.found ? baseline.state.cost : 0; proof.baselinePeak = baseline.found ? baseline.state.peak : 0;
                    if (baseline.found && better(baseline.state, candidate.state)) {
                        candidate = baseline; retry = true; break;
                    }
                    if (resolves || clearPeak || clearCost) proof.reason = resolves ? "one_handoff_candidate_not_found" : "significant_context_improvement";
                    else {
                        localCaps[id] = 1; candidate = baseline; retry = candidate.found; break;
                    }
                }
            }
            return candidate;
        };
        best = admitTransfers(std::move(best));
        if (failure.isEmpty() && (best.exhausted || work < 0)) {
            result.workLimited = true;
            if (!best.found) failure = mainReserved > 0 ? "window_work_limit" : "work_limit";
            else { pressureLimited = true; result.evidenceTruncated = result.truncated = true; }
        }
        work = qMax(qint64(0), work) + mainReserved;
        if (failure.isEmpty() && !best.found) failure = best.sideEffect ? "full_replay_target_failed" : "candidate_search_failed";
        if (failure.isEmpty()) {
            const qint64 futureMainFloor = qint64(qMax(0, remainingWindows - 1)) * 1000;
            const qint64 evidenceGrant = qMin(qMin(qMax(qint64(0), work - futureMainFloor), optionalRemaining),
                qMax(qint64(1000), mainGrant / 2));
            const qint64 reserved = work - evidenceGrant;
            work -= reserved;
            struct Component { int token = -1; QString name; SearchConstraint opposite; };
            const auto componentsFor = [&](const State& state) {
                QVector<Component> components;
                for (const auto& d : block) {
                    if (d.kind == Decision::Fixed) {
                        const auto& primitive = primitives.at(d.index); const auto& token = tokens.at(primitive.token);
                        if (!core.contains(primitive.token) || !token.target || token.protectedText || primitive.lock >= 0) continue;
                        const QString name = token.slide ? QStringLiteral("head") : QStringLiteral("note%1").arg(primitive.lane);
                        components.append({primitive.token, name, {d.index, -1, -1, 1 - state.primitiveHands.at(d.index), 0}});
                    } else if (d.kind == Decision::Slide) {
                        QVector<int> ids{d.index}; const auto& track = tracks.at(d.index);
                        if (track.wifi && !state.wifiSingle.at(d.index)) ids.append(track.peer);
                        for (int id : ids) {
                            const auto& path = tracks.at(id); const auto& token = tokens.at(path.token);
                            if (!core.contains(path.token) || !token.target || token.protectedText || !path.locks.isEmpty()) continue;
                            const auto& stops = state.tracks.at(id);
                            for (int i = 0; i < stops.size(); ++i) {
                                const double last = i + 1 < stops.size() ? stops.at(i + 1).progress : 1.0;
                                const double progress = (stops.at(i).progress + last) / 2;
                                const QString name = QStringLiteral("track:%1:sector:%2").arg(path.branch).arg(path.sector);
                                components.append({path.token, name, {-1, id, -1, 1 - fixedHand(stops.at(i).hand), progress}});
                            }
                        }
                    } else for (int id : d.members) {
                        const auto& touch = localTouches.at(id); if (touch.hold || touch.lock >= 0) continue;
                        int tokenId = -1;
                        for (int token : blockTokens) if (tokens.at(token).start == touch.tokenStart) tokenId = token;
                        if (tokenId < 0 || !core.contains(tokenId) || !tokens.at(tokenId).target || tokens.at(tokenId).protectedText) continue;
                        const int hand = maskHand(state.replay.judgements.at(id).handMask);
                        components.append({tokenId, QStringLiteral("touch"), {-1, -1, id, hand < 0 ? -1 : 1 - hand, 0}});
                    }
                }
                return components;
            };
            bool evidenceLimited = pressureLimited, settled = false;
            for (int pass = 0; pass < 3 && !settled && !evidenceLimited; ++pass) {
                for (int token : core) { confidenceEvidence[token].clear(); counter[token] = false; }
                const auto components = componentsFor(best.state);
                bool changed = false; int count = 0;
                for (const auto& component : components) {
                    BasicCounterfactualEvidence proof; proof.component = component.name;
                    proof.progress = component.opposite.progress; proof.alternativeHand = namedHand(component.opposite.hand);
                    if (component.opposite.hand < 0 || counterfactualRemaining <= 0 || work <= 0) {
                        proof.limited = true; evidenceLimited |= component.opposite.hand >= 0;
                        confidenceEvidence[component.token].append(proof); continue;
                    }
                    ++count; --counterfactualRemaining; proof.searched = true;
                    auto alternativeCaps = caps;
                    for (int& limit : alternativeCaps) limit = qMax(1, limit);
                    auto alternative = runSearch(alternativeCaps, component.opposite, true);
                    if (!alternative.found && !alternative.exhausted) {
                        alternativeCaps.fill(3); alternative = runSearch(alternativeCaps, component.opposite, true);
                    }
                    if (alternative.found) alternative = admitTransfers(std::move(alternative), component.opposite, &alternativeCaps);
                    result.truncated |= alternative.truncated;
                    proof.limited = best.truncated || alternative.truncated || alternative.exhausted;
                    if (alternative.exhausted) evidenceLimited = true;
                    proof.found = alternative.found;
                    if (alternative.found) {
                        // A counterfactual's temporary lock can remove the
                        // near-equal alternatives that made a carry ambiguous.
                        // It is not new evidence of stable free responsibility.
                        // Keep known same-owner carry proof, but do not let a
                        // conditional carry replace a proven direct stream.
                        for (int i = 0; i < localTouches.size(); ++i) {
                            if (localTouches[i].hold || localTouches[i].lock >= 0) continue;
                            auto& proposed = alternative.state.replay.judgements[i];
                            const auto& current = best.state.replay.judgements[i];
                            if (proposed.kind == QStringLiteral("group_carry")
                                && (current.kind != QStringLiteral("group_carry") || !current.stableResponsibility
                                    || current.handMask != proposed.handMask))
                                proposed.stableResponsibility = false;
                        }
                        if (better(alternative.state, best.state)) { best = std::move(alternative); changed = true; break; }
                        proof.signedCostGap = alternative.state.cost - best.state.cost;
                        proof.signedPeakTierGap = peakTier(alternative.state.focusPeak) - peakTier(best.state.focusPeak);
                        const auto riskTier = [](double risk) { return int(std::ceil(risk / 0.025 - epsilon)); };
                        proof.signedRiskTierGap = riskTier(alternative.state.risk) - riskTier(best.state.risk);
                        const double separation = qMax(0.0, proof.signedCostGap)
                            + (best.state.ordinaryOnly ? 0.0 : 2.0 * qMax(0, proof.signedPeakTierGap)) + 3.0 * qMax(0, proof.signedRiskTierGap);
                        proof.score = 1.0 - std::exp(-separation / 1.5);
                        if (proof.limited) proof.score = qMin(proof.score, 0.74);
                        counter[component.token] = true; gaps[component.token] = proof.signedCostGap;
                        tierGaps[component.token] = proof.signedPeakTierGap; counterHands[component.token] = proof.alternativeHand;
                        if (component.opposite.primitive >= 0) counterLanes[component.token] = primitives.at(component.opposite.primitive).lane;
                        if (proof.signedPeakTierGap == 0 && proof.signedRiskTierGap == 0 && proof.signedCostGap <= 0.25) {
                            for (int i = 0; i < localTouches.size(); ++i) {
                                auto& judged = best.state.replay.judgements[i];
                                if (judged.kind == "group_carry" && judged.handMask != alternative.state.replay.judgements.at(i).handMask)
                                    judged.stableResponsibility = false;
                            }
                        }
                    } else proof.score = proof.limited ? 0 : 0.95;
                    confidenceEvidence[component.token].append(proof);
                }
                settled = !changed;
                if (changed && pass == 2) {
                    // The new best already passed this scope's real replay.
                    // An unfinished optional confidence fixed point is not a
                    // failed motor stream; keep it for the final global audit.
                    evidenceLimited = true; confidenceIterationLimited = true;
                }
            }
            if (evidenceLimited) {
                result.evidenceTruncated = result.truncated = true;
                for (int token : core) {
                    counter[token] = false;
                    for (auto& proof : confidenceEvidence[token]) { proof.limited = true; proof.score = qMin(proof.score, 0.39); }
                }
            }
            optionalRemaining -= evidenceGrant - qMax(qint64(0), work);
            work = qMax(qint64(0), work) + reserved;
        }
        if (!failure.isEmpty()) for (int id : core) tokens[id].issue = failure;
        else {
            // A consumed input from a lookahead object is a real hard dependency.
            // Promote that complete source token, rather than depending on an
            // uncommitted speculative DOWN in an otherwise accepted result.
            bool promoted = true;
            while (promoted) {
                promoted = false;
                for (int id : orderedTokens(core)) for (const auto& marker : tokens.at(id).markers)
                    for (const auto& full : best.state.fullReplay.judgements) if (full.parseOrder == marker.parseOrder) {
                        auto causes = full.causes; causes += full.headCauses;
                        for (const auto& cause : causes) for (int other : committedTokens)
                            if (tokens.at(other).start == cause.tokenStart) join(id, other);
                        for (const auto& cause : causes) for (int other : windowTokens)
                            if (tokens.at(other).start == cause.tokenStart && !core.contains(other)) {
                                for (const auto& hard : cores) if (hard.contains(other)) core.unite(hard);
                                promoted = true;
                            }
                    }
            }
            for (int id : core) {
                participated[id] = true; costs[id] = best.state.cost; peaks[id] = best.state.focusPeak;
                playerCosts[id] = best.state.playerCost;
                timingAdjusted[id] = best.state.adjustedSlideTiming;
                tapRates[id] = best.state.peakTapRate; loads[id] = best.state.peakLoad;
                for (int other : core) join(id, other);
            }
            for (const auto& action : best.state.physical) {
                int owner = -1;
                for (int id : core) if (tokens.at(id).start == action.action.tokenStart) owner = id;
                if (owner < 0 || committedTokens.contains(owner)) continue;
                for (const auto& previous : committed.actions) if (previous.action.hand == action.action.hand
                    && qMax(previous.action.start, action.action.start)
                    < qMin(previous.action.end, action.action.end) - epsilon)
                    for (int other : committedTokens) if (tokens.at(other).start == previous.action.tokenStart) join(owner, other);
                auto fixed = action; fixed.committed = true; committed.actions.append(fixed);
            }
            for (int id : orderedTokens(core)) if (!committedTokens.contains(id)) { committedTokens.insert(id); committedMarkers += tokens.at(id).markers; }
            for (const auto& d : block) {
                if (d.kind == Decision::Fixed && core.contains(primitives.at(d.index).token)) assigned[d.index] = best.state.primitiveHands.at(d.index);
                else if (d.kind == Decision::Slide && core.contains(tracks.at(d.index).token)) {
                    planned[d.index] = best.state.tracks.at(d.index);
                    plannedWifiSingle[d.index] = best.state.wifiSingle.at(d.index);
                    transferEvidence[d.index] = best.state.transferEvidence.value(d.index);
                    if (tracks.at(d.index).wifi) {
                        const int peer = tracks.at(d.index).peer;
                        planned[peer] = best.state.tracks.at(peer);
                        plannedWifiSingle[peer] = best.state.wifiSingle.at(peer);
                        transferEvidence[peer] = best.state.transferEvidence.value(peer);
                    }
                }
            }
            committed.primitiveHands = assigned; committed.tracks = planned; committed.wifiSingle = plannedWifiSingle;
            for (const auto& judgement : best.state.fullReplay.judgements)
                if (judgement.parseOrder >= 0 && judgement.parseOrder < fullJudged.size()) fullJudged[judgement.parseOrder] = judgement;
            for (auto it = localIndex.cbegin(); it != localIndex.cend(); ++it) judged[it.key()] = best.state.replay.judgements.at(it.value());
        }
        BasicWindowEvidence scope; scope.start = text.size(); scope.tasks = tasks;
        scope.coreTokens = core.size(); scope.plannedTokens = windowTokens.size();
        scope.protectedTargetImports = protectedTargetImports; scope.protectedContextImports = protectedContextImports;
        scope.decisionTasks = block.size(); scope.touchTasks = localTouches.size();
        scope.carriedActions = seed.actions.size(); scope.searchAttempts = searchAttempt;
        scope.searchRequests = searchRequests; scope.searchCacheHits = searchCacheHits;
        scope.repeatedCompleteSearches = repeatedCompleteSearches; scope.repeatedCompleteWork = repeatedCompleteWork;
        scope.reusedSearchWork = reusedSearchWork;
        scope.confidenceIterationLimited = confidenceIterationLimited;
        for (int id : core) { scope.start = qMin(scope.start, tokens.at(id).start); scope.end = qMax(scope.end, tokens.at(id).end); }
        scope.workUsed = beforeMain - qMax(qint64(0), work);
        scope.outcome = failure.isEmpty() ? QStringLiteral("validated") : failure;
        result.searchWindows.append(scope);
        if (progress) progress({HandOrderProgress::Windows, coreIndex + 1, int(cores.size())});
    }
    if (progress) progress({HandOrderProgress::Replay});
    // Every retained label is audited against the same combined input stream.
    // Budget exhaustion in a later window does not invent inputs for that window
    // or discard independent, previously validated candidate scopes.
    work = qMax(qint64(0), work) + auditReserve;
    bool completeAudit = false;
    double completeRisk = 0;
    if (!committedMarkers.isEmpty()) {
        QVector<ChartInputAction> inputs;
        for (const auto& action : committed.actions) {
            int sourceOrder = action.touchParseOrder;
            if (const auto it = committed.holdMarkers.constFind(action.action.tokenStart);
                it != committed.holdMarkers.cend() && !action.motion)
                sourceOrder = it->parseOrder;
            inputs.append({action.action, action.button, action.lane, sourceOrder});
        }
        // Unresolved notes still participate in consumption/Touch groups. They
        // need no fabricated autoplay inputs and need not themselves succeed,
        // but may never disappear merely because no label was committed.
        auto audit = replayHandOrderChart(parsed.noteMarkers, inputs, {}, &work);
        completeAudit = audit.valid && audit.meetsTarget && !audit.exhausted
            && committedMarkers.size() == parsed.noteMarkers.size();
        for (const auto& judgement : audit.judgements)
            completeRisk += qMax(0.0, 0.025 - judgement.margin);
        result.auditedObjects = parsed.noteMarkers.size();
        QSet<int> failedUnits;
        if (audit.exhausted) {
            result.workLimited = true;
            for (int id : committedTokens) failedUnits.insert(root(id));
        } else {
            for (const auto& full : audit.judgements) {
                if (full.parseOrder >= 0 && full.parseOrder < fullJudged.size()) fullJudged[full.parseOrder] = full;
                bool valid = full.meetsTarget;
                for (int primitiveId = 0; primitiveId < primitives.size(); ++primitiveId) {
                    const auto& primitive = primitives.at(primitiveId);
                    if (primitive.marker.parseOrder != full.parseOrder || assigned.at(primitiveId) < 0) continue;
                    const int mask = 1 << assigned.at(primitiveId);
                    if (primitive.marker.type == "hold" || primitive.marker.type == "touch_hold") valid &= (full.singleHandCompletionMask & mask) != 0;
                    else {
                        int actual = 0;
                        const bool star = primitive.marker.type == "slide" || primitive.marker.type == "wifi";
                        for (const auto& cause : (star ? full.headCauses : full.causes)) actual |= cause.handMask;
                        valid &= actual == mask;
                    }
                }
                for (int touch = 0; touch < touches.size(); ++touch) if (touches.at(touch).parseOrder == full.parseOrder) {
                    auto& target = judged[touch];
                    if (!touches.at(touch).hold && target.handMask != full.handMask) valid = false;
                    target.judged = full.completed; target.highQuality = full.highQuality;
                    target.handMask = full.handMask; target.second = full.second; target.margin = full.margin;
                    target.quality = full.quality; target.causes = full.causes;
                }
                if (!valid) for (int id : committedTokens) for (const auto& marker : tokens.at(id).markers)
                    if (marker.parseOrder == full.parseOrder) failedUnits.insert(root(id));
            }
        }
        for (int id : committedTokens) if (failedUnits.contains(root(id))) {
            completeAudit = false;
            tokens[id].issue = audit.exhausted ? "work_limit" : "boundary_replay_failed";
            participated[id] = false; confidenceEvidence[id].clear();
            for (int primitive : tokens.at(id).primitives) assigned[primitive] = -1;
            for (int track : tokens.at(id).tracks) planned[track].clear();
        }
        if (p.capturePhysicalReplay && audit.valid && !audit.exhausted)
            result.physicalReplay = std::make_shared<BasicReplayEvidence>(BasicReplayEvidence{std::move(inputs), std::move(audit)});
    }
    if (p.evaluateCompleteAssignment && completeAudit) {
        State full = committed;
        full.origins = {}; full.effortOrigins = {}; full.focusSources.clear();
        full.focusSecond = std::numeric_limits<double>::infinity();
        full.scoreFrom = -std::numeric_limits<double>::infinity();
        // Preserved labels are locks, never free actions in the comparison.
        for (auto& action : full.actions) action.committed = false;
        for (int id = 0; id < primitives.size(); ++id) full.proposedHeads.insert(id);
        if (score(full, tracks, primitives, touches, work)) {
            result.evaluation = {true, full.cost, completeRisk, full.peak, full.playerCost,
                full.peakTapRate, full.peakLoad, full.ergonomicBreaks,
                full.interactionBreaks, full.longitudinalBreaks, full.sameKeyBreaks};
        }
    }
    for (int index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens.at(index);
        if (!token.target && !token.partial && !participated.at(index) && !included.at(index)) continue;
        BasicRow row; row.start = token.start; row.end = token.end; row.line = token.line; row.col = token.col;
        row.context = !token.target; row.originalAnnotation = token.suffix; row.dependencyUnit = root(index);
        row.hasCandidate = participated.at(index);
        row.counterfactualEvidence = confidenceEvidence.at(index);
        bool sufficient = !row.counterfactualEvidence.isEmpty();
        row.confidence = sufficient ? 1.0 : 0;
        for (const auto& proof : row.counterfactualEvidence) {
            sufficient &= proof.searched && !proof.limited;
            row.confidence = qMin(row.confidence, proof.score);
        }
        const double playerPressure = qMax(peaks.at(index) / p.player.comfortableMoveSpeed,
            qMax(tapRates.at(index) / p.player.maxComfortableTapRate, loads.at(index)));
        if (playerPressure > 1.5) { row.confidence = qMin(row.confidence, 0.39); sufficient = false; }
        else if (playerPressure > 1.0) row.confidence = qMin(row.confidence, 0.79);
        row.confidenceLevel = !sufficient ? QStringLiteral("insufficient_evidence")
            : row.confidence >= 0.8 ? QStringLiteral("high") : row.confidence >= 0.4 ? QStringLiteral("medium") : QStringLiteral("low");
        row.cost = costs.at(index); row.peakSpeed = peaks.at(index);
        row.playerCost = playerCosts.at(index); row.peakTapRate = tapRates.at(index); row.peakLoad = loads.at(index);
        if (row.hasCandidate) row.pressureLevel = playerPressure > 1.5 ? QStringLiteral("extreme")
            : playerPressure > 1.0 ? QStringLiteral("high")
            : playerPressure >= 0.66 ? QStringLiteral("medium") : QStringLiteral("low");
        row.hasCounterfactual = counter.at(index);
        row.signedCostGap = gaps.at(index); row.signedPeakTierGap = tierGaps.at(index); row.counterfactualHand = counterHands.at(index);
        for (const auto& proof : row.counterfactualEvidence) if (proof.found) row.counterfactualComponent = proof.component;
        chart_transform::HandOrderTokenEdit edit; edit.baseToken = token.base;
        bool complete = participated.at(index), expressible = true, mixedResponsibility = false; QStringList cluster;
        for (int id : token.primitives) {
            const auto& primitive = primitives.at(id); const auto hand = namedHand(assigned.at(id));
            complete &= hand != HandOrderHand::Unspecified; row.head = hand; edit.hasHead = true; edit.head = hand;
            if (!token.slide) { row.lanes.append(primitive.lane); row.noteHands.append(hand); cluster.append(QString::number(primitive.lane) + '~' + glyph(assigned.at(id))); }
        }
        for (int id : token.tracks) {
            const auto& track = tracks.at(id); if (track.sector == 1) continue;
            const auto& stops = planned.at(id); complete &= !stops.isEmpty();
            row.trackIntervals.append(stops); edit.tracks.append(stops); row.handoffCount += switches(stops);
            HandOrderWifiMode mode = HandOrderWifiMode::Legacy;
            QVector<QVector<HandOrderTrackStop>> sectors;
            if (track.wifi) {
                mode = plannedWifiSingle.at(id) ? (track.legacyAnnotated ? HandOrderWifiMode::Legacy : HandOrderWifiMode::Single)
                    : HandOrderWifiMode::Split;
                if (mode == HandOrderWifiMode::Split) {
                    sectors = {stops, planned.at(track.peer)}; complete &= !sectors.at(1).isEmpty();
                    row.handoffCount += switches(sectors.at(1));
                }
            }
            row.wifiModes.append(mode); row.wifiSectors.append(sectors);
            edit.wifiModes.append(mode); edit.wifiSectors.append(sectors);
            if (mode == HandOrderWifiMode::Split) edit.tracks.last().clear();
            const auto& proof = transferEvidence.at(id); row.handoffEvidence.append(proof);
            if (track.wifi && mode == HandOrderWifiMode::Split) row.handoffEvidence.append(transferEvidence.at(track.peer));
            if (row.handoffReason.isEmpty() && !proof.reason.isEmpty()) {
                row.handoffReason = proof.reason; row.oneHandoffCost = proof.baselineCost; row.oneHandoffPeak = proof.baselinePeak;
            }
            if (!stops.isEmpty()) row.track = stops.first().hand;
        }
        for (int id : token.touches) {
            const auto& judgement = judged.at(id); complete &= judgement.judged && judgement.highQuality;
            row.judgementKind = judgement.kind; row.judgementQuality = judgement.quality;
            row.judgementSecond = judgement.second; row.judgementMargin = judgement.margin; row.causes += judgement.causes;
            if (!touches.at(id).hold) row.head = namedHand(maskHand(judgement.handMask));
            if (!touches.at(id).hold) {
                expressible &= maskHand(judgement.handMask) >= 0 && judgement.stableResponsibility;
                mixedResponsibility |= maskHand(judgement.handMask) < 0;
            }
        }
        if (participated.at(index)) {
            row.fullReplayVerified = true;
            row.conditions = "community_profile_v2; target_critical; finite_physical_contacts; full_candidate_scope_replay; rolling_local; committed_history; player_profile_heuristic; window_peak";
            if (timingAdjusted.at(index)) row.conditions += QStringLiteral("; adjusted_slide_input_timing");
            for (const auto& marker : token.markers) {
                const auto& judgement = fullJudged.value(marker.parseOrder);
                row.fullReplayVerified &= judgement.meetsTarget;
                row.inputChannel = judgement.inputChannel;
                if (token.touches.isEmpty()) {
                    row.judgementKind = judgement.kind; row.judgementQuality = judgement.quality;
                    row.judgementSecond = judgement.second; row.judgementMargin = judgement.margin;
                    row.causes += judgement.causes;
                }
            }
            complete &= row.fullReplayVerified;
        }
        if (row.context) ++result.contextObjects;
        row.reason = token.partial ? "partial_token" : token.issue;
        if (token.partial) row.status = "skipped";
        else if (row.context && token.issue.isEmpty()) { row.status = "context"; row.reason = "context_hypothesis"; }
        else if (token.issue == "button_down_conflict") row.status = "conflict";
        else if (token.issue == "candidate_search_failed" || token.issue == "full_replay_target_failed" || token.issue == "boundary_replay_failed") row.status = "not_found";
        else if (token.issue == "resource_limit" || token.issue == "task_limit" || token.issue == "work_limit" || token.issue == "window_work_limit" || token.issue == "counterfactual_unstable") row.status = "undecided";
        else if (token.issue == "invalid_native" || token.issue == "invalid_annotation") row.status = "invalid";
        else if (!token.issue.isEmpty()) row.status = "unsupported";
        else if (token.protectedText) { row.status = "preserved"; row.reason = "existing_annotation"; }
        else if (!complete) { row.status = "not_found"; row.reason = "candidate_search_failed"; }
        else if (!expressible) { row.status = "undecided"; row.reason = mixedResponsibility
            ? "mixed_touch_responsibility" : "unstable_touch_responsibility"; }
        else {
            row.status = "review"; row.reason = row.judgementKind.isEmpty() ? "joint_action_candidate" : row.judgementKind;
            QString encoded;
            if (token.slide) encoded = token.base + chart_transform::handOrderSuffix(edit);
            else if (token.primitives.size() > 1) encoded = cluster.join('/');
            else encoded = token.base + '~' + (row.head == HandOrderHand::Left ? QLatin1Char('l') : QLatin1Char('r'));
            result.patches.append({token.start, token.end, encoded});
        }
        if (p.colorUnresolved && !p.highConfidenceOnly
            && token.target && !token.protectedText
            && (row.status == QStringLiteral("undecided")
                || row.status == QStringLiteral("not_found")
                || row.status == QStringLiteral("conflict")
                || (row.status == QStringLiteral("unsupported")
                    && token.issue != QStringLiteral("mine_unsupported")
                    && token.issue != QStringLiteral("unsupported_object")))
            && !token.layoutFallback.isEmpty()
            && token.layoutFallback != text.mid(token.start, token.end - token.start)) {
            row.status = QStringLiteral("heuristic");
            row.reason = QStringLiteral("layout_fallback_unverified");
            row.hasCandidate = true;
            row.confidence = 0;
            row.confidenceLevel = QStringLiteral("insufficient_evidence");
            row.fullReplayVerified = false;
            row.pressureLevel.clear();
            row.conditions.clear();
            row.lanes.clear(); row.noteHands.clear(); row.trackIntervals.clear();
            row.wifiModes.clear(); row.wifiSectors.clear();
            for (const auto& marker : token.markers) {
                const auto* suggested = layoutByOrder.value(marker.parseOrder, nullptr);
                if (!suggested) continue;
                if (suggested->handOrderHead != HandOrderHand::Unspecified)
                    row.head = suggested->handOrderHead;
                if (marker.type == QStringLiteral("tap") || marker.type == QStringLiteral("hold")) {
                    row.lanes.append(marker.lane);
                    row.noteHands.append(suggested->handOrderHead);
                }
                if (marker.type == QStringLiteral("slide")) {
                    row.trackIntervals.append(suggested->handOrderTrackStops);
                    row.wifiModes.append(HandOrderWifiMode::Legacy);
                    row.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
                } else if (marker.type == QStringLiteral("wifi")) {
                    row.trackIntervals.append(suggested->handOrderTrackStops);
                    row.wifiModes.append(suggested->handOrderWifiMode);
                    row.wifiSectors.append(suggested->handOrderWifiSectorStops);
                }
                if (!suggested->handOrderTrackStops.isEmpty())
                    row.track = suggested->handOrderTrackStops.constFirst().hand;
            }
            result.patches.append({token.start, token.end, token.layoutFallback});
        }
        result.rows.append(row);
    }
    if (p.highConfidenceOnly) {
        QMap<int, bool> eligible;
        for (const auto& row : result.rows) if (!row.context && row.dependencyUnit >= 0) {
            if (!eligible.contains(row.dependencyUnit)) eligible[row.dependencyUnit] = true;
            if (row.status != "preserved") eligible[row.dependencyUnit] = eligible.value(row.dependencyUnit)
                && row.status == "review" && row.confidenceLevel == "high";
        }
        result.patches.removeIf([&](const auto& patch) {
            for (const auto& row : result.rows) if (row.start == patch.start) return !eligible.value(row.dependencyUnit, false);
            return true;
        });
    }
    result.workUsed = p.maxWorkUnits - qMax(qint64(0), work);
    if (progress) progress({HandOrderProgress::Finished, 1, 1});
    return result;
}

namespace {
bool completeOriginalAssignment(const SimaiParseResult& parsed)
{
    const auto specified = [](HandOrderHand hand) { return hand == HandOrderHand::Left || hand == HandOrderHand::Right; };
    const auto completeStops = [&](const auto& stops) {
        return !stops.isEmpty() && std::all_of(stops.begin(), stops.end(), [&](const auto& stop) { return specified(stop.hand); });
    };
    if (parsed.noteMarkers.isEmpty()) return false;
    for (const auto& marker : parsed.noteMarkers) {
        const bool slide = marker.type == "slide" || marker.type == "wifi";
        if ((!slide || marker.hasHeadStar) && !specified(marker.handOrderHead)) return false;
        if (!slide) continue;
        if (marker.handOrderWifiMode == HandOrderWifiMode::Split) {
            if (marker.handOrderWifiSectorStops.size() != 2) return false;
            for (const auto& sector : marker.handOrderWifiSectorStops) if (!completeStops(sector)) return false;
        } else if (!completeStops(marker.handOrderTrackStops)) return false;
    }
    return true;
}
bool strictlyBetterEvaluation(const BasicAssignmentEvaluation& candidate, const BasicAssignmentEvaluation& baseline)
{
    if (!candidate.complete || !baseline.complete) return false;
    const auto structuralPriority = [](const auto& evaluation) {
        return std::tie(evaluation.interactionBreaks, evaluation.ergonomicBreaks,
            evaluation.longitudinalBreaks, evaluation.sameKeyBreaks);
    };
    if (structuralPriority(candidate) != structuralPriority(baseline))
        return structuralPriority(candidate) < structuralPriority(baseline);
    const int candidateTier = int(std::ceil(candidate.risk / 0.025 - epsilon));
    const int baselineTier = int(std::ceil(baseline.risk / 0.025 - epsilon));
    if (candidateTier != baselineTier) return candidateTier < baselineTier;
    if (std::abs(candidate.risk - baseline.risk) > epsilon) return candidate.risk < baseline.risk;
    return candidate.cost < baseline.cost - 1e-6;
}

std::shared_ptr<BasicReplayEvidence> originalReplaySources(const BasicReplayEvidence& evidence,
    const QVector<chart_transform::HandOrderEditPlan::Patch>& patches)
{
    QVector<QPair<int,int>> offsets;
    int delta = 0;
    for (const auto& patch : patches) {
        delta += patch.text.size() - (patch.end - patch.start);
        offsets.append({patch.end + delta, delta});
    }
    const auto restore = [&](int& source) {
        const auto boundary = std::upper_bound(offsets.cbegin(), offsets.cend(), source,
            [](int at, const auto& entry) { return at < entry.first; });
        if (boundary != offsets.cbegin()) source -= (boundary - 1)->second;
    };
    const auto restoreCauses = [&](auto& causes) {
        for (auto& cause : causes) restore(cause.tokenStart);
    };
    auto mapped = std::make_shared<BasicReplayEvidence>(evidence);
    for (auto& input : mapped->inputs) restore(input.contact.tokenStart);
    for (auto& hit : mapped->replay.judgements) {
        restoreCauses(hit.causes);
        restoreCauses(hit.headCauses);
        for (auto& area : hit.areas) restoreCauses(area.causes);
        for (auto& wing : hit.wifiAreas)
            for (auto& area : wing) restoreCauses(area.causes);
    }
    return mapped;
}
} // namespace

BasicResult inferBasicHandOrder(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const BasicParameters& parameters, const miacode::simai::SimaiTimingMetadata& metadata,
    const HandOrderProgressCallback& progress)
{
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    // Local selections and unlabelled/explicitly protected charts retain their
    // existing semantics. They do not provide a fixed complete-chart baseline.
    if (!validBasicParameters(parameters) || parameters.evaluateCompleteAssignment || !parameters.overwriteExisting
        || begin != 0 || end != text.size() || !parsed.ok || !parsed.annotationErrors.isEmpty()
        || parameters.maxWorkUnits < 8000 || !completeOriginalAssignment(parsed))
        return inferBasicHandOrderImpl(text, parsed, begin, end, parameters, metadata, progress);

    const qint64 referenceGrant = parameters.maxWorkUnits / 8;
    const auto reportReference = [&](HandOrderProgress update) {
        update.candidate = 0; if (progress) progress(update);
    };
    auto fixed = parameters;
    fixed.overwriteExisting = false; fixed.highConfidenceOnly = false; fixed.colorUnresolved = false;
    fixed.counterfactualBudget = 0; fixed.evaluateCompleteAssignment = true; fixed.maxWorkUnits = referenceGrant;
    auto original = inferBasicHandOrderImpl(text, parsed, begin, end, fixed, metadata,
        progress ? HandOrderProgressCallback(reportReference) : HandOrderProgressCallback{});
    const qint64 referenceSpent = original.workUsed;
    const qint64 serializedReserve = original.evaluation.complete ? referenceGrant : 0;
    auto searchParameters = parameters;
    searchParameters.maxWorkUnits = parameters.maxWorkUnits - referenceSpent - serializedReserve;
    searchParameters.evaluateCompleteAssignment = true;
    auto candidate = inferBasicHandOrderImpl(text, parsed, begin, end, searchParameters, metadata, progress);
    const auto internalEvaluation = candidate.evaluation;
    const qint64 searchSpent = candidate.workUsed;
    BasicAssignmentEvaluation serializedEvaluation;
    qint64 serializedSpent = 0;
    const auto finish = [&](BasicResult output, const QString& outcome) {
        output.contextGuardOutcome = outcome;
        output.contextBaselineEvaluation = original.evaluation;
        output.contextInternalEvaluation = internalEvaluation;
        output.contextSerializedEvaluation = serializedEvaluation;
        output.contextBaselineWorkUsed = referenceSpent;
        output.contextSearchWorkUsed = searchSpent;
        output.contextSerializedWorkUsed = serializedSpent;
        output.workUsed = referenceSpent + searchSpent + serializedSpent;
        Q_ASSERT(output.workUsed <= parameters.maxWorkUnits);
        return output;
    };
    const auto keepOriginal = [&](const QString& outcome) {
        auto kept = original;
        kept.patches.clear();
        kept.truncated |= candidate.truncated;
        for (auto& row : kept.rows) if (!row.context && row.fullReplayVerified) {
            row.status = QStringLiteral("preserved"); row.reason = QStringLiteral("safe_baseline_retained");
        }
        return finish(std::move(kept), outcome);
    };
    if (!original.evaluation.complete) return finish(std::move(candidate), QStringLiteral("baseline_unverified"));
    if (!strictlyBetterEvaluation(internalEvaluation, original.evaluation))
        return keepOriginal(internalEvaluation.complete ? QStringLiteral("retained_not_better") : QStringLiteral("retained_candidate_unverified"));

    QString serialized = text;
    for (auto patch = candidate.patches.crbegin(); patch != candidate.patches.crend(); ++patch)
        serialized.replace(patch->start, patch->end - patch->start, patch->text);
    if (serialized == text) return keepOriginal(QStringLiteral("retained_unchanged"));
    const auto serializedParsed = SimaiParser::validateSyntax(serialized, metadata);
    if (!serializedParsed.ok || !serializedParsed.annotationErrors.isEmpty() || !completeOriginalAssignment(serializedParsed))
        return keepOriginal(QStringLiteral("retained_serialized_unverified"));
    fixed.maxWorkUnits = qMin(serializedReserve, parameters.maxWorkUnits - referenceSpent - searchSpent);
    const auto reportSerialized = [&](HandOrderProgress update) {
        update.candidate = 1; if (progress) progress(update);
    };
    const auto reconstructed = inferBasicHandOrderImpl(serialized, serializedParsed, 0, serialized.size(), fixed, metadata,
        progress ? HandOrderProgressCallback(reportSerialized) : HandOrderProgressCallback{});
    serializedSpent = reconstructed.workUsed; serializedEvaluation = reconstructed.evaluation;
    if (!strictlyBetterEvaluation(serializedEvaluation, original.evaluation))
        return keepOriginal(serializedEvaluation.complete ? QStringLiteral("retained_serialized_not_better") : QStringLiteral("retained_serialized_unverified"));
    candidate.evaluation = serializedEvaluation;
    if (parameters.capturePhysicalReplay && reconstructed.physicalReplay)
        candidate.physicalReplay = originalReplaySources(*reconstructed.physicalReplay, candidate.patches);
    return finish(std::move(candidate), QStringLiteral("accepted_verified_improvement"));
}
} // namespace miacode::hand_order
