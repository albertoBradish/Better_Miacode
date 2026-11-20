#include "core/analysis/hand_order/HandPlayReview.h"

#include "common/TaskCancellation.h"
#include "core/analysis/hand_order/HandPlaySimulation.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace miacode::hand_order {
namespace {
using Hand = HandOrderHand;
constexpr double together = 0.004;
constexpr double groupGap = 0.18;
constexpr double contextSeconds = 0.70;
constexpr int maxGroupNotes = 18;
constexpr int maxEvaluatedGroups = 240;
constexpr int maxGroupMarkerWork = 8400;

struct EditToken {
    int start = 0, end = 0;
    QVector<int> markers;
    QString base;
    bool writable = false;
};

bool sustained(const TimelineNoteMarker& note)
{
    return isLockingHold(note) || note.type == "slide" || note.type == "wifi";
}

Hand opposite(Hand hand)
{
    return hand == Hand::Left ? Hand::Right : hand == Hand::Right ? Hand::Left : hand;
}

double screenX(const TimelineNoteMarker& note)
{
    if (note.type.startsWith("touch")) return (note.touchPoint.x() - 270.0) / 240.0;
    return std::sin((note.lane - 0.5) * std::numbers::pi / 4.0);
}

bool simultaneousReverse(const TimelineNoteMarker& a, const TimelineNoteMarker& b)
{
    if (std::abs(a.second - b.second) > together || a.handOrderHead == b.handOrderHead
        || !concreteHoldHand(a.handOrderHead) || !concreteHoldHand(b.handOrderHead)) return false;
    const auto& left = a.handOrderHead == Hand::Left ? a : b;
    const auto& right = a.handOrderHead == Hand::Right ? a : b;
    return screenX(left) - screenX(right) > 0.13;
}

bool editableMarker(const TimelineNoteMarker& note)
{
    return !note.isMine && !note.trackMine && std::isfinite(note.second) && note.second >= 0.0;
}

QString tokenWithHands(const EditToken& token, const QVector<TimelineNoteMarker>& notes)
{
    const auto& first = notes.at(token.markers.first());
    if (!sustained(first) || isLockingHold(first)) {
        const QString hand = first.handOrderHead == Hand::Left ? QStringLiteral("~l") : QStringLiteral("~r");
        if (token.markers.size() == 1) return token.base + hand;
        QStringList parts;
        for (int i = 0; i < token.markers.size(); ++i) {
            const auto& note = notes.at(token.markers.at(i));
            if (i >= token.base.size() || token.base.at(i) != QChar('0' + note.lane)) return {};
            parts.append(QString(token.base.at(i)) + (note.handOrderHead == Hand::Left ? QStringLiteral("~l") : QStringLiteral("~r")));
        }
        if (token.base.size() != parts.size()) return {};
        return parts.join(QLatin1Char('/'));
    }
    chart_transform::HandOrderTokenEdit edit;
    edit.baseToken = token.base;
    edit.hasHead = first.hasHeadStar;
    edit.head = first.handOrderHead;
    for (int id : token.markers) {
        const auto& note = notes.at(id);
        edit.tracks.append(note.handOrderTrackStops);
        edit.wifiModes.append(note.handOrderWifiMode);
        edit.wifiSectors.append(note.handOrderWifiSectorStops);
        edit.wifiBranches.append(note.type == "wifi");
    }
    const QString suffix = chart_transform::handOrderSuffix(edit);
    return suffix.isEmpty() ? QString() : token.base + suffix;
}

struct Assessment {
    bool verified = false;
    double loss = 0.0;
    int crossed = 0;
    double obstruction = 0.0;
    double strain = 0.0;
    int judgements = 0;
};

Assessment assess(const QVector<TimelineNoteMarker>& notes, const QSet<int>& phased, const MuriRenderOptions& options,
    double start, double end)
{
    const HandPlaySimulation simulation(notes, options, true);
    Assessment score;
    score.verified = simulation.replayVerified();
    for (const auto& grade : simulation.judgements()) {
        score.loss += grade.achievementLoss;
        ++score.judgements;
    }
    QHash<int, const TimelineNoteMarker*> bySource;
    for (const auto& note : notes) bySource.insert(note.parseOrder, &note);
    QSet<QPair<int, int>> crossings;
    for (const auto& issue : simulation.detectIssues(notes)) {
        if (issue.endSecond < start || issue.second > end) continue;
        if (issue.kind == PlayIssueKind::CrossedHands) {
            const auto* left = bySource.value(issue.source, nullptr);
            const auto* right = bySource.value(issue.otherSource, nullptr);
            const bool locked = (left && isLockingHold(*left)) || (right && isLockingHold(*right));
            if (!locked && !phased.contains(issue.source) && !phased.contains(issue.otherSource))
                crossings.insert({issue.source, issue.otherSource});
        } else if (issue.kind != PlayIssueKind::JudgementLoss) {
            score.obstruction += (issue.kind == PlayIssueKind::ArmCollision ? 2.0 : 1.0)
                * (0.06 + qMin(1.0, issue.endSecond - issue.second));
        }
    }
    score.crossed = crossings.size();
    for (double second = start; second < end; second += 0.085) {
        if (second < 0.0) continue;
        const auto frame = simulation.sample(second, false);
        score.strain += 0.006 * std::abs(simulation.bodyOffsetCm(second))
            + 0.008 * std::abs(simulation.distanceOffsetCm(second));
        if (frame.arms.interference == PlayInterference::Lifted) score.strain += 0.16;
        for (int hand = 0; hand < 2; ++hand) {
            const auto& motion = frame.contacts.hands[hand];
            if (!motion.touching || motion.source < 0 || phased.contains(motion.source)) continue;
            const double across = hand == 0 ? motion.contact.x() : -motion.contact.x();
            score.strain += qMax(0.0, across - 0.18) * 0.045;
        }
    }
    return score;
}

bool improvement(const Assessment& before, const Assessment& after)
{
    if (!before.verified || !after.verified || before.judgements != after.judgements) return false;
    if (after.crossed > before.crossed || after.loss > before.loss + 1e-6
        || after.obstruction > before.obstruction + 0.015) return false;
    return after.crossed < before.crossed || after.loss < before.loss - 1e-6
        || after.obstruction < before.obstruction - 0.02 || after.strain < before.strain - 0.045;
}

int reviewPriority(const PlayIssue& issue)
{
    if (issue.kind == PlayIssueKind::CrossedHands) return 0;
    if (issue.grade != PlayGrade::None) return 1;
    if (issue.kind == PlayIssueKind::Occupied || issue.kind == PlayIssueKind::TooFast) return 2;
    return 3;
}

void flipNote(TimelineNoteMarker& note)
{
    note.handOrderHead = opposite(note.handOrderHead);
    for (auto& stop : note.handOrderTrackStops) stop.hand = opposite(stop.hand);
    for (auto& sector : note.handOrderWifiSectorStops)
        for (auto& stop : sector) stop.hand = opposite(stop.hand);
}

bool sharesPhase(const PatternVariant& variant, const QVector<TimelineNoteMarker>& notes, const QHash<int, int>& byOrder)
{
    if (variant.choiceKey != "alternate" && variant.choiceKey != "mirror") return false;
    return std::all_of(variant.markers.cbegin(), variant.markers.cend(), [&](const auto& planned) {
        const int index = byOrder.value(planned.parseOrder, -1);
        return index >= 0 && notes.at(index).handOrderHead == planned.handOrderHead;
    });
}

} // namespace

