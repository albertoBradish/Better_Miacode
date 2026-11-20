#include "core/analysis/hand_order/HandPlaySimulation.h"

#include "common/TaskCancellation.h"
#include "core/analysis/MuriDiagnosticCollector.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/chart/ChartScoringEvents.h"
#include "core/chart/SlideJudgementTiming.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace miacode::hand_order {
namespace {
constexpr double eventGapSeconds = 0.09;
constexpr double epsilon = 1e-7;

bool isTrack(const TimelineNoteMarker& marker) { return marker.type == "slide" || marker.type == "wifi"; }
bool hasHand(HandOrderHand hand) { return hand != HandOrderHand::Unspecified; }

QString headKey(const TimelineNoteMarker& marker)
{
    return QStringLiteral("%1/%2/%3/%4/%5").arg(marker.sourceLine).arg(marker.sourceCol)
        .arg(marker.lane).arg(marker.eachGroupId).arg(marker.second, 0, 'g', 16);
}

double interpolate(const QVector<double>& times, const QVector<double>& values, double second)
{
    if (values.isEmpty() || !std::isfinite(second)) return 0.0;
    const auto after = std::upper_bound(times.cbegin(), times.cend(), second);
    if (after == times.cbegin() || after == times.cend()) return 0.0;
    const auto right = after - times.cbegin();
    const auto left = right - 1;
    const double mix = (second - times.at(left)) / (times.at(right) - times.at(left));
    return (1.0 - mix) * values.at(left) + mix * values.at(right);
}

bool externallyHit(const FullReplayArea& area, int source)
{
    return area.hit && std::any_of(area.causes.cbegin(), area.causes.cend(), [source](const auto& cause) {
        return cause.tokenStart != source;
    });
}

PlayTraceShortcuts clearedSlideSpans(const QVector<TimelineNoteMarker>& markers, const FullReplay& replay)
{
    PlayTraceShortcuts shortcuts;
    for (int i = 0; i < markers.size(); ++i) {
        task::CancellationScope::checkpoint();
        const auto& marker = markers.at(i);
        const auto& judged = replay.judgements.at(i);
        if (!isTrack(marker) || marker.trackMine
            || (marker.handOrderTrackStops.isEmpty() && marker.handOrderWifiSectorStops.isEmpty())) continue;
        QVector<PlayTraceShortcut> spans;
        const auto append = [&](double from, double to, double observed) {
            from = qMax(from, observed);
            if (to - from < 0.025) return;
            if (!spans.isEmpty() && from <= spans.last().toSecond + epsilon) {
                spans.last().toSecond = qMax(spans.last().toSecond, to);
                spans.last().availableSecond = qMax(spans.last().availableSecond, observed);
            } else {
                spans.append({from, to, observed});
            }
        };
        if (marker.type == "wifi" && judged.wifiAreas.size() == 3) {
            // Keep the native palm paths and skip only areas all three lanes have cleared.
            const double start = marker.slideTraceSecond;
            const double duration = marker.endSecond - start;
            const int areas = judged.wifiAreas.first().size();
            for (int area = 0; duration > 0.0 && area < areas; ++area) {
                double observed = -std::numeric_limits<double>::infinity();
                for (const auto& lane : judged.wifiAreas) {
                    const auto& hit = lane.value(area);
                    if (!externallyHit(hit, marker.parseOrder)) { observed = std::numeric_limits<double>::infinity(); break; }
                    observed = qMax(observed, hit.second + 1.0 / 120.0);
                }
                const double from = area == 0 ? 0.0 : marker.wifiTrackAreaThresholds.value(area, double(area) / areas);
                const double to = area + 1 == areas ? 1.0
                    : marker.wifiTrackAreaThresholds.value(area + 1, double(area + 1) / areas);
                append(start + duration * from, start + duration * to, observed);
            }
        }
        for (int part = 0; part < judged.segmentStartAreas.size(); ++part) {
            const auto& thresholds = marker.slideTrackAreaThresholds.value(part);
            const double begin = marker.slideSegmentShootSeconds.value(part);
            const double duration = marker.slideSegmentDurations.value(part);
            for (int area = 0; duration > 0.0 && area < thresholds.size(); ++area) {
                const auto& hit = judged.areas.value(judged.segmentStartAreas.at(part) + area);
                if (!externallyHit(hit, marker.parseOrder)) continue;
                const double observed = hit.second + 1.0 / 120.0;
                const double from = begin + duration * thresholds.at(area);
                const double to = begin + duration * thresholds.value(area + 1, 1.0);
                append(from, to, observed);
            }
        }
        if (!spans.isEmpty()) shortcuts.insert(marker.parseOrder, std::move(spans));
    }
    return shortcuts;
}

bool preservesJudgements(const FullReplayJudgement& before, const FullReplayJudgement& after)
{
    return (!before.trackCompleted || after.trackCompleted)
        && (!before.trackHighQuality || after.trackHighQuality)
        && (!before.headCompleted || after.headCompleted)
        && (!before.headHighQuality || after.headHighQuality)
        && (!before.completed || after.completed) && (!before.highQuality || after.highQuality)
        && after.heldRatio >= before.heldRatio - epsilon;
}

}

