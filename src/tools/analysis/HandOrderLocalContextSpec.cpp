#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace miacode::hand_order;
namespace {
struct TokenWindow {
    int start = 0, end = 0;
    bool supported = true, target = false, partial = false, sustained = false;
    double first = std::numeric_limits<double>::infinity(), last = -std::numeric_limits<double>::infinity();
    QVector<const TimelineNoteMarker*> markers;
    QVector<int> touches;
};
struct LegacyStats {
    int tokens = 0, targets = 0, seed = 0, included = 0, closureRounds = 0, decisions = 0, blocks = 0;
    int maxBlockTokens = 0, maxBlockTasks = 0, maxTargetTasks = 0;
    int targetBlocksOverTaskLimit = 0;
    double includedFirst = 0, includedLast = 0;
};
QVector<int> lineStarts(const QString& text) {
    QVector<int> starts{0}; for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') starts.append(i + 1); return starts;
}
QVector<const TimelineNoteMarker*> byOrder(const SimaiParseResult& parsed) {
    QVector<const TimelineNoteMarker*> ordered(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers) if (marker.parseOrder >= 0 && marker.parseOrder < ordered.size()) ordered[marker.parseOrder] = &marker;
    return ordered;
}

// Independent reconstruction of the pre-fix dependency model. It explains
// closure/block size using public parser data; it is never a judge or solver.
LegacyStats legacyStats(const QString& text, const SimaiParseResult& parsed, int begin, int end, const BasicParameters& parameters)
{
    LegacyStats stats; const auto lines = lineStarts(text); const auto ordered = byOrder(parsed);
    QVector<TokenWindow> tokens; QVector<TouchTarget> touches; QVector<int> touchToken;
    for (const auto& source : parsed.sourceTokens) {
        if (source.markerCount == 0) continue;
        TokenWindow token; token.start = lines.at(source.line - 1) + source.col - 1; token.end = token.start + source.length;
        token.target = token.start >= begin && token.end <= end;
        token.partial = token.start < end && token.end > begin && !token.target;
        token.supported = source.nativeValid && parsed.errors.isEmpty();
        for (const auto& error : parsed.annotationErrors) if (error.line == source.line && error.col >= source.col && error.col < source.col + source.length) token.supported = false;
        for (int i = 0; i < source.markerCount; ++i) {
            const auto* marker = ordered.value(source.firstParseOrder + i);
            if (!marker) { token.supported = false; continue; }
            token.markers.append(marker);
            const bool slide = marker->type == "slide" || marker->type == "wifi";
            const bool touch = marker->type == "touch", hold = marker->type == "hold" || marker->type == "touch_hold";
            token.supported &= !marker->isMine && !marker->headMine && !marker->trackMine;
            token.first = qMin(token.first, marker->second - FullReplayProfile::tapAvailableSeconds);
            token.last = qMax(token.last, qMax(marker->second + qMax(parameters.pressSeconds, FullReplayProfile::tapAvailableSeconds), marker->endSecond));
            if (slide) token.last = qMax(token.last, marker->endSecond + FullReplayProfile::slideAvailableSeconds);
            token.sustained |= slide || (hold && marker->endSecond - marker->second > parameters.pressSeconds + 1e-8);
            if (touch || marker->type == "touch_hold") {
                token.touches.append(touches.size()); touchToken.append(tokens.size());
                touches.append({token.start, marker->parseOrder, marker->eachGroupId, marker->touchPad, marker->second, marker->type == "touch_hold", -1});
            }
        }
        tokens.append(token); stats.targets += token.target;
    }
    stats.tokens = tokens.size();
    double selectedFirst = std::numeric_limits<double>::infinity(), selectedLast = -selectedFirst;
    for (const auto& token : tokens) if (token.target || token.partial) { selectedFirst = qMin(selectedFirst, token.first); selectedLast = qMax(selectedLast, token.last); }
    QVector<bool> included(tokens.size(), false);
    for (int i = 0; i < tokens.size(); ++i) {
        included[i] = tokens.at(i).supported && tokens.at(i).last >= selectedFirst - parameters.contextSeconds && tokens.at(i).first <= selectedLast + parameters.contextSeconds;
        stats.seed += included.at(i);
    }
    const auto groups = buildHandOrderTouchGroups(touches);
    bool changed = true;
    while (changed) {
        changed = false; ++stats.closureRounds;
        for (int i = 0; i < tokens.size(); ++i) if (included.at(i) && tokens.at(i).sustained)
            for (int j = 0; j < tokens.size(); ++j) if (!included.at(j) && tokens.at(j).supported
                && tokens.at(j).first <= tokens.at(i).last + 1e-8 && tokens.at(j).last >= tokens.at(i).first - 1e-8) {
                included[j] = true; changed = true;
            }
        for (const auto& group : groups) {
            bool any = false; for (int id : group) any |= included.at(touchToken.at(id));
            if (any) for (int id : group) if (!included.at(touchToken.at(id)) && tokens.at(touchToken.at(id)).supported) {
                included[touchToken.at(id)] = true; changed = true;
            }
        }
    }
    struct Decision { double first = 0, last = 0; QSet<int> tokens, touches; };
    QVector<Decision> decisions;
    double from = std::numeric_limits<double>::infinity(), to = -from;
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i)) {
        ++stats.included; from = qMin(from, tokens.at(i).first); to = qMax(to, tokens.at(i).last);
        bool headCreated = false;
        for (const auto* marker : tokens.at(i).markers) {
            const bool slide = marker->type == "slide" || marker->type == "wifi";
            if (marker->type != "touch" && (!slide || (marker->hasHeadStar && !headCreated))) {
                const bool hold = marker->type == "hold" || marker->type == "touch_hold";
                decisions.append({marker->second - FullReplayProfile::tapAvailableSeconds,
                    hold ? qMax(marker->endSecond, marker->second + parameters.pressSeconds) : marker->second + parameters.pressSeconds, {i}, {}});
                headCreated |= slide;
            }
            if (slide) decisions.append({marker->second - FullReplayProfile::slideLeadingSeconds, marker->endSecond + FullReplayProfile::slideAvailableSeconds, {i}, {}});
        }
    }
    for (const auto& group : groups) {
        Decision decision; decision.first = std::numeric_limits<double>::infinity(); decision.last = -decision.first;
        for (int id : group) if (included.at(touchToken.at(id))) {
            decision.tokens.insert(touchToken.at(id)); decision.touches.insert(id);
            decision.first = qMin(decision.first, touches.at(id).second - TouchJudgeProfile::availableSeconds);
            decision.last = qMax(decision.last, touches.at(id).second + TouchJudgeProfile::availableSeconds);
        }
        if (!decision.tokens.isEmpty()) decisions.append(decision);
    }
    stats.includedFirst = stats.included ? from : 0; stats.includedLast = stats.included ? to : 0; stats.decisions = decisions.size();
    std::stable_sort(decisions.begin(), decisions.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int first = 0; first < decisions.size();) {
        int last = first + 1; double activeEnd = decisions.at(first).last;
        while (last < decisions.size() && decisions.at(last).first <= activeEnd + parameters.contextSeconds) { activeEnd = qMax(activeEnd, decisions.at(last).last); ++last; }
        QSet<int> blockTokens, localTouches;
        for (int i = first; i < last; ++i) { blockTokens.unite(decisions.at(i).tokens); localTouches.unite(decisions.at(i).touches); }
        bool target = false;
        for (int id : blockTokens) {
            target |= tokens.at(id).target;
            for (int touch : tokens.at(id).touches) if (touches.at(touch).hold) localTouches.insert(touch);
        }
        const int tasks = last - first + localTouches.size();
        stats.maxBlockTasks = qMax(stats.maxBlockTasks, tasks); stats.maxBlockTokens = qMax(stats.maxBlockTokens, int(blockTokens.size()));
        if (target) { stats.maxTargetTasks = qMax(stats.maxTargetTasks, tasks); stats.targetBlocksOverTaskLimit += tasks > parameters.maxTasks; }
        ++stats.blocks; first = last;
    }
    return stats;
}
QString patchedText(const QString& chart, const BasicResult& result) {
    QString output = chart;
    for (int i = result.patches.size() - 1; i >= 0; --i) { const auto& patch = result.patches.at(i); output.replace(patch.start, patch.end - patch.start, patch.text); }
    return output;
}
QChar handLetter(HandOrderHand hand) {
    return hand == HandOrderHand::Left ? QChar('l') : hand == HandOrderHand::Right ? QChar('r') : hand == HandOrderHand::Either ? QChar('m') : QChar('?');
}
template<class Row> bool hasCandidate(const Row& row) {
    if constexpr (requires { row.hasCandidate; }) return row.hasCandidate;
    else return row.fullReplayVerified;
}
template<class Result> void printScope(QTextStream& out, const Result& result) {
    if constexpr (requires { result.includedObjects; result.windowCount; result.maxWindowTasks; result.taskLimited; result.workLimited; })
        out << "SCOPE included_objects=" << result.includedObjects << " windows=" << result.windowCount
            << " max_window_tasks=" << result.maxWindowTasks << " task_limited=" << result.taskLimited << " work_limited=" << result.workLimited << '\n';
    if constexpr (requires { result.auditedObjects; }) out << "AUDIT native_objects=" << result.auditedObjects << '\n';
    if constexpr (requires { result.searchWindows; }) {
        QVector<int> expensive; for (int i = 0; i < result.searchWindows.size(); ++i) expensive.append(i);
        std::stable_sort(expensive.begin(), expensive.end(), [&](int a, int b) { return result.searchWindows.at(a).workUsed > result.searchWindows.at(b).workUsed; });
        QSet<int> selected; for (int i = 0; i < qMin(12, int(result.searchWindows.size())); ++i) selected.insert(i);
        for (int i = 0; i < qMin(8, int(expensive.size())); ++i) selected.insert(expensive.at(i));
        auto indices = selected.values(); std::sort(indices.begin(), indices.end());
        for (int index : indices) {
            const auto& window = result.searchWindows.at(index);
            out << "WINDOW index=" << index << " begin=" << window.start << " end=" << window.end
                << " tasks=" << window.tasks << " work=" << window.workUsed << " outcome=" << window.outcome << '\n';
        }
    }
}
template<class Result> bool boundedWindows(const Result& result, int maximum, int minimumWindows = 2) {
    if constexpr (requires { result.windowCount; result.maxWindowTasks; result.taskLimited; })
        return result.windowCount >= minimumWindows && result.maxWindowTasks <= maximum && !result.taskLimited;
    else return false;
}
template<class Result> QString resourceMechanism(const Result& result, int resourceRows, const LegacyStats& legacy, const BasicParameters& parameters) {
    if constexpr (requires { result.taskLimited; result.workLimited; }) {
        if (result.taskLimited && result.workLimited) return QStringLiteral("task_and_work_budget");
        if (result.taskLimited) return QStringLiteral("task_limit");
        if (result.workLimited) return QStringLiteral("work_budget");
    }
    return resourceRows && result.workUsed >= parameters.maxWorkUnits ? QStringLiteral("work_budget")
        : resourceRows && result.workUsed == 0 && legacy.maxTargetTasks > parameters.maxTasks ? QStringLiteral("task_limit_before_search")
        : resourceRows ? QStringLiteral("reported_resource_limit") : QStringLiteral("none");
}
int diagnostic(const QString& chart, int begin, int end, const BasicParameters& parameters, const miacode::simai::SimaiTimingMetadata& metadata, bool onlyStats, bool showText)
{
    QTextStream out(stdout); QElapsedTimer parseTimer; parseTimer.start();
    const auto parsed = SimaiParser::validateSyntax(chart, metadata);
    const auto legacy = legacyStats(chart, parsed, begin, end, parameters);
    int annotatedTargets = 0; const auto starts = lineStarts(chart);
    for (const auto& source : parsed.sourceTokens) if (source.markerCount > 0) {
        const int start = starts.at(source.line - 1) + source.col - 1;
        if (start >= begin && start + source.length <= end && !splitToken(chart.mid(start, source.length)).suffix.isEmpty()) ++annotatedTargets;
    }
    out << "INPUT algorithm_version=" << parameters.version << " judge_revision=" << parameters.judgeProfileRevision
        << " chars=" << chart.size() << " markers=" << parsed.noteMarkers.size() << " tokens=" << legacy.tokens
        << " selected_tokens=" << legacy.targets << " duration_s=" << parsed.durationSeconds << " begin=" << begin << " end=" << end
        << " preset=" << parameters.inputPreference << " context_s=" << parameters.contextSeconds << " native_errors=" << parsed.errors.size()
        << " annotation_errors=" << parsed.annotationErrors.size() << " annotated_targets=" << annotatedTargets
        << " overwrite=" << parameters.overwriteExisting << " high_only=" << parameters.highConfidenceOnly << " parse_and_model_ms=" << parseTimer.elapsed() << '\n';
    out << "LEGACY_MODEL seed=" << legacy.seed << " closure=" << legacy.included << " rounds=" << legacy.closureRounds
        << " span_first=" << legacy.includedFirst << " span_last=" << legacy.includedLast << " decisions=" << legacy.decisions
        << " blocks=" << legacy.blocks << " max_block_tokens=" << legacy.maxBlockTokens << " max_block_tasks=" << legacy.maxBlockTasks
        << " max_target_tasks=" << legacy.maxTargetTasks << " max_tasks=" << parameters.maxTasks
        << " target_blocks_over_task_limit=" << legacy.targetBlocksOverTaskLimit << '\n';
    int diagnosticCount = 0;
    for (const auto& error : parsed.errors) if (diagnosticCount++ < 4)
        out << "PARSE_ERROR line=" << error.line << " col=" << error.col << " reason=" << error.message.left(180).replace('\n', ' ') << '\n';
    diagnosticCount = 0;
    for (const auto& error : parsed.annotationErrors) if (diagnosticCount++ < 4)
        out << "ANNOTATION_ERROR line=" << error.line << " col=" << error.col << " reason=" << error.message.left(180).replace('\n', ' ') << '\n';
    if (!parsed.errors.isEmpty()) { out << "RESULT skipped=invalid_native_input\n"; return 2; }
    if (onlyStats) return parsed.errors.isEmpty() ? 0 : 2;
    QElapsedTimer timer; timer.start(); const auto result = inferBasicHandOrder(chart, parsed, begin, end, parameters);
    const qint64 elapsed = timer.elapsed(); const auto output = patchedText(chart, result);
    const auto reread = SimaiParser::validateSyntax(output, metadata);
    QMap<QString, int> statuses, reasons; int targets = 0, contexts = 0, verified = 0, high = 0, resource = 0; QString sequence;
    for (const auto& row : result.rows) {
        ++statuses[row.status]; ++reasons[row.reason]; targets += !row.context; contexts += row.context; verified += row.fullReplayVerified;
        high += !row.context && row.confidenceLevel == "high";
        resource += row.reason == "resource_limit" || row.reason == "task_limit" || row.reason == "work_limit";
        if (!row.context) sequence.append(handLetter(row.head));
    }
    const QString mechanism = resourceMechanism(result, resource, legacy, parameters);
    out << "RESULT elapsed_ms=" << elapsed << " work=" << result.workUsed << " max_work=" << parameters.maxWorkUnits
        << " rows=" << result.rows.size() << " targets=" << targets << " context=" << contexts << " context_objects=" << result.contextObjects
        << " verified_rows=" << verified << " patches=" << result.patches.size() << " high_confidence=" << high
        << " truncated=" << result.truncated << " evidence_truncated=" << result.evidenceTruncated << " resource_mechanism=" << mechanism
        << " error=" << result.error << " sequence_count=" << sequence.size() << " sequence_prefix=" << sequence.left(64) << '\n';
    printScope(out, result);
    for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) out << "STATUS " << it.key() << '=' << it.value() << '\n';
    for (auto it = reasons.cbegin(); it != reasons.cend(); ++it) out << "REASON " << it.key() << '=' << it.value() << '\n';
    out << "REPARSE native_errors=" << reread.errors.size() << " annotation_errors=" << reread.annotationErrors.size()
        << " native_projection_same=" << (miacode::hand_order::stripAnnotations(chart) == miacode::hand_order::stripAnnotations(output)) << '\n';
    int printed = 0;
    for (const auto& row : result.rows) if (!row.context && printed++ < 12) {
        out << "ROW line=" << row.line << " col=" << row.col << " status=" << row.status << " reason=" << row.reason
            << " hand=" << handLetter(row.head) << " track=" << handLetter(row.track) << " unit=" << row.dependencyUnit
            << " candidate=" << hasCandidate(row) << " verified=" << row.fullReplayVerified << " confidence=" << row.confidence
            << " evidence=" << row.confidenceLevel << " signed_cost=" << row.signedCostGap << " signed_peak=" << row.signedPeakTierGap << '\n';
        int evidenceCount = 0;
        for (const auto& proof : row.counterfactualEvidence) if (evidenceCount++ < 6)
            out << "EVIDENCE line=" << row.line << " col=" << row.col << " component=" << proof.component
                << " searched=" << proof.searched << " found=" << proof.found << " limited=" << proof.limited
                << " score=" << proof.score << " cost_gap=" << proof.signedCostGap << " risk_gap=" << proof.signedRiskTierGap
                << " peak_gap=" << proof.signedPeakTierGap << '\n';
    }
    if (showText) out << "SYNTHETIC_OUTPUT " << output << '\n';
    return reread.errors.isEmpty() && reread.annotationErrors.isEmpty()
        && miacode::hand_order::stripAnnotations(chart) == miacode::hand_order::stripAnnotations(output) ? 0 : 2;
}

