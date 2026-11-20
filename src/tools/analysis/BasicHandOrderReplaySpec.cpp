#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCoreApplication>
#include <QTextStream>

using namespace miacode::hand_order;
namespace {
BasicResult solve(const QString& chart, BasicParameters parameters = {}, int begin = 0, int end = -1)
{
    return inferBasicHandOrder(chart, SimaiParser::validateSyntax(chart), begin,
        end < 0 ? chart.size() : end, parameters);
}
QString apply(const QString& chart, const BasicResult& result)
{
    QString output = chart;
    for (int i = result.patches.size() - 1; i >= 0; --i) {
        const auto& patch = result.patches.at(i); output.replace(patch.start, patch.end - patch.start, patch.text);
    }
    return output;
}
bool allVerified(const BasicResult& result)
{
    if (result.rows.isEmpty()) return false;
    for (const auto& row : result.rows) if (!row.fullReplayVerified) return false;
    return true;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message, const BasicResult& result = BasicResult{}) {
        QTextStream output(pass ? stdout : stderr);
        output << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass;
        if (!pass) for (const auto& row : result.rows)
            output << "  " << row.line << ':' << row.col << ' ' << row.status << ' ' << row.reason
                << " work=" << result.workUsed << " full=" << row.fullReplayVerified << '\n';
    };
    BasicParameters bounded; bounded.counterfactualBudget = 0;
    const QString wifi = "(120){4}1w5[2:1],";
    const auto dual = solve(wifi, bounded); const auto dualText = apply(wifi, dual);
    const auto dualParsed = SimaiParser::validateSyntax(dualText);
    expect(dual.patches.size() == 1 && allVerified(dual) && dualText.contains("split:")
        && dualParsed.ok && dualParsed.annotationErrors.isEmpty()
        && dualParsed.noteMarkers.first().handOrderWifiMode == HandOrderWifiMode::Split,
        "native Wifi solves two real palms, persists split sectors and strictly reparses", dual);
    const auto reread = solve(dualText, bounded);
    expect(reread.patches.isEmpty() && allVerified(reread) && reread.rows.first().status == "preserved",
        "generated split Wifi is a replayable preserved lock on its next read", reread);
    const QString single = "(120){4}1w5[1:2]~[head=r;track=single:0:l],";
    const auto one = solve(single, bounded);
    expect(one.patches.isEmpty() && allVerified(one) && one.rows.first().status == "preserved"
        && one.rows.first().wifiModes.first() == HandOrderWifiMode::Single,
        "explicit single Wifi executes one continuous hand through all three queues", one);
    const QString either = "(120){4}1w5[1:2]~[head=m;track=single:0:m],";
    const auto eitherResult = solve(either, bounded);
    expect(allVerified(eitherResult) && eitherResult.rows.first().track != HandOrderHand::Either
        && eitherResult.rows.first().originalAnnotation.contains("single:0:m"),
        "single Wifi Either selects one actual complete hand and retains the original source", eitherResult);
    const QString coincident = "(120){4}1-5[2:1]*-5[2:1],";
    const auto shared = solve(coincident, bounded);
    expect(shared.patches.size() == 1 && allVerified(shared)
        && shared.rows.first().trackIntervals.size() == 2
        && shared.rows.first().trackIntervals.at(0).first().hand == shared.rows.first().trackIntervals.at(1).first().hand
        && SimaiParser::validateSyntax(apply(coincident, shared)).annotationErrors.isEmpty(),
        "identical native branches share one physical trajectory while both judgement queues complete", shared);
    for (const QString chart : {QStringLiteral("(120){4}1-5[2:1]*-3[2:1],"),
            QStringLiteral("(120){4}1-5[2:1]/3-5[2:1],"), QStringLiteral("(120){4}1-3-5[2:1],")}) {
        const auto result = solve(chart, bounded);
        expect(!result.patches.isEmpty() && allVerified(result)
            && SimaiParser::validateSyntax(apply(chart, result)).annotationErrors.isEmpty(),
            "native divergence, convergence and one-stroke chain use real replay rather than a shared-action exclusion", result);
    }
    const QString channel = "(120){4}1~r/A1,"; const int selected = channel.indexOf("A1");
    const auto mixed = solve(channel, bounded, selected, selected + 2);
    expect(mixed.patches.size() == 1 && mixed.patches.first().text == "A1~r"
        && mixed.contextObjects == 1 && allVerified(mixed),
        "one finite exterior-button plus A-screen template serves the same hand and leaves context read-only", mixed);
    const QString heldChannel = "(120){4}1h[4:1]~r/A1,"; const int heldSelection = heldChannel.indexOf("A1");
    const auto heldMixed = solve(heldChannel,bounded,heldSelection,heldSelection+2);
    expect(heldMixed.patches.size()==1 && heldMixed.patches.first().text=="A1~l" && heldMixed.contextObjects==1 && allVerified(heldMixed),
        "a locked Hold keeps its palm reserved and the A-screen note uses the free hand",heldMixed);
    // The finite footprint is solved jointly. A chain of pairwise close
    // proposals cannot inflate one hand to cover all three extremes.
    QVector<ScheduledAction> proposals;
    for (double x : {-0.3, 0.0, 0.3}) {
        ContactAction contact; contact.start = 0; contact.end = 1; contact.times = {0}; contact.points = {{x, 0}};
        proposals.append({contact, false, true, 0});
    }
    QVector<ScheduledAction> physical; qint64 work = 100000;
    expect(!resolveHandOrderActions(proposals, &physical, work),
        "three-way physical sharing rejects a transitive cluster beyond the finite proposal reach");
    const auto held = [](double first, double last, int hand, int lane, QPointF point) {
        ContactAction contact; contact.start = first; contact.end = last; contact.hand = hand;
        contact.points = {point}; contact.times = {first}; return ScheduledAction{contact, true, false, lane};
    };
    auto tracing = held(0, 1, 0, 2, {0.92, -0.38});
    tracing.action.tokenStart = 11; tracing.button = false; tracing.motion = true;
    tracing.action.points = {{0.92, -0.38}, {0.89, -0.40}}; tracing.action.times = {0, 1};
    auto anchor = held(0, 1, 1, 1, {0.38, -0.92}); anchor.action.tokenStart = 12;
    work = 100000;
    const auto anchored = physicalHandCrossingEffort({tracing, anchor}, {12}, {}, work);
    auto duplicate = tracing; duplicate.action.tokenStart = 13;
    work = 100000;
    const auto duplicated = physicalHandCrossingEffort({tracing, duplicate, anchor}, {12}, {}, work);
    auto firstPart = tracing; firstPart.action.end = 0.5;
    firstPart.action.points = {{0.92, -0.38}, {0.905, -0.39}}; firstPart.action.times = {0, 0.5};
    auto secondPart = tracing; secondPart.action.start = 0.5;
    secondPart.action.points = {{0.905, -0.39}, {0.89, -0.40}}; secondPart.action.times = {0.5, 1};
    work = 100000;
    const auto subdivided = physicalHandCrossingEffort({firstPart, secondPart, anchor}, {12}, {}, work);
    expect(anchored.valid && anchored.flowingConflicts == 0 && anchored.stationarySeconds > 0.9,
        "a stationary anchor crossing is soft effort rather than an impossible flowing topology");
    expect(duplicated.valid && subdivided.valid
        && std::abs(anchored.stationarySeconds - duplicated.stationarySeconds) < 1e-8
        && std::abs(anchored.stationarySeconds - subdivided.stationarySeconds) < 1e-8,
        "crossing effort is unchanged by duplicate logical branches and constant-pose subdivision");
    auto moving = anchor; moving.motion = true; moving.button = false;
    moving.action.points = {{0.38, -0.92}, {0.41, -0.91}}; moving.action.times = {0, 1};
    work = 100000;
    const auto intertwined = physicalHandCrossingEffort({tracing, duplicate, moving}, {}, {}, work);
    expect(intertwined.valid && intertwined.flowingConflicts == 1 && intertwined.stationarySeconds == 0,
        "two intertwined flowing hands remain one physical conflict across duplicate logical branches");
    auto waiting = anchor; waiting.motion = true; waiting.button = false;
    work = 100000;
    const auto paused = physicalHandCrossingEffort({tracing, waiting}, {}, {}, work);
    expect(paused.valid && paused.flowingConflicts == 0 && paused.stationarySeconds > 0.9,
        "an actual stationary interval inside a logical slide is treated as an anchor");
    auto sweepTap = held(0.3, 0.325, 1, 1, {0.38, -0.92}); sweepTap.action.tokenStart = 14;
    work = 100000;
    const auto sweeping = physicalHandCrossingEffort({tracing, sweepTap}, {}, {14}, work);
    expect(sweeping.valid && sweeping.flowingConflicts == 1,
        "a tap belonging to a continuous key sweep retains its flowing conflict with the opposite slide");
    const QVector<ScheduledAction> occupations{held(-0.5, 0.4, 1, 1, {0.8, -0.8}),
        held(0.8, 1.3, 0, 5, {-0.8, 0.8}), held(1.6, 2.4, 1, 2, {0.8, 0.8})};
    const auto routed = [](double first, double last, int hand) {
        ContactAction contact; contact.start = first; contact.end = last; contact.hand = hand;
        contact.points = {{-0.5, 0}, {0.5, 0}}; contact.times = {0, 2};
        return ScheduledAction{contact, false, true, 0};
    };
    auto twice = occupations; twice += {routed(0, 0.75, 0), routed(0.75, 1.5, 1), routed(1.5, 2, 0)};
    auto once = occupations; once += {routed(0, 0.75, 0), routed(0.75, 2, 1)};
    work = 100000;
    const bool twiceFits = resolveHandOrderActions(twice, &physical, work);
    work = 100000;
    expect(twiceFits && !resolveHandOrderActions(once, &physical, work),
        "pure occupation component retains the L-R-L requirement without pretending its synthetic path is a native chart");