HandPlaySimulation::HandPlaySimulation(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options,
    bool computeJudgements)
    : profile_(options.playProfile)
    , palmRadius_(playPalmRadius(options))
{
    prepareMotion(markers, options);
    if (!computeJudgements || contacts_.isEmpty()) return;
    auto judged = replay(markers, options);
    if (!judged.valid || judged.exhausted) return;

    // A different input must have cleared an area before the hand skips it.
    auto shortcuts = clearedSlideSpans(markers, judged);
    for (int attempt = 0; attempt < 2 && !shortcuts.isEmpty(); ++attempt) {
        prepareMotion(markers, options, shortcuts);
        auto candidate = replay(markers, options);
        if (!candidate.valid || candidate.exhausted) { prepareMotion(markers, options); break; }
        QVector<QPair<double, double>> regressions;
        for (int i = 0; i < candidate.judgements.size(); ++i) {
            if (preservesJudgements(judged.judgements.at(i), candidate.judgements.at(i))) continue;
            const auto& note = markers.at(i);
            regressions.append({note.second - 0.24, qMax(note.second, note.endSecond) + 0.24});
        }
        if (regressions.isEmpty()) { judged = std::move(candidate); break; }
        prepareMotion(markers, options);
        for (auto it = shortcuts.begin(); it != shortcuts.end();) {
            const auto& spans = it.value();
            const bool nearRegression = std::any_of(regressions.cbegin(), regressions.cend(), [&](const auto& interval) {
                return spans.first().fromSecond <= interval.second && spans.last().toSecond >= interval.first;
            });
            if (nearRegression) it = shortcuts.erase(it);
            else ++it;
        }
    }
    replayVerified_ = true;
    buildJudgements(markers, judged);
    buildReport(markers, judged);
}

void HandPlaySimulation::prepareMotion(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options,
    const PlayTraceShortcuts& shortcuts)
{
    contacts_.rebuild(markers, palmRadius_, playReadyY(profile_), &options, shortcuts);
    reviewTimes_ = contacts_.reviewTimes();
    bodyOffsets_.clear();
    distanceOffsets_.clear();
    if ((!profile_.automaticHeight && !profile_.automaticDistance) || reviewTimes_.isEmpty())
        return;
    bodyOffsets_.resize(reviewTimes_.size());
    distanceOffsets_.resize(reviewTimes_.size());
    const double before = reviewTimes_.first() - 0.15;
    PlayBodyOffset current;
    double previous = before;
    for (int i = 0; i < reviewTimes_.size(); ++i) {
        task::CancellationScope::checkpoint();
        const double now = reviewTimes_.at(i);
        const auto target = preferredBodyOffset(contacts_.handsAt(now), profile_);
        const double vertical = (now - previous) * profile_.verticalSpeedCmPerSecond;
        const double depth = (now - previous) * profile_.foreAftSpeedCmPerSecond;
        current.verticalCm += qBound(-vertical, target.verticalCm - current.verticalCm, vertical);
        current.distanceCm += qBound(-depth, target.distanceCm - current.distanceCm, depth);
        bodyOffsets_[i] = current.verticalCm;
        distanceOffsets_[i] = current.distanceCm;
        previous = now;
    }
    reviewTimes_.prepend(before);
    bodyOffsets_.prepend(0.0);
    distanceOffsets_.prepend(0.0);
    const double recovery = qMax(std::abs(current.verticalCm) / profile_.verticalSpeedCmPerSecond,
        std::abs(current.distanceCm) / profile_.foreAftSpeedCmPerSecond);
    if (recovery > 0.0) {
        reviewTimes_.append(reviewTimes_.last() + recovery);
        bodyOffsets_.append(0.0);
        distanceOffsets_.append(0.0);
    }
}