QVector<const BasicRow*> targetRows(const BasicResult& result) {
    QVector<const BasicRow*> rows; for (const auto& row : result.rows) if (!row.context) rows.append(&row);
    std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->start < b->start; }); return rows;
}
QString targetSequence(const BasicResult& result) {
    QString sequence; for (const auto* row : targetRows(result)) sequence.append(handLetter(row->head)); return sequence;
}
bool allVerified(const BasicResult& result, int targets) {
    const auto rows = targetRows(result); if (rows.size() != targets) return false;
    return std::all_of(rows.cbegin(), rows.cend(), [](const auto* row) { return hasCandidate(*row) && row->fullReplayVerified; });
}
bool sameNative(const QString& chart, const BasicResult& result) {
    const auto output = patchedText(chart, result); const auto reread = SimaiParser::validateSyntax(output);
    return reread.errors.isEmpty() && reread.annotationErrors.isEmpty() && stripAnnotations(chart) == stripAnnotations(output)
        && output.count("~m") <= chart.count("~m");
}
BasicResult solveSynthetic(const QString& chart, BasicParameters parameters = {}, int begin = 0, int end = -1) {
    return inferBasicHandOrder(chart, SimaiParser::validateSyntax(chart), begin, end < 0 ? chart.size() : end, parameters);
}
int syntheticSpec()
{
    bool ok = true;
    const auto expect = [&](bool pass, const char* message, const BasicResult& result = {}) {
        QTextStream out(pass ? stdout : stderr); out << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass;
        if (!pass) {
            out << "  work=" << result.workUsed << " patches=" << result.patches.size() << " context=" << result.contextObjects
                << " sequence=" << targetSequence(result).left(64) << " truncated=" << result.truncated << " evidence_truncated=" << result.evidenceTruncated << '\n';
            printScope(out, result);
            int count = 0; for (const auto& row : result.rows) if (!row.context && count++ < 8)
                out << "  " << row.line << ':' << row.col << ' ' << row.status << ' ' << row.reason
                    << " candidate=" << hasCandidate(row) << " verified=" << row.fullReplayVerified << " confidence=" << row.confidenceLevel << '\n';
        }
    };
    const QString run = "(136.7){24}1,2,3,4,";
    const QString mirror = "(136.7){24}8,7,6,5,";
    const double nearAssociation = basicOrdinaryAssociationWeight(.05, 0);
    expect(nearAssociation > basicOrdinaryAssociationWeight(.5, 0)
        && basicOrdinaryAssociationWeight(.5, 0) > basicOrdinaryAssociationWeight(2, 0)
        && nearAssociation > basicOrdinaryAssociationWeight(.05, 1)
        && basicOrdinaryAssociationWeight(.05, 1) > basicOrdinaryAssociationWeight(.05, 4),
        "ordinary soft association decreases independently with elapsed time and intervening-note count");
    for (int input : {0, 1}) {
        BasicParameters parameters; parameters.inputPreference = input;
        const auto right = solveSynthetic(run, parameters), left = solveSynthetic(mirror, parameters);
        expect(allVerified(right, 4) && right.patches.size() == 4 && targetSequence(right) == "rrrr" && sameNative(run, right),
            "136.7 BPM 24th-note 1..4 is one coherent right-hand sweep with no new gray fallback", right);
        expect(allVerified(left, 4) && left.patches.size() == 4 && targetSequence(left) == "llll" && sameNative(mirror, left),
            "mirroring 1..4 to 8..5 mirrors the complete local sweep to the left hand", left);
        for (int bpm : {180, 240}) {
            const QString faster = QStringLiteral("(%1){24}1,2,3,4,").arg(bpm);
            const QString fasterMirror = QStringLiteral("(%1){24}8,7,6,5,").arg(bpm);
            const auto fastRight = solveSynthetic(faster, parameters), fastLeft = solveSynthetic(fasterMirror, parameters);
            expect(allVerified(fastRight, 4) && targetSequence(fastRight) == "rrrr" && sameNative(faster, fastRight),
                "180/240 BPM finite local 1..4 sweeps retain the right hand from its lane-three default", fastRight);
            expect(allVerified(fastLeft, 4) && targetSequence(fastLeft) == "llll" && sameNative(fasterMirror, fastLeft),
                "180/240 BPM mirrored finite sweeps retain the left hand from its lane-six default", fastLeft);
        }
    }
    BasicParameters screen; screen.inputPreference = 1;
    const QString visiblePrefix = "(136.7){16}1/5<1[16:3],1h[16:3],";
    const auto visible = solveSynthetic(visiblePrefix, screen);
    expect(allVerified(visible, 3) && visible.patches.size() == 3 && sameNative(visiblePrefix, visible)
        && std::none_of(visible.rows.cbegin(), visible.rows.cend(), [](const auto& row) { return !row.context
            && (row.reason == "resource_limit" || row.reason == "task_limit" || row.reason == "work_limit"); }),
        "only the three screenshot-visible objects form a bounded synthetic prefix, without claiming the screenshot's unknown full chart", visible);
    QString distantDense = visiblePrefix + "{1},,,,,,,,{24}";
    for (int i = 0; i < 200; ++i) distantDense += QString::number(i % 8 + 1) + ',';
    const auto visibleIsolated = solveSynthetic(distantDense, screen, 0, visiblePrefix.size());
    expect(allVerified(visibleIsolated, 3) && visibleIsolated.patches.size() == 3
        && targetSequence(visibleIsolated) == targetSequence(visible) && visibleIsolated.contextObjects <= 4
        && sameNative(distantDense, visibleIsolated),
        "the screenshot-visible synthetic prefix cannot absorb a distant dense independent phrase through transitive soft context", visibleIsolated);
    const QString occupied = "(136.7){24}7h[1:1]~r,1,2,3,4,";
    const int selected = occupied.indexOf(",1,") + 1;
    const auto constrained = solveSynthetic(occupied, screen, selected, occupied.size());
    expect(allVerified(constrained, 4) && targetSequence(constrained) == "llll" && constrained.contextObjects >= 1 && sameNative(occupied, constrained),
        "a real locked right-hand hold overrides the ordinary rightward sweep prior", constrained);
    const auto local = solveSynthetic(run, screen);
    const QString remote = run + "{1},,,,,,,,8,7,6,5,";
    const auto isolated = solveSynthetic(remote, screen, 0, run.size());
    bool stable = targetSequence(local) == targetSequence(isolated) && allVerified(isolated, 4);
    const auto localRows = targetRows(local), isolatedRows = targetRows(isolated);
    if (localRows.size() == isolatedRows.size()) for (int i = 0; i < localRows.size(); ++i)
        stable &= std::abs(localRows.at(i)->cost - isolatedRows.at(i)->cost) < 1e-6
            && std::abs(localRows.at(i)->confidence - isolatedRows.at(i)->confidence) < 1e-6;
    else stable = false;
    expect(stable && sameNative(remote, isolated), "a distant independent phrase cannot change the original local assignment or its evidence scale", isolated);
    QString continuous = "(136.7){24}";
    for (int i = 0; i < 200; ++i) continuous += QString::number(i % 8 + 1) + ',';
    BasicParameters bounded = screen; bounded.maxTasks = 8;
    const auto rolling = solveSynthetic(continuous, bounded);
    expect(allVerified(rolling, 200) && rolling.patches.size() == 200 && boundedWindows(rolling, bounded.maxTasks)
        && rolling.workUsed <= bounded.maxWorkUnits && sameNative(continuous, rolling),
        "two hundred ordinary notes use bounded rolling windows instead of one transitive time-neighbor task block", rolling);
    const int firstNote = continuous.indexOf('}') + 1;
    const auto firstOnly = solveSynthetic(continuous, bounded, firstNote, firstNote + 1);
    expect(allVerified(firstOnly, 1) && firstOnly.contextObjects <= 3 && boundedWindows(firstOnly, bounded.maxTasks, 1),
        "a one-note selection imports only a bounded non-transitive ordinary neighborhood", firstOnly);
    QString sustained = "(136.7){24}7h[1:2]~r,";
    for (int i = 0; i < 23; ++i) sustained += "8,";
    const int sustainedBegin = sustained.size(); sustained += "1,2,3,4,";
    const auto crossed = solveSynthetic(sustained, screen, sustainedBegin, sustained.size());
    bool holdVisible = false; for (const auto& row : crossed.rows) holdVisible |= row.context && row.start == sustained.indexOf("7h") && row.head == HandOrderHand::Right;
    expect(allVerified(crossed, 4) && targetSequence(crossed) == "llll" && holdVisible && sameNative(sustained, crossed),
        "a sustained hard dependency survives many intervening notes and still occupies its locked hand across a rolling boundary", crossed);
    const auto sustainedParsed = SimaiParser::validateSyntax(sustained); const auto sustainedOrder = byOrder(sustainedParsed);
    bool earlierConsumer = sustainedParsed.ok && sustainedOrder.size() == 28;
    if (earlierConsumer) {
        const auto& context = *sustainedOrder.at(23); ChartInputAction input;
        input.contact.kind = "screen_button"; input.contact.hand = 0; input.contact.start = context.second;
        input.contact.end = context.second + screen.pressSeconds; input.contact.points = {touchPadPoint("A8")};
        input.contact.times = {context.second}; input.contact.tokenStart = sustainedBegin - 2;
        const auto alone = replayHandOrderChart({context}, {input});
        const auto native = replayHandOrderChart(sustainedParsed.noteMarkers, {input});
        earlierConsumer = alone.meetsTarget;
        bool consumed = false;
        for (const auto& row : native.judgements) {
            if (row.parseOrder == context.parseOrder) earlierConsumer &= !row.meetsTarget;
            for (const auto& cause : row.causes) consumed |= row.parseOrder < context.parseOrder && cause.tokenStart == input.contact.tokenStart;
        }
        earlierConsumer &= consumed;
    }
    expect(earlierConsumer, "the preceding same-pad soft context has a real earlier native DOWN consumer, which hard judgement closure must preserve");
    const QString lateHold = "(120){24},C,7h[1:1]~r,";
    const int touchBegin = lateHold.indexOf('C'); const auto late = solveSynthetic(lateHold, screen, touchBegin, touchBegin + 1);
    bool futureHoldVisible = false; for (const auto& row : late.rows) futureHoldVisible |= row.context && row.start == lateHold.indexOf("7h");
    expect(allVerified(late, 1) && futureHoldVisible && sameNative(lateHold, late),
        "a future locked Hold intersecting the Touch judgement tail remains a real hard context constraint", late);
    TimelineNoteMarker hold; hold.type = "touch_hold"; hold.touchPad = "C"; hold.second = 0; hold.endSecond = 1; hold.parseOrder = 0;
    TimelineNoteMarker touch; touch.type = "touch"; touch.touchPad = "C"; touch.second = .6; touch.endSecond = .6; touch.parseOrder = 1;
    ChartInputAction contact; contact.contact.kind = "touch_hold"; contact.contact.start = 0; contact.contact.end = 1;
    contact.contact.points = {{0, 0}}; contact.contact.times = {0};
    const auto realOn = replayHandOrderChart({hold, touch}, {contact});
    auto wronglyClipped = contact; wronglyClipped.contact.start = .5;
    const auto phantom = replayHandOrderChart({touch}, {wronglyClipped});
    expect(realOn.judgements.first().highQuality && !realOn.judgements.last().highQuality && phantom.judgements.first().highQuality,
        "a committed continuous ON must retain its original start; clipping it at a window would fabricate a consumable DOWN");
    BasicParameters highOnly = bounded; highOnly.highConfidenceOnly = true;
    const QString twelve = continuous.left(continuous.indexOf(',') + 1) + "2,3,4,5,6,7,8,1,2,3,4,";
    const auto filtered = solveSynthetic(twelve, highOnly);
    expect(allVerified(filtered, 12) && boundedWindows(filtered, highOnly.maxTasks) && sameNative(twelve, filtered),
        "high-confidence-only filtering follows hard components rather than closing ordinary neighbors into a whole phrase", filtered);
    BasicParameters selective; selective.highConfidenceOnly = true;
    const QString asymmetric = "(120){4}6,1,"; const auto selectedByProof = solveSynthetic(asymmetric, selective);
    bool exactGate = allVerified(selectedByProof, 2) && selectedByProof.rows.at(0).dependencyUnit != selectedByProof.rows.at(1).dependencyUnit;
    for (const auto* row : targetRows(selectedByProof)) {
        const bool patched = std::any_of(selectedByProof.patches.cbegin(), selectedByProof.patches.cend(), [&](const auto& patch) { return patch.start == row->start; });
        exactGate &= patched == (hasCandidate(*row) && row->fullReplayVerified && row->confidenceLevel == "high");
    }
    expect(exactGate && sameNative(asymmetric, selectedByProof),
        "high-only patch membership exactly follows each independently verified hard unit rather than an obsolete weak-prior assumption", selectedByProof);
    const auto symmetric = solveSynthetic("(120){4}C,", selective);
    expect(allVerified(symmetric, 1) && symmetric.patches.isEmpty() && symmetric.rows.first().confidenceLevel != "high"
        && symmetric.rows.first().confidence < .8,
        "a centered symmetric Touch supplies a stable weak-evidence control that high-only must withhold", symmetric);
    BasicParameters atomic = screen; atomic.maxTasks = 4;
    const QString simultaneous = "(120){4}1/2/3/4/5,";
    const auto wholeMoment = solveSynthetic(simultaneous, atomic);
    QSet<int> units; bool noCandidates = wholeMoment.patches.isEmpty();
    for (const auto& row : wholeMoment.rows) if (!row.context) { units.insert(row.dependencyUnit); noCandidates &= !hasCandidate(row); }
    expect(wholeMoment.taskLimited && wholeMoment.maxWindowTasks >= 5 && units.size() == 1 && noCandidates,
        "more than three same-time objects form one complete atomic hard unit before a four-task guard", wholeMoment);
    const int firstSimultaneous = simultaneous.indexOf('}') + 1;
    const auto oneFromMoment = solveSynthetic(simultaneous, atomic, firstSimultaneous, firstSimultaneous + 1);
    expect(oneFromMoment.taskLimited && oneFromMoment.includedObjects >= 5 && oneFromMoment.maxWindowTasks >= 5
        && oneFromMoment.patches.isEmpty(),
        "selecting one member still imports all five simultaneous hard members rather than only two lookahead notes", oneFromMoment);
    QString blockedMoment = "(120){32}";
    for (int i = 0; i < 20; ++i) { if (i) blockedMoment += '/'; blockedMoment += '1'; }
    const QString stolenDown = blockedMoment + ",1,";
    const auto stolenParsed = SimaiParser::validateSyntax(stolenDown); const auto stolenOrder = byOrder(stolenParsed);
    BasicParameters guarded = screen; guarded.maxTasks = 8;
    const auto guardedResult = solveSynthetic(stolenDown, guarded);
    bool stolenProof = stolenParsed.ok && stolenOrder.size() == 21;
    if (stolenProof) {
        const auto& last = *stolenOrder.last(); ChartInputAction input; input.button = true; input.lane = 1;
        input.contact.kind = "button"; input.contact.hand = 1; input.contact.start = last.second;
        input.contact.end = last.second + guarded.pressSeconds; input.contact.tokenStart = stolenDown.lastIndexOf("1,");
        const auto alone = replayHandOrderChart({last}, {input});
        const auto complete = replayHandOrderChart(stolenParsed.noteMarkers, {input});
        stolenProof = alone.meetsTarget && complete.judgements.first().judged && !complete.judgements.last().meetsTarget
            && !complete.judgements.first().causes.isEmpty()
            && complete.judgements.first().causes.first().tokenStart == input.contact.tokenStart;
    }
    expect(stolenProof && guardedResult.taskLimited && guardedResult.patches.isEmpty()
        && std::none_of(guardedResult.rows.cbegin(), guardedResult.rows.cend(), [](const auto& row) { return row.fullReplayVerified; }),
        "unresolved native notes in a guarded same-time group still consume a later same-pad DOWN and forbid a false verified patch", guardedResult);
    const QString expiredMoment = blockedMoment + ",{1},1,";
    const auto expiredResult = solveSynthetic(expiredMoment, guarded); const auto expiredRows = targetRows(expiredResult);
    expect(expiredResult.taskLimited && !expiredRows.isEmpty() && expiredRows.last()->fullReplayVerified
        && hasCandidate(*expiredRows.last()) && expiredResult.patches.size() == 1 && sameNative(expiredMoment, expiredResult),
        "an unresolved same-time group stops consuming after its real window; an independent later note remains writable", expiredResult);
    const QString active = "(120){4}1-5[2:1]~[head=l;track=0:l|0.3:r],,C,";
    const int activeTouch = active.indexOf('C'); const int activeSlide = active.indexOf("1-5");
    const auto activeLocal = solveSynthetic(active, screen, activeTouch, activeTouch + 1);
    const auto activeWhole = solveSynthetic(active, screen);
    const auto localTargets = targetRows(activeLocal), wholeTargets = targetRows(activeWhole);
    bool actualCause = allVerified(activeLocal, 1) && allVerified(activeWhole, 2)
        && localTargets.first()->head == HandOrderHand::Right && wholeTargets.last()->head == HandOrderHand::Right;
    if (actualCause) {
        actualCause = false;
        for (const auto& cause : localTargets.first()->causes)
            actualCause |= cause.tokenStart == activeSlide && cause.kind == "slide" && cause.pad == "C"
                && cause.handMask == 2 && cause.progress > .3 && cause.progress < .5;
    }
    expect(actualCause && sameNative(active, activeLocal) && sameNative(active, activeWhole),
        "narrow and whole requests agree on the committed slide interval's real trigger hand and source progress", activeLocal);
    BasicParameters tiny = screen; tiny.maxWorkUnits = 1000;
    const auto limited = solveSynthetic(continuous, tiny); bool honest = limited.workUsed <= tiny.maxWorkUnits;
    for (const auto& row : limited.rows) {
        if (!hasCandidate(row)) honest &= !row.fullReplayVerified && row.confidenceLevel != "high";
        if (row.confidenceLevel == "high") {
            honest &= row.fullReplayVerified && hasCandidate(row) && !row.counterfactualEvidence.isEmpty();
            for (const auto& proof : row.counterfactualEvidence) honest &= proof.searched && !proof.limited;
        }
    }
    for (const auto& patch : limited.patches) {
        const auto at = std::find_if(limited.rows.cbegin(), limited.rows.cend(), [&](const auto& row) { return row.start == patch.start; });
        honest &= at != limited.rows.cend() && hasCandidate(*at)
            && (at->fullReplayVerified || (at->status == "heuristic"
                && at->reason == "layout_fallback_unverified"
                && at->confidenceLevel == "insufficient_evidence"));
    }
    expect(honest && sameNative(continuous, limited),
        "resource exhaustion marks basic-layout fallback unverified without inventing high confidence or native notes", limited);
    return ok ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); QCommandLineParser parser; parser.setApplicationDescription("Bounded local-context diagnostics; external chart data is never embedded or written."); parser.addHelpOption();
    const auto add = [&](const QString& name, const QString& description, const QString& value = {}, const QString& defaults = {}) {
        parser.addOption(QCommandLineOption(QStringList{name}, description, value, defaults));
    };
    add("file", "External maidata copy (read only).", "path"); add("difficulty", "Native difficulty slot.", "id", "6");
    add("text", "Synthetic chart text supplied by the caller.", "chart"); add("baseline", "Use the synthetic 136.7 BPM 24th-note 1..4 run.");
    add("screenshot-prefix", "Synthetic reconstruction of only the three screenshot-visible objects; not the user's full screenshot chart.");
    add("overwrite", "Recompute selected existing annotations in memory; never writes the external file.");
    add("high-only", "Request only high-confidence writable patches; keep candidate diagnostics.");
    add("tokens", "One or more zero-based source-token windows: start:count[,start:count...].", "ranges");
    add("first-three", "Select the first three emitted source tokens."); add("all", "Select the full difficulty.");
    add("begin", "Selection UTF-16 start.", "offset"); add("end", "Selection UTF-16 end.", "offset");
    add("preset", "button, screen or mixed.", "name", "screen"); add("context", "Legacy context-seconds parameter.", "seconds", "1");
    add("work", "Shared work-unit budget.", "units", "30000000"); add("max-tasks", "Task guard.", "count", "512");
    add("beam", "Beam width.", "count", "64"); add("counterfactual", "Counterfactual budget.", "count", "64");
    add("stats-only", "Print parsed legacy dependency statistics without inference."); add("show-text", "Print synthetic output only; disabled with --file.");
    parser.process(app);
    if (!parser.isSet("file") && !parser.isSet("text") && !parser.isSet("baseline") && !parser.isSet("screenshot-prefix")) {
        return syntheticSpec();
    }
    QString chart; miacode::simai::SimaiTimingMetadata metadata;
    if (parser.isSet("file")) {
        QFile file(parser.value("file")); if (!file.open(QIODevice::ReadOnly)) { QTextStream(stderr) << "Cannot read requested external file.\n"; return 2; }
        const auto document = SimaiDocument::fromText(QString::fromUtf8(file.readAll()));
        const auto* difficulty = document.difficulty(parser.value("difficulty").toInt());
        if (!difficulty) {
            QStringList available; for (int id : document.difficultyIds()) available.append(QString::number(id));
            QTextStream(stderr) << "Requested difficulty slot is absent; available_slots=" << available.join(',') << '\n'; return 2;
        }
        chart = difficulty->chart; metadata = miacode::simai::buildTimingMetadata(document);
    } else chart = parser.isSet("screenshot-prefix") ? QStringLiteral("(136.7){16}1/5<1[16:3],1h[16:3],")
        : parser.isSet("baseline") ? QStringLiteral("(136.7){24}1,2,3,4,") : parser.value("text");
    BasicParameters parameters; const auto preset = parser.value("preset");
    if (preset != "button" && preset != "screen" && preset != "mixed") { QTextStream(stderr) << "Unknown preset.\n"; return 2; }
    parameters.inputPreference = preset == "button" ? 0 : preset == "screen" ? 1 : 2;
    parameters.overwriteExisting = parser.isSet("overwrite");
    parameters.highConfidenceOnly = parser.isSet("high-only");
    parameters.contextSeconds = parser.value("context").toDouble(); parameters.maxWorkUnits = parser.value("work").toLongLong();
    parameters.maxTasks = parser.value("max-tasks").toInt(); parameters.beamWidth = parser.value("beam").toInt(); parameters.counterfactualBudget = parser.value("counterfactual").toInt();
    if (parser.isSet("tokens")) {
        const auto parsed = SimaiParser::validateSyntax(chart, metadata); const auto starts = lineStarts(chart);
        QVector<QPair<int, int>> spans;
        for (const auto& source : parsed.sourceTokens) if (source.markerCount > 0) {
            const int first = starts.at(source.line - 1) + source.col - 1; spans.append({first, first + source.length});
        }
        int result = 0;
        for (const auto& range : parser.value("tokens").split(',')) {
            const auto parts = range.split(':'); bool firstOk = false, countOk = false;
            const int first = parts.value(0).toInt(&firstOk), count = parts.value(1).toInt(&countOk);
            if (parts.size() != 2 || !firstOk || !countOk || first < 0 || first >= spans.size() || count <= 0) {
                QTextStream(stderr) << "Invalid --tokens range; use zero-based start:count.\n"; return 2;
            }
            const int last = qMin(qint64(spans.size()), qint64(first) + count) - 1;
            QTextStream(stdout) << "REQUEST token_start=" << first << " token_count=" << last - first + 1 << '\n';
            result = qMax(result, diagnostic(chart, spans.at(first).first, spans.at(last).second, parameters, metadata,
                parser.isSet("stats-only"), parser.isSet("show-text") && !parser.isSet("file")));
        }
        return result;
    }
    int begin = 0, end = chart.size();
    if (parser.isSet("begin") || parser.isSet("end")) { begin = parser.value("begin").toInt(); end = parser.isSet("end") ? parser.value("end").toInt() : chart.size(); }
    else if (parser.isSet("first-three") || (parser.isSet("file") && !parser.isSet("all"))) {
        const auto parsed = SimaiParser::validateSyntax(chart, metadata); const auto lines = lineStarts(chart); int count = 0;
        for (const auto& source : parsed.sourceTokens) if (source.markerCount > 0) {
            const int start = lines.at(source.line - 1) + source.col - 1;
            if (count++ == 0) begin = start; end = start + source.length; if (count == 3) break;
        }
    }
    begin = qBound(0, begin, int(chart.size())); end = qBound(begin, end, int(chart.size()));
    return diagnostic(chart, begin, end, parameters, metadata, parser.isSet("stats-only"), parser.isSet("show-text") && !parser.isSet("file"));
}
