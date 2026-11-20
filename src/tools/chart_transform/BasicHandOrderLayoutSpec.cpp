#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <QCoreApplication>
#include <QTextStream>
#include <limits>

namespace {
bool sameNative(const QString& a, const QString& b)
{
    const auto first = SimaiParser::parseForTimeline(a);
    const auto second = SimaiParser::parseForTimeline(b);
    if (!first.ok || !second.ok || first.noteMarkers.size() != second.noteMarkers.size()
        || first.durationSeconds != second.durationSeconds) return false;
    for (int index = 0; index < first.noteMarkers.size(); ++index) {
        const auto& x = first.noteMarkers.at(index);
        const auto& y = second.noteMarkers.at(index);
        if (x.type != y.type || x.lane != y.lane || x.endLane != y.endLane
            || x.tempoBpm != y.tempoBpm || x.subdivision != y.subdivision
            || x.second != y.second || x.endSecond != y.endSecond || x.slideTraceSecond != y.slideTraceSecond
            || x.slideSegmentDurations != y.slideSegmentDurations || x.slideSegmentKeys != y.slideSegmentKeys
            || x.slideSegmentPoints != y.slideSegmentPoints || x.wifiLanePoints != y.wifiLanePoints
            || x.wifiTrackAreaPoints != y.wifiTrackAreaPoints || x.touchPoint != y.touchPoint
            || x.isEach != y.isEach || x.eachGroupId != y.eachGroupId || x.isBreak != y.isBreak
            || x.isEx != y.isEx || x.isMine != y.isMine || x.headMine != y.headMine || x.trackMine != y.trackMine
            || x.headBreak != y.headBreak || x.trackBreak != y.trackBreak || x.headEx != y.headEx
            || x.hasHeadStar != y.hasHeadStar || x.headlessImmediate != y.headlessImmediate
            || x.hsMultiplier != y.hsMultiplier || x.onSlide != y.onSlide) return false;
    }
    return true;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe-times")) {
        const auto parsed = SimaiParser::parseForTimeline(QTextStream(stdin).readAll());
        for (const auto& marker : parsed.noteMarkers)
            QTextStream(stdout) << marker.parseOrder << ' ' << marker.type << ' '
                << marker.lane << " head=" << marker.second
                << " trace=" << marker.slideTraceSecond << " end=" << marker.endSecond
                << " grid=" << marker.subdivision << " bpm=" << marker.tempoBpm << " isEach=" << marker.isEach << " star=" << marker.hasHeadStar
                << " each=" << marker.eachGroupId << '\n';
        return parsed.ok ? 0 : 1;
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe-links")) {
        const auto plan = miacode::chart_transform::basicHandOrderLayoutInSelection(
            QTextStream(stdin).readAll(), 0, 0);
        for (const auto& link : plan.sweepLinks)
            QTextStream(stdout) << "sweep " << link.first << " -> " << link.second << '\n';
        for (const auto& link : plan.interactionLinks)
            QTextStream(stdout) << "interaction " << link.first << " -> " << link.second << '\n';
        for (int order : plan.keySweepSplitTracks)
            QTextStream(stdout) << "head_track_split " << order << '\n';
        for (const auto& link : plan.reboundLinks)
            QTextStream(stdout) << "rebound " << link.first << " -> " << link.second << '\n';
        for (const auto& link : plan.alternatingSweepLinks)
            QTextStream(stdout) << "alternating_sweep " << link.first << " -> " << link.second << '\n';
        return 0;
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--probe")) {
        const QString chart = QTextStream(stdin).readAll();
        QTextStream(stdout) << miacode::chart_transform::basicHandOrderLayoutInSelection(chart, 0, 0).edit.replacement;
        return 0;
    }
    using namespace miacode::chart_transform;
    bool ok = true;
    const auto expect = [&](bool pass, const char* label) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << label << '\n';
        ok &= pass;
    };
    const QString buttons = "(120){4}1,2,3,4,5,6,7,8,1h[4:1],5bh[4:1],2xm,6m,";
    const auto base = basicHandOrderLayoutInSelection(buttons, 5, 5);
    expect(base.edit.diagnostics.isEmpty() && base.edit.replacement.contains("1~r,2~r,3~r,4~r,5~l,6~l,7~l,8~l")
        && base.edit.replacement.contains("1h[4:1]~r") && base.edit.replacement.contains("5bh[4:1]~l"),
        "empty selection lays out the whole difficulty, right 1-4 and left 5-8, including holds");
    expect(sameNative(buttons, base.edit.replacement), "basic assignments preserve original geometry, timing, Each and modifiers");
    expect(basicHandOrderLayoutInSelection(base.edit.replacement, 0, 0).edit.changed == 0,
        "repeating basic layout preserves every generated annotation without an empty undo edit");

    const QString each = "(120){8}4/5,6/5,7/6,4/5,";
    const auto laidEach = basicHandOrderLayoutInSelection(each, 0, 0);
    expect(laidEach.edit.replacement == "(120){8}4~r/5~l,6~l/5~r,7~l/6~r,4~r/5~l,"
        && sameNative(each, laidEach.edit.replacement),
        "each pairs use different hands even when both notes occupy one half-screen");
    const QString pseudoEach = "(120){128}4,3,";
    const auto laidPseudoEach = basicHandOrderLayoutInSelection(pseudoEach, 0, 0);
    expect(laidPseudoEach.edit.replacement == "(120){128}4~l,3~r,"
        && basicHandOrderLayoutInSelection("(120){64}4,3,", 0, 0).edit.replacement == "(120){64}4~r,3~r,",
        "heads less than one actual 1/64 note apart form a pseudo-each, excluding the exact boundary");
    const QString impossible = "(120){4}123,";
    const auto rejected = basicHandOrderLayoutInSelection(impossible, 0, 0);
    expect(rejected.edit.unsupported == 1 && rejected.edit.replacement == impossible,
        "three simultaneous independent heads are left unassigned instead of putting two on one hand");

    const QString occupied = "(120){16}2p6[8:1],,,6,,,{8}5,{16}4q8[8:1],,,3,,,{8}2,";
    const auto laidOccupied = basicHandOrderLayoutInSelection(occupied, 0, 0);
    expect(laidOccupied.edit.replacement.contains("2p6[8:1]~[head=r;track=0:r]")
        && laidOccupied.edit.replacement.contains("6~l") && laidOccupied.edit.replacement.contains("5~l")
        && laidOccupied.edit.replacement.contains("4q8[8:1]~[head=l;track=0:l]")
        && laidOccupied.edit.replacement.contains("3~r") && laidOccupied.edit.replacement.contains("2~r")
        && sameNative(occupied, laidOccupied.edit.replacement),
        "slide occupancy and future neighbours select the head and whole track without an implausible crossing");
    const QString fixedSlide = "(120){16}2p6[8:1]~[head=r;track=0:r],,,6,,,{8}5,";
    const auto aroundFixed = basicHandOrderLayoutInSelection(fixedSlide, 0, 0);
    expect(aroundFixed.preserved == 1 && aroundFixed.edit.replacement.contains("2p6[8:1]~[head=r;track=0:r]")
        && aroundFixed.edit.replacement.contains("6~l") && aroundFixed.edit.replacement.contains("5~l"),
        "a protected slide track still reserves its hand for unannotated neighbours");
    const QString held = "(120){16}2h[2:1],,,3,,4,,1,,2,";
    const auto laidHeld = basicHandOrderLayoutInSelection(held, 0, 0);
    expect(laidHeld.edit.replacement.contains("2h[2:1]~r")
        && laidHeld.edit.replacement.contains("3~l") && laidHeld.edit.replacement.contains("4~l")
        && laidHeld.edit.replacement.contains("1~l") && sameNative(held, laidHeld.edit.replacement),
        "a sustained hold reserves its hand for every intervening head");
    const QString fixedHold = "(120){16}2h[2:1]~r,,,3,,4,,1,";
    const auto aroundHold = basicHandOrderLayoutInSelection(fixedHold, 0, 0);
    expect(aroundHold.preserved == 1 && aroundHold.edit.replacement.contains("2h[2:1]~r")
        && aroundHold.edit.replacement.contains("3~l") && aroundHold.edit.replacement.contains("4~l")
        && aroundHold.edit.replacement.contains("1~l"),
        "an existing hold annotation constrains newly assigned notes without being rewritten");
    const QString touchHeld = "(120){16}A1h[2:1],,,2,";
    const auto laidTouchHeld = basicHandOrderLayoutInSelection(touchHeld, 0, 0);
    expect(laidTouchHeld.edit.replacement.contains("A1h[2:1]~r")
        && laidTouchHeld.edit.replacement.contains("2~l") && sameNative(touchHeld, laidTouchHeld.edit.replacement),
        "Touch Hold reserves its assigned hand through the body");
    const QString bothOccupied = "(120){16}2h[2:1]/6h[2:1],,,3,";
    const auto blocked = basicHandOrderLayoutInSelection(bothOccupied, 0, 0);
    expect(blocked.edit.unsupported == 1 && blocked.edit.replacement.endsWith("3,")
        && sameNative(bothOccupied, blocked.edit.replacement),
        "a third head during two holds stays unassigned when neither hand is free");

    const QString slides = "(120){4}1-5[4:1],7-3[4:1],1-3-7[1:1],5?-1[4:1],";
    const auto laidSlides = basicHandOrderLayoutInSelection(slides, 0, 0);
    const auto parsedSlides = SimaiParser::validateSyntax(laidSlides.edit.replacement);
    expect(laidSlides.edit.diagnostics.isEmpty() && parsedSlides.annotationErrors.isEmpty() && parsedSlides.noteMarkers.size() == 4,
        "ordinary, chained and headless slides accept the deterministic basis");
    int assignedTracks = 0;
    int unassignedTracks = 0;
    for (const auto& marker : parsedSlides.noteMarkers) {
        if (marker.handOrderTrackStops.isEmpty()) ++unassignedTracks;
        else if (marker.handOrderTrackStops.size() == 1
            && (marker.handOrderTrackStops.constFirst().hand == HandOrderHand::Left
                || marker.handOrderTrackStops.constFirst().hand == HandOrderHand::Right)) ++assignedTracks;
    }
    expect(assignedTracks == 3 && unassignedTracks == 1 && laidSlides.edit.unsupported == 1,
        "overlapping slides use one hand each and leave a third head unassigned when both are occupied");
    const QString isolatedSlide = "(120){4}1-3-7[1:1],";
    const auto laidIsolated = basicHandOrderLayoutInSelection(isolatedSlide, 0, 0);
    const auto parsedIsolated = SimaiParser::validateSyntax(laidIsolated.edit.replacement);
    expect(parsedIsolated.noteMarkers.size() == 1
        && parsedIsolated.noteMarkers.constFirst().handOrderTrackStops.size() == 1
        && parsedIsolated.noteMarkers.constFirst().handOrderTrackStops.constFirst().hand
            == basicLayoutTrackHand(parsedIsolated.noteMarkers.constFirst()),
        "an isolated chained slide retains its motion-start default hand");
    expect(sameNative(slides, laidSlides.edit.replacement), "slide layout changes only hand-order annotation");

    const QString slideTailHold = "(120){4}7-5[4:1]~[head=l;track=0:l],,6h[4:1]~r,";
    const auto repairedTailHold = basicHandOrderLayoutInSelection(slideTailHold, 0, 0);
    expect(repairedTailHold.edit.replacement == "(120){4}7-5[4:1]~[head=l;track=0:l],,6h[4:1]~l,"
        && sameNative(slideTailHold, repairedTailHold.edit.replacement),
        "the first hold at a slide tail follows the released hand and repairs an old simple label");
    const QString reportedTail = "(180){4}7V15[4:1]~[head=l;track=0:l],1~r,,,,"
        "{8}6h[4:1]~r,,5~l,6h[4:1]~r,,4~l,6h[4:1]~r,,";
    const auto reportedTailLayout = basicHandOrderLayoutInSelection(reportedTail, 0, 0);
    expect(reportedTailLayout.edit.replacement.contains("{8}6h[4:1]~l,,")
        && sameNative(reportedTail, reportedTailLayout.edit.replacement),
        "the first hold after the reported V-chain reuses its left-side basis");
    expect(basicHandOrderLayoutInSelection(reportedTailLayout.edit.replacement, 0, 0).edit.changed == 0,
        "the repaired V-chain tail label is stable on a second layout pass");
    const QString interveningNote = "(120){4}7-5[4:1]~[head=l;track=0:l],1~r,,2~r,,6h[4:1]~r,";
    expect(basicHandOrderLayoutInSelection(interveningNote, 0, 0).edit.replacement
        .endsWith("6h[4:1]~r,"),
        "a later hold is not rewritten after another head has intervened since the slide tail");

    const QString joined = "(120){4}1-5[4:1],5-1[4:1],";
    const auto joinedLayout = basicHandOrderLayoutInSelection(joined, 0, 0);
    expect(joinedLayout.edit.replacement == "(120){4}1-5[4:1]~[head=r;track=0:r],"
        "5-1[4:1]~[head=l;track=0:r]," && sameNative(joined, joinedLayout.edit.replacement),
        "head-to-tail slides keep one track hand while the next playable star uses the other hand");
    const QString adjacentStroke = "(120){4}2-4-6[4:1],7-5-3[4:1],";
    const auto adjacentLayout = basicHandOrderLayoutInSelection(adjacentStroke, 0, 0);
    expect(adjacentLayout.edit.replacement.contains("7-5-3[4:1]~[head=l;track=0:r]")
        && sameNative(adjacentStroke, adjacentLayout.edit.replacement),
        "adjacent tail and next start can continue one stroke through a bend");
    const QString headlessStroke = "(120){4}1-5[4:1],5?-1[4:1],";
    const auto headlessLayout = basicHandOrderLayoutInSelection(headlessStroke, 0, 0);
    expect(headlessLayout.edit.replacement.contains("5?-1[4:1]~[track=0:r]")
        && sameNative(headlessStroke, headlessLayout.edit.replacement),
        "a headless continuation inherits the moving hand without inventing a star");
    const QString headlessStart = "(120){4}1?-5[4:1],5?-1[4:1],";
    const auto headlessStartLayout = basicHandOrderLayoutInSelection(headlessStart, 0, 0);
    expect(headlessStartLayout.edit.replacement.contains("1?-5[4:1]~[track=0:r]")
        && headlessStartLayout.edit.replacement.contains("5?-1[4:1]~[track=0:r]")
        && sameNative(headlessStart, headlessStartLayout.edit.replacement),
        "a headless first track still supplies the hand for its continuation");
    const QString threeSlides = "(120){4}1-5[4:1],5-1[4:1],1-5[4:1],";
    const auto threeLayout = basicHandOrderLayoutInSelection(threeSlides, 0, 0);
    expect(threeLayout.edit.replacement.contains("5-1[4:1]~[head=l;track=0:r]")
        && threeLayout.edit.replacement.endsWith("1-5[4:1]~[head=l;track=0:r],")
        && sameNative(threeSlides, threeLayout.edit.replacement),
        "one-stroke hand responsibility propagates across three separate tokens");
    const QString oldStroke = "(120){4}1-5[4:1]~[head=r;track=0:r],5-1[4:1]~[head=l;track=0:l],";
    const auto repairedStroke = basicHandOrderLayoutInSelection(oldStroke, 0, 0);
    expect(repairedStroke.edit.replacement == "(120){4}1-5[4:1]~[head=r;track=0:r],"
        "5-1[4:1]~[head=l;track=0:r]," && sameNative(oldStroke, repairedStroke.edit.replacement),
        "a recognized one-stroke group repairs a complete old simple track label");
    expect(basicHandOrderLayoutInSelection(repairedStroke.edit.replacement, 0, 0).edit.changed == 0,
        "a repeated one-stroke layout does not create another edit");
    const int secondStrokeStart = oldStroke.indexOf("5-1[");
    const auto strokeSelection = basicHandOrderLayoutInSelection(oldStroke, secondStrokeStart,
        oldStroke.indexOf(',', secondStrokeStart));
    expect(strokeSelection.edit.changed == 0,
        "a one-stroke pattern does not rewrite a selected slide without its predecessor");
    const QString manualStroke = "(120){4}1-5[4:1]~[head=r;track=0:r|0.5:l],5-1[4:1],";
    const auto manualLayout = basicHandOrderLayoutInSelection(manualStroke, 0, 0);
    expect(manualLayout.edit.replacement.startsWith("(120){4}1-5[4:1]~[head=r;track=0:r|0.5:l],")
        && sameNative(manualStroke, manualLayout.edit.replacement),
        "a complex manual track remains protected when nearby slides connect geometrically");
    const QString collidingStar = "(120){4}1-5[4:1],,5-1[0##0.5],";
    const auto collisionLayout = basicHandOrderLayoutInSelection(collidingStar, 0, 0);
    expect(collisionLayout.edit.replacement.contains("5-1[0##0.5]~[head=l;track=0:l]")
        && sameNative(collidingStar, collisionLayout.edit.replacement),
        "a previous tail hitting the next star at its head time blocks one-stroke priority");
    const QString separated = "(120){4}1-5[4:1],,5-1[4:1],";
    const auto separatedLayout = basicHandOrderLayoutInSelection(separated, 0, 0);
    expect(separatedLayout.edit.replacement.contains("5-1[4:1]~[head=l;track=0:l]"),
        "a long break between moving tracks is not a one-stroke connection");
    const QString moderateGap = "(120){4}1-5[4:1],5-1[0.6##0.5],";
    const auto moderateLayout = basicHandOrderLayoutInSelection(moderateGap, 0, 0);
    expect(moderateLayout.edit.replacement.contains("5-1[0.6##0.5]~[head=l;track=0:r]")
        && sameNative(moderateGap, moderateLayout.edit.replacement),
        "half a 120 BPM quarter-note grid accepts a short moving-track gap");
    const QString fastGrid = "(120){4}1-5[4:1],(240){8}5-1[0.6##0.5],";
    const auto fastLayout = basicHandOrderLayoutInSelection(fastGrid, 0, 0);
    expect(fastLayout.edit.replacement.contains("5-1[0.6##0.5]~[head=l;track=0:r]")
        && sameNative(fastGrid, fastLayout.edit.replacement),
        "an exact endpoint dwell can continue across a BPM and subdivision change");
    const QString cappedGap = "(120){4}1-5[4:1],5-1[0.66##0.5],";
    expect(basicHandOrderLayoutInSelection(cappedGap, 0, 0).edit.replacement
        .contains("5-1[0.66##0.5]~[head=l;track=0:r]"),
        "an exact endpoint dwell beyond the old grid ceiling stays on one hand within a beat");
    const QString flooredGap = "(120){4}1-5[4:1],(600){64}5-1[0.505##0.5],";
    expect(basicHandOrderLayoutInSelection(flooredGap, 0, 0).edit.replacement
        .contains("5-1[0.505##0.5]~[head=l;track=0:r]"),
        "the 1/180-second floor permits a tiny gap at an extreme BPM and subdivision");

    const QString touches = "(120){4}A1,B8,C,D1,D3,D5,D7,E1,";
    const auto laidTouches = basicHandOrderLayoutInSelection(touches, 0, 0);
    expect(laidTouches.edit.diagnostics.isEmpty() && laidTouches.edit.replacement.contains("A1~r")
        && laidTouches.edit.replacement.contains("B8~l") && laidTouches.edit.replacement.contains("C~m")
        && laidTouches.edit.replacement.contains("D1~m") && laidTouches.edit.replacement.contains("D3~r")
        && laidTouches.edit.replacement.contains("D5~m") && laidTouches.edit.replacement.contains("D7~l")
        && laidTouches.edit.replacement.contains("E1~m"), "Touch uses actual pad geometry and centre-axis Either, not pad digits");
    expect(sameNative(touches, laidTouches.edit.replacement), "Touch positions and timing remain unchanged");
    expect(basicLayoutHandForPoint({0.0, 10.0}) == HandOrderHand::Either
        && basicLayoutHandForPoint({-1.0, 0.0}) == HandOrderHand::Left
        && basicLayoutHandForPoint({1.0, 0.0}) == HandOrderHand::Right
        && basicLayoutHandForPoint({std::numeric_limits<double>::quiet_NaN(), 0.0}) == HandOrderHand::Unspecified,
        "shared geometry distinguishes a genuine centre preference from invalid input");

    const QString wifi = "(120){4}1w5[4:1],";
    const auto laidWifi = basicHandOrderLayoutInSelection(wifi, 0, 0);
    const auto parsedWifi = SimaiParser::validateSyntax(laidWifi.edit.replacement);
    expect(laidWifi.edit.diagnostics.isEmpty() && parsedWifi.annotationErrors.isEmpty()
        && laidWifi.edit.replacement.contains("track=split:"),
        "an isolated Wifi emits explicit native sectors");
    for (const auto& marker : parsedWifi.noteMarkers) {
        if (marker.type != QStringLiteral("wifi")) continue;
        expect(marker.handOrderWifiMode == HandOrderWifiMode::Split && marker.handOrderTrackStops.isEmpty()
            && marker.handOrderWifiSectorStops.size() == 2
            && marker.handOrderWifiSectorStops.at(0).first().hand != marker.handOrderWifiSectorStops.at(1).first().hand,
            "unannotated Wifi gets two different hands rather than an incorrect ordinary one-color track");
    }
    expect(sameNative(wifi, laidWifi.edit.replacement), "Wifi native fan geometry, counts and timing remain unchanged");
    const QString wifiWithThirdTrack = "(120){4}1w5[4:1]*-7[4:1],";
    const auto thirdWifiTrack = basicHandOrderLayoutInSelection(wifiWithThirdTrack, 0, 0);
    expect(thirdWifiTrack.edit.replacement == wifiWithThirdTrack
        && thirdWifiTrack.edit.unsupported == 1,
        "a two-hand Wifi and independent third branch are left unresolved");
    const QString sharedStarHead = "(120){4}1~l/1-5[4:1]~[head=r;track=0:r],";
    const auto sharedStarLayout = basicHandOrderLayoutInSelection(sharedStarHead, 0, 0);
    expect(sharedStarLayout.edit.replacement == "(120){4}1~r/1-5[4:1]~[head=r;track=0:r],"
        && sameNative(sharedStarHead, sharedStarLayout.edit.replacement),
        "a tap starting a headed slide takes the same hand as that star");
    const QString sharedOneStroke = "(120){4}1~l/1-5[4:1]~[head=r;track=0:r],"
        "5-1[4:1]~[head=l;track=0:r],";
    const auto sharedOneStrokeLayout = basicHandOrderLayoutInSelection(sharedOneStroke, 0, 0);
    expect(sharedOneStrokeLayout.edit.replacement.startsWith("(120){4}1~r/1-5[4:1]~[head=r;track=0:r],")
        && sameNative(sharedOneStroke, sharedOneStrokeLayout.edit.replacement),
        "the startup tap shares the first star hand even when its track continues into one stroke");
    const QString sharedMultipleStars = "(120){4}1~l/1-5[4:1]~[head=r;track=0:r]/"
        "1-7[4:1]~[head=r;track=0:l],";
    const auto sharedMultipleLayout = basicHandOrderLayoutInSelection(sharedMultipleStars, 0, 0);
    expect(sharedMultipleLayout.edit.replacement.startsWith("(120){4}1~r/1-5[4:1]~[head=r;track=0:r]/")
        && sameNative(sharedMultipleStars, sharedMultipleLayout.edit.replacement),
        "one tap shares the head hand with multiple simultaneous stars on its lane");
    const QString doubleStarOld = "(120){4}1-5[4:1]~[head=r;track=0:r]/"
        "1-7[4:1]~[head=r;track=0:r],";
    const auto doubleStarLayout = basicHandOrderLayoutInSelection(doubleStarOld, 0, 0);
    expect(doubleStarLayout.edit.replacement == "(120){4}1-5[4:1]~[head=r;track=0:r]/"
        "1-7[4:1]~[head=r;track=0:l],"
        && sameNative(doubleStarOld, doubleStarLayout.edit.replacement),
        "same-head divergent double stars share one button head but split their tracks");
    const QString branchedDoubleOld = "(120){4}1-5[4:1]*-7[4:1]"
        "~[head=r;branch0=0:r;branch1=0:r],";
    const auto branchedDoubleLayout = basicHandOrderLayoutInSelection(branchedDoubleOld, 0, 0);
    expect(branchedDoubleLayout.edit.replacement == "(120){4}1-5[4:1]*-7[4:1]"
        "~[head=r;branch0=0:r;branch1=0:l],"
        && sameNative(branchedDoubleOld, branchedDoubleLayout.edit.replacement),
        "native multi-branch double stars repair simple same-hand track labels");
    const QString sharedHeadWithEach = "(120){4}1~l/1-5[4:1]~[head=r;track=0:r]/5~l,";
    const auto mixedHeadLayout = basicHandOrderLayoutInSelection(sharedHeadWithEach, 0, 0);
    expect(mixedHeadLayout.edit.replacement == "(120){4}1~r/1-5[4:1]~[head=r;track=0:r]/5~l,"
        && sameNative(sharedHeadWithEach, mixedHeadLayout.edit.replacement),
        "a shared star and tap use one hand while an independent simultaneous note uses the other");
    const QString headlessPair = "(120){4}1~l/1?-5[4:1],";
    expect(basicHandOrderLayoutInSelection(headlessPair, 0, 0).edit.replacement.contains("1~l/1?-5[4:1]"),
        "a headless slide has no playable star for a same-key tap to inherit");
    TimelineNoteMarker sameHalf;
    sameHalf.type = QStringLiteral("wifi");
    sameHalf.wifiLanePoints = {{QPointF(0, 0), QPointF(100, 10)}, {QPointF(0, 0), QPointF(150, 0)},
        {QPointF(0, 0), QPointF(200, -10)}};
    expect(basicLayoutWifiSectorHand(sameHalf, 0) == HandOrderHand::Left
        && basicLayoutWifiSectorHand(sameHalf, 1) == HandOrderHand::Right,
        "a Wifi fan wholly on one half still receives two opposite hands by relative native sector x");

    const QString locked = "(120){4}1~l,5~m,1w5[4:1]~r,2w6[4:1]~[track=split:0:r&0:l],"
        "3w7[4:1]~[track=single:0:m],8~z,2,";
    const auto protectedLayout = basicHandOrderLayoutInSelection(locked, 0, 0);
    expect(protectedLayout.preserved == 6 && protectedLayout.edit.changed == 0
        && protectedLayout.edit.replacement == locked
        && protectedLayout.edit.unsupported == 1
        && protectedLayout.edit.diagnostics.size() == 1,
        "existing Wifi locks stay intact and an overlapping uncertain Single hand blocks guessed colors");

    const QString partial = "(120){4}1-5[4:1]/8,2,";
    const int begin = partial.indexOf("-5");
    const int end = partial.indexOf(",2");
    const auto selection = basicHandOrderLayoutInSelection(partial, begin, end);
    expect(selection.edit.partial == 1 && selection.edit.changed == 1 && selection.edit.replacement.endsWith("/8~l"),
        "selection boundaries protect an incomplete slide token while assigning complete neighbours");
    expect(sameNative(partial, partial.left(begin) + selection.edit.replacement + partial.mid(end)),
        "partial selection retains the native chart projection");
    const QString unicode = QStringLiteral("|| 中文 😀\r\n(120){4}8/1-5[4:1],\r\nE");
    const auto ordered = basicHandOrderLayoutInSelection(unicode, 0, 0);
    expect(ordered.edit.diagnostics.isEmpty() && ordered.edit.replacement.startsWith(QStringLiteral("|| 中文 😀\r\n"))
        && ordered.edit.replacement.contains("8~l/1-5[4:1]~[head=r;track=0:r]")
        && sameNative(unicode, ordered.edit.replacement), "UTF-16, CRLF, comments and parser sort order do not alter source identity");
    const QString badTiming = "(wrong){4}1-5[4:1],";
    const auto invalid = basicHandOrderLayoutInSelection(badTiming, 0, 0);
    expect(invalid.edit.changed == 0 && invalid.edit.replacement == badTiming && !invalid.edit.diagnostics.isEmpty(),
        "invalid native timing cannot produce a silently partial layout");
    const QString fastLongitudinal = "(120){16}4,4,4,4,4,4,";
    const QString fastLongitudinalExpected = "(120){16}4~r,4~l,4~r,4~l,4~r,4~l,";
    expect(basicHandOrderLayoutInSelection(fastLongitudinal, 0, 0).edit.replacement == fastLongitudinalExpected
        && basicHandOrderLayoutInSelection("(120){16}4~r,4~r,4~r,4~r,4~r,4~r,", 0, 0).edit.replacement
            == fastLongitudinalExpected && sameNative(fastLongitudinal, fastLongitudinalExpected),
        "fast unlocked longitudinal taps alternate hands and correct old simple labels");
    const QString lockedLongitudinal = "(120){16}2p6[8:1]~[head=r;track=0:r],,,4,4,4,";
    const auto laidLockedLongitudinal = basicHandOrderLayoutInSelection(lockedLongitudinal, 0, 0);
    expect(laidLockedLongitudinal.edit.replacement.contains("4~l,4~l,4~l,")
        && sameNative(lockedLongitudinal, laidLockedLongitudinal.edit.replacement),
        "a fast same-lane run does not alternate into a hand reserved by a slide");
    const QString winding = "(120){16}1,1,2,2,3,3,4,4,5,5,";
    const QString windingExpected = "(120){16}1~r,1~r,2~l,2~l,3~r,3~r,4~l,4~l,5~r,5~r,";
    const QString windingOld = "(120){16}1~r,1~r,2~r,2~r,3~r,3~r,4~r,4~r,5~l,5~l,";
    expect(basicHandOrderLayoutInSelection(winding, 0, 0).edit.replacement == windingExpected
        && basicHandOrderLayoutInSelection(windingOld, 0, 0).edit.replacement == windingExpected
        && sameNative(winding, windingExpected),
        "winding rebounds stay on one hand per lane pair and alternate only between pairs");
    const QString windingWithJumps = "(120){16}1,1,,2,2,,3,3,,4,4,";
    expect(basicHandOrderLayoutInSelection(windingWithJumps, 0, 0).edit.replacement
        == "(120){16}1~r,1~r,,2~l,2~l,,3~r,3~r,,4~l,4~l,",
        "empty subdivisions between winding pairs do not turn each rebound into an alternating tap");
    const QString fullCircle = "(180){24}1,2,3,4,5,6,7,8,1/5,";
    const QString fullCircleOld = "(180){24}1~r,2~r,3~r,4~r,5~l,6~l,7~l,8~l,1~r/5~l,";
    const QString fullCircleExpected = "(180){24}1~r,2~r,3~r,4~r,5~r,6~r,7~r,8~r,1~r/5~l,";
    expect(basicHandOrderLayoutInSelection(fullCircle, 0, 0).edit.replacement == fullCircleExpected
        && basicHandOrderLayoutInSelection(fullCircleOld, 0, 0).edit.replacement == fullCircleExpected
        && sameNative(fullCircle, fullCircleExpected),
        "a full-circle sweep stays on one hand and leaves the final each partner to the other hand");
    const QString counterClockwise = "(180){24}1,8,7,6,5,4,3,2,5/1,";
    expect(basicHandOrderLayoutInSelection(counterClockwise, 0, 0).edit.replacement
        == "(180){24}1~r,8~r,7~r,6~r,5~r,4~r,3~r,2~r,5~l/1~r,",
        "the full-circle rule also accepts reverse motion and a reversed final each token order");
    const QString circleWithoutEach = "(180){24}1,2,3,4,5,6,7,8,1,";
    expect(basicHandOrderLayoutInSelection(circleWithoutEach, 0, 0).edit.replacement
        == "(180){24}1~r,2~r,3~r,4~r,5~r,6~r,7~r,8~r,1~r,",
        "a full circle without an end each still keeps the sweep on one hand");
    const QString shortSweep = "(120){32}8~l,7~r,6~l,5~r,4~l,";
    const auto repairedSweep = basicHandOrderLayoutInSelection(shortSweep, 0, 0);
    expect(repairedSweep.edit.replacement == "(120){32}8~l,7~l,6~l,5~l,4~l,"
        && sameNative(shortSweep, repairedSweep.edit.replacement),
        "a fast five-key directional sweep stays on its starting hand");
    const QString sweepAfterSlides = "(180){8}8-5[8:1],1,2,3,4-1[8:1],6,7,8,"
        "{32}8~l,7~r,6~l,5~r,4~l,";
    const auto contextualSweep = basicHandOrderLayoutInSelection(sweepAfterSlides, 0, 0);
    expect(contextualSweep.edit.replacement.endsWith("{32}8~l,7~l,6~l,5~l,4~l,")
        && sameNative(sweepAfterSlides, contextualSweep.edit.replacement),
        "the directional sweep is still recognized after nearby slides and taps");
    for (int bpm : {120, 180, 240}) {
        const QString chart = QStringLiteral("(%1){32}8-4[8:1],7,6,5,4,").arg(bpm);
        const auto result = basicHandOrderLayoutInSelection(chart, 0, 0);
        expect(result.edit.replacement.contains("7~l,6~l,5~l,4~l,")
            && sameNative(chart, result.edit.replacement),
            "four dense sweep keys stay on one free hand while the preceding slide uses the other");
    }
    const QString holdThenSweep = "(190){48}7~l,,,,5~r,,,,6~l,,,,1~r,,,,4~l,,,,3~r,,,,5h[4:1]~l,,,,,,,,,,,,"
        "4~r,,,3~r,,,2~r,,,1~r,,,\n{32}5~l,4~r,3~r,2~r,1~r,8~l,7~l,6~l,3~r/5~l,,,,";
    const auto holdSweepLayout = basicHandOrderLayoutInSelection(holdThenSweep, 0, 0);
    expect(holdSweepLayout.edit.replacement.contains("4~r,,,3~r,,,2~r,,,1~r,,,\n{32}5~l,4~l,3~l,2~l,1~l,8~l,7~l,6~l,")
        && sameNative(holdThenSweep, holdSweepLayout.edit.replacement),
        "the right-hand lead-in stays right while the following 5-led sweep stays left");
    const QString alternatingLanes = "(120){16}3,4,3,4,3,";
    const auto laidAlternatingLanes = basicHandOrderLayoutInSelection(alternatingLanes, 0, 0);
    expect(laidAlternatingLanes.edit.replacement == "(120){16}3~r,4~l,3~r,4~l,3~r,"
        && sameNative(alternatingLanes, laidAlternatingLanes.edit.replacement),
        "adjacent-lane back-and-forth interaction alternates hands instead of one-hand sweeping");
    for (int bpm : {100, 150, 190, 240}) {
        const QString crossed = QStringLiteral("(%1){8}8~r/2~l,7~r/3~l,").arg(bpm);
        expect(basicHandOrderLayoutInSelection(crossed, 0, 0).edit.replacement
            == QStringLiteral("(%1){8}8~l/2~r,7~l/3~r,").arg(bpm),
            "basic layout repairs extreme crossed Each labels throughout the normal BPM range");
        const QString reversed = QStringLiteral("(%1){8}2~l/8~r,3~l/7~r,").arg(bpm);
        expect(basicHandOrderLayoutInSelection(reversed, 0, 0).edit.replacement
            == QStringLiteral("(%1){8}2~r/8~l,3~r/7~l,").arg(bpm),
            "crossed Each repair is independent of slash source order");
    }
    const QString fourStepInteraction = "(150){16}2~r,1~r,2~r,1~r,";
    expect(basicHandOrderLayoutInSelection(fourStepInteraction, 0, 0).edit.replacement
        == "(150){16}2~r,1~l,2~r,1~l,",
        "four-step adjacent-lane interaction repairs old one-hand labels");
    const QString fiveNoteAxis = "(190){16}6~l,5~r,6~l,7~l,6~l,";
    expect(basicHandOrderLayoutInSelection(fiveNoteAxis, 0, 0).edit.replacement
        == "(190){16}6~l,5~r,6~l,7~r,6~l,",
        "five-note axis keeps centre repeats separate from opposite-neighbour travel");
    const QString longLongitudinal = "(190){16},,,,3~r/6~l,,4~r,5~l,5~l,5~l,5~l,5~l,5~l,5~l,4~r,5~l,"
        "\n{16}5~l,5~l,5~l,5~l,5~l,,,,";
    const auto verticalLayout = basicHandOrderLayoutInSelection(longLongitudinal, 0, 0);
    expect(verticalLayout.edit.replacement.contains("5~l,5~r,5~l,5~r,5~l,5~r,5~l,")
        && verticalLayout.edit.replacement.contains("{16}5~r,5~l,5~r,5~l,5~r,")
        && sameNative(longLongitudinal, verticalLayout.edit.replacement),
        "a long 190 BPM vertical alternates after surrounding Each and nearby taps");
    const auto leftHome = miacode::chart::handOrderNaturalRestPoint(0);
    const QPointF departed{1.0, 0.0};
    const auto waitingRest = miacode::chart::handOrderRestedPoint(departed, 0.0, 0.4, 0, 1.0);
    const auto slowRest = miacode::chart::handOrderRestedPoint(departed, 0.0, 0.9, 0, 1.0);
    const auto fastRest = miacode::chart::handOrderRestedPoint(departed, 0.0, 0.9, 0, 8.0);
    expect(waitingRest == departed && slowRest.x() < departed.x() && slowRest.x() > leftHome.x()
        && fastRest == leftHome && leftHome.x() < 0
        && miacode::chart::handOrderNaturalRestPoint(1).x() > 0,
        "hands wait 0.4 seconds, then return by configurable distance-based speed to E7 and E3");
    for (int bpm : {80, 240}) {
        const QString chart = QStringLiteral("(%1){16}3,4,3,4,3,").arg(bpm);
        expect(basicHandOrderLayoutInSelection(chart, 0, 0).edit.replacement
            == QStringLiteral("(%1){16}3~r,4~l,3~r,4~l,3~r,").arg(bpm),
            "the alternating-lane interaction follows the score grid across normal BPM values");
    }
    for (int bpm : {120, 150, 190, 240}) {
        const auto check = [&](const QString& body, const QString& expected, const char* pattern) {
            const QString prefix = QStringLiteral("(%1)").arg(bpm);
            const QByteArray label = QStringLiteral("%1 BPM basic %2 is stable")
                .arg(bpm).arg(QString::fromLatin1(pattern)).toUtf8();
            expect(basicHandOrderLayoutInSelection(prefix + body, 0, 0).edit.replacement
                == prefix + expected, label.constData());
        };
        check("{16}5,5,5,5,5,5,", "{16}5~l,5~r,5~l,5~r,5~l,5~r,", "vertical");
        check("{16}6,5,6,7,6,", "{16}6~l,5~r,6~l,7~r,6~l,", "five-note axis");
        check("{32}5,4,3,2,1,8,7,6,", "{32}5~l,4~l,3~l,2~l,1~l,8~l,7~l,6~l,", "sweep");
        check("{16}E7,E6,E5,", "{16}E7~l,E6~l,E5~l,", "E-ring sweep");
        check("{4}1-5[4:1],5-1[4:1],",
            "{4}1-5[4:1]~[head=r;track=0:r],5-1[4:1]~[head=l;track=0:r],", "one-stroke");
    }
    const QString relaxed = "(222){8}4,4,5,5,3b/6b,";
    const QString relaxedOld = "(222){8}4~l,4~r,5~l,5~l,3b~r/6b~l,";
    const QString relaxedExpected = "(222){8}4~r,4~r,5~l,5~l,3b~r/6b~l,";
    expect(basicHandOrderLayoutInSelection(relaxed, 0, 0).edit.replacement == relaxedExpected
        && basicHandOrderLayoutInSelection(relaxedOld, 0, 0).edit.replacement == relaxedExpected
        && sameNative(relaxed, relaxedExpected),
        "222 BPM eighth-note rebounds do not split a low-pressure pair");
    for (int bpm : {80, 120, 185, 240, 288}) {
        const QString source = QStringLiteral("(%1){8}5~l,5~r,").arg(bpm);
        expect(basicHandOrderLayoutInSelection(source, 0, 0).edit.replacement
            == QStringLiteral("(%1){8}5~l,5~l,").arg(bpm), "a two-hit eighth-note rebound stays with its first hand");
    }
    const QString fastEighthOld = "(240){8}4~r/7~l,,2~r/5~l,,4~r,4~l,5~l,5~r,";
    const QString fastEighthRaw = "(240){8}4/7,,2/5,,4,4,5,5,";
    const QString fastEighthExpected = "(240){8}4~r/7~l,,2~r/5~l,,4~r,4~r,5~l,5~l,";
    expect(basicHandOrderLayoutInSelection(fastEighthRaw, 0, 0).edit.replacement == fastEighthExpected
        && basicHandOrderLayoutInSelection(fastEighthOld, 0, 0).edit.replacement == fastEighthExpected
        && sameNative(fastEighthOld, fastEighthExpected),
        "eighth-note rebounds keep each lane on one hand despite high BPM and preceding each pairs");
    const QString slowEighthOld = "(80){8}4~r/7~l,,2~r/5~l,,4~r,4~l,5~l,5~r,";
    expect(basicHandOrderLayoutInSelection(slowEighthOld, 0, 0).edit.replacement
        == "(80){8}4~r/7~l,,2~r/5~l,,4~r,4~r,5~l,5~l,",
        "slower eighth-note pairs keep the same-hand rebound across a longer actual step");
    expect(basicHandOrderLayoutInSelection("(240){16}4,4,4,4,", 0, 0).edit.replacement
        == "(240){16}4~r,4~l,4~r,4~l,",
        "sixteenth-note repeated taps still alternate at the same BPM");
    const auto subdivisionChange = SimaiParser::validateSyntax("(240){8}4,4,{16}4,4,");
    expect(subdivisionChange.noteMarkers.size() == 4
        && subdivisionChange.noteMarkers.at(0).subdivision == 8
        && subdivisionChange.noteMarkers.at(1).subdivision == 8
        && subdivisionChange.noteMarkers.at(2).subdivision == 16
        && subdivisionChange.noteMarkers.at(3).subdivision == 16,
        "the parser retains the active subdivision on each note across {8}/{16} changes");

    const QString touchGroupOld = "(120){8}B4~l/E4~r,B7~r/E8~l,B2~l/E2~r,B5~r/E6~l,";
    const QString touchGroupRaw = "(120){8}B4/E4,B7/E8,B2/E2,B5/E6,";
    const QString touchGroupExpected = "(120){8}B4~r/E4~r,B7~l/E8~l,B2~r/E2~r,B5~l/E6~l,";
    expect(basicHandOrderLayoutInSelection(touchGroupRaw, 0, 0).edit.replacement == touchGroupExpected
        && basicHandOrderLayoutInSelection(touchGroupOld, 0, 0).edit.replacement == touchGroupExpected
        && sameNative(touchGroupOld, touchGroupExpected),
        "adjacent Touchgroup pairs may share one hand while old simple labels are corrected");
    const QString touchSweep = "(120){12}B6,E6,,E2,B2,,E8,B7,,B3,E4,,";
    const QString touchSweepExpected = "(120){12}B6~l,E6~l,,E2~r,B2~r,,E8~l,B7~l,,B3~r,E4~r,,";
    expect(basicHandOrderLayoutInSelection(touchSweep, 0, 0).edit.replacement == touchSweepExpected
        && sameNative(touchSweep, touchSweepExpected),
        "nearby sequential Touches use one hand for each short sweep");
    const QString touchSweepOld = "(120){12}B6~r,E6~l,,E2~l,B2~r,,E8~r,B7~l,,B3~l,E4~r,,";
    expect(basicHandOrderLayoutInSelection(touchSweepOld, 0, 0).edit.replacement == touchSweepExpected,
        "a short Touch sweep also corrects old simple annotations");
    const QString eRingSweep = "(190){16},,E5~l,E4~r,E3~l,,,,,,,,,,,,\n"
        "{16},,E7~l,E6~r,E5~l,,,,,,,,,,,,\n"
        "{16},,E3~r,E4~l,E5~r,,,,,,,,,,,,";
    const auto laidERingSweep = basicHandOrderLayoutInSelection(eRingSweep, 0, 0);
    expect(laidERingSweep.edit.replacement.contains("E5~r,E4~r,E3~r")
        && laidERingSweep.edit.replacement.contains("E7~l,E6~l,E5~l")
        && laidERingSweep.edit.replacement.contains("E3~r,E4~r,E5~r")
        && sameNative(eRingSweep, laidERingSweep.edit.replacement),
        "regular adjacent E-ring sweeps use one hand across all three sensors");
    const auto simultaneousE = SimaiParser::validateSyntax(
        basicHandOrderLayoutInSelection("(190){16}E3/E4,", 0, 0).edit.replacement);
    expect(simultaneousE.noteMarkers.size() == 2
        && simultaneousE.noteMarkers.at(0).handOrderHead != simultaneousE.noteMarkers.at(1).handOrderHead,
        "simultaneous adjacent E sensors remain a two-hand Each, not a timed sweep");
    const QString nonGroupTouches = "(120){8}A4/A5,";
    expect(basicHandOrderLayoutInSelection(nonGroupTouches, 0, 0).edit.replacement == "(120){8}A4~r/A5~l,",
        "simultaneous Touches outside the native Touchgroup still use separate hands");
    const auto pseudoTouch = basicHandOrderLayoutInSelection("(120){128}B4,E4,", 0, 0);
    const auto parsedPseudoTouch = SimaiParser::validateSyntax(pseudoTouch.edit.replacement);
    expect(parsedPseudoTouch.noteMarkers.size() == 2
        && parsedPseudoTouch.noteMarkers.at(0).handOrderHead != parsedPseudoTouch.noteMarkers.at(1).handOrderHead,
        "near-simultaneous Touches outside one Touchgroup are not treated as a sequential sweep");
    const QString adjacentHolds = "(120){8}4h[8:1]/5h[8:1],";
    expect(basicHandOrderLayoutInSelection(adjacentHolds, 0, 0).edit.replacement
        == "(120){8}4h[8:1]~r/5h[8:1]~l,",
        "adjacent button Holds remain a two-hand each rather than using the Touch-only exception");

    const QString starsOld = "(120){16}2x-5[16:1]~[head=r;track=0:r],,,7x-4[16:1]~[head=l;track=0:r],,,"
        "1x-5[16:1]~[head=l;track=0:r],,,8x-4[16:1]~[head=l;track=0:l],,,2x~r,,2x~r,,"
        "{16}3x-8[16:1]~[head=r;track=0:r],,,6x-1[16:1]~[head=l;track=0:r],,,"
        "4x-8[16:1]~[head=l;track=0:r],,,5x-1[16:1]~[head=l;track=0:l],";
    const QString starsExpected = "(120){16}2x-5[16:1]~[head=r;track=0:r],,,7x-4[16:1]~[head=l;track=0:l],,,"
        "1x-5[16:1]~[head=r;track=0:r],,,8x-4[16:1]~[head=l;track=0:l],,,2x~r,,2x~r,,"
        "{16}3x-8[16:1]~[head=r;track=0:r],,,6x-1[16:1]~[head=l;track=0:l],,,"
        "4x-8[16:1]~[head=r;track=0:r],,,5x-1[16:1]~[head=l;track=0:l],";
    const auto correctedStars = basicHandOrderLayoutInSelection(starsOld, 0, 0);
    expect(correctedStars.edit.replacement == starsExpected && sameNative(starsOld, starsExpected),
        "overlapping alternating stars correct old head and track labels instead of looking like one-hand dealing");
    expect(basicHandOrderLayoutInSelection(starsExpected, 0, 0).edit.changed == 0,
        "repeating the corrected pattern does not create another edit");
    const int selectedStar = starsOld.indexOf("7x-4");
    const int selectedStarEnd = starsOld.indexOf(',', selectedStar);
    const auto partialStars = basicHandOrderLayoutInSelection(starsOld, selectedStar, selectedStarEnd);
    expect(partialStars.edit.changed == 0,
        "a selected star cannot rewrite protected pattern members outside the selection");
    const QString customStars = "(120){16}2x-5[16:1]~[head=r;track=0:r],,,"
        "7x-4[16:1]~[head=l;track=0:l|0.5:r],,,1x-5[16:1]~[head=r;track=0:r],";
    expect(basicHandOrderLayoutInSelection(customStars, 0, 0).edit.changed == 0,
        "a hand-edited slide transfer prevents automatic rewriting of its star sequence");
    for (int bpm : {160, 180, 200, 240}) {
        const QString prefix = QStringLiteral("(%1)").arg(bpm);
        const QString shortRun = prefix + "{16}2~r,3~l,4~r,";
        const QString shortExpected = prefix + "{16}2~r,3~r,4~r,";
        const QString pair = prefix + "{16}2~r,3~l,";
        const QString pairExpected = prefix + "{16}2~r,3~r,";
        expect(basicHandOrderLayoutInSelection(shortRun, 0, 0).edit.replacement == shortExpected
            && basicHandOrderLayoutInSelection(pair, 0, 0).edit.replacement == pairExpected
            && sameNative(shortRun, shortExpected),
            "BPM-scaled 16th-note two- and three-key sweeps retain one moving hand");
        const QString starRun = prefix + "{16}7x~l,6x~r,5x-8[8:1]~[head=l;track=0:l],";
        const QString starExpected = prefix + "{16}7x~l,6x~l,5x-8[8:1]~[head=l;track=0:r],";
        expect(basicHandOrderLayoutInSelection(starRun, 0, 0).edit.replacement == starExpected
            && sameNative(starRun, starExpected),
            "a star-ending short sweep keeps the head hand and gives its later trace the free hand");
        const QString rebound = prefix + "{16}1~r/8~l,,8~r,";
        expect(basicHandOrderLayoutInSelection(rebound, 0, 0).edit.replacement
            == prefix + "{16}1~r/8~l,,8~l,",
            "an Each's same-key return preserves the striking hand after an empty subdivision");
        const QString crossing = prefix + "{32}3b~r/7b~l,4b~r/8b~l,1b~r/5b~l,,,,,,8~r,";
        const QString crossed = basicHandOrderLayoutInSelection(crossing, 0, 0).edit.replacement;
        expect(crossed.contains("3b~r/7b~l,4b~r/8b~l,1b~l/5b~r,,,,,,8~l,")
            && sameNative(crossing, crossed),
            "a paired 32nd-note sweep crosses at 8/1 and carries its next neighbour");
    }
    expect(basicHandOrderLayoutInSelection("(100){16}2~r,3~l,", 0, 0).edit.replacement
        == "(100){16}2~r,3~l,",
        "a slower 16th-note pair is not forcibly grouped as a sweep");
    expect(basicHandOrderLayoutInSelection("(159){16}2~r,3~l,4~r,", 0, 0).edit.replacement
        == "(159){16}2~r,3~l,4~r,"
        && basicHandOrderLayoutInSelection("(120){16}2~r,3~l,4~r,", 0, 0, {}, 3.0, 120)
            .edit.replacement == "(120){16}2~r,3~r,4~r,"
        && basicHandOrderLayoutInSelection("(150){32}2~r,3~l,4~r,", 0, 0, {}, 3.0, 300)
            .edit.replacement == "(150){32}2~r,3~r,4~r,"
        && basicHandOrderLayoutInSelection("(150){32}2~r,3~l,4~r,", 0, 0, {}, 3.0, 301)
            .edit.replacement == "(150){32}2~r,3~l,4~r,",
        "integer 16th-BPM sweep threshold includes exact 160/300 boundaries and excludes slower grids");
    expect(basicHandOrderLayoutInSelection("(240){12}2~r,3~l,4~r,", 0, 0).edit.replacement
        == "(240){12}2~r,3~r,4~r,"
        && basicHandOrderLayoutInSelection("(120){24}2~r,3~l,4~r,", 0, 0).edit.replacement
            == "(120){24}2~r,3~r,4~r,",
        "equal real sweep spacing behaves alike across BPM and subdivision spellings");
    expect(basicHandOrderLayoutInSelection("(180){16}7h[1:1]~l,4~r,5~l,", 0, 0).edit.replacement
        == "(180){16}7h[1:1]~l,4~r,5~r,",
        "two fast keys may sweep across the hand boundary while the other hand is held");
    for (int bpm : {120, 155, 190}) {
        const QString prefix = QStringLiteral("(%1){8}").arg(bpm);
        const QString right = prefix + "3>1[8:1]~[head=r;track=0:r],3~l,3~l,3~l,";
        const QString left = prefix + "6>4[8:1]~[head=l;track=0:l],6~r,6~r,6~r,";
        expect(basicHandOrderLayoutInSelection(right, 0, 0).edit.replacement
                == prefix + "3>1[8:1]~[head=r;track=0:l],3~r,3~r,3~r,"
            && basicHandOrderLayoutInSelection(left, 0, 0).edit.replacement
                == prefix + "6>4[8:1]~[head=l;track=0:r],6~l,6~l,6~l,"
            && sameNative(right, basicHandOrderLayoutInSelection(right, 0, 0).edit.replacement),
            "a star trace gives the repeated head-lane taps their own hand across BPMs");
    }
    const QString fastLaneJacks = "(240){8}3>1[8:1]~[head=r;track=0:r],3~l,3~l,3~l,";
    expect(basicHandOrderLayoutInSelection(fastLaneJacks, 0, 0).edit.replacement == fastLaneJacks,
        "very fast same-lane jacks are not forced into the relaxed tap-lane motif");
    const QString finerLaneGrid = "(155){16}3>1[8:1]~[head=r;track=0:r],,3~l,,3~l,,3~l,,";
    expect(basicHandOrderLayoutInSelection(finerLaneGrid, 0, 0).edit.replacement
        == "(155){16}3>1[8:1]~[head=r;track=0:l],,3~r,,3~r,,3~r,,",
        "the same star-and-tap phrase survives a finer written subdivision");
    const QString adjacentCross = "(155){8}4~l/5~r,";
    expect(basicHandOrderLayoutInSelection(adjacentCross, 0, 0).edit.replacement == "(155){8}4~r/5~l,",
        "a near-centre crossed Each clears stale simple labels and uses uncrossed hands");
    for (int bpm : {120, 155, 190}) {
        const QString prefix = QStringLiteral("(%1){48}").arg(bpm);
        const QString right = prefix + "1~r,2~r,3~r,,,,1~r,2~r,3~r,";
        const QString left = prefix + "8~l,7~l,6~l,,,,8~l,7~l,6~l,";
        expect(basicHandOrderLayoutInSelection(right, 0, 0).edit.replacement
                == prefix + "1~r,2~r,3~r,,,,1~l,2~l,3~l,"
            && basicHandOrderLayoutInSelection(left, 0, 0).edit.replacement
                == prefix + "8~l,7~l,6~l,,,,8~r,7~r,6~r,",
            "two close repetitions of the same three-key sweep alternate their moving hands");
    }
    const QString ladder = "(155){8}1~l/3~r,3~r,3~r/4~l,4~r,4~r/5~l,5~l,5~l/6~r,6~r,";
    const QString ladderExpected = "(155){8}1~l/3~r,3~r,3~r/4~l,4~l,4~l/5~r,5~r,5~r/6~l,6~l,";
    expect(basicHandOrderLayoutInSelection(ladder, 0, 0).edit.replacement == ladderExpected,
        "relaxed same-key carry follows each step of a staggered two-hand ladder");
    const QString isolatedPair = "(155){8}2b~l/3b~r,,4~r,3~r/4~l,";
    expect(basicHandOrderLayoutInSelection(isolatedPair, 0, 0).edit.replacement == isolatedPair,
        "an isolated note before an Each does not force its hand through the new pair");
    for (const QString& axis : {QStringLiteral("(190){16}3~r,4~l,3~r,2~l,3~r,"),
                                QStringLiteral("(150){32}3~r,4~l,3~r,2~l,3~r,")})
        expect(basicHandOrderLayoutInSelection(axis, 0, 0).edit.replacement == axis,
            "the middle 4-3-2 of a five-note axis is not isolated as a sweep");
    const QString switchedTrack = "(120){16}2-5[4:1]~[head=r;track=0:r|0.5:l],,,3-7[4:1],";
    const QString switchedExpected = "(120){16}2-5[4:1]~[head=r;track=0:r|0.5:l],,,"
        "3-7[4:1]~[head=l;track=0:r],";
    expect(basicHandOrderLayoutInSelection(switchedTrack, 0, 0).edit.replacement == switchedExpected
        && sameNative(switchedTrack, switchedExpected),
        "a later slide trace avoids the prior track's receiving hand after a handoff");
    const QString narrowHandoff = "(120){16}2-5[4:1]"
        "~[head=r;track=0:r|0.5:l|0.54:r],,3-7[4:1],";
    const auto narrowLayout = basicHandOrderLayoutInSelection(narrowHandoff, 0, 0);
    expect(narrowLayout.edit.replacement == narrowHandoff && narrowLayout.edit.unsupported == 1
        && narrowLayout.edit.diagnostics.isEmpty(),
        "a brief handoff occupying both candidates leaves the later trace unresolved");
    const auto optedNarrow = basicHandOrderLayoutInSelection(narrowHandoff, 0, 0, {}, 3.0,
        miacode::chart::handOrderDefaultSweepThreshold, true);
    expect(optedNarrow.unresolvedColored == 1
        && optedNarrow.edit.replacement.endsWith("3-7[4:1]~[head=l;track=0:r],"),
        "explicit opt-in colors an otherwise unresolved trace with its default hand");
    const QString blockedHeadless = "(120){4}2-5[1:1]/7-3[1:1],3?-6[1:1],";
    const auto headless = basicHandOrderLayoutInSelection(blockedHeadless, 0, 0);
    expect(headless.edit.replacement.endsWith("3?-6[1:1],") && headless.edit.unsupported == 1,
        "a headless third trace does not silently reuse either occupied hand");
    const QString crossedHold = "(120){16}2-5[4:1]"
        "~[head=r;track=0:r|0.5:l],,,,,7h[#0.5],";
    const auto heldDuringHandoff = basicHandOrderLayoutInSelection(crossedHold, 0, 0);
    expect(heldDuringHandoff.edit.replacement == crossedHold
        && heldDuringHandoff.edit.unsupported == 1,
        "a Hold head cannot claim a hand that a slide receives during the Hold body");
    const QString wifiOccupied = "(120){4}1w5[1:1]"
        "~[head=l;track=split:0:l&0:r],3-7[1:1],";
    const auto aroundWifi = basicHandOrderLayoutInSelection(wifiOccupied, 0, 0);
    expect(aroundWifi.edit.replacement == wifiOccupied && aroundWifi.edit.unsupported == 1,
        "a split Wifi's two moving hands block a later independent slide");
    const QString wifiSharedHead = "(120){4}1w5[1:1]"
        "~[head=l;track=split:0:l&0:r],2/2-6[4:1],";
    const auto sharedDuringWifi = basicHandOrderLayoutInSelection(wifiSharedHead, 0, 0);
    expect(sharedDuringWifi.edit.replacement == wifiSharedHead
        && sharedDuringWifi.edit.unsupported >= 1,
        "a shared tap-and-star unit cannot bypass an active two-hand Wifi");
    const QString eachLead = "(200){32}1~r/7~l,6~l,5~r,4~r,2~r/8~l,3~r,4~l,5~l,";
    expect(basicHandOrderLayoutInSelection(eachLead, 0, 0).edit.replacement
        == "(200){32}1~r/7~l,6~l,5~l,4~l,2~r/8~l,3~r,4~r,5~r,",
        "an opposite Each at a fast sweep's entrance reserves the other hand");
    return ok ? 0 : 1;
}
