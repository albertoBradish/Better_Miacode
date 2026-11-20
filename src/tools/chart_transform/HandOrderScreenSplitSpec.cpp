#include "core/chart/transform/HandOrderScreenSplit.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/ChartBatchTransform.h"

#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::chart_transform;
    namespace hand_order = miacode::hand_order;
    bool ok = true;
    const auto expect = [&](bool pass, const char* name) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << name << '\n';
        ok &= pass;
    };
    const QString source = "(120){4}1p4[8:1]/8q5[8:1],";
    const auto split = screenSplitHandOrderInSelection(source, 0, source.size());
    const auto before = SimaiParser::parseForTimeline(source);
    const auto after = SimaiParser::parseForTimeline(split.replacement);
    expect(split.diagnostics.isEmpty() && split.changed == 2 && after.ok && after.annotationErrors.isEmpty()
        && hand_order::stripAnnotations(split.replacement) == source,
        "screen split uses the A8/A1 starts' D1 axis and changes only annotations");
    bool geometryPreserved = after.noteMarkers.size() == before.noteMarkers.size();
    bool crossingsMatch = geometryPreserved;
    for (int i = 0; geometryPreserved && i < after.noteMarkers.size(); ++i) {
        const auto& original = before.noteMarkers.at(i);
        const auto& marker = after.noteMarkers.at(i);
        geometryPreserved &= marker.slideSegmentPoints == original.slideSegmentPoints
            && marker.slideSegmentShootSeconds == original.slideSegmentShootSeconds
            && marker.slideSegmentDurations == original.slideSegmentDurations
            && marker.second == original.second && marker.endSecond == original.endSecond;
        const auto& stops = marker.handOrderTrackStops;
        const auto firstHand = marker.lane == 1 ? HandOrderHand::Right : HandOrderHand::Left;
        const auto otherHand = marker.lane == 1 ? HandOrderHand::Left : HandOrderHand::Right;
        crossingsMatch &= stops.size() == 3 && marker.handOrderHead == firstHand;
        if (stops.size() == 3)
            crossingsMatch &= stops.at(0).hand == firstHand && stops.at(1).hand == otherHand
                && stops.at(2).hand == firstHand
                && std::abs(stops.at(1).progress - 0.22099771638446827) < 1e-9
                && std::abs(stops.at(2).progress - 0.7790022836155317) < 1e-9;
    }
    expect(geometryPreserved && crossingsMatch,
        "real native curve crossings determine both progress values, head hands and track sides");
    const auto repeat = screenSplitHandOrderInSelection(split.replacement, 0, split.replacement.size());
    expect(repeat.diagnostics.isEmpty() && repeat.changed == 0 && repeat.replacement == split.replacement,
        "geometric screen split is deterministic when rerun");
    const QString rotated = transformChartText(source, ChartTransformOp::Rotate45Clockwise);
    const auto rotatedSplit = screenSplitHandOrderInSelection(rotated, 0, rotated.size());
    const auto rotatedMarkers = SimaiParser::parseForTimeline(rotatedSplit.replacement).noteMarkers;
    bool rotatedCrossings = rotatedSplit.diagnostics.isEmpty() && rotatedMarkers.size() == 2;
    for (const auto& marker : rotatedMarkers) {
        rotatedCrossings &= marker.handOrderTrackStops.size() == 3;
        if (marker.handOrderTrackStops.size() == 3)
            rotatedCrossings &= std::abs(marker.handOrderTrackStops.at(1).progress - 0.22099771638446827) < 1e-5
                && std::abs(marker.handOrderTrackStops.at(2).progress - 0.7790022836155317) < 1e-5;
    }
    expect(rotatedCrossings, "a rotated start pair derives a rotated common axis with the same crossing proportions");
    const QString asymmetricTracks = "(120){4}2pp4[8:1]/6qq4[8:1],";
    const auto allowed = screenSplitHandOrderInSelection(asymmetricTracks, 0, asymmetricTracks.size());
    expect(allowed.diagnostics.isEmpty() && allowed.changed == 2
        && hand_order::stripAnnotations(allowed.replacement) == asymmetricTracks,
        "symmetric starts permit different paths and coinciding endpoints");
    for (const QString value : {QStringLiteral("1-5[4:1],"),
        QStringLiteral("1-5[4:1]/2-6[4:1]/4-8[4:1],"),
        QStringLiteral("1-5[4:1]/3-7[4:1]/5-1[4:1]/7-3[4:1],")}) {
        const auto rejected = screenSplitHandOrderInSelection(value, 0, value.size());
        expect(rejected.changed == 0 && rejected.replacement == value && !rejected.diagnostics.isEmpty(),
            "single, non-symmetric or ambiguous start sets are rejected without guessing an axis");
    }
    for (const QString singleHead : {QStringLiteral("(120){4}1-3[4:1]*-7[4:1],"),
        QStringLiteral("(120){4}1-3[4:1]/1-7[4:1],")}) {
        const auto plan = screenSplitHandOrderInSelection(singleHead,0,singleHead.size());
        if (!plan.diagnostics.isEmpty()) for (const auto& diagnostic : plan.diagnostics)
            QTextStream(stderr) << diagnostic.message << '\n';
        const auto notes = SimaiParser::parseForTimeline(plan.replacement).noteMarkers;
        expect(plan.diagnostics.isEmpty() && plan.changed > 0 && notes.size() == 2
            && !notes[0].handOrderTrackStops.isEmpty() && !notes[1].handOrderTrackStops.isEmpty()
            && notes[0].handOrderTrackStops[0].hand != notes[1].handOrderTrackStops[0].hand
            && notes[0].handOrderHead == HandOrderHand::Right
            && hand_order::stripAnnotations(plan.replacement) == singleHead,
            "single-head Each branches split around their own radial axis and retain one concrete head");
    }
    const QString chain = "(120){4}1-3-7[1:1]/8-6-2[1:1],";
    const auto chainSplit = screenSplitHandOrderInSelection(chain, 0, chain.size());
    expect(chainSplit.diagnostics.isEmpty() && chainSplit.changed == 2
        && hand_order::stripAnnotations(chainSplit.replacement) == chain,
        "chained Slides use the parser's actual segment timing without changing native structure");
    return ok ? 0 : 1;
}