double HandPlaySimulation::bodyOffsetCm(double second) const
{
    return interpolate(reviewTimes_, bodyOffsets_, second);
}

double HandPlaySimulation::distanceOffsetCm(double second) const
{
    return interpolate(reviewTimes_, distanceOffsets_, second);
}

PlaySimulationFrame HandPlaySimulation::sample(double second, bool withTrail) const
{
    auto frame = contacts_.sample(second, withTrail);
    auto arms = resolvePlayArms(frame.hands, palmRadius_, profile_, bodyOffsetCm(second), distanceOffsetCm(second));
    return {std::move(frame), std::move(arms)};
}

FullReplay HandPlaySimulation::replay(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options) const
{
    // One contact stream per physical hand. Changes of palm size or channel
    // split the stream at the shared time boundary.
    QVector<ChartInputAction> actions;
    std::array<ChartInputAction, 4> current;
    std::array<bool, 4> open{};
    double previous = -std::numeric_limits<double>::infinity();
    const auto close = [&](int hand, double second) {
        if (!open[hand]) return;
        auto& input = current[hand];
        input.contact.end = input.contact.motionEnd = second;
        if (second > input.contact.start + epsilon) {
            compactContactAction(input.contact);
            actions.append(std::move(input));
        }
        input = {};
        open[hand] = false;
    };
    for (double second : reviewTimes_) {
        task::CancellationScope::checkpoint();
        if (second - previous > 0.041)
            for (int slot = 0; slot < 4; ++slot) close(slot, previous + 1.0 / 120.0);
        const auto hands = contacts_.handsAt(second);
        const auto pose = resolvePlayArms(hands, palmRadius_, profile_, bodyOffsetCm(second), distanceOffsetCm(second));
        for (int hand = 0; hand < 2; ++hand) {
            const auto& motion = hands[hand];
            for (int type = 0; type < 2; ++type) {
                const bool button = type == 0;
                const int slot = 2 * hand + type;
                const bool active = motion.touching && playArmCanPress(pose, hands, hand)
                    && (button ? motion.buttonLane > 0 : motion.screen);
                if (!active) { close(slot, second); continue; }
                auto& input = current[slot];
                const int lane = button ? motion.buttonLane : 0;
                const double radius = qMin(FullReplayProfile::maximumPalmRadius, pose.arms[hand].palmRadius);
                if (open[slot] && (input.lane != lane || input.sourceParseOrder != motion.source
                    || std::abs(input.contact.radius - radius) > epsilon)) close(slot, second);
                if (!open[slot]) {
                    open[slot] = true;
                    input.button = button;
                    input.lane = lane;
                    input.sourceParseOrder = motion.source;
                    input.contact.hand = hand;
                    input.contact.tokenStart = motion.source;
                    input.contact.radius = radius;
                    input.contact.kind = button ? QStringLiteral("button") : QStringLiteral("slide");
                    input.contact.start = input.contact.motionStart = second;
                }
                if (!button && (input.contact.times.isEmpty() || second > input.contact.times.last() + epsilon)) {
                    input.contact.times.append(second);
                    input.contact.points.append(motion.contact);
                }
            }
        }
        previous = second;
    }
    for (int slot = 0; slot < 4; ++slot) close(slot, previous + 1.0 / 120.0);
    FullReplayOptions model;
    model.requireHighQuality = false;
    model.wifiNeedC = options.wifiNeedC;
    qint64 work = 25000000;
    return replayHandOrderChart(markers, actions, model, &work);
}

QString playGradeName(PlayGrade grade)
{
    switch (grade) {
    case PlayGrade::Critical: return QStringLiteral("Critical Perfect");
    case PlayGrade::Perfect: return QStringLiteral("Perfect");
    case PlayGrade::Great: return QStringLiteral("Great");
    case PlayGrade::Good: return QStringLiteral("Good");
    case PlayGrade::Miss: return QStringLiteral("Miss");
    case PlayGrade::None: return {};
    }
    return {};
}