    ContactAction inherited; inherited.tokenStart = 9; inherited.branch = 0; inherited.kind = "slide";
    inherited.start = 0; inherited.end = 2; inherited.motionStart = 0; inherited.motionEnd = 2;
    inherited.points = {{-0.8, 0}, {-0.8, 0}, {-0.5, 0}, {0, 0}, {0.5, 0}};
    inherited.times = {0, 0.5, 1, 1.5, 2};
    auto oldHand = inherited; oldHand.hand = 0; oldHand.end = 1 + 1.0 / 180;
    auto newHand = inherited; newHand.hand = 1; newHand.start = 1 - 1.0 / 180;
    const auto inheritedTouch = replayHandOrderTouches({{80, 0, -1, "C", 1.25, false, 1}}, {oldHand, newHand});
    expect(inheritedTouch.judgements.first().highQuality && inheritedTouch.judgements.first().handMask == 2
        && inheritedTouch.judgements.first().causes.first().progress > 0.5,
        "pure contact replay inherits the executing interval hand and full-motion progress after a real transfer");

    const QString legacy = "(120){4}1w5[1:2]~[head=r;track=0:l|0.5:r],";
    const auto oldWifi = solve(legacy, bounded);
    expect(oldWifi.patches.isEmpty() && allVerified(oldWifi) && oldWifi.rows.first().status == "preserved"
        && oldWifi.rows.first().wifiModes.first() == HandOrderWifiMode::Legacy
        && oldWifi.rows.first().trackIntervals.first().size() == 2,
        "legacy whole-Wifi transfer keeps its meaning and executes a real shared handoff instead of becoming explicit Single", oldWifi);
    BasicParameters recompute; recompute.overwriteExisting = true;
    const QString annotated = "(120){4}5h[1:1]~l,6~l,"; const int rewrite = annotated.indexOf("6~l");
    const auto revised = solve(annotated, recompute, rewrite, rewrite + 3);
    expect(revised.patches.size() == 1 && revised.patches.first().text == "6~r"
        && revised.patches.first().start == rewrite && allVerified(revised),
        "recompute changes only the selected annotation and keeps the outside locked Hold", revised);
    const auto touchConfidence = solve("(120){4}B1,");
    bool touchEvidence = false;
    for (const auto& proof : touchConfidence.rows.first().counterfactualEvidence)
        touchEvidence |= proof.component == "touch" && proof.searched;
    const auto trackConfidence = solve("(120){4}1-5[2:1],");
    bool trackEvidence = false;
    for (const auto& proof : trackConfidence.rows.first().counterfactualEvidence)
        trackEvidence |= proof.component.startsWith("track:") && proof.searched;
    expect(touchEvidence && trackEvidence && allVerified(touchConfidence) && allVerified(trackConfidence),
        "Touch and track intervals receive actual full-block opposite-hand evidence");
    BasicParameters selective; selective.highConfidenceOnly = true;
    const auto closed = solve("(120){4}6,1,", selective);
    const auto unfiltered = solve("(120){4}6,1,");
    bool exactConfidenceGate = closed.rows.size() == 2 && allVerified(closed);
    int eligible = 0;
    for (const auto& row : unfiltered.rows) {
        bool applied = false;
        for (const auto& patch : closed.patches) if (patch.start == row.start) applied = true;
        const bool high = row.confidenceLevel == "high" && row.fullReplayVerified;
        eligible += high; exactConfidenceGate &= applied == high;
    }
    expect(exactConfidenceGate && closed.patches.size() == eligible
        && closed.rows.first().dependencyUnit != closed.rows.last().dependencyUnit,
        "high-confidence application exactly gates independent ordinary candidates without closing their soft neighbourhood", closed);
    const auto extreme = solve("(120){4}1/3/5,", selective);
    expect(extreme.patches.isEmpty() && allVerified(extreme) && extreme.rows.first().pressureLevel == "extreme"
        && extreme.rows.first().confidence < 0.8,
        "Critical-window three-press route reports extreme pressure and never enters high-confidence application", extreme);
    for (const QString key : {QStringLiteral("1-3"), QStringLiteral("1<1"), QStringLiteral("1>1"),
            QStringLiteral("1p1"), QStringLiteral("1q1"), QStringLiteral("1pp1"), QStringLiteral("1qq1"),
            QStringLiteral("1v2"), QStringLiteral("1s5"), QStringLiteral("1z5"), QStringLiteral("1V72"), QStringLiteral("1^6")}) {
        const QString chart = "(120){4}" + key + "[2:1],";
        const auto family = solve(chart, bounded);
        expect(family.patches.size() == 1 && allVerified(family)
            && SimaiParser::validateSyntax(apply(chart, family)).ok,
            qPrintable(QStringLiteral("native shape family %1 solves and strictly writes its physical replay").arg(key)), family);
    }
    for (int lane = 1; lane <= 8; ++lane) {
        const QString chart = QStringLiteral("(120){4}%1w%2[2:1],").arg(lane).arg((lane + 3) % 8 + 1);
        const auto rotated = solve(chart, bounded);
        expect(rotated.patches.size() == 1 && allVerified(rotated)
            && SimaiParser::validateSyntax(apply(chart, rotated)).ok,
            "all eight native Wifi orientations pass the same two-palm generation and writeback", rotated);
    }
    BasicParameters zeroContext = bounded; zeroContext.contextSeconds = 0;
    const QString tailWindow = "(120){8}1-5[4:1],,,,,A7,";
    const int tailTouch = tailWindow.indexOf("A7");
    const auto contextTail = solve(tailWindow, zeroContext, tailTouch, tailTouch + 2);
    bool precedingSlide = false;
    for (const auto& row : contextTail.rows) precedingSlide |= row.context && !row.trackIntervals.isEmpty();
    expect(contextTail.patches.size() == 1 && precedingSlide && allVerified(contextTail),
        "zero extra context still imports a preceding slide inside its complete tail judging window", contextTail);
    const QString bodyTransfer = "(120){16}1h[1:1]~l/1!-5[0##0.25]~l,1h[1:1]~r,";
    const auto bodyParsed = SimaiParser::validateSyntax(bodyTransfer);
    const auto bodyLocked = solve(bodyTransfer, bounded);
    bool falselyPreserved = false;
    for (const auto& row : bodyLocked.rows) falselyPreserved |= row.status == "preserved" && row.fullReplayVerified;
    expect(bodyParsed.ok && bodyLocked.patches.isEmpty() && !falselyPreserved,
        "a real head-left/body-right Hold setup cannot pass a whole-Hold Left annotation", bodyLocked);
    const auto redundantTouchHold = solve("(120){4}A1h[1:4]~l,,,,,1-5[4:1]~r,", bounded);
    expect(allVerified(redundantTouchHold) && redundantTouchHold.rows.first().status == "preserved"
        && redundantTouchHold.rows.first().judgementKind == "touch_hold",
        "a redundant foreign slide brush does not turn a sufficient single-hand TouchHold into a false carry conflict", redundantTouchHold);
    ContactAction uniform; uniform.points = {{0, 0}, {0.25, 0}, {0.5, 0}, {1, 0}}; uniform.times = {0, 0.25, 0.5, 1};
    compactContactAction(uniform);
    ContactAction velocityChange; velocityChange.points = {{0, 0}, {0.5, 0}, {1, 0}}; velocityChange.times = {0, 0.25, 1};
    compactContactAction(velocityChange);
    expect(uniform.points.size() == 2 && velocityChange.points.size() == 3,
        "contact compaction removes only time-linear samples and preserves a collinear velocity change");
    const auto compressionParsed = SimaiParser::validateSyntax("(120){4}1!-5[0##1],");
    const auto& compressionMarker = compressionParsed.noteMarkers.first();
    ContactAction raw; raw.kind = "slide"; raw.start = raw.motionStart = compressionMarker.slideTraceSecond;
    raw.end = raw.motionEnd = compressionMarker.endSecond;
    const auto points = compressionMarker.slideSegmentPoints.first();
    for (int i = 0; i < points.size(); ++i) {
        raw.points.append(points.at(i) / 240.0); raw.times.append(raw.start + (raw.end - raw.start) * i / (points.size() - 1));
    }
    auto compacted = raw; compactContactAction(compacted);
    const auto beforeCompaction = replayHandOrderChart({compressionMarker}, {{raw, false, 0}});
    const auto afterCompaction = replayHandOrderChart({compressionMarker}, {{compacted, false, 0}});
    bool identicalReplay = beforeCompaction.meetsTarget && afterCompaction.meetsTarget
        && beforeCompaction.judgements.first().second == afterCompaction.judgements.first().second;
    const auto a = beforeCompaction.judgements.first().areas, b = afterCompaction.judgements.first().areas;
    identicalReplay &= a.size() == b.size();
    for (int i = 0; identicalReplay && i < a.size(); ++i)
        identicalReplay &= a.at(i).hit == b.at(i).hit && a.at(i).skipped == b.at(i).skipped && a.at(i).second == b.at(i).second;
    expect(compacted.points.size() == 2 && identicalReplay,
        "native straight contact compaction keeps every complete-replay area and final judgement time identical");
    return ok ? 0 : 1;
}
