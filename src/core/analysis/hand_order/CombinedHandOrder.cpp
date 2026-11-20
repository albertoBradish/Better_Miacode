#include "core/analysis/hand_order/CombinedHandOrder.h"

#include "common/TaskCancellation.h"
#include "core/analysis/hand_order/HandOrderConflictReview.h"
#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/ChartNormalization.h"
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace miacode::hand_order {
namespace {
constexpr double timingTolerance = 1e-6;

bool equivalentNativeMarker(const TimelineNoteMarker& a, const TimelineNoteMarker& b)
{
    return a.type == b.type && a.lane == b.lane && a.endLane == b.endLane && a.touchPad == b.touchPad
        && std::abs(a.second - b.second) < timingTolerance
        && std::abs(a.endSecond - b.endSecond) < timingTolerance
        && std::abs(a.slideTraceSecond - b.slideTraceSecond) < timingTolerance
        && a.slideTrackKey == b.slideTrackKey && a.slideSegmentKeys == b.slideSegmentKeys
        && a.slideBranchIndex == b.slideBranchIndex && a.hasHeadStar == b.hasHeadStar
        && a.isMine == b.isMine && a.headMine == b.headMine && a.trackMine == b.trackMine
        && a.isEach == b.isEach && a.slideEach == b.slideEach && a.sameHeadSlide == b.sameHeadSlide
        && a.isBreak == b.isBreak && a.isEx == b.isEx && a.isFirework == b.isFirework
        && a.headBreak == b.headBreak && a.headEx == b.headEx && a.trackBreak == b.trackBreak
        && a.tapUsesStarMaterial == b.tapUsesStarMaterial
        && std::abs(a.hsMultiplier - b.hsMultiplier) < timingTolerance;
}

SimaiParseResult normalizedPatternInput(const QString& text, const SimaiParseResult& parsed,
    const simai::SimaiTimingMetadata& metadata, bool* used)
{
    *used = false;
    task::CancellationScope::check();
    const auto organized = chart_transform::normalizeChartText(text, metadata);
    if (!organized.ok) return parsed;
    task::CancellationScope::check();
    const auto normalized = SimaiParser::validateSyntax(organized.text, metadata);
    if (!normalized.ok || !normalized.annotationErrors.isEmpty()
        || normalized.noteMarkers.size() != parsed.noteMarkers.size()) return parsed;
    auto mapped = parsed;
    const auto count = parsed.noteMarkers.size();
    QVector<bool> matched(count, false);
    for (int i = 0, first = 0; i < count; ++i) {
        task::CancellationScope::checkpoint();
        auto& marker = mapped.noteMarkers[i];
        while (first < count && normalized.noteMarkers.at(first).second < marker.second - timingTolerance) ++first;
        int match = -1;
        for (int at = first; at < count && normalized.noteMarkers.at(at).second <= marker.second + timingTolerance; ++at) {
            if (!matched.at(at) && equivalentNativeMarker(marker, normalized.noteMarkers.at(at))) {
                match = at;
                break;
            }
        }
        if (match < 0) return parsed;
        matched[match] = true;
        // Native time, track ownership, original annotations and source
        // ranges stay authoritative. Structural rules see the organized grid.
        marker.tempoBpm = normalized.noteMarkers.at(match).tempoBpm;
        marker.subdivision = normalized.noteMarkers.at(match).subdivision;
    }
    *used = true;
    return mapped;
}

void withholdConflictingUnits(BasicResult& output, const SimaiParseResult& parsed,
    const QString& text, const HandOrderConflictReview& review)
{
    if (review.regressedOrders.isEmpty() || output.patches.isEmpty()) return;
    QVector<int> lines{0};
    for (int i = 0; i < text.size(); ++i) if (text.at(i) == QLatin1Char('\n')) lines.append(i + 1);
    QSet<int> affected = review.affectedSources;
    QHash<QPair<int,int>, int> orderAtLocation;
    for (const auto& marker : parsed.noteMarkers) {
        if (!review.regressedOrders.contains(marker.parseOrder)) continue;
        const QPair<int,int> location{marker.sourceLine, marker.sourceCol};
        const int other = orderAtLocation.value(location, -1);
        if (other < 0 || review.affectedJudgements.constFind(marker.parseOrder)->quality == "miss")
            orderAtLocation.insert(location, marker.parseOrder);
    }
    for (const auto& token : parsed.sourceTokens) {
        if (token.line < 1 || token.line > lines.size()) continue;
        if (orderAtLocation.contains({token.line, token.col})) affected.insert(lines.at(token.line - 1) + token.col - 1);
    }
    QSet<int> units;
    for (const auto& row : output.rows)
        if (row.dependencyUnit >= 0 && affected.contains(row.start)) units.insert(row.dependencyUnit);
    for (auto& row : output.rows) {
        if (!affected.contains(row.start) && !units.contains(row.dependencyUnit)) continue;
        affected.insert(row.start);
        if (row.context || row.status == "preserved") continue;
        row.status = QStringLiteral("conflict");
        row.reason = QStringLiteral("physical_conflict_replay");
        row.fullReplayVerified = false;
        row.hasCandidate = false;
        if (const auto order = orderAtLocation.value({row.line, row.col}, -1); order >= 0) {
            const auto& hit = *review.affectedJudgements.constFind(order);
            row.judgementQuality = hit.quality;
            row.judgementSecond = hit.second;
            row.judgementMargin = hit.margin;
            row.causes = hit.causes;
            if (!hit.kind.isEmpty()) row.judgementKind = hit.kind;
            row.inputChannel = hit.inputChannel;
        }
    }
    const auto before = output.patches.size();
    output.patches.removeIf([&](const auto& patch) { return affected.contains(patch.start); });
    output.conflictPatchesWithheld = before - output.patches.size();
    if (output.conflictPatchesWithheld) output.evaluation = {};
    for (auto& pattern : output.patternSuggestions) {
        for (const auto& variant : pattern.variants) {
            if (std::any_of(variant.patches.cbegin(), variant.patches.cend(),
                [&](const auto& patch) { return affected.contains(patch.start); })) {
                pattern.preferred = -1;
                break;
            }
        }
    }
}
}

QString handOrderPatternFingerprint(const PatternSuggestion& suggestion)
{
    auto markers = suggestion.variants.first().markers;
    std::sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second;
        if (a.lane != b.lane) return a.lane < b.lane;
        if (a.endLane != b.endLane) return a.endLane < b.endLane;
        return a.slideTrackKey < b.slideTrackKey;
    });
    QJsonArray native{suggestion.registryId};
    for (const auto& marker : markers) native.append(QJsonArray{marker.type, marker.lane, marker.endLane,
        marker.touchPad, marker.slideTrackKey, qRound64(marker.second * 1e7),
        qRound64(marker.endSecond * 1e7), qRound64(marker.slideTraceSecond * 1e7)});
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(native).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

