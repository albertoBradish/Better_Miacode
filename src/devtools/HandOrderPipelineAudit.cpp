#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "common/TaskCancellation.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace miacode;
using namespace miacode::hand_order;
namespace {
double milliseconds(const QElapsedTimer& timer) { return timer.nsecsElapsed() / 1000000.0; }
QString sha256(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
QString resolvedDestination(const QString& path)
{
    QFileInfo ancestor(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QStringList tail;
    while (!ancestor.exists()) {
        tail.prepend(ancestor.fileName());
        const auto parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath()) return {};
        ancestor = QFileInfo(parent);
    }
    return QDir::cleanPath(QDir(ancestor.canonicalFilePath()).filePath(tail.join('/')));
}
QJsonArray messages(const QVector<SimaiMessage>& source)
{
    QJsonArray result;
    for (const auto& item : source) result.append(QJsonObject{{"line", item.line}, {"col", item.col}, {"message", item.message}});
    return result;
}
QJsonArray numbers(const QVector<double>& source)
{
    QJsonArray result; for (double value : source) result.append(value); return result;
}
QJsonArray points(const QVector<QPointF>& source)
{
    QJsonArray result;
    for (const auto& point : source) result.append(QJsonArray{point.x(), point.y()});
    return result;
}
QJsonObject nativeMarker(const TimelineNoteMarker& note)
{
    QJsonArray shapes, segmentPoints, angles, wifiPoints, wifiAngles, padTimes, wifiPadTimes;
    for (const auto& key : note.slideSegmentKeys) shapes.append(key);
    for (const auto& path : note.slideSegmentPoints) segmentPoints.append(points(path));
    for (const auto& path : note.slideSegmentAngles) angles.append(numbers(path));
    for (const auto& path : note.wifiLanePoints) wifiPoints.append(points(path));
    for (const auto& path : note.wifiLaneAngles) wifiAngles.append(numbers(path));
    for (const auto& pad : note.wifiPadEnterTimes) wifiPadTimes.append(QJsonArray{pad.pad, pad.proportion});
    for (const auto& segment : note.slideSegmentPadEnterTimes) {
        QJsonArray pads;
        for (const auto& pad : segment) pads.append(QJsonArray{pad.pad, pad.proportion});
        padTimes.append(pads);
    }
    return {{"type", note.type}, {"lane", note.lane}, {"end_lane", note.endLane}, {"pad", note.touchPad},
        {"second", note.second}, {"end_second", note.endSecond}, {"trace_second", note.slideTraceSecond},
        {"available_second", note.availableSecond}, {"tempo", note.tempoBpm}, {"subdivision", note.subdivision},
        {"hs", note.hsMultiplier}, {"head", note.hasHeadStar}, {"headless_immediate", note.headlessImmediate},
        {"branch", note.slideBranchIndex}, {"each", note.isEach}, {"break", note.isBreak}, {"ex", note.isEx},
        {"firework", note.isFirework}, {"mine", note.isMine}, {"head_mine", note.headMine}, {"track_mine", note.trackMine},
        {"head_break", note.headBreak}, {"head_ex", note.headEx}, {"track_break", note.trackBreak},
        {"star_material", note.tapUsesStarMaterial}, {"double_star", note.tapStarDouble},
        {"head_tap_material", note.slideHeadUsesTapMaterial}, {"display_key", note.slideDisplayKey},
        {"on_slide", note.onSlide}, {"slide_head", note.slideHead}, {"tail_on_slide_head", note.tailOnSlideHead},
        {"slide_each", note.slideEach}, {"same_head_slide", note.sameHeadSlide},
        {"before_slide", note.beforeSlide}, {"after_slide", note.afterSlide}, {"head_each", note.headEach},
        {"track_key", note.slideTrackKey}, {"segment_keys", shapes},
        {"segment_starts", numbers(note.slideSegmentShootSeconds)}, {"segment_durations", numbers(note.slideSegmentDurations)},
        {"segment_points", segmentPoints}, {"segment_angles", angles}, {"segment_pad_times", padTimes},
        {"segment_critical_proportions", numbers(note.slideSegmentCriticalProportions)},
        {"wifi_points", wifiPoints}, {"wifi_angles", wifiAngles}, {"wifi_pad_times", wifiPadTimes},
        {"wifi_critical_proportion", note.wifiCriticalProportion},
        {"touch_point", QJsonArray{note.touchPoint.x(), note.touchPoint.y()}},
        {"native_length", note.slideNativeTrackLength}, {"runtime_length", note.slideRuntimeTrackLength}};
}
QJsonObject timeMarker(const TimelineNoteMarker& note)
{
    return {{"second", note.second}, {"end_second", note.endSecond}, {"trace_second", note.slideTraceSecond},
        {"tempo", note.tempoBpm}, {"subdivision", note.subdivision}, {"hs", note.hsMultiplier},
        {"segment_starts", numbers(note.slideSegmentShootSeconds)}, {"segment_durations", numbers(note.slideSegmentDurations)}};
}
QJsonArray fingerprint(const SimaiParseResult& parsed, bool timingOnly = false)
{
    QJsonArray result;
    for (const auto& marker : parsed.noteMarkers) result.append(timingOnly ? timeMarker(marker) : nativeMarker(marker));
    return result;
}
QJsonArray stops(const QVector<HandOrderTrackStop>& values)
{
    QJsonArray result;
    for (const auto& stop : values) result.append(QJsonArray{stop.progress, int(stop.hand)});
    return result;
}
QJsonObject annotation(const TimelineNoteMarker& marker)
{
    QJsonArray sectors;
    for (const auto& sequence : marker.handOrderWifiSectorStops) sectors.append(stops(sequence));
    return {{"head", int(marker.handOrderHead)}, {"track", stops(marker.handOrderTrackStops)},
        {"wifi_mode", int(marker.handOrderWifiMode)}, {"wifi_sectors", sectors}};
}
QJsonObject evaluationJson(const BasicAssignmentEvaluation& value)
{
    const auto metric = [&](double number) -> QJsonValue { return value.complete ? QJsonValue(number) : QJsonValue(QJsonValue::Null); };
    return {{"complete", value.complete}, {"cost", metric(value.cost)}, {"risk", metric(value.risk)},
        {"ergonomic_breaks", value.complete ? QJsonValue(value.ergonomicBreaks) : QJsonValue(QJsonValue::Null)},
        {"interaction_breaks", value.complete ? QJsonValue(value.interactionBreaks) : QJsonValue(QJsonValue::Null)},
        {"longitudinal_breaks", value.complete ? QJsonValue(value.longitudinalBreaks) : QJsonValue(QJsonValue::Null)},
        {"same_key_breaks", value.complete ? QJsonValue(value.sameKeyBreaks) : QJsonValue(QJsonValue::Null)},
        {"peak_speed", metric(value.peakSpeed)}, {"player_cost", metric(value.playerCost)},
        {"peak_tap_rate", metric(value.peakTapRate)}, {"peak_load", metric(value.peakLoad)}};
}
QJsonArray absoluteTiming(const SimaiParseResult& parsed)
{
    QJsonArray result;
    const auto normalized = [](double value) { return std::round(value * 1000000000.0) / 1000000000.0; };
    const auto normalizedSequence = [&](const QVector<double>& values) {
        QJsonArray times; for (double value : values) times.append(normalized(value)); return times;
    };
    for (const auto& marker : parsed.noteMarkers) result.append(QJsonObject{
        {"second", normalized(marker.second)}, {"end_second", normalized(marker.endSecond)},
        {"trace_second", normalized(marker.slideTraceSecond)},
        {"segment_starts", normalizedSequence(marker.slideSegmentShootSeconds)},
        {"segment_durations", normalizedSequence(marker.slideSegmentDurations)}});
    return result;
}
bool concrete(HandOrderHand hand) { return hand == HandOrderHand::Left || hand == HandOrderHand::Right; }
bool concreteMarker(const TimelineNoteMarker& marker)
{
    const bool slide = marker.type == "slide" || marker.type == "wifi";
    if ((!slide || marker.hasHeadStar) && !concrete(marker.handOrderHead)) return false;
    if (!slide) return true;
    const auto valid = [](const auto& sequence) {
        return !sequence.isEmpty() && std::all_of(sequence.begin(), sequence.end(), [](const auto& stop) { return concrete(stop.hand); });
    };
    if (marker.handOrderWifiMode != HandOrderWifiMode::Split) return valid(marker.handOrderTrackStops);
    return marker.handOrderWifiSectorStops.size() == 2
        && valid(marker.handOrderWifiSectorStops[0]) && valid(marker.handOrderWifiSectorStops[1]);
}
QJsonObject parametersJson(const BasicParameters& p)
{
    return {{"algorithm_version", p.version}, {"judge_profile_revision", p.judgeProfileRevision},
        {"max_work_units", p.maxWorkUnits}, {"beam_width", p.beamWidth}, {"counterfactual_budget", p.counterfactualBudget},
        {"max_tasks", p.maxTasks}, {"context_seconds", p.contextSeconds}, {"press_seconds", p.pressSeconds},
        {"return_speed", p.returnSpeed}, {"sweep_threshold", p.sweepThreshold}, {"input_preference", p.inputPreference},
        {"overwrite_existing", p.overwriteExisting}, {"high_confidence_only", p.highConfidenceOnly},
        {"color_unresolved", p.colorUnresolved}, {"evaluate_complete_assignment", p.evaluateCompleteAssignment},
        {"legacy_fixed_target_lookahead", p.legacyFixedTargetLookahead},
        {"capture_rejected_candidates", p.captureRejectedCandidates},
        {"reuse_window_search", p.reuseWindowSearch},
        {"diagnostic_slide_input_offset_seconds", std::isfinite(p.diagnosticSlideInputOffsetSeconds)
            ? QJsonValue(p.diagnosticSlideInputOffsetSeconds) : QJsonValue(QJsonValue::Null)},
        {"player", QJsonObject{{"max_comfortable_tap_rate", p.player.maxComfortableTapRate},
            {"comfortable_move_speed", p.player.comfortableMoveSpeed}, {"stamina_capacity", p.player.staminaCapacity},
            {"recovery_half_life", p.player.recoveryHalfLife}, {"technique_preference", p.player.techniquePreference},
            {"posture_weight", p.player.postureWeight}}}};
}
QJsonObject resultJson(const BasicResult& result, bool detailed = false)
{
    QJsonObject statuses, reasons;
    QJsonArray rows;
    int unresolved = 0, verified = 0;
    for (const auto& row : result.rows) {
        statuses[row.status] = statuses.value(row.status).toInt() + 1;
        reasons[row.reason] = reasons.value(row.reason).toInt() + 1;
        unresolved += row.status != "review" && row.status != "preserved" && row.status != "context";
        verified += row.fullReplayVerified;
        QJsonObject rowData{{"start", row.start}, {"end", row.end}, {"status", row.status}, {"reason", row.reason},
            {"verified", row.fullReplayVerified}, {"cost", row.cost}, {"player_cost", row.playerCost},
            {"peak_speed", row.peakSpeed}, {"peak_tap_rate", row.peakTapRate}, {"peak_load", row.peakLoad},
            {"confidence", row.confidence}, {"confidence_level", row.confidenceLevel}, {"dependency_unit", row.dependencyUnit},
            {"context", row.context}, {"has_candidate", row.hasCandidate}, {"head", int(row.head)}, {"track", int(row.track)},
            {"input_channel", row.inputChannel}, {"judgement_kind", row.judgementKind}, {"judgement_quality", row.judgementQuality},
            {"judgement_second", row.judgementSecond}, {"judgement_margin", row.judgementMargin}};
        if (detailed) {
            QJsonArray causes, notes;
            int handMask = 0;
            for (const auto& cause : row.causes) {
                handMask |= cause.handMask;
                causes.append(QJsonObject{{"token_start", cause.tokenStart}, {"branch", cause.branch}, {"kind", cause.kind},
                    {"pad", cause.pad}, {"second", cause.second}, {"progress", cause.progress}, {"hand_mask", cause.handMask}});
            }
            for (int i = 0; i < row.lanes.size(); ++i) notes.append(QJsonArray{row.lanes[i], int(row.noteHands.value(i))});
            rowData["causes"] = causes; rowData["cause_hand_mask_union"] = handMask; rowData["notes"] = notes;
            rowData["conditions"] = row.conditions; rowData["original_annotation"] = row.originalAnnotation;
        }
        rows.append(rowData);
    }
    QJsonObject summary{{"error", result.error}, {"work_used", result.workUsed}, {"truncated", result.truncated},
        {"evidence_truncated", result.evidenceTruncated}, {"work_limited", result.workLimited}, {"task_limited", result.taskLimited},
        {"window_count", result.windowCount}, {"max_window_tasks", result.maxWindowTasks},
        {"included_objects", result.includedObjects}, {"audited_objects", result.auditedObjects},
        {"patch_count", result.patches.size()}, {"unresolved_rows", unresolved}, {"verified_rows", verified},
        {"status_counts", statuses}, {"reason_counts", reasons}, {"rows", rows},
        {"pattern_outcome", result.patternOutcome}, {"pattern_segments", result.patternSegments},
        {"pattern_candidates", result.patternCandidates}, {"pattern_accepted", result.patternAccepted}};
    if (detailed) {
        QJsonArray windows;
        for (const auto& window : result.searchWindows) windows.append(QJsonObject{
            {"start", window.start}, {"end", window.end}, {"tasks", window.tasks}, {"core_tokens", window.coreTokens},
            {"planned_tokens", window.plannedTokens}, {"protected_target_imports", window.protectedTargetImports},
            {"protected_context_imports", window.protectedContextImports}, {"decision_tasks", window.decisionTasks},
            {"touch_tasks", window.touchTasks}, {"carried_actions", window.carriedActions},
            {"search_attempts", window.searchAttempts}, {"confidence_iteration_limited", window.confidenceIterationLimited},
            {"search_requests", window.searchRequests}, {"search_cache_hits", window.searchCacheHits},
            {"repeated_complete_searches", window.repeatedCompleteSearches}, {"repeated_complete_work", window.repeatedCompleteWork},
            {"reused_search_work", window.reusedSearchWork},
            {"work_used", window.workUsed}, {"outcome", window.outcome}});
        summary["windows"] = windows;
        QJsonObject rejectedCounts;
        QJsonArray rejectedSamples;
        for (auto it = result.rejectionCounts.cbegin(); it != result.rejectionCounts.cend(); ++it)
            rejectedCounts[it.key()] = it.value();
        for (const auto& sample : result.rejectionSamples) rejectedSamples.append(QJsonObject{
            {"category", sample.category}, {"window", sample.window}, {"detail", sample.detail}});
        summary["rejected_candidate_counts"] = rejectedCounts;
        summary["rejected_candidate_samples"] = rejectedSamples;
        summary["rejected_candidate_sample_limit"] = 12;
    }
    if (result.patternOptimization) {
        summary["internal_acceptance_baseline"] = evaluationJson(result.originalEvaluation);
        summary["internal_acceptance_result"] = evaluationJson(result.evaluation);
    }
    if (!result.contextGuardOutcome.isEmpty()) {
        summary["context_guard_outcome"] = result.contextGuardOutcome;
        summary["context_guard_baseline"] = evaluationJson(result.contextBaselineEvaluation);
        summary["context_guard_internal_candidate"] = evaluationJson(result.contextInternalEvaluation);
        summary["context_guard_serialized_candidate"] = evaluationJson(result.contextSerializedEvaluation);
        summary["context_guard_work"] = QJsonObject{{"baseline", result.contextBaselineWorkUsed},
            {"search", result.contextSearchWorkUsed}, {"serialized", result.contextSerializedWorkUsed}};
    }
    return summary;
}
QJsonObject coverageJson(const SimaiParseResult& parsed)
{
    QJsonArray unresolved;
    for (const auto& marker : parsed.noteMarkers) if (!concreteMarker(marker)) unresolved.append(QJsonObject{
        {"parse_order", marker.parseOrder}, {"type", marker.type}, {"lane", marker.lane}, {"pad", marker.touchPad},
        {"second", marker.second}, {"line", marker.sourceLine}, {"col", marker.sourceCol}, {"annotation", annotation(marker)}});
    return {{"markers", parsed.noteMarkers.size()}, {"unresolved_count", unresolved.size()}, {"unresolved", unresolved}};
}
QJsonObject fixedEvaluation(const QString& chart, const SimaiParseResult& parsed, BasicParameters fixed,
    const simai::SimaiTimingMetadata& metadata, bool detailed, const HandOrderProgressCallback& progress = {})
{
    fixed.overwriteExisting = false; fixed.highConfidenceOnly = false; fixed.colorUnresolved = false;
    fixed.counterfactualBudget = 0; fixed.evaluateCompleteAssignment = true;
    auto evaluation = evaluationJson({});
    const auto coverage = coverageJson(parsed);
    const bool concreteInput = coverage.value("unresolved_count").toInt() == 0;
    evaluation["attempted"] = concreteInput; evaluation["work_used"] = QJsonValue::Null;
    evaluation["annotation_coverage"] = coverage;
    evaluation["parameters"] = parametersJson(fixed);
    if (!concreteInput) {
        evaluation["reason"] = "missing_or_nonconcrete_annotations_no_inferred_inputs_substituted";
        evaluation["outcome"] = "unavailable"; return evaluation;
    }
    QElapsedTimer algorithm; algorithm.start();
    const auto audit = inferBasicHandOrder(chart, parsed, 0, chart.size(), fixed, metadata, progress);
    const double algorithmMs = milliseconds(algorithm);
    auto value = audit.evaluation;
    if (!audit.patches.isEmpty()) value.complete = false;
    evaluation = evaluationJson(value);
    evaluation["attempted"] = true; evaluation["work_used"] = audit.workUsed;
    evaluation["annotation_coverage"] = coverage;
    evaluation["parameters"] = parametersJson(fixed); evaluation["algorithm_elapsed_ms"] = algorithmMs;
    evaluation["error"] = audit.error; evaluation["work_limited"] = audit.workLimited;
    evaluation["task_limited"] = audit.taskLimited; evaluation["fixed_patches_empty"] = audit.patches.isEmpty();
    evaluation["diagnostics"] = resultJson(audit, detailed);
    evaluation["outcome"] = value.complete ? "completed" : "unavailable";
    if (!audit.patches.isEmpty()) evaluation["reason"] = "fixed_assignment_was_rewritten";
    else if (!value.complete) evaluation["reason"] = "complete_fixed_replay_or_physical_scoring_not_verified";
    return evaluation;
}
QString applyPatches(const QString& source, const QVector<chart_transform::HandOrderEditPlan::Patch>& patches, int& changed)
{
    QString output = source;
    int next = source.size();
    for (auto it = patches.crbegin(); it != patches.crend(); ++it) {
        if (it->start < 0 || it->end < it->start || it->end > next) throw QStringLiteral("Overlapping or invalid patches");
        changed += source.mid(it->start, it->end - it->start) != it->text;
        output.replace(it->start, it->end - it->start, it->text); next = it->start;
    }
    return output;
}

HandOrderHand trackHandAt(const QVector<HandOrderTrackStop>& stops, double progress)
{
    auto hand = HandOrderHand::Unspecified;
    for (const auto& stop : stops) if (stop.progress <= progress + 1e-7) hand = stop.hand;
    return hand;
}
QJsonObject referenceAgreement(const QVector<TimelineNoteMarker>& actual,
    const QVector<TimelineNoteMarker>& reference)
{
    QHash<int, TimelineNoteMarker> byOrder;
    for (const auto& marker : reference) byOrder.insert(marker.parseOrder, marker);
    int heads = 0, headsMatched = 0, headsMissing = 0, tracks = 0, tracksMatched = 0;
    int samples = 0, samplesMatched = 0, relations = 0, relationsMatched = 0;
    QJsonObject headMismatches;
    auto ordered = actual;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second < b.second : a.parseOrder < b.parseOrder;
    });
    HandOrderHand previousActual = HandOrderHand::Unspecified, previousReference = HandOrderHand::Unspecified;
    for (const auto& marker : ordered) {
        if (!byOrder.contains(marker.parseOrder)) continue;
        const auto expected = byOrder.value(marker.parseOrder);
        const bool slide = marker.type == "slide" || marker.type == "wifi";
        if ((!slide || marker.hasHeadStar) && concrete(expected.handOrderHead)) {
            ++heads;
            headsMatched += marker.handOrderHead == expected.handOrderHead;
            headsMissing += !concrete(marker.handOrderHead);
            if (marker.handOrderHead != expected.handOrderHead) {
                const QString category = marker.type + ':' + QString::number(marker.lane) + ':'
                    + QString::number(int(marker.handOrderHead)) + "->" + QString::number(int(expected.handOrderHead));
                headMismatches[category] = headMismatches.value(category).toInt() + 1;
            }
            if (concrete(previousActual) && concrete(previousReference) && concrete(marker.handOrderHead)) {
                ++relations;
                relationsMatched += (previousActual == marker.handOrderHead) == (previousReference == expected.handOrderHead);
            }
            previousActual = marker.handOrderHead; previousReference = expected.handOrderHead;
        }
        if (!slide) continue;
        const auto compareStops = [&](const auto& predicted, const auto& wanted) {
            if (wanted.isEmpty()) return;
            ++tracks;
            tracksMatched += stops(predicted) == stops(wanted);
            for (double progress : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                const auto expectedHand = trackHandAt(wanted, progress);
                if (!concrete(expectedHand)) continue;
                ++samples; samplesMatched += trackHandAt(predicted, progress) == expectedHand;
            }
        };
        if (expected.handOrderWifiMode == HandOrderWifiMode::Split) {
            for (int sector = 0; sector < expected.handOrderWifiSectorStops.size(); ++sector)
                compareStops(marker.handOrderWifiMode == HandOrderWifiMode::Split
                    ? marker.handOrderWifiSectorStops.value(sector) : marker.handOrderTrackStops,
                    expected.handOrderWifiSectorStops[sector]);
        } else compareStops(marker.handOrderTrackStops, expected.handOrderTrackStops);
    }
    return {{"heads_compared", heads}, {"heads_matched", headsMatched}, {"heads_unassigned", headsMissing},
        {"track_sequences_compared", tracks}, {"track_sequences_matched", tracksMatched},
        {"track_samples_compared", samples}, {"track_samples_matched", samplesMatched},
        {"head_relations_compared", relations}, {"head_relations_matched", relationsMatched},
        {"head_mismatch_categories", headMismatches}};
}