BasicResult reviewHandPlay(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const MuriRenderOptions& options, const HandOrderProgressCallback& progress, double timelineOffset)
{
    BasicResult result;
    if (progress) progress({HandOrderProgress::Preparing});
    if (!parsed.ok || !parsed.annotationErrors.isEmpty()) { result.error = "invalid_chart"; return result; }
    if (begin >= end) { begin = 0; end = text.size(); }
    const auto original = timeline::offset::shiftedNoteMarkers(parsed.noteMarkers, timelineOffset,
        timeline::offset::NonFiniteHandling::PassThrough);
    const HandPlaySimulation baseline(original, options, true);
    if (!baseline.replayVerified()) { result.workLimited = !baseline.isEmpty(); return result; }
    auto issues = baseline.detectIssues(original);
    if (issues.isEmpty()) { if (progress) progress({HandOrderProgress::Finished, 1, 1}); return result; }
    std::stable_sort(issues.begin(), issues.end(), [](const auto& a, const auto& b) {
        const int first = reviewPriority(a), second = reviewPriority(b);
        return first != second ? first < second : a.achievementLoss > b.achievementLoss;
    });
    result.includedObjects = original.size();
    QHash<int, int> byOrder;
    for (int i = 0; i < original.size(); ++i) byOrder.insert(original.at(i).parseOrder, i);
    QVector<int> lines{0};
    for (int i = 0; i < text.size(); ++i) if (text.at(i) == QLatin1Char('\n')) lines.append(i + 1);
    QVector<EditToken> tokens;
    QHash<int, int> tokenByMarker;
    for (const auto& source : parsed.sourceTokens) {
        if (!source.nativeValid || source.markerCount <= 0 || source.line < 1 || source.line > lines.size()) continue;
        const int start = lines.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        const auto parts = splitToken(text.mid(start, source.length));
        EditToken token{start, finish, {}, parts.baseToken, start >= begin && finish <= end && parts.suffixOffset >= 0};
        for (int i = 0; i < source.markerCount; ++i) {
            const int order = source.firstParseOrder + i;
            const int id = byOrder.value(order, -1);
            if (id < 0) { token.writable = false; continue; }
            token.markers.append(id);
            tokenByMarker.insert(id, tokens.size());
            const auto& note = original.at(id);
            token.writable &= editableMarker(note) && !isLockingHold(note)
                && (!note.hasHeadStar || concreteHoldHand(note.handOrderHead) || sustained(note));
        }
        tokens.append(std::move(token));
    }
    const auto patterns = recognizeHandOrderPatterns(text, parsed, 0, text.size());
    QSet<int> phased;
    QVector<QVector<int>> groups;
    QVector<const PatternSuggestion*> groupPatterns;
    QHash<int, int> namedGroupByMarker;
    for (const auto& pattern : patterns) {
        if (!pattern.definite || pattern.variants.isEmpty()) continue;
        const auto& members = pattern.variants.first().markers;
        QVector<int> group;
        for (const auto& marker : members) {
            const int id = byOrder.value(marker.parseOrder, -1);
            if (id < 0) continue;
            group.append(id);
        }
        if (group.isEmpty()) continue;
        if (pattern.interactionPhase != InteractionPhaseMode::None
            && std::any_of(pattern.variants.cbegin(), pattern.variants.cend(),
                [&](const auto& variant) { return sharesPhase(variant, original, byOrder); })) {
            for (int id : group) phased.insert(original.at(id).parseOrder);
        }
        const int groupIndex = groups.size();
        groups.append(group);
        groupPatterns.append(&pattern);
        for (int id : group) if (!namedGroupByMarker.contains(id)) namedGroupByMarker.insert(id, groupIndex);
    }
    auto current = original;
    QSet<int> attempted, changedTokens;
    int evaluated = 0, markerWork = 0;
    for (const auto& issue : issues) {
        task::CancellationScope::checkpoint();
        if (evaluated >= maxEvaluatedGroups || markerWork >= maxGroupMarkerWork) { result.truncated = true; break; }
        const int target = byOrder.value(issue.source, -1);
        if (target < 0 || attempted.contains(target)) continue;
        const auto& note = current.at(target);
        int groupIndex = namedGroupByMarker.value(target, -1);
        QVector<int> candidates = groupIndex < 0 ? QVector<int>{target} : groups.at(groupIndex);
        if (groupIndex < 0) {
            for (int j = qMax(0, target - 3); j < qMin(current.size(), qsizetype(target + 5)); ++j) {
                if (j == target || namedGroupByMarker.contains(j)) continue;
                const auto& peer = current.at(j);
                if (std::abs(peer.second - note.second) <= together
                    || (issue.kind == PlayIssueKind::TooFast && std::abs(peer.second - note.second) < groupGap && !sustained(peer)))
                    candidates.append(j);
            }
        }
        if (candidates.isEmpty() || candidates.size() > maxGroupNotes) continue;
        QSet<int> tokenIds;
        for (int id : candidates) {
            attempted.insert(id);
            const int token = tokenByMarker.value(id, -1);
            if (token >= 0 && tokens.at(token).writable) tokenIds.insert(token);
        }
        if (tokenIds.isEmpty() || (groupIndex >= 0 && std::any_of(candidates.cbegin(), candidates.cend(),
            [&](int id) { return !tokenIds.contains(tokenByMarker.value(id, -1)); }))) continue;
        QSet<int> changed;
        for (int token : tokenIds) for (int id : tokens.at(token).markers) changed.insert(id);
        double start = note.second, endTime = qMax(note.second, note.endSecond);
        for (int id : changed) { start = qMin(start, current.at(id).second); endTime = qMax(endTime, current.at(id).endSecond); }
        QVector<int> window;
        QVector<TimelineNoteMarker> before;
        double replayStart = start - contextSeconds, replayEnd = endTime + contextSeconds;
        for (int j = 0; j < current.size(); ++j) {
            const auto& other = current.at(j);
            if (other.second > replayEnd || qMax(other.second, other.endSecond) < replayStart) continue;
            window.append(j); before.append(other);
        }
        if (window.size() > 180 || replayEnd - replayStart > 17.0) continue;
        ++evaluated;
        if (progress) progress({HandOrderProgress::Windows, evaluated, maxEvaluatedGroups});
        markerWork += before.size();
        const auto oldScore = assess(before, phased, options, start - 0.15, endTime + 0.17);
        Assessment bestScore = oldScore;
        QVector<TimelineNoteMarker> winner;
        QSet<int> winningPhase;
        const auto consider = [&](QVector<TimelineNoteMarker> proposal, QSet<int> candidatePhase) {
            task::CancellationScope::checkpoint();
            QVector<TimelineNoteMarker> after, edited;
            for (int id : window) after.append(proposal.at(id));
            for (int id : changed) edited.append(proposal.at(id));
            if (!respectsHoldLocks(before, edited)) return;
            for (int token : tokenIds)
                if (tokenWithHands(tokens.at(token), proposal).isEmpty()) return;
            if (markerWork >= maxGroupMarkerWork) { result.truncated = true; return; }
            markerWork += after.size();
            const auto newScore = assess(after, candidatePhase, options, start - 0.15, endTime + 0.17);
            if (!improvement(bestScore, newScore)) return;
            bestScore = newScore;
            winner = std::move(proposal);
            winningPhase = std::move(candidatePhase);
        };
        auto flipped = current;
        for (int id : changed) flipNote(flipped[id]);
        consider(std::move(flipped), phased);
        if (groupIndex >= 0) {
            const auto& pattern = *groupPatterns.at(groupIndex);
            for (const auto& variant : pattern.variants) {
                if (pattern.interactionPhase != InteractionPhaseMode::None
                    && variant.choiceKey != "alternate" && variant.choiceKey != "mirror") continue;
                auto proposal = current;
                QSet<int> candidatePhase = phased;
                for (const auto& proposed : variant.markers) {
                    const int id = byOrder.value(proposed.parseOrder, -1);
                    if (!changed.contains(id)) continue;
                    auto& existing = proposal[id];
                    // Retain the user's handoff positions and WiFi partition.
                    if (concreteHoldHand(existing.handOrderHead) && opposite(existing.handOrderHead) == proposed.handOrderHead)
                        flipNote(existing);
                }
                if (pattern.interactionPhase != InteractionPhaseMode::None && sharesPhase(variant, proposal, byOrder))
                    for (const auto& proposed : variant.markers) candidatePhase.insert(proposed.parseOrder);
                consider(std::move(proposal), std::move(candidatePhase));
            }
        } else {
            auto natural = current;
            bool crossedEach = false;
            for (int a : changed) for (int b : changed) {
                if (a >= b || !simultaneousReverse(current.at(a), current.at(b))) continue;
                natural[a].handOrderHead = screenX(current.at(a)) < screenX(current.at(b)) ? Hand::Left : Hand::Right;
                natural[b].handOrderHead = opposite(natural.at(a).handOrderHead);
                crossedEach = true;
            }
            if (crossedEach) consider(std::move(natural), phased);
            const int targetToken = tokenByMarker.value(target, -1);
            if (tokenIds.contains(targetToken) && changed.size() > tokens.at(targetToken).markers.size()) {
                auto onlyTarget = current;
                for (int id : tokens.at(targetToken).markers) flipNote(onlyTarget[id]);
                consider(std::move(onlyTarget), phased);
            }
        }
        if (winner.isEmpty()) continue;
        current = std::move(winner);
        phased = std::move(winningPhase);
        changedTokens.unite(tokenIds);
        ++result.patternAccepted;
    }
    for (int i = 0; i < tokens.size(); ++i) {
        if (!changedTokens.contains(i)) continue;
        const auto& token = tokens.at(i);
        const QString replacement = tokenWithHands(token, current);
        if (replacement.isEmpty() || text.mid(token.start, token.end - token.start) == replacement) continue;
        result.patches.append({token.start, token.end, replacement});
        BasicRow row;
        row.start = token.start; row.end = token.end;
        row.line = std::upper_bound(lines.cbegin(), lines.cend(), token.start) - lines.cbegin();
        row.col = token.start - lines.at(row.line - 1) + 1;
        row.originalAnnotation = text.mid(token.start, token.end - token.start);
        row.status = QStringLiteral("play_review");
        row.fullReplayVerified = true;
        result.rows.append(std::move(row));
    }
    std::sort(result.patches.begin(), result.patches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    std::sort(result.rows.begin(), result.rows.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    result.windowCount = evaluated;
    if (!result.patches.isEmpty()) {
        const HandPlaySimulation verified(current, options, true);
        const auto totalLoss = [](const HandPlaySimulation& simulation) {
            return simulation.judgements().isEmpty() ? 0.0 : simulation.judgements().last().cumulativeLoss;
        };
        if (!verified.replayVerified() || verified.judgements().size() != baseline.judgements().size()
            || totalLoss(verified) > totalLoss(baseline) + 1e-6) {
            result.patches.clear();
            result.rows.clear();
            result.patternAccepted = 0;
            result.workLimited |= !verified.replayVerified();
        }
    }
    if (progress) progress({HandOrderProgress::Finished, 1, 1});
    return result;
}

} // namespace miacode::hand_order
