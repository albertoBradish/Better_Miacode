#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/analysis/MuriAnalyzer.h"
#include "core/scene/PreviewSfxTimeline.h"
#include <QCoreApplication>
#include <QTextStream>
#include <limits>
#include <algorithm>
#include <cmath>
using namespace miacode::hand_order;
BasicResult solve(const QString& text, int begin = 0, int end = -1, BasicParameters p = {}) {
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), begin, end < 0 ? text.size() : end, p);
}
void syntheticMotion(TimelineNoteMarker& marker, double first, double last, const QVector<QPointF>& points) {
    marker.hasHeadStar = false; marker.slideTraceSecond = first; marker.second = first - 0.5; marker.endSecond = last;
    marker.slideSegmentPoints = {points}; for (auto& point : marker.slideSegmentPoints[0]) point *= 240.0;
    marker.slideSegmentShootSeconds = {first}; marker.slideSegmentDurations = {last - first};
    marker.slideSegmentPadEnterTimes.clear();
    marker.onSlide = marker.slideHead = marker.beforeSlide = marker.afterSlide = marker.tailOnSlideHead = false;
}
QString applyPatches(const QString& text, const BasicResult& result) {
    QString output = text;
    for (int i = result.patches.size() - 1; i >= 0; --i) { const auto& patch = result.patches.at(i); output.replace(patch.start, patch.end - patch.start, patch.text); }
    return output;
}
bool coldFixedProof(const QString& text, const BasicResult& result) {
    BasicParameters fixed; fixed.evaluateCompleteAssignment = true; fixed.counterfactualBudget = 0;
    const auto proof = solve(applyPatches(text, result), 0, -1, fixed);
    return proof.evaluation.complete && proof.patches.isEmpty();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 2 && (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe")
        || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe-preserve")
        || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--audit")
        || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--audit-wide"))) {
        const QString chart = QTextStream(stdin).readAll();
        BasicParameters parameters;
        parameters.overwriteExisting = QString::fromLocal8Bit(argv[1]) != QStringLiteral("--probe-preserve");
        if (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--audit-wide"))
            parameters.maxWorkUnits = 100000000;
        const auto result = solve(chart, 0, chart.size(), parameters);
        QTextStream(stdout) << "error=" << result.error << " patches=" << result.patches.size()
            << " work=" << result.workUsed << " windows=" << result.windowCount
            << " workLimited=" << result.workLimited << '\n';
        if (QString::fromLocal8Bit(argv[1]).startsWith(QStringLiteral("--audit"))) {
            QMap<QString, int> counts;
            for (const auto& row : result.rows) {
                ++counts[row.status];
                if (row.status != QStringLiteral("review") && row.status != QStringLiteral("preserved")
                    && !row.context)
                    QTextStream(stdout) << "ROW " << row.line << ':' << row.col << ' '
                        << row.status << ' ' << row.reason << '\n';
            }
            for (auto it = counts.cbegin(); it != counts.cend(); ++it)
                QTextStream(stdout) << "COUNT " << it.key() << ' ' << it.value() << '\n';
            for (const auto& window : result.searchWindows)
                if (window.outcome != QStringLiteral("validated"))
                    QTextStream(stdout) << "WINDOW " << window.start << ':' << window.end
                        << " tasks=" << window.tasks << " work=" << window.workUsed
                        << " outcome=" << window.outcome << '\n';
        } else {
            QTextStream(stdout) << applyPatches(chart, result);
            if (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe-preserve"))
                for (const auto& row : result.rows)
                    QTextStream(stdout) << "\nROW " << row.status << " " << row.reason
                        << " replay=" << row.fullReplayVerified;
        }
        return result.error.isEmpty() ? 0 : 1;
    }
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) { QTextStream(pass ? stdout : stderr)
        << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass; };
    const auto natural = solve("(120){4}6,");
    expect(natural.patches.size() == 1 && natural.patches.first().text == "6~l",
        "lane six has a mild natural left-hand preference");
    const auto interaction = solve("(150){16}2,1,2,1,");
    const QString interactionText = applyPatches("(150){16}2,1,2,1,", interaction);
    expect(interactionText == "(150){16}2~r,1~l,2~r,1~l,",
        "automatic hand order favors four-step adjacent-lane hand alternation");
    BasicParameters recompute;
    recompute.overwriteExisting = true;
    for (int bpm : {80, 150, 222, 240, 288}) {
        const QString source = QStringLiteral("(%1){16}5~l,,5$~r,").arg(bpm);
        expect(applyPatches(source, solve(source, 0, -1, recompute))
            == QStringLiteral("(%1){16}5~l,,5$~l,").arg(bpm),
            "tap and tap-star on the same key keep one hand across an eighth-note interval");
    }
    const QString twoSixteenths = "(120){16}5~l,5~r,";
    expect(applyPatches(twoSixteenths, solve(twoSixteenths, 0, -1, recompute)) == "(120){16}5~l,5~l,",
        "a comfortable isolated same-key pair has no automatic handoff");
    for (int bpm : {100, 180, 240}) {
        const QString head = QStringLiteral("(%1)").arg(bpm);
        const QString relay = head + QStringLiteral(
            "{4}4/8h[2:1],,7h[2:1],,6h[2:1],,5h[4:1],1h[2:1],,,"
            "{2}2h[2:1],3h[2:1],");
        const QString expected = head + QStringLiteral(
            "{4}4~r/8h[2:1]~l,,7h[2:1]~r,,6h[2:1]~l,,5h[4:1]~r,1h[2:1]~l,,,"
            "{2}2h[2:1]~r,3h[2:1]~l,");
        expect(applyPatches(relay, solve(relay, 0, -1, recompute)) == expected,
            "different-key holds meeting at their nominal tails relay hands across BPMs");

        const QString touchSweep = head + QStringLiteral(
            "{32}E2/D2,A2,D3,A3,D4,A4,,,E8/D8,A7,D7,A6,D6,A5,");
        const QString touchResult = applyPatches(touchSweep, solve(touchSweep, 0, -1, recompute));
        expect(touchResult.contains(QStringLiteral("A2~r,D3~r,A3~r,D4~r,A4~r"))
            && touchResult.contains(QStringLiteral("A7~l,D7~l,A6~l,D6~l,A5~l")),
            "cross-ring Touch sweeps keep one hand through the moving path across BPMs");
    }
    for (int bpm : {100, 135, 200}) {
        const QString prefix = QStringLiteral("(%1)").arg(bpm);
        const QString alternating = prefix + QStringLiteral("{16}7,8,7,");
        expect(applyPatches(alternating, solve(alternating, 0, -1, recompute))
            == prefix + QStringLiteral("{16}7~l,8~r,7~l,"),
            "three adjacent returning taps alternate across BPMs");

        const QString handed = prefix + QStringLiteral(
            "{4}8-4[32:3],7/8,1-5[32:3]/6,1/2,");
        const auto handedResult = solve(handed, 0, -1, recompute);
        const QString expected = applyPatches(handed, handedResult);
        expect(expected.contains(";track=0:r],7~l/8~r,")
            && expected.contains(";track=0:l]/6~") && expected.endsWith("1~l/2~r,")
            && std::all_of(handedResult.rows.cbegin(), handedResult.rows.cend(), [](const auto& row) {
                return row.fullReplayVerified;
            }),
            "the tap at trace start hands an edge star to the other palm across BPMs");
        const QString oldLabels = prefix + QStringLiteral(
            "{4}8-4[32:3]~[head=r;track=0:l],7~r/8~l,"
            "1-5[32:3]~[head=l;track=0:r]/6~r,1~r/2~l,");
        expect(applyPatches(oldLabels, solve(oldLabels, 0, -1, recompute)) == expected,
            "recalculation uses native notes rather than previous selected labels");
    }
    const QString ordinaryTraceTap = "(135){4}8-4[32:3],1/8,";
    expect(applyPatches(ordinaryTraceTap, solve(ordinaryTraceTap, 0, -1, recompute))
        == "(135){4}8-4[32:3]~[head=l;track=0:l],1~r/8~l,",
        "a same-key trace tap without the inner neighbour keeps the ordinary hand");
    const QString fiveAxis = "(190){16}6~l,5~r,6~l,7~l,6~l,";
    const auto repairedAxis = solve(fiveAxis, 0, -1, recompute);
    expect(repairedAxis.error.isEmpty()
        && applyPatches(fiveAxis, repairedAxis) == "(190){16}6~l,5~r,6~l,7~r,6~l,",
        "automatic recomputation separates the outer moves of a five-note axis");
    const QString longVertical = "(190){16},,,,3~r/6~l,,4~r,5~l,5~l,5~l,5~l,5~l,5~l,5~l,4~r,5~l,"
        "\n{16}5~l,5~l,5~l,5~l,5~l,,,,";
    const auto repairedVertical = solve(longVertical, 0, -1, recompute);
    expect(repairedVertical.error.isEmpty()
        && applyPatches(longVertical, repairedVertical).contains("5~l,5~r,5~l,5~r,5~l,5~r,5~l,")
        && applyPatches(longVertical, repairedVertical).contains("{16}5~r,5~l,5~r,5~l,5~r,"),
        "automatic recomputation alternates a long vertical without splitting winding pairs");
    const QString holdSweep = "(190){48}7~l,,,,5~r,,,,6~l,,,,1~r,,,,4~l,,,,3~r,,,,5h[4:1]~l,,,,,,,,,,,,"
        "4~r,,,3~r,,,2~r,,,1~r,,,\n{32}5~l,4~r,3~r,2~r,1~r,8~l,7~l,6~l,3~r/5~l,,,,"
        "2~r,,4~l,,2~r,,4~l,,2~r,,4~l,,3~r,,4~l,,2~r,,6~l,,";
    const auto repairedSweep = solve(holdSweep, 0, -1, recompute);
    expect(repairedSweep.error.isEmpty()
        && applyPatches(holdSweep, repairedSweep).contains(
            "4~r,,,3~r,,,2~r,,,1~r,,,\n{32}5~l,4~l,3~l,2~l,1~l,8~l,7~l,6~l,"),
        "automatic recomputation keeps a rapid 5-led sweep on the left after a right-hand lead-in");
    for (int bpm : {120, 180, 240}) {
        const QString chart = QStringLiteral("(%1){32}8-4[8:1],7,6,5,4,").arg(bpm);
        const auto result = solve(chart);
        expect(result.error.isEmpty()
            && applyPatches(chart, result).contains("7~l,6~l,5~l,4~l,")
            && std::all_of(result.rows.cbegin(), result.rows.cend(),
                [](const BasicRow& row) { return row.fullReplayVerified; }),
            "automatic four-key sweep stays intact after a simultaneous slide across BPM values");
    }
    const QString eSweep = "(190){16},,E5~l,E4~r,E3~l,,,,,,,,,,,,\n"
        "{16},,E7~l,E6~r,E5~l,,,,,,,,,,,,\n"
        "{16},,E3~r,E4~l,E5~r,,,,,,,,,,,,";
    const auto repairedTouches = solve(eSweep, 0, -1, recompute);
    const QString touchText = applyPatches(eSweep, repairedTouches);
    expect(repairedTouches.error.isEmpty() && touchText.contains("E5~r,E4~r,E3~r")
        && touchText.contains("E7~l,E6~l,E5~l")
        && touchText.contains("E3~r,E4~r,E5~r"),
        "automatic recomputation keeps three timed E-ring sweeps on one hand each");
    for (int bpm : {100, 150, 190, 240}) {
        const auto check = [&](const QString& body, const QString& expected, const char* pattern) {
            const QString source = QStringLiteral("(%1)").arg(bpm) + body;
            const auto result = solve(source);
            const QByteArray label = QStringLiteral("%1 BPM %2 follows the same hand pattern")
                .arg(bpm).arg(QString::fromLatin1(pattern)).toUtf8();
            expect(result.error.isEmpty() && applyPatches(source, result)
                == QStringLiteral("(%1)").arg(bpm) + expected
                && std::all_of(result.rows.cbegin(), result.rows.cend(),
                    [](const BasicRow& row) { return row.fullReplayVerified; }), label.constData());
        };
        check("{16}5,5,5,5,5,5,", "{16}5~l,5~r,5~l,5~r,5~l,5~r,", "vertical");
        check("{16}6,5,6,7,6,", "{16}6~l,5~r,6~l,7~r,6~l,", "five-note axis");
        check("{32}5,4,3,2,1,8,7,6,", "{32}5~l,4~l,3~l,2~l,1~l,8~l,7~l,6~l,", "sweep");
        check("{16}E7,E6,E5,", "{16}E7~l,E6~l,E5~l,", "E-ring sweep");
        check("{4}1-5[4:1],5-1[4:1],",
            "{4}1-5[4:1]~[head=r;track=0:r],5-1[4:1]~[head=l;track=0:r],", "one-stroke");
    }
    for (int bpm : {160, 180, 200, 240}) {
        const QString prefix = QStringLiteral("(%1)").arg(bpm);
        const auto checkSweep = [&](const QString& source, const QString& expected, const char* label) {
            const auto result = solve(prefix + source, 0, -1, recompute);
            expect(result.error.isEmpty() && applyPatches(prefix + source, result) == prefix + expected
                && std::all_of(result.rows.cbegin(), result.rows.cend(),
                    [](const BasicRow& row) { return row.fullReplayVerified; }), label);
        };
        checkSweep("{16}2~r,3~l,4~r,", "{16}2~r,3~r,4~r,",
            "automatic 16th-note short sweep stays on one hand across BPMs");
        checkSweep("{16}7x~l,6x~r,5x-8[8:1]~[head=l;track=0:l],",
            "{16}7x~l,6x~l,5x-8[8:1]~[head=l;track=0:r],",
            "automatic short sweep can pass its final star trace to the other hand");
        checkSweep("{16}1~r/8~l,,8~r,", "{16}1~r/8~l,,8~l,",
            "automatic same-key return after an Each keeps the previous striker");
        checkSweep("{32}3b~r/7b~l,4b~r/8b~l,1b~r/5b~l,,,,,,8~r,",
            "{32}3b~r/7b~l,4b~r/8b~l,1b~l/5b~r,,,,,,8~l,",
            "automatic paired sweep preserves both adjacent paths through the crossing");
        checkSweep("{32}1~l/2~r,3~r,4~l,5~l,",
            "{32}1~l/2~r,3~r,4~r,5~r,",
            "automatic Each-led sweep uses one hand for its whole moving line");
    }
    const QString densePassage = "(200){32}2xh[32:15]~r/6xh[32:15]~l,,,,,,,,,,,,,,,,"
        "3b~r/7b~l,4b~r/8b~l,1b~r/5b~l,,,,,,8~r,,,,8~l,,,,\n"
        "{32}1~r/7~l,6~l,5~l,4~l,1~r,,,,1~r,,,,2~r/8~l,3~r,4~l,5~r,8~l,,,,8~l,,,,1~r,,,,1~r,,,,\n"
        "{32}7~l/8~r,6~l,5~r,4~l,8~r,,,,8~r,,,,1~l/2~r,3~r,4~l,5~l,1~r,,,,1~r,,,,8~l,,,,8~l,,,,";
    const auto denseResult = solve(densePassage, 0, -1, recompute);
    const QString denseText = applyPatches(densePassage, denseResult);
    expect(denseResult.error.isEmpty()
        && denseText.contains("3b~r/7b~l,4b~r/8b~l,1b~l/5b~r,,,,,,8~l,")
        && denseText.contains("1~r/7~l,6~l,5~l,4~l,")
        && denseText.contains("2~r/8~l,3~r,4~r,5~r,")
        && denseText.contains("7~l/8~r,6~l,5~l,4~l,")
        && denseText.contains("1~l/2~r,3~r,4~r,5~r,"),
        "the complete 200 BPM passage keeps crossed and Each-led sweep phrases coherent");
    const QString correctCircle = "(196){32}2b~r/8b~l,3~r,4~r,5~r,6~r,7~r,8~r,1~r,"
        "2b~r/6b~l,5~l,4~l,3~l,2~l,1~l/3~r,4~r/8~l,5~r/7~l,"
        "(190)6b~l,,,,,,,,,,,,,,,,";
    const auto preservedCircle = solve(correctCircle, 0, -1, recompute);
    expect(preservedCircle.error.isEmpty()
        && applyPatches(correctCircle, preservedCircle) == correctCircle,
        "a correct complete-ring sweep with two-hand Each endings remains intact");
    const QString equivalentGrid = "(240){12}2~r,3~l,4~r,";
    const auto equivalent = solve(equivalentGrid, 0, -1, recompute);
    expect(equivalent.error.isEmpty()
        && applyPatches(equivalentGrid, equivalent) == "(240){12}2~r,3~r,4~r,",
        "automatic sweep continuity depends on real spacing, not a 16th-note spelling");
    for (int bpm : {120, 155, 190}) {
        const QString prefix = QStringLiteral("(%1){8}").arg(bpm);
        const QString right = prefix + "3>1[8:1]~[head=r;track=0:r],3~l,3~l,3~l,";
        const QString left = prefix + "6>4[8:1]~[head=l;track=0:l],6~r,6~r,6~r,";
        const auto rightResult = solve(right, 0, -1, recompute);
        const auto leftResult = solve(left, 0, -1, recompute);
        expect(rightResult.error.isEmpty() && leftResult.error.isEmpty()
            && applyPatches(right, rightResult)
                == prefix + "3>1[8:1]~[head=r;track=0:l],3~r,3~r,3~r,"
            && applyPatches(left, leftResult)
                == prefix + "6>4[8:1]~[head=l;track=0:r],6~l,6~l,6~l,"
            && std::all_of(rightResult.rows.cbegin(), rightResult.rows.cend(),
                [](const BasicRow& row) { return row.fullReplayVerified; }),
            "automatic replay keeps relaxed head-lane taps opposite their star trace");
    }
    const QString finerLaneGrid = "(155){16}3>1[8:1]~[head=r;track=0:r],,3~l,,3~l,,3~l,,";
    const auto finerLaneResult = solve(finerLaneGrid, 0, -1, recompute);
    expect(finerLaneResult.error.isEmpty()
        && applyPatches(finerLaneGrid, finerLaneResult)
            == "(155){16}3>1[8:1]~[head=r;track=0:l],,3~r,,3~r,,3~r,,",
        "automatic star-and-tap handoff is independent of a finer written subdivision");
    const QString adjacentCross = "(155){8}4~l/5~r,";
    const auto retainedCross = solve(adjacentCross);
    expect(retainedCross.patches.isEmpty()
        && std::all_of(retainedCross.rows.cbegin(), retainedCross.rows.cend(),
            [](const BasicRow& row) { return row.fullReplayVerified; }),
        "an explicitly crossed adjacent Each can remain a verified play hypothesis");
    for (int bpm : {120, 155, 190}) {
        const QString prefix = QStringLiteral("(%1){48}").arg(bpm);
        const QString right = prefix + "1~r,2~r,3~r,,,,1~r,2~r,3~r,";
        const QString left = prefix + "8~l,7~l,6~l,,,,8~l,7~l,6~l,";
        const auto rightResult = solve(right, 0, -1, recompute);
        const auto leftResult = solve(left, 0, -1, recompute);
        expect(rightResult.error.isEmpty() && leftResult.error.isEmpty()
            && applyPatches(right, rightResult)
                == prefix + "1~r,2~r,3~r,,,,1~l,2~l,3~l,"
            && applyPatches(left, leftResult)
                == prefix + "8~l,7~l,6~l,,,,8~r,7~r,6~r,"
            && std::all_of(rightResult.rows.cbegin(), rightResult.rows.cend(),
                [](const BasicRow& row) { return row.fullReplayVerified; }),
            "automatic paired short sweeps alternate without splitting either sweep");
    }
    const QString ladder = "(155){8}1~l/3~r,3~r,3~r/4~l,4~r,4~r/5~l,5~l,5~l/6~r,6~r,";
    const auto ladderResult = solve(ladder, 0, -1, recompute);
    expect(ladderResult.error.isEmpty()
        && applyPatches(ladder, ladderResult)
            == "(155){8}1~l/3~r,3~r,3~r/4~l,4~l,4~l/5~r,5~r,5~r/6~l,6~l,"
        && std::all_of(ladderResult.rows.cbegin(), ladderResult.rows.cend(),
            [](const BasicRow& row) { return row.fullReplayVerified; }),
        "automatic staggered Each ladder carries each relaxed rebound into its next pair");
    const QString isolatedPair = "(155){8}2b~l/3b~r,,4~r,3~r/4~l,";
    expect(applyPatches(isolatedPair, solve(isolatedPair, 0, -1, recompute)) == isolatedPair,
        "automatic carry does not lock an unrelated isolated note into the next Each");
    BasicParameters lowerSweep = recompute;
    lowerSweep.sweepThreshold = 120;
    const QString lowThresholdStar = "(120){16}7x~l,6x~r,5x-8[8:1]~[head=l;track=0:l],";
    const auto lowered = solve(lowThresholdStar, 0, -1, lowerSweep);
    expect(lowered.error.isEmpty() && applyPatches(lowThresholdStar, lowered)
        == "(120){16}7x~l,6x~l,5x-8[8:1]~[head=l;track=0:r],",
        "automatic sweep threshold applies the chosen integer speed at 120 BPM");
    for (const QString& axis : {QStringLiteral("(190){16}3~r,4~l,3~r,2~l,3~r,"),
                                QStringLiteral("(150){32}3~r,4~l,3~r,2~l,3~r,")}) {
        const auto result = solve(axis, 0, -1, recompute);
        expect(result.error.isEmpty() && applyPatches(axis, result) == axis,
            "automatic five-note axis keeps 4-3-2 out of the sweep rule");
    }
    const QString pressuredPair = "(180){16}7h[1:1]~l,4~r,5~l,";
    const auto pressured = solve(pressuredPair, 0, -1, recompute);
    expect(pressured.error.isEmpty()
        && applyPatches(pressuredPair, pressured) == "(180){16}7h[1:1]~l,4~r,5~r,",
        "automatic two-key sweep uses the free hand through a crossed boundary");
    const QString occupied = "(120){4}5h[1:1]~l,6,";
    const int six = occupied.indexOf("6,");
    const auto busy = solve(occupied, six, six + 1);
    expect(busy.patches.size() == 1 && busy.patches.first().text == "6~r"
        && busy.contextObjects == 1, "locked hold outside selection occupies left hand across the context boundary");
    expect(busy.rows.size() == 2 && busy.rows.first().context
        && busy.rows.first().originalAnnotation == "~l" && busy.rows.first().head == HandOrderHand::Left,
        "read-only hold context exposes the hand assumed by the successful candidate");
    const QString eitherHold = "(120){4}5h[1:1]~m,6,";
    const int selectedSix = eitherHold.indexOf("6,");
    const auto conditional = solve(eitherHold, selectedSix, selectedSix + 1);
    expect(conditional.patches.size() == 1 && conditional.patches.first().start == selectedSix
        && conditional.rows.first().context && conditional.rows.first().originalAnnotation == "~m"
        && conditional.rows.first().head != HandOrderHand::Unspecified
        && conditional.rows.first().head != conditional.rows.last().head,
        "Either context keeps its source annotation while showing the conditional execution hand");
    const auto preservedEither = solve(eitherHold);
    expect(preservedEither.rows.first().status == "preserved" && !preservedEither.rows.first().context
        && preservedEither.rows.first().originalAnnotation == "~m"
        && preservedEither.rows.first().head != HandOrderHand::Unspecified,
        "preserved Either inside the target also exposes its occupancy assumption");
    const auto each = solve("(120){4}15,");
    expect(each.patches.size() == 1 && each.patches.first().text == "1~r/5~l"
        && each.rows.first().noteHands.size() == 2 && each.rows.first().noteHands[0] != each.rows.first().noteHands[1],
        "simultaneous cluster receives a coherent two-hand plan and exposes both labels");
    const auto reversed = solve("(120){4}5/1,");
    expect(reversed.patches.size() == 2 && reversed.patches.first().text == "5~l"
        && reversed.patches.last().text == "1~r", "same-time assignment is independent of source token ordering");
    const auto triple = solve("(120){4}1/3/5,");
    expect(triple.patches.size() == 3 && triple.rows.size() == 3 && triple.rows.first().fullReplayVerified
        && triple.rows.first().peakSpeed >= 12 && triple.rows.first().confidence < 0.8,
        "three nominally simultaneous presses use the Critical window and expose extreme pressure without high confidence");
    const auto buttons = solve("(120){4}1h[1:1]~l,1,");
    expect(buttons.patches.size() == 1 && buttons.rows.last().fullReplayVerified
        && buttons.rows.first().inputChannel != buttons.rows.last().inputChannel
        && ((buttons.rows.first().inputChannel == "A1" && buttons.rows.last().inputChannel == "Button1")
            || (buttons.rows.first().inputChannel == "Button1" && buttons.rows.last().inputChannel == "A1"))
        && !buttons.rows.last().causes.isEmpty(),
        "held same-lane notes use distinct actual Button/A channels rather than a fabricated repeated DOWN");
    const QString waiting = "(120){8}5h[1:1]~l/1-5[4:1],2,";
    const auto slide = solve(waiting);
    bool hasSlide = false, hasTap = false;
    for (const auto& patch : slide.patches) { hasSlide |= patch.text.contains("track="); hasTap |= patch.text == "2~r"; }
    expect(hasSlide && hasTap, "slide wait does not occupy its head hand before the independent trace task starts");
    const auto protectedText = solve("(120){4}6~m,");
    expect(protectedText.patches.isEmpty() && protectedText.rows.first().status == "preserved",
        "existing annotations including Either are preserved exactly");
    const auto far = solve("(120){4}6,,,,,,,,,,A1,");
    expect(far.patches.size() == 2 && far.patches.first().text == "6~l"
        && far.rows.last().judgementKind == "direct_touch", "remote Touch now has an independent screen-contact candidate");
    const auto near = solve("(120){4}6/A1,");
    expect(near.patches.size() == 2, "external button and screen Touch are separate simultaneous channels");
    const auto spanning = solve("(120){4}5h[1:4]~l,6,,,,1w5[4:1],");
    bool spanningVerified = !spanning.patches.isEmpty();
    for (const auto& row : spanning.rows) spanningVerified &= row.fullReplayVerified;
    expect(spanningVerified, "long hold context and Wifi are jointly solved through actual hand occupancy");
    const auto wifi = solve("(120){4}1w5[4:1],");
    expect(wifi.patches.size() == 1 && wifi.rows.first().fullReplayVerified && wifi.patches.first().text.contains("split:"),
        "Wifi executes two real palms and writes explicit sectors instead of an unsupported or Either placeholder");
    const QString headedWifi = "(120){4}1/1w5[4:1],";
    const auto headedWifiResult = solve(headedWifi);
    const auto headedWifiText = applyPatches(headedWifi, headedWifiResult);
    expect(headedWifiResult.error.isEmpty()
        && headedWifiText.contains("1~r/1w5[4:1]~[head=r;track=split:0:r&0:l]")
        && std::all_of(headedWifiResult.rows.cbegin(), headedWifiResult.rows.cend(),
            [](const BasicRow& row) { return row.fullReplayVerified; }),
        "a playable headed Wifi shares its button press but uses two distinct tracking palms");
    const QString doubleStarOld = "(120){4}1-5[4:1]~[head=r;track=0:r]/"
        "1-7[4:1]~[head=r;track=0:r],";
    const auto doubleStarResult = solve(doubleStarOld, 0, -1, recompute);
    expect(doubleStarResult.error.isEmpty()
        && applyPatches(doubleStarOld, doubleStarResult).contains(
            "1-5[4:1]~[head=r;track=0:r]/1-7[4:1]~[head=r;track=0:l]")
        && std::all_of(doubleStarResult.rows.cbegin(), doubleStarResult.rows.cend(),
            [](const BasicRow& row) { return row.fullReplayVerified; }),
        "automatic recomputation assigns divergent same-head star tracks to separate hands");
    const QString branchedStars = "(120){4}1-5[4:1]*-7[4:1]"
        "~[head=r;branch0=0:r;branch1=0:r],";
    const auto branchResult = solve(branchedStars, 0, -1, recompute);
    expect(branchResult.error.isEmpty()
        && applyPatches(branchedStars, branchResult).contains(
            "1-5[4:1]*-7[4:1]~[head=r;branch0=0:r;branch1=0:l]"),
        "automatic recomputation also splits tracks of a single native multi-branch star");
    QString longChart = "(120){16}";
    for (int i = 0; i < 200; ++i) longChart += "6,";
    BasicParameters local; local.maxTasks = 16; local.counterfactualBudget = 4;
    const int first = longChart.indexOf("6,");
    const auto bounded = solve(longChart, first, first + 1, local);
    int visibleContexts = 0;
    for (const auto& row : bounded.rows) visibleContexts += row.context;
    expect(bounded.patches.size() == 1 && bounded.contextObjects == visibleContexts
        && visibleContexts > 0 && visibleContexts < 16,
        "local context count matches only displayed participating rows, not 199 remote taps");
    const auto rolling = solve(longChart, 0, longChart.size(), local);
    expect(rolling.patches.size() == 200 && rolling.maxWindowTasks <= local.maxTasks && !rolling.workLimited,
        "a long ordinary sequence rolls bounded windows instead of treating the full difficulty as one task block");
    QString dense = "(120){4}";
    for (int i = 0; i < 20; ++i) dense += (i ? "/6" : "6");
    dense += ',';
    const auto blocked = solve(dense, 0, dense.size(), local);
    expect(blocked.patches.isEmpty() && blocked.rows.first().status == "undecided"
        && blocked.taskLimited && !blocked.workLimited && !blocked.rows.first().hasCandidate
        && blocked.rows.first().pressureLevel.isEmpty(), "inseparable simultaneous task limits remain explicit and have no invented zero-pressure candidate");
    const double one = basicMotionCost({{0,0},{1,0}}, {1});
    const double split = basicMotionCost({{0,0},{0.25,0},{0.5,0},{1,0}}, {0.25,0.25,0.5});
    expect(std::abs(one - split) < 1e-12, "subdividing the same physical motion cannot lower cost");
    BasicParameters invalid; invalid.contextSeconds = std::numeric_limits<double>::quiet_NaN();
    expect(solve("6,", 0, -1, invalid).error == "invalid_parameters", "non-finite search parameters are rejected");
    invalid = {}; invalid.sweepThreshold = 0;
    expect(solve("6,", 0, -1, invalid).error == "invalid_parameters",
        "the integer sweep threshold rejects values outside its range");
    BasicParameters exhaustedBudget;
    exhaustedBudget.maxWorkUnits = 1000;
    exhaustedBudget.counterfactualBudget = 0;
    const QString fallbackChart = "(120){4}1-5[4:1],5-1[4:1],";
    const auto baselineFallback = solve(fallbackChart, 0, -1, exhaustedBudget);
    expect(baselineFallback.patches.isEmpty()
        && baselineFallback.rows.first().status == QStringLiteral("undecided")
        && !baselineFallback.rows.first().fullReplayVerified,
        "a search budget limit leaves undecided notes uncolored by default");
    exhaustedBudget.colorUnresolved = true;
    const auto optedFallback = solve(fallbackChart, 0, -1, exhaustedBudget);
    expect(optedFallback.patches.size() == 2
        && applyPatches(fallbackChart, optedFallback).contains("5-1[4:1]~[head=l;track=0:r]")
        && optedFallback.rows.first().status == QStringLiteral("heuristic")
        && !optedFallback.rows.first().fullReplayVerified,
        "opted-in default color keeps an explicit unverified result");
    exhaustedBudget.highConfidenceOnly = true;
    expect(solve(fallbackChart, 0, -1, exhaustedBudget).patches.isEmpty(),
        "high-confidence-only application excludes unverified layout fallbacks");
    exhaustedBudget.highConfidenceOnly = false;
    const QString compoundBudgetChart = "(190){16}6,5,6,7,6,,5,5,5,5,,"
        "{4}1-5[4:1],5-1[4:1],";
    const auto baseCoverage = miacode::chart_transform::basicHandOrderLayoutInSelection(
        compoundBudgetChart, 0, 0);
    const auto automaticCoverage = solve(compoundBudgetChart, 0, -1, exhaustedBudget);
    bool retained = !baseCoverage.edit.patches.isEmpty();
    for (const auto& basePatch : baseCoverage.edit.patches)
        retained &= std::any_of(automaticCoverage.patches.cbegin(), automaticCoverage.patches.cend(),
            [&](const auto& automaticPatch) { return automaticPatch.start == basePatch.start; });
    expect(retained && automaticCoverage.error.isEmpty(),
        "a bounded automatic run retains every available basic-layout token across mixed patterns");
    expect(natural.rows.first().hasCounterfactual && natural.rows.first().signedCostGap >= -1e-9,
        "reverse-hand evidence retains a signed complete-block cost gap");
    BasicParameters old; old.version = 1;
    expect(solve("6,", 0, -1, old).error == "invalid_parameters", "old algorithm requests are explicitly rejected");
    old = {}; old.judgeProfileRevision = 0;
    expect(solve("6,", 0, -1, old).error == "invalid_parameters", "old judging profiles cannot produce an applicable result");
    BasicParameters narrow; narrow.beamWidth = 1; narrow.counterfactualBudget = 0;
    const auto lockedEach = solve("(120){4}6/1~l,", 0, -1, narrow);
    expect(lockedEach.patches.size() == 2
        && applyPatches("(120){4}6/1~l,", lockedEach) == "(120){4}6~l/1~r,",
        "an unmistakably crossed simple Each lock is repaired even with a width-one beam");
    const QString crossedDefault = "(190){8}8~r/2~l,7~r/3~l,";
    expect(applyPatches(crossedDefault, solve(crossedDefault))
        == "(190){8}8~l/2~r,7~l/3~r,",
        "default automatic inference repairs unmistakably crossed simple Each labels");
    for (int bpm : {100, 150, 190, 240}) {
        const QString crossed = QStringLiteral("(%1){8}8~r/2~l,7~r/3~l,").arg(bpm);
        const QString expected = QStringLiteral("(%1){8}8~l/2~r,7~l/3~r,").arg(bpm);
        BasicParameters overwrite; overwrite.overwriteExisting = true;
        expect(applyPatches(crossed, solve(crossed, 0, -1, overwrite)) == expected,
            "automatic same-time opposite-side Each never emits an extreme hand crossing");
        const QString reversed = QStringLiteral("(%1){8}2~l/8~r,3~l/7~r,").arg(bpm);
        expect(applyPatches(reversed, solve(reversed, 0, -1, overwrite))
            == QStringLiteral("(%1){8}2~r/8~l,3~r/7~l,").arg(bpm),
            "automatic crossed Each protection is independent of slash source order");
    }
    const auto distinctGroups = solve("(120){4}A1/A5,");
    expect(distinctGroups.patches.size() == 2, "disconnected same-time Touch components do not prematurely reject one another");
    const auto group = solve("(120){4}B1/B2/B3,");
    bool mixedCarry = false;
    for (const auto& row : group.rows) mixedCarry |= row.judgementKind == "group_carry" && row.head == HandOrderHand::Unspecified
        && row.status == "undecided" && row.reason == "mixed_touch_responsibility" && row.causes.size() >= 2;
    bool finiteSingleCarry = false;
    for (const auto& row : group.rows) finiteSingleCarry |= row.judgementKind == "group_carry" && row.fullReplayVerified
        && row.head != HandOrderHand::Unspecified && row.causes.size() >= 2;
    expect((mixedCarry && group.patches.size() == 2) || (finiteSingleCarry && group.patches.size() == 3),
        "joint Touchgroup carries retain complete actual contributors, including valid finite same-hand sharing");
    BasicParameters synthetic; synthetic.counterfactualBudget = 0;
    const QString inheritedText = "(120){4}1-5[4:1]~[head=r;track=0:l|0.5:r]/C,";
    auto inheritedParse = SimaiParser::validateSyntax(inheritedText);
    for (auto& marker : inheritedParse.noteMarkers) {
        if (marker.type == "slide") syntheticMotion(marker, 0, 2, {{-0.8,0},{-0.8,0},{-0.5,0},{0,0},{0.5,0}});
        else if (marker.type == "touch") marker.second = 1.25;
    }
    const auto inherited = inferBasicHandOrder(inheritedText, inheritedParse, 0, inheritedText.size(), synthetic);
    expect(inherited.patches.isEmpty() && !inherited.rows.first().fullReplayVerified,
        "inconsistent synthetic motion cannot bypass the native slide queues in complete replay");
    const QString transferText = "(120){4}1-5[4:1]/1h[4:1]~r/5h[4:1]~l/2h[4:1]~r,";
    auto transferParse = SimaiParser::validateSyntax(transferText);
    for (auto& marker : transferParse.noteMarkers) {
        marker.onSlide = marker.slideHead = marker.tailOnSlideHead = marker.beforeSlide = marker.afterSlide = false;
        if (marker.type == "slide") syntheticMotion(marker, 0, 2, {{-0.5,0},{0,0},{0.5,0}});
        else if (marker.lane == 1) { marker.second = -0.5; marker.endSecond = 0.4; }
        else if (marker.lane == 5) { marker.second = 0.8; marker.endSecond = 1.3; }
        else { marker.second = 1.6; marker.endSecond = 2.4; }
    }
    const auto transferred = inferBasicHandOrder(transferText, transferParse, 0, transferText.size(), synthetic);
    expect(transferred.patches.isEmpty() && !transferred.rows.first().fullReplayVerified,
        "synthetic L-R-L geometry with a stale native key remains rejected; occupation is tested independently");
    const auto unneeded = solve("(120){4}1-5[1:4],", 0, -1, synthetic);
    expect(unneeded.patches.size() == 1 && unneeded.rows.first().handoffCount <= 1,
        "an isolated long track does not invent repeated handoffs");
    const QString sideEffectText = "(120){4}1-5[4:1]/B3,";
    auto sideEffectParse = SimaiParser::validateSyntax(sideEffectText);
    for (auto& marker : sideEffectParse.noteMarkers) {
        if (marker.type == "slide") {
            syntheticMotion(marker, 0, 2, {touchPadPoint("B1"), touchPadPoint("B3")});
            marker.slideSegmentPadEnterTimes = {{{"B3", 0.8}}};
        } else marker.second = 0.2;
    }
    const auto sideEffect = inferBasicHandOrder(sideEffectText, sideEffectParse, 0, sideEffectText.size(), synthetic);
    const auto onlyIndependentTouch = [&](const BasicResult& result) {
        bool staleSlideRejected = false, directTouchVerified = false;
        for (const auto& row : result.rows) {
            if (row.start == sideEffectText.indexOf("1-5")) staleSlideRejected = !row.fullReplayVerified && !row.hasCandidate;
            if (row.start == sideEffectText.indexOf("B3")) directTouchVerified = row.fullReplayVerified && row.hasCandidate
                && row.judgementKind == "direct_touch";
        }
        return staleSlideRejected && directTouchVerified && result.patches.size() == 1
            && result.patches.first().start == sideEffectText.indexOf("B3");
    };
    expect(onlyIndependentTouch(sideEffect),
        "stale synthetic slide geometry remains rejected while an unrelated native-queue Touch can be independently verified");
    QString hundred = "(120){16}"; for (int i = 0; i < 100; ++i) hundred += (i % 2 ? "6," : "2,");
    const auto ordinary = solve(hundred);
    expect(ordinary.patches.size() == 100 && ordinary.workUsed <= BasicParameters{}.maxWorkUnits,
        "one hundred ordinary taps retain the incremental candidate path within the total work budget");
    BasicParameters measure; measure.counterfactualBudget = 0;
    const auto measuredMain = solve(hundred, 0, -1, measure);
    BasicParameters tiny; tiny.maxWorkUnits = measuredMain.workUsed + 1000;
    const auto limited = solve(hundred, 0, -1, tiny);
    int verifiedButtons = 0;
    for (const auto& row : limited.rows)
        if (!row.context && row.status == QStringLiteral("review") && row.fullReplayVerified)
            ++verifiedButtons;
    expect(limited.patches.size() == verifiedButtons && verifiedButtons >= 90
        && limited.evidenceTruncated
        && std::none_of(limited.rows.cbegin(), limited.rows.cend(),
            [](const BasicRow& row) { return row.status == QStringLiteral("heuristic"); }),
        "a tight budget keeps verified buttons while leaving the remaining notes uncolored");
    const QString actualCover = "(120){4}1-5[2:1]~r,,C,";
    const auto actualCovered = solve(actualCover, 0, -1, synthetic);
    bool actualRightCover = false;
    for (const auto& row : actualCovered.rows) actualRightCover |= row.judgementKind == "slide_cover"
        && row.head == HandOrderHand::Right && !row.causes.isEmpty();
    expect(actualRightCover && applyPatches(actualCover, actualCovered).contains("C~r"),
        "unmodified Simai parser timing and native geometry carry C with the actual right slide");
    const QString actualPartial = "(120){4}1-5[2:1]~r,,B1/C/B3,";
    const auto actualGroup = solve(actualPartial, 0, -1, synthetic);
    int directMembers = 0;
    bool honestCarry = true;
    for (const auto& row : actualGroup.rows) {
        directMembers += row.judgementKind == "direct_touch";
        if (row.judgementKind == "group_carry") {
            int mask = 0; for (const auto& cause : row.causes) mask |= cause.handMask;
            const bool patched = std::any_of(actualGroup.patches.cbegin(), actualGroup.patches.cend(),
                [&](const auto& patch) { return patch.start == row.start; });
            honestCarry &= !patched || (mask == (row.head == HandOrderHand::Left ? 1 : 2)
                && row.fullReplayVerified && row.reason != "unstable_touch_responsibility");
        }
    }
    expect(directMembers >= 2 && honestCarry && coldFixedProof(actualPartial, actualGroup),
        "a native partial-cover group chooses cold-verifiable direct contacts without relabelling mixed or unstable carry");
    const int selectedC = actualPartial.indexOf("C/");
    const auto groupSelection = solve(actualPartial, selectedC, selectedC + 1, synthetic);
    expect(groupSelection.patches.size() == 1 && groupSelection.patches.first().start == selectedC && groupSelection.contextObjects == 3,
        "a selection cut through a Touchgroup reads the whole group and slide as context but writes only the complete selected token");
    const QString actualTransfers = "(120){4}1-5[1:1]/3h[8:3]~r,,7h[4:1]~l,,2h[2:1]~r,";
    const auto actualParsed = SimaiParser::validateSyntax(actualTransfers);
    bool actualTiming = false;
    for (const auto& marker : actualParsed.noteMarkers) if (marker.type == "slide")
        actualTiming = std::abs(marker.slideTraceSecond - 0.5) < 1e-9 && std::abs(marker.endSecond - 2.5) < 1e-9;
    const auto actualTransferred = solve(actualTransfers, 0, -1, synthetic);
    bool actualTwo = false;
    for (const auto& row : actualTransferred.rows) actualTwo |= row.handoffCount == 2 && row.status == "review";
    expect(actualTiming && actualTwo && SimaiParser::validateSyntax(applyPatches(actualTransfers, actualTransferred)).annotationErrors.isEmpty(),
        "real Simai hold timing forces a legal generated L-R-L track annotation without marker rewriting");
    auto waitingParse = sideEffectParse;
    for (auto& marker : waitingParse.noteMarkers) {
        if (marker.type == "slide") {
            marker.second = -0.5; marker.slideTraceSecond = 0.5; marker.endSecond = 2.5;
            marker.slideSegmentShootSeconds = {0.5}; marker.slideSegmentDurations = {2};
        } else marker.second = 0.0;
    }
    const auto waitingSideEffect = inferBasicHandOrder(sideEffectText, waitingParse, 0, sideEffectText.size(), synthetic);
    expect(onlyIndependentTouch(waitingSideEffect),
        "waiting-phase stale geometry never becomes verified merely because an independent Touch has a valid contact");
    const QString lockedTouchText = "(120){4}1-5[4:1]~r/C~l/5h[1:1]~l,";
    auto lockedTouchParse = SimaiParser::validateSyntax(lockedTouchText);
    for (auto& marker : lockedTouchParse.noteMarkers) {
        marker.onSlide = marker.slideHead = marker.tailOnSlideHead = marker.beforeSlide = marker.afterSlide = false;
        if (marker.type == "slide") syntheticMotion(marker, 0, 2, {{-0.8,0},{-0.8,0},{-0.5,0},{0,0},{0.5,0}});
        else if (marker.type == "touch") marker.second = 1.25;
        else { marker.second = 0; marker.endSecond = 2; }
    }
    const auto lockedTouch = inferBasicHandOrder(lockedTouchText, lockedTouchParse, 0, lockedTouchText.size(), synthetic);
    bool fakePreserved = false; for (const auto& row : lockedTouch.rows) if (row.originalAnnotation == "~l" && row.judgementKind != "direct_touch") fakePreserved |= row.status == "preserved";
    expect(lockedTouch.patches.isEmpty() && !fakePreserved,
        "a Touch locked to the occupied hand cannot silently accept opposite slide-cover responsibility");
    const QString sharedA = "1-5[4:1]~[track=0:l|0.25:r|0.75:l]", sharedB = "2-6[4:1]~[track=0:r|0.5:l]";
    const auto sharedCost = [&](const QString& chart) {
        auto parsed = SimaiParser::validateSyntax(chart);
        for (auto& marker : parsed.noteMarkers) if (marker.lane == 1) syntheticMotion(marker, 0, 2, {{-0.5,0},{0.5,0}});
            else syntheticMotion(marker, 1, 2, {{0,0},{0.5,0}});
        return inferBasicHandOrder(chart, parsed, 0, chart.size(), synthetic);
    };
    const auto sharedForward = sharedCost("(120){4}" + sharedA + '/' + sharedB + ',');
    const auto sharedReverse = sharedCost("(120){4}" + sharedB + '/' + sharedA + ',');
    QVector<PhysicalHandTransfer> physicalTransfers{{0.5, {0, 0}, 0, 1, 1}, {1.5, {0.5, 0}, 1, 0, 2},
        {1.5, {0.5, 0}, 1, 0, 1}};
    const double unionCharge = physicalHandTransferCharge(physicalTransfers);
    std::reverse(physicalTransfers.begin(), physicalTransfers.end());
    expect(std::abs(unionCharge - 3.0) < 1e-9
        && std::abs(unionCharge - physicalHandTransferCharge(physicalTransfers)) < 1e-9
        && sharedForward.patches.isEmpty() && sharedReverse.patches.isEmpty(),
        "pure shared-transfer charge uses the source-order-independent maximum marginal while stale geometry is rejected");
    const auto existingChain = solve("(120){4}1-3-7[1:1]~[track=0:l|0.2:r|0.8:l],", 0, -1, synthetic);
    expect(existingChain.rows.size() == 1 && existingChain.rows.first().status == "preserved"
        && existingChain.rows.first().handoffCount == 2 && existingChain.rows.first().trackIntervals.first().size() == 3,
        "existing chained-slide transfers retain one whole-motion history across native segments");
    const QString longWait = "(120){4}1-5[5##1],C,";
    const auto longWaitResult = solve(longWait, 0, -1, synthetic);
    expect(longWaitResult.patches.size() == 2 && longWaitResult.rows.first().fullReplayVerified
        && longWaitResult.rows.last().fullReplayVerified
        && longWaitResult.rows.last().judgementKind == "direct_touch"
        && longWaitResult.rows.first().dependencyUnit != longWaitResult.rows.last().dependencyUnit,
        "a long waiting slide and a physically independent early Touch both replay without creating a time-overlap dependency");
    const QString closure = "(120){4}1-5[5##1]~r,A7,,,,,3h[2:1]~r,";
    const int closureTouch = closure.indexOf("A7");
    const auto closureResult = solve(closure, closureTouch, closureTouch + 2, synthetic);
    bool lateHoldContext = false;
    for (const auto& row : closureResult.rows) lateHoldContext |= row.context && row.start == closure.indexOf("3h") && row.status == "context";
    expect(closureResult.patches.size() == 1 && closureResult.patches.first().start == closureTouch && lateHoldContext
        && closureResult.contextObjects == 2, "selected Touch imports later locked Hold context across the included slide's whole activity interval");
    const QString closureConflict = "(120){4}1-5[5##1]~r,A7,,,,,3h[1:2]~r,";
    const int conflictTouch = closureConflict.indexOf("A7");
    const auto closureBlocked = solve(closureConflict, conflictTouch, conflictTouch + 2, synthetic);
    int failedContext = 0; bool independentTarget = false;
    for (const auto& row : closureBlocked.rows) {
        if (row.context) failedContext += row.status == "not_found" && !row.hasCandidate && !row.fullReplayVerified && row.pressureLevel.isEmpty();
        else independentTarget |= row.fullReplayVerified && row.hasCandidate && row.judgementKind == "direct_touch";
    }
    expect(closureBlocked.patches.size() == 1 && closureBlocked.patches.first().start == conflictTouch
        && independentTarget && failedContext == 2 && closureBlocked.contextObjects == 2,
        "an independent early Touch stays writable while the conflicting future slide and Hold remain visible failed read-only contexts");
    const auto& transferRow = actualTransferred.rows.first();
    expect(transferRow.handoffEvidence.size() == 1 && transferRow.handoffEvidence.first().branch == 0
        && transferRow.handoffEvidence.first().handoffCount == transferRow.handoffCount
        && transferRow.handoffEvidence.first().reason == transferRow.handoffReason
        && !transferRow.handoffEvidence.first().baselineFound,
        "selected trajectory retains its own per-branch admission proof and explicit missing baseline");
    const QString proofBranchesText = "(120){4}1-5[4:1]*-5[4:1]~[branch0=0:l|0.5:r;branch1=0:l|0.5:r],";
    auto proofBranchesParse = SimaiParser::validateSyntax(proofBranchesText);
    const auto proofBranches = inferBasicHandOrder(proofBranchesText, proofBranchesParse, 0, proofBranchesText.size(), synthetic);
    expect(proofBranches.rows.size() == 1 && proofBranches.rows.first().handoffEvidence.size() == 2
        && proofBranches.rows.first().handoffEvidence.at(0).branch == 0 && proofBranches.rows.first().handoffEvidence.at(1).branch == 1
        && proofBranches.rows.first().handoffEvidence.at(0).handoffCount == 1 && proofBranches.rows.first().handoffEvidence.at(1).handoffCount == 1,
        "every branch carries separate final-state handoff evidence even when it shares the physical transfer");
    const QString counterTransferText = "(120){8}1-5[1:1]/3h[8:3]~r,,,,7h[4:1]~l,,,6,2h[2:1]~r,";
    BasicParameters counterParameters; counterParameters.beamWidth = 128;
    const auto counterTransfer = solve(counterTransferText, 0, -1, counterParameters);
    bool admittedReverseHand = false;
    for (const auto& row : counterTransfer.rows) if (row.lanes.contains(6))
        for (const auto& proof : row.counterfactualEvidence) admittedReverseHand |= proof.searched
            && proof.alternativeHand != HandOrderHand::Unspecified && proof.alternativeHand != row.head
            && (!proof.found || proof.signedCostGap >= -1e-8);
    expect(admittedReverseHand && counterTransfer.rows.first().fullReplayVerified,
        "native timing retains the forced opposite hand through full-block transfer admission and its signed evidence");
    const QString unstableCarryText = "(120){4}B8/C/B1,";
    const auto unstableCarry = solve(unstableCarryText, 0, -1, synthetic);
    bool directAlternative = unstableCarry.rows.size() == 3;
    for (const auto& row : unstableCarry.rows) {
        int mask = 0; for (const auto& cause : row.causes) mask |= cause.handMask;
        directAlternative &= row.judgementKind == "direct_touch" && row.fullReplayVerified
            && row.judgementQuality == "critical" && mask == (row.head == HandOrderHand::Left ? 1 : 2);
    }
    expect(directAlternative && coldFixedProof(unstableCarryText, unstableCarry),
        "symmetric finite contacts choose real single-hand direct alternatives instead of encoding unstable carry");
    const QString native = "(120){4}1-5[4:1],,,,,B1/B2/B3,6,";
    const auto nativeResult = solve(native);
    const auto annotated = applyPatches(native, nativeResult);
    const auto nativeParsed = SimaiParser::parseForTimeline(native), annotatedParsed = SimaiParser::parseForTimeline(annotated);
    expect(!nativeResult.patches.isEmpty() && stripAnnotations(annotated) == native
        && annotatedParsed.annotationErrors.isEmpty() && nativeParsed.noteMarkers.size() == annotatedParsed.noteMarkers.size(),
        "automatic interval and Touch patches keep the exact native projection and pass strict annotation grammar");
    QVector<miacode::preview_sfx_timeline::Event> beforeSfx, afterSfx;
    QVector<miacode::preview_sfx_timeline::TouchholdSpan> beforeSpans, afterSpans;
    miacode::preview_sfx_timeline::buildTimeline(nativeParsed.noteMarkers, 1, {}, &beforeSfx, &beforeSpans);
    miacode::preview_sfx_timeline::buildTimeline(annotatedParsed.noteMarkers, 1, {}, &afterSfx, &afterSpans);
    bool sameSfx = beforeSfx.size() == afterSfx.size();
    for (int i = 0; sameSfx && i < beforeSfx.size(); ++i) sameSfx &= beforeSfx.at(i).second == afterSfx.at(i).second
        && beforeSfx.at(i).kind == afterSfx.at(i).kind && beforeSfx.at(i).gain == afterSfx.at(i).gain;
    expect(sameSfx, "automatic annotations leave native sound events unchanged");
    const auto beforeMuri = MuriAnalyzer::analyze(nativeParsed.noteMarkers), afterMuri = MuriAnalyzer::analyze(annotatedParsed.noteMarkers);
    bool sameMuri = beforeMuri.diagnostics.size() == afterMuri.diagnostics.size()
        && beforeMuri.judgeSpriteEvents.size() == afterMuri.judgeSpriteEvents.size();
    for (int i = 0; sameMuri && i < beforeMuri.diagnostics.size(); ++i) sameMuri &= beforeMuri.diagnostics.at(i).kind == afterMuri.diagnostics.at(i).kind
        && beforeMuri.diagnostics.at(i).second == afterMuri.diagnostics.at(i).second;
    for (int i = 0; sameMuri && i < beforeMuri.judgeSpriteEvents.size(); ++i) sameMuri &= beforeMuri.judgeSpriteEvents.at(i).kind == afterMuri.judgeSpriteEvents.at(i).kind
        && beforeMuri.judgeSpriteEvents.at(i).second == afterMuri.judgeSpriteEvents.at(i).second;
    expect(sameMuri, "the independent hand-order replay does not change original Muri diagnostics or judgement timing");
    if (!ok) {
        const auto dump = [&](const char* name, const BasicResult& result) {
            QTextStream out(stderr); out << "DETAIL " << name << " patches=" << result.patches.size() << " work=" << result.workUsed << '\n';
            for (const auto& row : result.rows.mid(0, 6)) out << "  " << row.line << ':' << row.col << ' ' << row.status << ' ' << row.reason
                << " channel=" << row.inputChannel << " kind=" << row.judgementKind << " full=" << row.fullReplayVerified << " confidence=" << row.confidence
                << " hand=" << int(row.head) << " causes=" << row.causes.size() << " transfer=" << row.handoffCount << '\n';
        };
        dump("native projection", nativeResult); dump("held exterior", buttons); dump("three presses", triple); dump("Wifi", wifi); dump("mixed group", group);
        dump("small budget", limited); dump("actual transfers", actualTransferred); dump("spanning", spanning); dump("long wait", longWaitResult); dump("synthetic inherited", inherited);
        dump("synthetic shared", sharedForward); dump("synthetic counter transfer", counterTransfer); dump("unstable carry", unstableCarry);
        dump("stale geometry independent Touch", sideEffect); dump("waiting stale geometry", waitingSideEffect); dump("closure conflict", closureBlocked);
    }
    return ok ? 0 : 1;
}