int referenceRulesAudit(const QString& inputPath, const QByteArray& sourceBytes,
    const QString& referencePath, const QString& outputDir, int difficultyId, const BasicParameters& parameters)
{
    // This path never serializes chart text, marker lists, patches or individual annotations.
    // Both documents and all generated annotations live only for this invocation.
    QFile referenceFile(referencePath);
    if (!referenceFile.open(QIODevice::ReadOnly)) throw QStringLiteral("Cannot read reference.");
    const auto referenceBytes = referenceFile.readAll(); referenceFile.close();
    const auto source = SimaiDocument::fromText(QString::fromUtf8(sourceBytes));
    const auto reference = SimaiDocument::fromText(QString::fromUtf8(referenceBytes));
    const auto* sourceDifficulty = source.difficulty(difficultyId);
    const auto* referenceDifficulty = reference.difficulty(difficultyId);
    if (!sourceDifficulty || !referenceDifficulty) throw QStringLiteral("Source or reference difficulty is absent.");
    const auto metadata = simai::buildTimingMetadata(source);
    const auto referenceParsed = SimaiParser::validateSyntax(referenceDifficulty->chart, simai::buildTimingMetadata(reference));
    const auto chart = stripAnnotations(sourceDifficulty->chart);
    const auto parsed = SimaiParser::validateSyntax(chart, metadata);
    const bool nativeSame = fingerprint(parsed) == fingerprint(referenceParsed);
    const bool timingSame = fingerprint(parsed, true) == fingerprint(referenceParsed, true);
    QJsonObject report{{"schema_version", 1}, {"mode", "read_only_reference_rules"},
        {"source_path", inputPath}, {"source_sha256", sha256(sourceBytes)},
        {"reference_path", referencePath}, {"reference_sha256", sha256(referenceBytes)},
        {"difficulty", difficultyId}, {"algorithm_version", basicAlgorithmVersion},
        {"note_marker_count", parsed.noteMarkers.size()}, {"native_reference_equal", nativeSame},
        {"timing_reference_equal", timingSame}, {"chart_files_written", 0},
        {"interpretation", "Agreement with one human reference is a regression signal, not a proof of player comfort, physical feasibility or a unique correct hand assignment. Marker heads include shared-star branch markers; track samples use progress 0, 0.25, 0.5, 0.75 and 1."}};
    QJsonArray stages;
    bool valid = parsed.ok && parsed.annotationErrors.isEmpty() && referenceParsed.ok
        && referenceParsed.annotationErrors.isEmpty() && nativeSame && timingSame;
    if (valid) {
        QElapsedTimer timer; timer.start();
        int changed = 0;
        const auto layout = chart_transform::basicHandOrderLayoutInSelection(chart, 0, 0, metadata,
            parameters.returnSpeed, parameters.sweepThreshold, parameters.colorUnresolved);
        const auto laidOut = applyPatches(chart, layout.edit.patches, changed);
        const auto layoutParsed = SimaiParser::validateSyntax(laidOut, metadata);
        const bool layoutValid = layoutParsed.ok && layoutParsed.annotationErrors.isEmpty()
            && fingerprint(layoutParsed) == fingerprint(parsed);
        stages.append(QJsonObject{{"id", "layout"}, {"elapsed_ms", milliseconds(timer)},
            {"native_unchanged", layoutValid}, {"changed_tokens", changed},
            {"agreement", referenceAgreement(layoutParsed.noteMarkers, referenceParsed.noteMarkers)}});
        valid &= layoutValid;
        const auto scanStage = [&](const QString& before, const SimaiParseResult& beforeParsed, const QString& id) {
            timer.restart(); changed = 0;
            const auto result = optimizeHandOrderPatterns(before, beforeParsed, 0, before.size(), parameters, {}, metadata);
            const auto after = applyPatches(before, result.patches, changed);
            const auto afterParsed = SimaiParser::validateSyntax(after, metadata);
            const bool unchanged = afterParsed.ok && afterParsed.annotationErrors.isEmpty()
                && fingerprint(afterParsed) == fingerprint(parsed)
                && stripAnnotations(after) == stripAnnotations(before);
            int overlappingPatches = 0;
            for (int i = 1; i < result.patches.size(); ++i)
                overlappingPatches += result.patches[i].start < result.patches[i - 1].end;
            QHash<QString, QVector<TimelineNoteMarker>> defaults;
            QJsonObject counts;
            int preferred = 0;
            for (const auto& group : result.patternSuggestions) {
                counts[group.registryId] = counts.value(group.registryId).toInt() + 1;
                if (group.preferred < 0) continue;
                ++preferred; defaults[group.registryId] += group.variants[group.preferred].markers;
            }
            QJsonObject agreementByRule;
            for (auto it = defaults.cbegin(); it != defaults.cend(); ++it)
                agreementByRule[it.key()] = referenceAgreement(it.value(), referenceParsed.noteMarkers);
            stages.append(QJsonObject{{"id", id}, {"elapsed_ms", milliseconds(timer)},
                {"native_unchanged", unchanged}, {"changed_tokens", changed},
                {"patch_overlaps", overlappingPatches}, {"syntax_valid", afterParsed.ok},
                {"annotation_error_count", afterParsed.annotationErrors.size()},
                {"native_markers_unchanged", fingerprint(afterParsed) == fingerprint(parsed)},
                {"native_text_unchanged", stripAnnotations(after) == stripAnnotations(before)},
                {"pattern_count", result.patternSuggestions.size()}, {"preferred_count", preferred},
                {"pattern_counts", counts}, {"default_agreement_by_rule", agreementByRule},
                {"agreement", referenceAgreement(afterParsed.noteMarkers, referenceParsed.noteMarkers)}});
            valid &= unchanged && result.error.isEmpty();
        };
        scanStage(chart, parsed, "rules_on_native");
        scanStage(laidOut, layoutParsed, "rules_after_layout");
    }
    QFile sourceAgain(inputPath), referenceAgain(referencePath);
    const bool sourceUnchanged = sourceAgain.open(QIODevice::ReadOnly) && sourceAgain.readAll() == sourceBytes;
    const bool referenceUnchanged = referenceAgain.open(QIODevice::ReadOnly) && referenceAgain.readAll() == referenceBytes;
    report["source_bytes_unchanged"] = sourceUnchanged; report["reference_bytes_unchanged"] = referenceUnchanged;
    report["stages"] = stages;
    report["outcome"] = valid && sourceUnchanged && referenceUnchanged ? "completed" : "reference_or_invariant_failed";
    const auto reportPath = QDir(outputDir).filePath("report.json");
    if (!writeFile(reportPath, QJsonDocument(report).toJson())) throw QStringLiteral("Cannot write reference report.");
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"report", reportPath}, {"outcome", report["outcome"]},
        {"algorithm_version", basicAlgorithmVersion}, {"chart_files_written", 0}}).toJson(QJsonDocument::Compact) << Qt::endl;
    return valid && sourceUnchanged && referenceUnchanged ? 0 : 4;
}
class Deadline final {
public:
    Deadline(task::CancellationFlag token, qint64 timeout) : token_(std::move(token))
    {
        if (timeout > 0) worker_ = std::thread([this, timeout] {
            std::unique_lock lock(mutex_);
            if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return done_; }))
                token_->store(true, std::memory_order_relaxed);
        });
    }
    ~Deadline()
    {
        { std::lock_guard lock(mutex_); done_ = true; }
        condition_.notify_all(); if (worker_.joinable()) worker_.join();
    }