BasicResult optimizeHandOrder(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const BasicParameters& parameters, const simai::SimaiTimingMetadata& metadata,
    const MuriRenderOptions& playOptions, const QSet<QString>& ignoredPatterns, const HandOrderProgressCallback& progress)
{
    using Work = HandOrderProgress;
    const auto report = [&](Work update) { if (progress) progress(update); };
    const auto reportConflictSlice = [&](const Work& update, int beginPercent, int endPercent) {
        if (update.total > 0)
            report({Work::ConflictReview, beginPercent + (endPercent - beginPercent) * update.completed / update.total, 100});
    };
    report({Work::Normalizing, 0, 2});
    bool normalized = false;
    const auto patternInput = parsed.ok && parsed.annotationErrors.isEmpty()
        ? normalizedPatternInput(text, parsed, metadata, &normalized) : parsed;
    report({Work::Normalizing, 2, 2});
    report({Work::Patterns, 0, 0});
    PatternOptimizationParameters structural;
    structural.recognition = PatternRecognitionMode::Prioritized;
    auto detected = optimizeHandOrderPatterns(text, patternInput, begin, end, parameters, structural, metadata,
        [&](const Work& update) {
            if (update.phase == Work::Windows) report({Work::Patterns, update.completed, update.total + 1});
        });
    detected.patternSuggestions.removeIf([&](const auto& group) {
        return ignoredPatterns.contains(handOrderPatternFingerprint(group));
    });
    report({Work::Patterns, 1, 1});

    const qint64 reserve = parameters.maxWorkUnits >= 10000 && parameters.maxWorkUnits <= BasicParameters::maxAllowedWorkUnits
        ? qMin(qint64(15000000), parameters.maxWorkUnits / 10) : 0;
    BasicParameters search = parameters;
    search.maxWorkUnits -= reserve;
    if (!detected.patternSuggestions.isEmpty())
        search.recognizedPatterns = std::make_shared<const QVector<PatternSuggestion>>(detected.patternSuggestions);
    search.capturePhysicalReplay = true;
    const auto runSearch = [&](const BasicParameters& active, bool isRetry) {
        return inferBasicHandOrder(text, parsed, begin, end, active, metadata, [&](Work update) {
            if (isRetry) {
                if (update.phase == Work::Windows && update.candidate < 0)
                    reportConflictSlice(update, 55, 80);
                return;
            }
            if (update.phase == Work::Replay && update.window == 0 && update.candidate < 0) {
                Work complete{Work::Finished, 1, 1};
                complete.pass = Work::ContextSearch;
                report(complete);
                update.pass = Work::VerifyAssignments;
                report(update);
                return;
            }
            update.pass = update.candidate >= 0 ? Work::VerifyAssignments : Work::ContextSearch;
            report(update);
        });
    };
    auto output = runSearch(search, false);
    output.combinedOptimization = true;
    output.combinedNormalizationUsed = normalized;
    output.patternSuggestions = std::move(detected.patternSuggestions);
    output.patternSegments = output.patternSuggestions.size();
    for (const auto& group : output.patternSuggestions) output.patternCandidates += group.variants.size();
    output.patternOutcome = output.patternSegments ? "recognized" : "no_patterns";
    output.truncated |= detected.truncated;

    report({Work::ConflictReview, 0, 100});
    if (!output.patches.isEmpty()) {
        qint64 work = reserve + qMax(qint64(0), search.maxWorkUnits - output.workUsed);
        const qint64 initialWork = work;
        HandOrderConflictReview checked;
        if (output.physicalReplay)
            checked = reviewPhysicalConflicts(parsed.noteMarkers, *output.physicalReplay, playOptions.playProfile,
                playOptions.wifiNeedC, work, [&](const auto& update) { reportConflictSlice(update, 0, 55); });
        else checked.complete = false;
        output.workUsed += initialWork - qMax(qint64(0), work);
        output.conflictWindows = checked.intervals;
        output.confirmedConflictNotes = checked.regressedOrders.size();
        output.conflictReviewLimited = !checked.complete;
        // If the physical plan depends on a losing contact, compare another
        // candidate from the existing solver before withholding that unit.
        const qint64 remaining = parameters.maxWorkUnits - output.workUsed;
        if (checked.complete && !checked.regressedOrders.isEmpty() && remaining >= 120000) {
            report({Work::ConflictReview, 55, 100});
            auto alternativeParameters = search;
            alternativeParameters.inputPreference = parameters.inputPreference == 1 ? 0 : 1;
            alternativeParameters.counterfactualBudget = parameters.highConfidenceOnly ? parameters.counterfactualBudget : 0;
            alternativeParameters.maxWorkUnits = remaining * 4 / 5;
            auto alternative = runSearch(alternativeParameters, true);
            output.workUsed += alternative.workUsed;
            if (alternative.physicalReplay && !alternative.patches.isEmpty()) {
                qint64 available = qMax(qint64(0), parameters.maxWorkUnits - output.workUsed);
                const qint64 initial = available;
                auto rechecked = reviewPhysicalConflicts(parsed.noteMarkers, *alternative.physicalReplay, playOptions.playProfile,
                    playOptions.wifiNeedC, available, [&](const auto& update) { reportConflictSlice(update, 80, 100); });
                output.workUsed += initial - qMax(qint64(0), available);
                const auto& originalHits = output.physicalReplay->replay.judgements;
                const auto& alternateHits = alternative.physicalReplay->replay.judgements;
                const bool preservesCoverage = originalHits.size() == alternateHits.size()
                    && std::equal(originalHits.cbegin(), originalHits.cend(), alternateHits.cbegin(),
                        [](const auto& before, const auto& after) { return !before.meetsTarget || after.meetsTarget; });
                const bool improvesConflict = std::all_of(rechecked.regressedOrders.cbegin(), rechecked.regressedOrders.cend(),
                    [&](int order) { return checked.regressedOrders.contains(order); });
                if (rechecked.complete && improvesConflict && rechecked.regressedOrders.size() < checked.regressedOrders.size()
                    && preservesCoverage
                    && alternative.patches.size() >= output.patches.size()) {
                    output.rows = std::move(alternative.rows);
                    output.patches = std::move(alternative.patches);
                    output.evaluation = alternative.evaluation;
                    output.contextGuardOutcome = alternative.contextGuardOutcome;
                    output.conflictWindows = rechecked.intervals;
                    output.confirmedConflictNotes = rechecked.regressedOrders.size();
                    checked = std::move(rechecked);
                }
            }
        }
        withholdConflictingUnits(output, parsed, text, checked);
    }
    if (!parameters.overwriteExisting) {
        for (auto& group : output.patternSuggestions)
            if (std::any_of(group.variants.first().patches.cbegin(), group.variants.first().patches.cend(), [&](const auto& patch) {
                return !splitToken(text.mid(patch.start, patch.end - patch.start)).suffix.isEmpty();
            })) group.preferred = -1;
    }
    output.physicalReplay.reset();
    report({Work::ConflictReview, 100, 100});
    report({Work::Finished, 1, 1});
    return output;
}

} // namespace miacode::hand_order