void HandPlaySimulation::buildJudgements(const QVector<TimelineNoteMarker>& markers, const FullReplay& replayed)
{
    const auto scoring = chart::buildScoringEvents(markers);
    const slide_judge::LaunchBeatIndex launches(markers);
    qint64 total = 0, breaks = 0;
    for (const auto& event : scoring.events) {
        total += event.baseScore();
        breaks += event.isBreak;
    }
    const auto append = [&](const TimelineNoteMarker& note, bool body, chart::ScoringKind kind,
        bool isBreak, PlayGrade grade, double second) {
        const double angle = (note.lane - 0.5) * std::numbers::pi / 4.0;
        const bool touch = kind == chart::ScoringKind::Touch || kind == chart::ScoringKind::TouchHold;
        QPointF point = touch ? (note.touchPoint - QPointF(270, 270)) / playFieldRadius
            : QPointF(std::sin(angle), -std::cos(angle)) * 0.85;
        if (body) {
            const auto points = note.type == "wifi" ? note.wifiLanePoints.value(1) : note.slideSegmentPoints.value(note.slideSegmentPoints.size() - 1);
            if (!points.isEmpty()) point = points.last() / playFieldRadius;
        }
        PlayJudgement result{note.parseOrder, body, second, point, grade};
        const int base = chart::ScoringEvent{second, kind, isBreak}.baseScore();
        if (grade == PlayGrade::Miss) { result.baseLoss = base; result.breakBonusLoss = isBreak ? 100 : 0; }
        if (grade == PlayGrade::Good) { result.baseLoss = isBreak ? 1500 : base / 2; result.breakBonusLoss = isBreak ? 70 : 0; }
        if (grade == PlayGrade::Great) { result.baseLoss = base / 5; result.breakBonusLoss = isBreak ? 60 : 0; }
        if (grade == PlayGrade::Perfect && isBreak) result.breakBonusLoss = 25;
        if (total > 0) result.achievementLoss = 100.0 * result.baseLoss / total;
        if (breaks > 0) result.achievementLoss += result.breakBonusLoss / (100.0 * breaks);
        judgements_.append(result);
    };
    const auto simpleGrade = [](bool hit, double offset, bool ex, bool touch = false) {
        if (!hit) return PlayGrade::Miss;
        const double delta = std::abs(offset);
        if (ex || delta <= (touch ? FullReplayProfile::touchCriticalSeconds : FullReplayProfile::tapCriticalSeconds) + epsilon)
            return PlayGrade::Critical;
        if (delta <= 6.0 / FullReplayProfile::ticksPerSecond + epsilon) return PlayGrade::Perfect;
        if (delta <= 12.0 / FullReplayProfile::ticksPerSecond + epsilon) return PlayGrade::Great;
        return PlayGrade::Good;
    };
    QSet<QString> countedHeads;
    for (int i = 0; i < markers.size(); ++i) {
        task::CancellationScope::checkpoint();
        const auto& marker = markers.at(i);
        const auto& judged = replayed.judgements.at(i);
        if (!judged.supported || marker.parseOrder < 0 || marker.isMine) continue;
        const bool slide = isTrack(marker);
        const bool hold = marker.type == "hold" || marker.type == "touch_hold";
        const double missAt = marker.second + FullReplayProfile::tapAvailableSeconds;
        if (slide) {
            const auto key = headKey(marker);
            if (marker.hasHeadStar && !marker.headMine && hasHand(marker.handOrderHead) && !countedHeads.contains(key)) {
                countedHeads.insert(key);
                append(marker, false, chart::ScoringKind::Star, marker.headBreak,
                    simpleGrade(judged.headCompleted, judged.headHitDelta, marker.headEx),
                    judged.headCompleted ? judged.headSecond : missAt);
            }
            if (marker.trackMine) continue;
            bool assigned = false;
            for (const auto& stop : marker.handOrderTrackStops) assigned |= hasHand(stop.hand);
            for (const auto& sector : marker.handOrderWifiSectorStops)
                for (const auto& stop : sector) assigned |= hasHand(stop.hand);
            if (!assigned) continue;
            const auto timing = slide_judge::timingForMarker(marker, &launches);
            const bool perfect = timing.valid ? judged.second >= timing.perfectStartSecond - epsilon
                && judged.second <= timing.perfectEndSecond + epsilon : judged.trackHighQuality;
            const auto grade = !judged.trackCompleted ? PlayGrade::Miss : perfect
                ? PlayGrade::Critical : std::abs(judged.hitDelta) <= 0.38 ? PlayGrade::Great : PlayGrade::Good;
            append(marker, true, chart::ScoringKind::Slide, marker.trackBreak, grade,
                judged.trackCompleted ? judged.second : marker.endSecond + FullReplayProfile::slideAvailableSeconds);
            continue;
        }
        if (!hasHand(marker.handOrderHead)) continue;
        auto grade = simpleGrade(hold ? judged.headCompleted : judged.completed, hold ? judged.headHitDelta : judged.hitDelta,
            marker.isEx, marker.type == "touch" || marker.type == "touch_hold");
        if (hold && grade != PlayGrade::Miss) {
            if (judged.heldRatio < 0.20) grade = PlayGrade::Miss;
            else if (judged.heldRatio < 0.60) grade = PlayGrade::Good;
            else if (judged.heldRatio < 0.90 && grade < PlayGrade::Great) grade = PlayGrade::Great;
        }
        const auto kind = marker.type == "touch_hold" ? chart::ScoringKind::TouchHold
            : marker.type == "touch" ? chart::ScoringKind::Touch : hold ? chart::ScoringKind::Hold : chart::ScoringKind::Tap;
        append(marker, false, kind, marker.isBreak, grade, hold ? qMax(marker.second, marker.endSecond)
            : judged.completed ? judged.second : missAt);
    }
    std::stable_sort(judgements_.begin(), judgements_.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    double totalLoss = 0.0;
    for (auto& judgement : judgements_) {
        totalLoss += judgement.achievementLoss;
        judgement.cumulativeLoss = totalLoss;
    }
}

void HandPlaySimulation::buildReport(const QVector<TimelineNoteMarker>& markers, const FullReplay& replayed)
{
    QHash<int, const TimelineNoteMarker*> notes;
    for (int i = 0; i < markers.size(); ++i) {
        task::CancellationScope::checkpoint();
        const auto& marker = markers.at(i);
        notes.insert(marker.parseOrder, &marker);
        const auto& judged = replayed.judgements.at(i);
        if (!isTrack(marker) || !judged.supported) continue;
        MarkerMuriState state;
        state.markerKey = makeMarkerAnalysisKey(marker);
        state.markerType = marker.type;
        state.second = marker.second;
        state.endSecond = marker.endSecond;
        state.line = marker.sourceLine;
        state.col = marker.sourceCol;
        if (marker.type == "wifi") {
            for (const auto& lane : judged.wifiAreas) {
                QVector<double> times;
                for (const auto& hit : lane) {
                    if (!hit.hit) break;
                    times.append(hit.second);
                }
                state.wifiLaneProgressSeconds.append(times);
            }
            state.wifiCompletedSecond = judged.trackCompleted ? judged.second : -1.0;
            state.wifiPadCSecond = std::isfinite(judged.wifiPadCSecond) ? judged.wifiPadCSecond : -1.0;
        } else {
            for (int seg = 0; seg < judged.segmentStartAreas.size(); ++seg) {
                MuriSegmentState segment;
                const auto areas = marker.slideTrackAreaPoints.value(seg);
                const int start = judged.segmentStartAreas.at(seg);
                for (int area = 0; area < areas.size(); ++area) {
                    const auto hit = judged.areas.value(start + area);
                    const int count = qMax(qsizetype(1), marker.slideTrackAreaCheckpoints.value(seg).value(area).size());
                    QVector<MuriCheckpointState> checkpoints;
                    for (int j = 0; j < count; ++j) {
                        MuriCheckpointState checkpoint;
                        checkpoint.second = hit.hit ? hit.second : -1.0;
                        checkpoint.skipped = hit.skipped;
                        if (hit.hit) checkpoint.cause = externallyHit(hit, marker.parseOrder)
                            ? AreaJudgeCause::Other : AreaJudgeCause::Self;
                        checkpoints.append(checkpoint);
                    }
                    segment.areaCheckpoints.append(checkpoints);
                }
                const double completed = judged.segmentCompletedSeconds.value(seg, -1.0);
                segment.completedSecond = std::isfinite(completed) ? completed : -1.0;
                state.slideSegments.append(segment);
            }
        }
        report_.markerStates.insert(state.markerKey, state);
    }
    muri::detail::MuriDiagnosticCollector feedback;
    for (const auto& judgement : judgements_) {
        if (judgement.grade != PlayGrade::Good) continue;
        const auto* marker = notes.value(judgement.source, nullptr);
        if (!marker) continue;
        if (judgement.slideBody) {
            // The native Maimuri shape assets indicate a Good Slide.
            feedback.appendSlideJudgeSpriteEvent(*marker, judgement.second);
        } else {
            MuriJudgeSpriteEvent event;
            event.second = event.spawnSecond = judgement.second;
            event.markerKey = makeMarkerAnalysisKey(*marker);
            event.pad = marker->type.startsWith("touch") ? marker->touchPad : QStringLiteral("A%1").arg(marker->lane);
            feedback.judgeSpriteEvents.append(std::move(event));
        }
    }
    report_.judgeSpriteEvents = std::move(feedback.judgeSpriteEvents);
}

QVector<PlayIssue> HandPlaySimulation::detectIssues(const QVector<TimelineNoteMarker>& markers) const
{
    QVector<PlayIssue> issues;
    if (reviewTimes_.isEmpty()) return issues;
    QHash<int, const TimelineNoteMarker*> bySource;
    for (const auto& marker : markers)
        if (marker.parseOrder >= 0) bySource.insert(marker.parseOrder, &marker);

    QHash<QPair<int, QPair<int, int>>, int> previous;
    const auto report = [&](PlayIssueKind kind, int handMask, int source, double second, int otherSource = -1) {
        const auto* marker = bySource.value(source, nullptr);
        if (!marker || second < -1e-7) return;
        second = qMax(0.0, second);
        const auto key = qMakePair(static_cast<int>(kind), qMakePair(handMask, source));
        const int last = previous.value(key, -1);
        if (last >= 0 && second <= issues.at(last).endSecond + eventGapSeconds) {
            issues[last].endSecond = second;
            return;
        }
        previous.insert(key, issues.size());
        const int column = marker->sourceCol;
        issues.append({kind, second, second, marker->sourceLine, column,
            qMax(column, marker->handOrderAnnotationEndCol), handMask, source, otherSource});
    };

    for (const double second : reviewTimes_) {
        task::CancellationScope::checkpoint();
        const auto frame = sample(second, false);
        for (const auto& cue : frame.contacts.cues) {
            if (cue.block == PlayBlock::None) continue;
            report(cue.block == PlayBlock::TooFast ? PlayIssueKind::TooFast : PlayIssueKind::Occupied,
                cue.hand == HandOrderHand::Left ? 1 : 2, cue.source, second);
        }
        if (frame.arms.interference == PlayInterference::Collision) {
            const auto& hands = frame.contacts.hands;
            const int source = hands[0].touching && hands[0].source >= 0 ? hands[0].source
                : hands[1].source >= 0 ? hands[1].source : hands[0].source;
            report(PlayIssueKind::ArmCollision, 3, source, second);
        } else if (frame.arms.interference == PlayInterference::OutOfReach) {
            const int hand = frame.arms.interferenceHand;
            report(PlayIssueKind::OutOfReach, 1 << hand, frame.contacts.hands[hand].source, second);
        }
        const auto& left = frame.contacts.hands[0];
        const auto& right = frame.contacts.hands[1];
        if (left.touching && right.touching && left.source != right.source
            && left.contact.x() - right.contact.x() > 0.13)
            report(PlayIssueKind::CrossedHands, 3, left.source, second, right.source);
    }
    QMultiHash<int, int> issuesBySource;
    for (int i = 0; i < issues.size(); ++i)
        if (issues.at(i).kind != PlayIssueKind::CrossedHands) issuesBySource.insert(issues.at(i).source, i);
    for (const auto& judgement : judgements_) {
        if (judgement.baseLoss == 0 && judgement.breakBonusLoss == 0) continue;
        const auto* marker = bySource.value(judgement.source, nullptr);
        if (!marker || judgement.second < 0.0) continue;
        // A physical warning and the lost judgement on the same object share
        // one row. An accidental pad judgement can also lose score on its own.
        int match = -1;
        double bestGap = std::numeric_limits<double>::infinity();
        for (int i : issuesBySource.values(judgement.source)) {
            const auto& issue = issues.at(i);
            if (issue.grade != PlayGrade::None || issue.second > judgement.second + epsilon) continue;
            const double gap = qMax(0.0, judgement.second - issue.endSecond);
            if (gap < bestGap) { match = i; bestGap = gap; }
        }
        if (match < 0) {
            const int column = marker->sourceCol;
            issues.append({PlayIssueKind::JudgementLoss, judgement.second, judgement.second,
                marker->sourceLine, column, qMax(column, marker->handOrderAnnotationEndCol), 0, judgement.source});
            match = issues.size() - 1;
        }
        auto& issue = issues[match];
        issue.grade = judgement.grade;
        issue.judgeSecond = judgement.second;
        issue.baseLoss = judgement.baseLoss;
        issue.achievementLoss = judgement.achievementLoss;
    }
    std::stable_sort(issues.begin(), issues.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second, a.line, a.column, a.kind) < std::tie(b.second, b.line, b.column, b.kind);
    });
    return issues;
}

} // namespace miacode::hand_order
