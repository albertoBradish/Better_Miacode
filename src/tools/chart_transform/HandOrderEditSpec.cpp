#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/ChartBatchTransform.h"
#include "core/chart/transform/ChartNormalization.h"
#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

namespace {
bool sameNative(const QString& a, const QString& b)
{
    const auto x = SimaiParser::parseForTimeline(a);
    const auto y = SimaiParser::parseForTimeline(b);
    if (!x.ok || !y.ok || x.noteMarkers.size() != y.noteMarkers.size()
        || x.durationSeconds != y.durationSeconds) return false;
    for (int i = 0; i < x.noteMarkers.size(); ++i) {
        const auto& p = x.noteMarkers.at(i);
        const auto& q = y.noteMarkers.at(i);
        if (p.type != q.type || p.lane != q.lane || p.endLane != q.endLane
            || p.second != q.second || p.endSecond != q.endSecond || p.slideTraceSecond != q.slideTraceSecond
            || p.slideSegmentKeys != q.slideSegmentKeys || p.slideSegmentDurations != q.slideSegmentDurations
            || p.slideSegmentPoints != q.slideSegmentPoints || p.wifiLanePoints != q.wifiLanePoints
            || p.wifiTrackAreaPoints != q.wifiTrackAreaPoints || p.wifiTrackAreaRotations != q.wifiTrackAreaRotations
            || p.wifiTrackAreaImageIndices != q.wifiTrackAreaImageIndices
            || p.isEach != q.isEach || p.isBreak != q.isBreak || p.isEx != q.isEx
            || p.isFirework != q.isFirework || p.isMine != q.isMine || p.headMine != q.headMine
            || p.trackMine != q.trackMine || p.hasHeadStar != q.hasHeadStar
            || p.headlessImmediate != q.headlessImmediate || p.hsMultiplier != q.hsMultiplier) return false;
    }
    return true;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::chart_transform;
    bool ok = true;
    const auto expect = [&](bool pass, const char* name) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << name << '\n';
        ok &= pass;
    };
    const QString source = QStringLiteral("(120){4}12/3bx/4m/A1/Ch[4:1]/1-5[4:1]*-7[4:1]~r,\r\n|| 中文 😀~r 56\r\nE");
    const auto left = setHandOrderInSelection(source, 0, source.size(), HandOrderHand::Left);
    expect(left.diagnostics.isEmpty() && left.replacement.contains("1~l/2~l/3bx~l/4m~l")
        && left.replacement.endsWith(QStringLiteral("|| 中文 😀~r 56\r\nE")), "clusters expand and modifiers, comments, CRLF remain intact");
    expect(sameNative(source, left.replacement), "batch annotation preserves native timing, flags, geometry and Each");
    expect(setHandOrderInSelection(left.replacement, 0, left.replacement.size(), HandOrderHand::Left).changed == 0,
        "repeating an identical batch assignment is inert");
    const QString partial = "1-5[4:1]~l/2,";
    const auto cropped = setHandOrderInSelection(partial, 2, partial.size() - 1, HandOrderHand::Either);
    expect(cropped.partial == 1 && cropped.replacement == "5[4:1]~l/2~m", "partial slide containers are skipped as one token");
    const QString unicode = QStringLiteral("|| 中文 😀\r\n1~l`2,\r\n");
    const int begin = unicode.indexOf("1~l");
    const auto utf16 = setHandOrderInSelection(unicode, begin, begin + 5, HandOrderHand::Right);
    expect(utf16.replacement == "1~r`2~r", "UTF-16 locations and backtick Each boundaries are respected");
    const QString invalid = "1~z/2,";
    const auto rejected = setHandOrderInSelection(invalid, 0, invalid.size(), HandOrderHand::Left);
    expect(rejected.replacement == "1~z/2~l," && rejected.diagnostics.size() == 1
        && rejected.diagnostics.first().line == 1 && rejected.diagnostics.first().col == 2,
        "invalid annotations are preserved with precise diagnostics while valid tokens can change");
    expect(handOrderSplitProgresses({1.0, 3.0}) == QVector<double>{0.25},
        "one-second plus three-second actual movement splits at progress 0.25");
    const QString chain = "(120){4}1-3-7[1:1]~l,";
    const auto split = splitHandOrderInSelection(chain, 0, chain.size());
    const auto nativeChain = SimaiParser::parseForTimeline(chain);
    const auto splitChain = SimaiParser::parseForTimeline(split.replacement);
    const auto& nativeMarker = nativeChain.noteMarkers.first();
    const double actualBoundary = (nativeMarker.slideSegmentShootSeconds.at(1) - nativeMarker.slideTraceSecond)
        / (nativeMarker.endSecond - nativeMarker.slideTraceSecond);
    expect(split.diagnostics.isEmpty() && std::abs(actualBoundary - 0.5) > 0.01
        && splitChain.noteMarkers.first().handOrderTrackStops.size() == 2
        && std::abs(splitChain.noteMarkers.first().handOrderTrackStops.at(1).progress - actualBoundary) < 1e-12,
        "unequal native shapes integrate the parser's actual duration boundary instead of equal halves");
    const QString sorted = "(120){4}8/1-5[2:1]-1[2:3]~l,";
    const auto sourceOrder = splitHandOrderInSelection(sorted, 0, sorted.size());
    expect(sourceOrder.diagnostics.isEmpty() && sourceOrder.intervals.size() == 1
        && sourceOrder.intervals.first().baseToken == "1-5[2:1]-1[2:3]",
        "token identity follows parse order after production markers are sorted by lane");
    expect(miacode::hand_order::stripAnnotations(chain) == miacode::hand_order::stripAnnotations(split.replacement)
        && sameNative(chain, split.replacement), "split changes only annotations and preserves native semantics");
    expect(splitHandOrderInSelection(split.replacement, 0, split.replacement.size()).changed == 0,
        "same-color nodes survive and repeated split adds no nodes");
    const QString branches = "1-5[4:1]*-7[4:1]~[branch0=0:l;branch1=0:r],";
    const auto branchSplit = splitHandOrderInSelection(branches, 0, branches.size());
    expect(branchSplit.diagnostics.isEmpty() && branchSplit.replacement.contains("branch0=0:l|0.5:l;branch1=0:r|0.5:r")
        && !branchSplit.replacement.contains("head="), "every source branch splits and an omitted head stays omitted");
    const QString precise = "1-5[4:1]~[track=0:l|0.00000000000000000001:r|0.9999999999999999:l],";
    const auto preciseSplit = splitHandOrderInSelection(precise, 0, precise.size());
    const auto parsed = SimaiParser::validateSyntax(preciseSplit.replacement);
    expect(preciseSplit.diagnostics.isEmpty() && parsed.annotationErrors.isEmpty()
        && !parsed.noteMarkers.isEmpty() && parsed.noteMarkers.first().handOrderTrackStops.size() == 4,
        "tiny positive and near-one existing nodes round-trip without becoming zero or one");
    const QString unknown = "1-5[4:1]*-7[4:1],";
    const auto temporary = splitHandOrderInSelection(unknown, 0, unknown.size());
    expect(temporary.replacement == unknown && temporary.changed == 0 && temporary.intervals.size() == 1
        && temporary.intervals.first().tracks.size() == 2 && !handOrderTokenComplete(temporary.intervals.first()),
        "unannotated branches create temporary intervals without inventing Either");
    const QString badTiming = "(wrong){4}1-5[2:1]-1[2:3]~l,";
    const auto bad = splitHandOrderInSelection(badTiming, 0, badTiming.size());
    expect(bad.replacement == badTiming && bad.intervals.isEmpty() && !bad.diagnostics.isEmpty(),
        "invalid timing directives block split and produce diagnostics");
    expect(setHandOrderInSelection(source, 5, 5, HandOrderHand::Left).changed == 0,
        "empty selection has no edit");
    const QString halvesSource = "(120){4}1-3-7[1:1]~r/8-4[4:1],";
    expect(bisectHandOrderInSelection(halvesSource, 5, 5).changed == 0
        && swapHandOrderInSelection(halvesSource, 5, 5).changed == 0,
        "direct bisection and hand swap are inert at a caret without selection");
    const auto halves = bisectHandOrderInSelection(halvesSource, 0, halvesSource.size());
    expect(halves.diagnostics.isEmpty() && halves.changed == 2
        && halves.replacement.contains("head=r;track=0:l|0.5:r")
        && halves.replacement.contains("8-4[4:1]~[track=0:l|0.5:r]")
        && sameNative(halvesSource, halves.replacement),
        "one-key bisection assigns L/R at half of total movement and retains existing head");
    const auto quarters = bisectHandOrderInSelection(halves.replacement, 0, halves.replacement.size());
    expect(quarters.diagnostics.isEmpty() && quarters.changed == 2
        && quarters.replacement.contains("head=r;track=0:l|0.25:l|0.5:r|0.75:r"),
        "repeated bisection inserts inherited same-color nodes without changing default hands");
    const auto eighths = bisectHandOrderInSelection(quarters.replacement, 0, quarters.replacement.size());
    expect(SimaiParser::parseForTimeline(eighths.replacement).noteMarkers.first().handOrderTrackStops.size() == 8,
        "subsequent bisection keeps every same-color editing boundary");
    const QString customIntervals = "1-5[4:1]~[head=m;track=0:r|0.3:m|0.8:l],";
    const auto customHalves = bisectHandOrderInSelection(customIntervals, 0, customIntervals.size());
    const auto customMarker = SimaiParser::parseForTimeline(customHalves.replacement).noteMarkers.first();
    expect(customMarker.handOrderHead == HandOrderHand::Either && customMarker.handOrderTrackStops.size() == 6
        && customMarker.handOrderTrackStops.at(1).hand == HandOrderHand::Right
        && customMarker.handOrderTrackStops.at(3).hand == HandOrderHand::Either
        && std::abs(customMarker.handOrderTrackStops.at(3).progress - 0.55) < 1e-12,
        "existing unequal intervals are individually bisected while preserving their hands");
    const QString swapSource = "(120){4}12/3~m/4~l/1-5[4:1]~[head=r;track=0:l|0.123456789012345:r]"
        "/8w4[4:1]~[track=split:0:l|0.5:m&0:r],\r\n|| ~l 中文 😀\r\nE";
    const auto swapped = swapHandOrderInSelection(swapSource, 0, swapSource.size());
    expect(swapped.diagnostics.isEmpty() && swapped.replacement.contains("12/3~m/4~r")
        && swapped.replacement.contains("head=l;track=0:r|0.123456789012345:l")
        && swapped.replacement.contains("track=split:0:r|0.5:m&0:l")
        && sameNative(swapSource, swapped.replacement)
        && swapHandOrderInSelection(swapped.replacement, 0, swapped.replacement.size()).replacement == swapSource,
        "selection hand swap is an exact involution across heads, branches, Wifi, clusters and comments");
    const auto swapPartial = swapHandOrderInSelection(partial, 2, partial.size() - 1);
    expect(swapPartial.partial == 1 && swapPartial.changed == 0 && swapPartial.replacement == "5[4:1]~l/2",
        "hand swap leaves incomplete tokens and unannotated notes untouched");
    const auto bisectPartial = bisectHandOrderInSelection(partial, 2, partial.size() - 1);
    expect(bisectPartial.partial == 1 && bisectPartial.changed == 0 && bisectPartial.unsupported == 1,
        "direct bisection skips a partially selected Slide and does not annotate other note types");
    const QString bisectWifi = "1w5[4:1]~[track=single:0:l],";
    expect(bisectHandOrderInSelection(bisectWifi, 0, bisectWifi.size()).unsupported == 1
        && bisectHandOrderInSelection(bisectWifi, 0, bisectWifi.size()).replacement == bisectWifi,
        "one-key Slide bisection leaves explicit Wifi modes intact");
    const QString wifiSplit = "(120){4}1w5[4:1]~[head=m;track=split:0:l|0.5:r&0:r|0.75:l],";
    const auto wifi = SimaiParser::validateSyntax(wifiSplit);
    expect(wifi.annotationErrors.isEmpty() && wifi.noteMarkers.size() == 1
        && wifi.noteMarkers.first().handOrderWifiMode == HandOrderWifiMode::Split
        && wifi.noteMarkers.first().handOrderTrackStops.isEmpty()
        && wifi.noteMarkers.first().handOrderWifiSectorStops.size() == 2,
        "Wifi stores exactly two native sectors separately from legacy tracks");
    expect(sameNative(wifiSplit, miacode::hand_order::stripAnnotations(wifiSplit)),
        "Wifi sectors preserve native geometry, time, modifiers and counts");
    const auto wifiAssigned = setHandOrderInSelection(wifiSplit, 0, wifiSplit.size(), HandOrderHand::Either);
    expect(wifiAssigned.replacement.contains("track=split:0:m|0.5:m&0:m|0.75:m")
        && sameNative(wifiSplit, wifiAssigned.replacement),
        "batch assignment preserves explicit Wifi split mode and editing boundaries");
    const QString wifiSingle = "1w5[4:1]~[track=single:0:l|0.5:l],";
    expect(setHandOrderInSelection(wifiSingle, 0, wifiSingle.size(), HandOrderHand::Right)
        .replacement.contains("track=single:0:r|0.5:r"),
        "batch assignment retains explicit single mode and same-hand nodes");
    const auto wifiDraft = splitHandOrderInSelection(wifiSplit, 0, wifiSplit.size());
    expect(wifiDraft.diagnostics.isEmpty() && wifiDraft.intervals.size() == 1
        && wifiDraft.intervals.first().wifiBranches == QVector<bool>{true}
        && handOrderSuffix(wifiDraft.intervals.first()).contains("track=split:")
        && !wifiDraft.replacement.contains("track=0:"), "split editing retains Wifi sector identity");
    const auto mirrored = transformChartText(wifiSplit, ChartTransformOp::MirrorLeftRight);
    expect(mirrored.contains("track=split:0:r|0.75:l&0:l|0.5:r")
        && transformChartText(mirrored, ChartTransformOp::MirrorLeftRight) == wifiSplit,
        "reflection exchanges native Wifi sectors with exact involution and preserves hand names");
    expect(transformChartText(wifiSplit, ChartTransformOp::Rotate45Clockwise)
        .contains("track=split:0:l|0.5:r&0:r|0.75:l"), "rotation preserves local sector indexing");
    expect(transformChartText(wifiSplit, ChartTransformOp::MirrorUpDown, nullptr, HandOrderMirrorPolicy::SwapLeftRight)
        .contains("track=split:0:l|0.75:r&0:r|0.5:l"), "explicit swap reflects sectors and swaps only L/R");
    const auto normalizedWifi = normalizeChartText(wifiSplit);
    expect(normalizedWifi.ok && normalizedWifi.text.contains("track=split:0:l|0.5:r&0:r|0.75:l")
        && sameNative(wifiSplit, normalizedWifi.text), "formatter preserves Wifi mode and branch payload");
    const QString mixedBranches = "1w5[4:1]*-7[4:1]~[branch0=split:0:l&0:r;branch1=0:m],";
    expect(SimaiParser::validateSyntax(mixedBranches).annotationErrors.isEmpty()
        && splitHandOrderInSelection(mixedBranches, 0, mixedBranches.size()).replacement
            .contains("branch0=split:0:l|0.5:l&0:r|0.5:r;branch1=0:m|0.5:m"),
        "mixed native branches retain stable branch and Wifi sector numbers");
    for (const auto& value : {QStringLiteral("single:0:l|0.5:r"), QStringLiteral("single:0:m|0.5:l"),
        QStringLiteral("split:0:l"), QStringLiteral("split:0:l&0:r&0:m"), QStringLiteral("split:0:l&"),
        QStringLiteral("split:0.1:l&0:r"), QStringLiteral("split:0:l&0:r|0.5:l|0.5:r"),
        QStringLiteral("split:0:l&0:r|1:l"), QStringLiteral("unknown:0:l")}) {
        const auto invalidWifi = SimaiParser::parseForTimeline("1w5[4:1]~[track=" + value + "],2~l,");
        expect(invalidWifi.ok && invalidWifi.annotationErrors.size() == 1 && invalidWifi.noteMarkers.size() == 2
            && invalidWifi.noteMarkers.first().handOrderWifiMode == HandOrderWifiMode::Legacy
            && invalidWifi.noteMarkers.first().handOrderTrackStops.isEmpty(),
            "invalid Wifi modes keep native notes and attach no partial hand annotation");
    }
    expect(SimaiParser::validateSyntax("1w5[4:1]~[track=single:0:m|0.5:m],").annotationErrors.isEmpty(),
        "single Either remains an explicit choice of one hand for the whole Wifi");
    for (const auto& text : {QStringLiteral("1-5[4:1]~[track=single:0:l],"),
        QStringLiteral("1-5[4:1]~[track=split:0:l&0:r],"),
        QStringLiteral("1w5[4:1]*-7[4:1]~[head=l;branch0=split:0:l&0:r;branch1=single:0:l],")}) {
        const auto invalidMode = SimaiParser::parseForTimeline(text);
        bool noPartial = true;
        for (const auto& marker : invalidMode.noteMarkers)
            noPartial &= marker.handOrderHead == HandOrderHand::Unspecified && marker.handOrderTrackStops.isEmpty()
                && marker.handOrderWifiSectorStops.isEmpty();
        expect(invalidMode.annotationErrors.size() == 1 && noPartial,
            "Wifi-only mode rejects an entire invalid annotation group atomically");
    }
    return ok ? 0 : 1;
}