private:
    task::CancellationFlag token_;
    std::mutex mutex_; std::condition_variable condition_; bool done_ = false; std::thread worker_;
};
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.setApplicationDescription("Sequential Basic Layout -> Context Optimization -> Pattern Optimization audit. Synthetic/local chart input is read only.");
    cli.addHelpOption(); cli.addPositionalArgument("maidata", "Input maidata.txt (read only).");
    cli.addOption({"output-dir", "Fresh/empty workspace directory for report.json and all three stage maidata files.", "path"});
    cli.addOption({"difficulty", "Difficulty slot, 1..7.", "id", "5"});
    cli.addOption({"timeout-ms", "Cooperative deadline for the complete process, including extra evaluations; 0 disables it.", "ms", "0"});
    cli.addOption({"max-work", "Per-operation work budget; default matches the GUI standard profile.", "units", "300000000"});
    cli.addOption({"beam-width", "Diagnostic beam width, 1..128; standard acceptance remains 64.", "count", "64"});
    cli.addOption({"fixed-evaluation-only", "Cold fixed-label evaluation of the input chart; do not run layout/context/pattern or apply patches."});
    cli.addOption({"reference", "Read-only human maidata reference for aggregate rule/layout agreement; requires --rules-only --report-only.", "path"});
    cli.addOption({"rules-only", "Reference audit: run layout and structural rules in memory, without physical search."});
    cli.addOption({"report-only", "Reference audit: write only aggregate report.json; never save chart text or marker/token lists."});
    cli.addOption({"diagnostics", "Include detailed row causes, channels and rolling-window evidence."});
    cli.addOption({"no-window-search-cache", "Diagnostic: rerun completed window attempts instead of memoizing; report exact bounded duplicate work."});
    cli.addOption({"legacy-fixed-target-lookahead", "Diagnostic v18 comparison: import all overlapping target locks into fixed-evaluation windows."});
    cli.addOption({"slide-input-offset-ms", "Diagnostic: pin every non-Wifi Slide input within -200..200ms; default proposals and note times stay fixed.", "ms"});
    cli.process(app);
    const auto fail = [](const QString& error) { QTextStream(stderr) << error << Qt::endl; return 2; };
    if (cli.positionalArguments().size() != 1 || !cli.isSet("output-dir")) return fail("Expected maidata and --output-dir.");
    bool numberOk = false;
    const int difficultyId = cli.value("difficulty").toInt(&numberOk);
    if (!numberOk || difficultyId < 1 || difficultyId > 7) return fail("Invalid difficulty.");
    const qint64 timeout = cli.value("timeout-ms").toLongLong(&numberOk);
    if (!numberOk || timeout < 0 || timeout > 86400000) return fail("Invalid timeout-ms.");
    BasicParameters parameters; parameters.overwriteExisting = true;
    const bool fixedOnly = cli.isSet("fixed-evaluation-only");
    const bool detailed = fixedOnly || cli.isSet("diagnostics");
    parameters.captureRejectedCandidates = detailed;
    parameters.reuseWindowSearch = !cli.isSet("no-window-search-cache");
    parameters.legacyFixedTargetLookahead = cli.isSet("legacy-fixed-target-lookahead");
    if (fixedOnly) { parameters.overwriteExisting = false; parameters.evaluateCompleteAssignment = true; parameters.counterfactualBudget = 0; }
    parameters.maxWorkUnits = cli.value("max-work").toLongLong(&numberOk);
    if (!numberOk || parameters.maxWorkUnits < 1000 || parameters.maxWorkUnits > BasicParameters::maxAllowedWorkUnits)
        return fail("Invalid max-work.");
    parameters.beamWidth = cli.value("beam-width").toInt(&numberOk);
    if (!numberOk || parameters.beamWidth < 1 || parameters.beamWidth > 128) return fail("Invalid beam-width.");
    if (cli.isSet("slide-input-offset-ms")) {
        const double value = cli.value("slide-input-offset-ms").toDouble(&numberOk);
        if (!numberOk || !std::isfinite(value) || std::abs(value) > 200) return fail("Invalid slide-input-offset-ms.");
        parameters.diagnosticSlideInputOffsetSeconds = value / 1000;
    }
    const QString inputPath = QFileInfo(cli.positionalArguments().first()).canonicalFilePath();
    const QString outputDir = resolvedDestination(cli.value("output-dir"));
    const QString workspace = QFileInfo(QStringLiteral(MIACODE_PIPELINE_WORKSPACE)).canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    if (inputPath.isEmpty() || outputDir.isEmpty() || workspace.isEmpty()
        || !outputDir.startsWith(workspace + '/', pathCase)) return fail("Input must exist; output must be a child of this workspace.");
    if (QFileInfo::exists(outputDir) && (!QFileInfo(outputDir).isDir()
        || !QDir(outputDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()))
        return fail("Output directory must be fresh or empty; no existing output/input is overwritten.");
    if (cli.isSet("reference") || cli.isSet("rules-only") || cli.isSet("report-only")) {
        if (!cli.isSet("reference") || !cli.isSet("rules-only") || !cli.isSet("report-only") || fixedOnly)
            return fail("Reference comparison requires --reference, --rules-only and --report-only together, without fixed evaluation.");
        const auto referencePath = QFileInfo(cli.value("reference")).canonicalFilePath();
        if (referencePath.isEmpty()) return fail("Reference must exist.");
        QFile sourceFile(inputPath);
        if (!sourceFile.open(QIODevice::ReadOnly)) return fail("Cannot read input.");
        const auto sourceBytes = sourceFile.readAll(); sourceFile.close();
        try { return referenceRulesAudit(inputPath, sourceBytes, referencePath, outputDir, difficultyId, parameters); }
        catch (const QString& error) { return fail(error); }
    }
    QFile sourceFile(inputPath);
    if (!sourceFile.open(QIODevice::ReadOnly)) return fail("Cannot read input.");
    const QByteArray sourceBytes = sourceFile.readAll();
    const bool bom = sourceBytes.startsWith("\xEF\xBB\xBF");
    const QString documentText = QString::fromUtf8(bom ? sourceBytes.mid(3) : sourceBytes);
    const auto document = SimaiDocument::fromText(documentText);
    const auto* difficulty = document.difficulty(difficultyId);
    if (!difficulty || difficulty->chart.trimmed().isEmpty()) return fail("Difficulty is empty.");
    int chartOffset = -1, headersFound = 0;
    auto fields = QRegularExpression(QStringLiteral(R"((?m)^[^\S\r\n]*&([^\r\n=]+)=)")).globalMatch(documentText);
    while (fields.hasNext()) {
        const auto field = fields.next();
        if (field.captured(1) == QStringLiteral("inote_%1").arg(difficultyId)) { chartOffset = field.capturedEnd(); ++headersFound; }
    }
    if (headersFound != 1 || documentText.mid(chartOffset, difficulty->chart.size()) != difficulty->chart)
        return fail("Expected one unambiguous inote source field.");
    const auto metadata = simai::buildTimingMetadata(document);
    const QString original = difficulty->chart;
    const auto token = std::make_shared<std::atomic_bool>(false);
    Deadline deadline(token, timeout);
    task::CancellationScope cancellation(token);
    QElapsedTimer wall; wall.start();
    QJsonArray stages;
    QJsonObject report{{"schema_version", 1}, {"outcome", "running"}, {"source_path", inputPath},
        {"mode", fixedOnly ? "fixed_evaluation" : "pipeline"},
        {"source_sha256", sha256(sourceBytes)}, {"output_dir", outputDir}, {"title", document.title},
        {"difficulty", difficultyId}, {"timeout_ms", timeout}, {"parameters", parametersJson(parameters)},
        {"pipeline_elapsed_ms", 0.0}, {"extra_evaluation_elapsed_ms", 0.0}, {"verification_elapsed_ms", 0.0},
        {"main_pipeline_completed", false}, {"absolute_timing_precision_seconds", 1e-9},
        {"interpretation", fixedOnly
            ? "Completed means cold fixed-label evaluation returned; complete=false is not proof of unplayability. No chart is edited or inferred to fill missing labels."
            : "Completed means the three operations ran without native changes. Named-strategy coverage, human comfort and cabinet correctness are not implied. Incomplete fixed assignments have null evaluation metrics."}};
    const auto persist = [&] {
        report["stages"] = stages; report["wall_elapsed_ms"] = milliseconds(wall);
        if (!writeFile(QDir(outputDir).filePath("report.json"), QJsonDocument(report).toJson())) throw QStringLiteral("Cannot write report");
    };
    const auto event = [&](const QString& name, const QString& stage) {
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"event", name}, {"stage", stage},
            {"wall_elapsed_ms", milliseconds(wall)}}).toJson(QJsonDocument::Compact) << Qt::endl;
    };
    QElapsedTimer phaseTimer;
    QString phase = "input_validation";
    int currentStage = -1;
    double operationTotal = 0, evaluationTotal = 0, verificationTotal = 0;
    try {
        persist(); event("start", phase); phaseTimer.start();
        const auto originalParsed = SimaiParser::validateSyntax(original, metadata);
        report["input_parse_elapsed_ms"] = milliseconds(phaseTimer);
        report["input_parse_errors"] = messages(originalParsed.errors);
        report["input_annotation_errors"] = messages(originalParsed.annotationErrors);
        report["note_marker_count"] = originalParsed.noteMarkers.size();
        if (!originalParsed.ok || !originalParsed.annotationErrors.isEmpty()) {
            report["outcome"] = "parse_rejected"; persist(); event("parse_rejected", phase); return 3;
        }
        if (fixedOnly) {
            currentStage = 0; phase = "evaluation";
            auto pending = evaluationJson({}); pending["attempted"] = true; pending["outcome"] = "running";
            stages.append(QJsonObject{{"id", "fixed_evaluation"}, {"outcome", "running"}, {"evaluation", pending}});
            report["current_stage"] = "fixed_evaluation"; report["current_phase"] = phase;
            persist(); event("evaluation_start", "fixed_evaluation"); phaseTimer.restart();
            QElapsedTimer progressClock; progressClock.start();
            const auto progress = [&](const HandOrderProgress& update) {
                if (progressClock.elapsed() < 2000) return;
                progressClock.restart();
                report["current_progress"] = QJsonObject{{"phase", int(update.phase)}, {"completed", update.completed},
                    {"total", update.total}, {"window", update.window}, {"windows", update.windows}, {"attempt", update.attempt}};
                persist(); event("progress", "fixed_evaluation");
            };
            auto evaluated = fixedEvaluation(original, originalParsed, parameters, metadata, true, progress);
            const double elapsed = milliseconds(phaseTimer);
            report["diagnostics"] = evaluated.take("diagnostics");
            report["annotation_coverage"] = evaluated.take("annotation_coverage");
            report["evaluation"] = evaluated; report["extra_evaluation_elapsed_ms"] = elapsed;
            stages[0] = QJsonObject{{"id", "fixed_evaluation"}, {"outcome", "completed"},
                {"evaluation", evaluated}, {"evaluation_elapsed_ms", elapsed}};
            task::CancellationScope::check();
            report["outcome"] = "completed"; report["current_phase"] = "finished";
            persist(); event("complete", "fixed_evaluation"); return 0;
        }
        const auto nativeReference = fingerprint(originalParsed);
        const auto timeReference = fingerprint(originalParsed, true);
        QString chart = original;
        bool allEvaluated = true;
        QVector<QString> completedCharts;
        QVector<SimaiParseResult> completedParses;
        const QStringList stageIds{"layout", "context", "pattern"};
        const QStringList outputNames{"stage1_layout", "stage2_context", "stage3_pattern"};
        for (int index = 0; index < stageIds.size(); ++index) {
            currentStage = index;
            const QString id = stageIds[index];
            const QString before = chart;
            QJsonObject stage{{"id", id}, {"number", index + 1}, {"outcome", "running"},
                {"input_chart_sha256", sha256(before.toUtf8())}, {"elapsed_ms", QJsonValue::Null},
                {"algorithm_elapsed_ms", QJsonValue::Null}, {"verification_elapsed_ms", QJsonValue::Null},
                {"evaluation_elapsed_ms", QJsonValue::Null}};
            stages.append(stage);
            const auto saveStage = [&] { stages[index] = stage; persist(); };
            phase = "operation"; report["current_stage"] = id; report["current_phase"] = phase;
            saveStage(); event("start", id); phaseTimer.start();
            const auto parsed = SimaiParser::validateSyntax(before, metadata);
            QElapsedTimer algorithm; algorithm.start();
            int changed = 0;
            BasicResult result;
            if (index == 0) {
                const auto layout = chart_transform::basicHandOrderLayoutInSelection(before, 0, 0, metadata,
                    parameters.returnSpeed, parameters.sweepThreshold, parameters.colorUnresolved);
                stage["algorithm_elapsed_ms"] = milliseconds(algorithm);
                chart = applyPatches(before, layout.edit.patches, changed);
                stage["elapsed_ms"] = milliseconds(phaseTimer);
                stage["work_used"] = QJsonValue::Null;
                stage["unresolved_rows"] = QJsonValue::Null;
                stage["layout_preserved"] = layout.preserved;
                stage["layout_unsupported"] = layout.edit.unsupported;
                stage["diagnostics"] = messages(layout.edit.diagnostics);
                stage["patch_count"] = layout.edit.patches.size();
                stage["algorithm_outcome"] = "layout_only_not_feasibility_proof";
            } else {
                QElapsedTimer progressClock; progressClock.start();
                const auto progress = [&](const HandOrderProgress& update) {
                    if (progressClock.elapsed() < 2000) return;
                    progressClock.restart();
                    report["current_progress"] = QJsonObject{{"phase", int(update.phase)}, {"completed", update.completed},
                        {"total", update.total}, {"window", update.window}, {"windows", update.windows},
                        {"attempt", update.attempt}, {"candidate", update.candidate}};
                    saveStage(); event("progress", id);
                };
                result = index == 1 ? inferBasicHandOrder(before, parsed, 0, before.size(), parameters, metadata, progress)
                    : optimizeHandOrderPatterns(before, parsed, 0, before.size(), parameters, {}, metadata, progress);
                stage["algorithm_elapsed_ms"] = milliseconds(algorithm);
                chart = applyPatches(before, result.patches, changed);
                stage["elapsed_ms"] = milliseconds(phaseTimer);
                const auto details = resultJson(result, detailed);
                for (auto it = details.begin(); it != details.end(); ++it) stage[it.key()] = it.value();
                stage["algorithm_outcome"] = index == 1 ? QStringLiteral("inference_completed") : result.patternOutcome;
            }
            operationTotal += stage["elapsed_ms"].toDouble(); report["pipeline_elapsed_ms"] = operationTotal;
            stage["changed_tokens"] = changed;
            stage["output_chart_sha256"] = sha256(chart.toUtf8()); saveStage(); event("operation_complete", id);

            phase = "verification"; report["current_phase"] = phase; saveStage(); phaseTimer.restart();
            const auto verified = SimaiParser::validateSyntax(chart, metadata);
            const auto actualNative = fingerprint(verified);
            const bool nativeSame = actualNative == nativeReference;
            const bool timingSame = fingerprint(verified, true) == timeReference;
            stage["parse_ok"] = verified.ok;
            stage["parse_errors"] = messages(verified.errors); stage["annotation_errors"] = messages(verified.annotationErrors);
            stage["native_structure_unchanged"] = nativeSame; stage["timing_unchanged"] = timingSame;
            stage["native_source_unchanged"] = stripAnnotations(chart) == stripAnnotations(original);
            stage["native_fingerprint_sha256"] = sha256(QJsonDocument(actualNative).toJson(QJsonDocument::Compact));
            QJsonArray annotations;
            for (const auto& marker : verified.noteMarkers) annotations.append(annotation(marker));
            stage["annotation_fingerprint_sha256"] = sha256(QJsonDocument(annotations).toJson(QJsonDocument::Compact));
            stage["absolute_timing_sha256"] = sha256(QJsonDocument(absoluteTiming(verified)).toJson(QJsonDocument::Compact));
            int unresolved = 0, markerChanges = 0;
            for (int marker = 0; marker < verified.noteMarkers.size(); ++marker) {
                unresolved += !concreteMarker(verified.noteMarkers[marker]);
                if (marker < parsed.noteMarkers.size()) markerChanges += annotation(verified.noteMarkers[marker]) != annotation(parsed.noteMarkers[marker]);
            }
            stage["unresolved_annotations"] = unresolved; stage["changed_markers"] = markerChanges;
            stage["verification_elapsed_ms"] = milliseconds(phaseTimer);
            verificationTotal += stage["verification_elapsed_ms"].toDouble(); report["verification_elapsed_ms"] = verificationTotal;
            if (!verified.ok || !verified.annotationErrors.isEmpty() || !nativeSame || !timingSame) {
                stage["outcome"] = "invariant_failed"; stages[index] = stage;
                report["outcome"] = "invariant_failed"; persist(); event("invariant_failed", id); return 4;
            }
            QString outputDocument = documentText;
            outputDocument.replace(chartOffset, original.size(), chart);
            QByteArray bytes = outputDocument.toUtf8(); if (bom) bytes.prepend("\xEF\xBB\xBF");
            const QString output = QDir(outputDir).filePath(outputNames[index] + "/maidata.txt");
            if (QFileInfo(output).canonicalFilePath().compare(inputPath, pathCase) == 0 || !writeFile(output, bytes))
                throw QStringLiteral("Cannot save distinct stage output");
            stage["output_path"] = output; stage["output_sha256"] = sha256(bytes); saveStage();

            stage["outcome"] = result.error.isEmpty() ? "completed" : "algorithm_error";
            auto pendingEvaluation = evaluationJson({});
            pendingEvaluation["attempted"] = false; pendingEvaluation["outcome"] = "pending";
            stage["evaluation"] = pendingEvaluation;
            stages[index] = stage; persist(); event("complete", id);
            if (!result.error.isEmpty()) { report["outcome"] = "algorithm_error"; persist(); return 5; }
            completedCharts.append(chart); completedParses.append(verified);
        }
        report["main_pipeline_completed"] = true;
        report["main_pipeline_wall_elapsed_ms"] = milliseconds(wall);
        persist(); event("main_pipeline_complete", "pipeline");
        // These independent audits deliberately occur only after all three
        // primary operations. They cannot prewarm or delay the next operation.
        for (int index = 0; index < stages.size(); ++index) {
            currentStage = index;
            auto stage = stages[index].toObject();
            const auto id = stage.value("id").toString();
            chart = completedCharts[index];
            const auto& verified = completedParses[index];
            const auto saveStage = [&] { stages[index] = stage; persist(); };
            report["current_stage"] = id;
            phase = "evaluation"; report["current_phase"] = phase; saveStage(); event("evaluation_start", id); phaseTimer.restart();
            auto evaluation = fixedEvaluation(chart, verified, parameters, metadata, detailed);
            allEvaluated &= evaluation.value("complete").toBool();
            stage["evaluation"] = evaluation; stage["evaluation_elapsed_ms"] = milliseconds(phaseTimer);
            evaluationTotal += stage["evaluation_elapsed_ms"].toDouble(); report["extra_evaluation_elapsed_ms"] = evaluationTotal;
            evaluation["outcome"] = evaluation.value("complete").toBool() ? "completed" : "unavailable";
            stage["evaluation"] = evaluation;
            stages[index] = stage; persist(); event("evaluation_complete", id);
        }
        phase = "finalization";
        task::CancellationScope::check();
        report["outcome"] = "completed";
        report["result_quality"] = allEvaluated ? "all_stages_fully_evaluated" : "incomplete_evaluation";
        report["current_phase"] = "finished"; persist(); event("complete", "pipeline"); return 0;
    } catch (const task::Cancelled&) {
        report["outcome"] = "timeout"; report["interrupted_phase"] = phase;
        if (currentStage >= 0) {
            auto stage = stages[currentStage].toObject();
            if (!report.value("main_pipeline_completed").toBool()) stage["outcome"] = "timed_out";
            if (phase == "evaluation") {
                auto evaluation = stage.value("evaluation").toObject(); evaluation["outcome"] = "timed_out";
                stage["evaluation"] = evaluation; stage["evaluation_elapsed_ms"] = milliseconds(phaseTimer);
                report["extra_evaluation_elapsed_ms"] = evaluationTotal + milliseconds(phaseTimer);
            }
            stage["interrupted_phase"] = phase; stage["interrupted_phase_elapsed_ms"] = milliseconds(phaseTimer);
            stages[currentStage] = stage;
        }
        try { persist(); event("timeout", currentStage < 0 ? "input_validation" : stages[currentStage].toObject().value("id").toString()); } catch (...) {}
        return 124;
    } catch (const QString& error) {
        report["outcome"] = "error"; report["error"] = error;
        try { persist(); event("error", phase); } catch (...) {}
        return fail(error);
    }
}
