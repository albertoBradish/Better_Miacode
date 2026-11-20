#include "app/ui/editor/EditorController.h"
#include "app/ui/editor/NoteSelectionEdit.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/ChartNoteSelection.h"
#include "core/chart/transform/HandOrderScreenSplit.h"
#include "core/chart/transform/HandOrderAlternation.h"
#include "timeline/TimelineQuickModel.h"

#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

namespace {
bool sameTiming(const TimelineNoteMarker& left, const TimelineNoteMarker& right)
{
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    return left.type == right.type && left.lane == right.lane && left.endLane == right.endLane
        && close(left.second, right.second) && close(left.endSecond, right.endSecond)
        && close(left.slideTraceSecond, right.slideTraceSecond);
}
bool sameNativeTiming(const QString& left, const QString& right)
{
    const auto a = SimaiParser::parseForTimeline(left), b = SimaiParser::parseForTimeline(right);
    if (!a.ok || !b.ok || a.noteMarkers.size() != b.noteMarkers.size()) return false;
    for (int index = 0; index < a.noteMarkers.size(); ++index)
        if (!sameTiming(a.noteMarkers[index], b.noteMarkers[index])) return false;
    return true;
}
QString applied(const QString& original, const miacode::editor::SimaiTextEditResult& edit)
{
    if (!edit.transaction.hasEdit) return original;
    auto text = original;
    const auto& tx = edit.transaction;
    text.replace(tx.replacementStart, tx.replacementEnd - tx.replacementStart, tx.replacementText);
    return text;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        QTextStream(condition ? stdout : stderr) << (condition ? "PASS: " : "FAIL: ") << message << '\n';
        if (!condition) ++failures;
    };
    using namespace miacode::chart_selection;
    // Synthetic forms follow HAND_ORDER_ANNOTATION_SPEC_ZH and HandOrderEditSpec.
    const QString source = QStringLiteral("(120){4}1~l/5~r,2~m,(180){8}3h[4:1]~m,1-3-7[1:1]~l,4~r,E");
    TimelineQuickModel model;
    model.rebuildFromText(source, 0.0);
    const auto all = model.noteSourceRanges();
    const auto span = [&](const QString& token) {
        const int start = source.indexOf(token);
        return NoteRange{start, start + int(token.size())};
    };
    const auto tap = span(QStringLiteral("1~l"));
    const auto hold = span(QStringLiteral("3h[4:1]~m"));
    const auto chain = span(QStringLiteral("1-3-7[1:1]~l"));
    const auto last = span(QStringLiteral("4~r"));
    const auto native = SimaiParser::parseForTimeline(source);
    expect(native.ok && native.annotationErrors.isEmpty() && !native.sourceTokens.isEmpty(),
        "annotated fixture retains native sourceTokens and separate annotation diagnostics");
    bool suffixToken = false;
    for (const auto& token : native.sourceTokens)
        suffixToken |= source.mid(token.col - 1, token.length) == QStringLiteral("1~l");
    expect(suffixToken, "sourceTokens include the complete annotation suffix");
    const auto copiedTap = copySelectedNotes(source, {{tap.start + 1, tap.start + 2}}, all);
    const auto tapParse = SimaiParser::parseForTimeline(copiedTap + QStringLiteral("E"));
    expect(copiedTap.contains(QStringLiteral("1~l")) && !copiedTap.contains(QStringLiteral("5~r"))
        && tapParse.ok && tapParse.annotationErrors.isEmpty(), "a suffix hit selects one complete simultaneous note");
    const NoteRanges selected{tap, hold, chain};
    const auto copied = copySelectedNotes(source, selected, all);
    const auto copiedParse = SimaiParser::parseForTimeline(copied + QStringLiteral("E"));
    expect(copiedParse.ok && copiedParse.annotationErrors.isEmpty() && copiedParse.noteMarkers.size() == 3
        && copied.contains(QStringLiteral("3h[4:1]~m")) && copied.contains(QStringLiteral("1-3-7[1:1]~l")),
        "copy owns Hold and chained Slide suffixes without partial annotation fragments");
    bool copyTimes = copiedParse.noteMarkers.size() == 3;
    for (const auto& note : copiedParse.noteMarkers) {
        bool found = false;
        for (const auto& original : native.noteMarkers) found |= sameTiming(note, original);
        copyTimes &= found;
    }
    expect(copyTimes, "copied note timing survives intervening unselected notes and BPM changes");
    const auto deleted = deleteSelectedNotes(source, selected, all);
    const auto deletedParse = SimaiParser::parseForTimeline(deleted);
    bool survivors = deletedParse.ok && deletedParse.annotationErrors.isEmpty();
    for (const auto& note : deletedParse.noteMarkers) {
        bool found = false;
        for (const auto& original : native.noteMarkers) found |= sameTiming(note, original);
        survivors &= found;
    }
    expect(survivors && deleted.contains(QStringLiteral("5~r")) && deleted.contains(QStringLiteral("2~m"))
        && deleted.contains(QStringLiteral("4~r")) && deleted.count(',') == source.count(','),
        "deletion keeps legal same-beat suffixes, timing commas and surviving note times");

    miacode::ui::EditorController controller;
    controller.setHandOrderFeaturesEnabled(true);
    controller.activateHandOrderDocument(QStringLiteral("synthetic:selection"), source);
    const auto command = [&](const QString& text, const NoteRanges& ranges, const QString& operation) {
        if (operation == QStringLiteral("hand_order.screen_split"))
            return controller.screenSplitHandOrderSelection(text, ranges);
        return miacode::ui::editSelectedNoteRanges(text, ranges, operation,
            [&](const QString& current, int start, int end) {
                return controller.handOrderCommand(current, start, end, operation);
            });
    };
    const auto set = command(source, {tap, last}, QStringLiteral("hand_order.set_left"));
    const auto assigned = applied(source, set);
    expect(set.consumed && set.transaction.hasEdit && set.transaction.replacementStart == 0
        && set.transaction.replacementEnd == source.size() && assigned.contains(QStringLiteral("4~l"))
        && assigned.contains(QStringLiteral("5~r")) && assigned.contains(QStringLiteral("2~m"))
        && sameNativeTiming(source, assigned), "discontiguous set-hand collects one transaction without editing intervening notes");
    const auto swap = command(source, {tap, chain}, QStringLiteral("hand_order.swap_hands"));
    const auto swapped = applied(source, swap);
    expect(swap.consumed && swapped.contains(QStringLiteral("1~r/5~r"))
        && swapped.contains(QStringLiteral("1-3-7[1:1]~r")) && swapped.contains(QStringLiteral("2~m"))
        && sameNativeTiming(source, swapped), "multi-range swap changes every selected suffix and preserves native timing");
    {
        // A1/A8 share the D1 axis. The unselected A2 Slide would make the
        // envelope's full start set invalid, so it must not influence the axis.
        const QString screenSource = QStringLiteral("(120){4}1p4[8:1],2-6[8:1]~m,8q5[8:1],E");
        TimelineQuickModel screenModel;
        screenModel.rebuildFromText(screenSource, 0.0);
        const auto screenRanges = screenModel.noteSourceRanges();
        const NoteRanges selectedSlides{screenRanges.first(), screenRanges.last()};
        const auto alone = controller.handOrderCommand(screenSource, selectedSlides.first().start,
            selectedSlides.first().end, QStringLiteral("hand_order.screen_split"));
        expect(!alone.transaction.hasEdit, "one Slide alone cannot establish the screen-split reflection axis");
        const auto plan = miacode::chart_transform::screenSplitHandOrderInSelection(screenSource, selectedSlides);
        expect(plan.diagnostics.isEmpty() && plan.changed == 2 && plan.patches.size() == 2
            && plan.patches.first().start == selectedSlides.first().start
            && plan.patches.last().start == selectedSlides.last().start,
            "one core screen-split plan derives the axis from exactly the two selected Slide tokens");
        const auto screen = command(screenSource, selectedSlides, QStringLiteral("hand_order.screen_split"));
        const auto divided = applied(screenSource, screen);
        const auto dividedParse = SimaiParser::parseForTimeline(divided);
        bool commonAxisMatches = dividedParse.ok && dividedParse.annotationErrors.isEmpty();
        int modifiedSlides = 0;
        bool middleUntouched = false;
        for (const auto& marker : dividedParse.noteMarkers) {
            if (marker.lane == 2) {
                middleUntouched = marker.handOrderHead == HandOrderHand::Either
                    && marker.handOrderTrackStops.size() == 1
                    && marker.handOrderTrackStops.first().hand == HandOrderHand::Either;
                continue;
            }
            ++modifiedSlides;
            const auto firstHand = marker.lane == 1 ? HandOrderHand::Right : HandOrderHand::Left;
            const auto otherHand = marker.lane == 1 ? HandOrderHand::Left : HandOrderHand::Right;
            const auto& stops = marker.handOrderTrackStops;
            commonAxisMatches &= marker.handOrderHead == firstHand && stops.size() == 3;
            if (stops.size() == 3)
                commonAxisMatches &= stops[0].hand == firstHand && stops[1].hand == otherHand
                    && stops[2].hand == firstHand
                    && std::abs(stops[1].progress - 0.22099771638446827) < 1e-9
                    && std::abs(stops[2].progress - 0.7790022836155317) < 1e-9;
        }
        expect(screen.consumed && screen.transaction.hasEdit && modifiedSlides == 2 && commonAxisMatches
            && screen.transaction.anchor == screen.transaction.position,
            "separated Slides both change with the same D1 axis, real curve crossings and opposite side hands");
        expect(middleUntouched && divided.contains(QStringLiteral("2-6[8:1]~m"))
            && miacode::hand_order::stripAnnotations(divided) == miacode::hand_order::stripAnnotations(screenSource)
            && sameNativeTiming(screenSource, divided),
            "the intervening unselected Slide retains exact source annotation and all native note timing");
    }
    {
        const QString alternating = QStringLiteral("(120){8}4,5,4,6,4,7,,6,E");
        const auto tx = controller.handOrderCommand(alternating, 0, alternating.size(), QStringLiteral("hand_order.alternate"));
        expect(applied(alternating, tx) == QStringLiteral("(120){8}4~r,5~l,4~r,6~l,4~r,7~l,,6~l,E")
            && sameNativeTiming(alternating, applied(alternating, tx)), "comma slots including rests alternate from the first note half-screen");
        const QString sparse = QStringLiteral("(120){8}5,1,6,2,7,,8,E");
        TimelineQuickModel sparseModel; sparseModel.rebuildFromText(sparse,0);
        const auto spans = sparseModel.noteSourceRanges();
        const auto selected = controller.alternateHandOrderSelection(sparse, {spans[0],spans[2],spans[5]});
        expect(applied(sparse,selected) == QStringLiteral("(120){8}5~l,1,6~l,2,7,,8~l,E"),
            "noncontiguous selected objects share one comma phase while skipped objects and rests consume slots");
        const QString comment = QStringLiteral("(120){8}5,|| ignore ,,,\n6,7,,8,E");
        const auto commentTx = controller.handOrderCommand(comment,0,comment.size(),QStringLiteral("hand_order.alternate"));
        expect(applied(comment,commentTx) == QStringLiteral("(120){8}5~l,|| ignore ,,,\n6~r,7~l,,8~l,E"),
            "comment commas do not advance the alternating phase");
        const QString same = QStringLiteral("(150){16}3,4,1,,3,4,1,E");
        const auto sameTx = controller.handOrderCommand(same, 0, same.size(), QStringLiteral("hand_order.alternate"));
        expect(applied(same, sameTx) == QStringLiteral("(150){16}3~r,4~l,1~r,,3~r,4~l,1~r,E"),
            "a repeated triangle across an empty slot can restart with the same hand");
        const QString mirrored = QStringLiteral("(150){16}3,4,1,|| rest ,,,\n,6,5,8,E");
        const auto mirroredTx = controller.handOrderCommand(mirrored, 0, mirrored.size(), QStringLiteral("hand_order.alternate"));
        const auto mirroredText = applied(mirrored, mirroredTx);
        expect(mirroredText == QStringLiteral("(150){16}3~r,4~l,1~r,|| rest ,,,\n,6~l,5~r,8~l,E")
            && sameNativeTiming(mirrored, mirroredText), "a reflected triangle restarts with the opposite hand across the same empty slot");
        const QString locked = QStringLiteral("(150){16}3h[4:1]~r,4,3,5,E");
        const auto lockedTx = controller.handOrderCommand(locked, locked.indexOf("4,3,5"), locked.size(), QStringLiteral("hand_order.alternate"));
        expect(applied(locked, lockedTx) == QStringLiteral("(150){16}3h[4:1]~r,4~l,3~r,5~l,E"),
            "the one-click triangle phase honors an occupied hand outside the selection");
        const QString mixed = QStringLiteral("(185){16}2,1-5[4:1]~[head=r;track=0:r|0.5:l],3,2,1,3,E");
        const auto mixedTx = controller.handOrderCommand(mixed, 0, mixed.size(), QStringLiteral("hand_order.alternate"));
        const auto mixedResult = applied(mixed, mixedTx);
        const auto parsedMixed = SimaiParser::validateSyntax(mixedResult);
        bool strictAlternation = parsedMixed.noteMarkers.size() == 6;
        for (int i = 1; strictAlternation && i < 6; ++i)
            strictAlternation &= parsedMixed.noteMarkers.at(i - 1).handOrderHead != parsedMixed.noteMarkers.at(i).handOrderHead;
        expect(strictAlternation && mixedResult.contains("track=0:r|0.5:l"),
            "one-click alternation counts a star head and preserves its independently annotated track");
        const QString pair = QStringLiteral("(222){8}5~l,5$~r,E");
        const auto pairTx = controller.handOrderCommand(pair, 0, pair.size(), QStringLiteral("hand_order.alternate"));
        expect(applied(pair, pairTx) == QStringLiteral("(222){8}5~l,5$~l,E"),
            "one-click interaction keeps an isolated eighth-note rebound on one hand");
    }
    const QString slides = QStringLiteral("(120){4}1-3-7[1:1]~l,2~m,1-5[4:1]~r,E");
    TimelineQuickModel slideModel;
    slideModel.rebuildFromText(slides, 0.0);
    const auto slideRanges = slideModel.noteSourceRanges();
    const auto bisect = command(slides, {slideRanges.first(), slideRanges.last()}, QStringLiteral("hand_order.bisect_annotation"));
    const auto bisected = applied(slides, bisect);
    const auto bisectedParse = SimaiParser::parseForTimeline(bisected);
    expect(bisect.consumed && bisect.transaction.hasEdit && bisected.count(QStringLiteral("0.5:r")) == 2
        && bisected.contains(QStringLiteral("2~m")) && bisectedParse.annotationErrors.isEmpty()
        && sameNativeTiming(slides, bisected), "multi-range bisection updates both complete Slide objects in one transaction");
    for (const auto& operation : {QStringLiteral("hand_order.basic_layout"), QStringLiteral("hand_order.split_annotation"),
        QStringLiteral("hand_order.split_native_slide")}) {
        bool called = false;
        const auto blocked = miacode::ui::editSelectedNoteRanges(source, {tap, last}, operation,
            [&](const QString&, int, int) { called = true; return miacode::editor::SimaiTextEditResult{}; });
        expect(!called && !blocked.transaction.hasEdit
            && blocked.error == QStringLiteral("hand_order.object_selection_requires_contiguous"),
            "contextual hand commands explicitly reject discontiguous selections before any edit");
    }
    controller.handOrderCommand(source, tap.start, tap.end,
        QStringLiteral("hand_order.object_selection_requires_contiguous"));
    expect(!controller.handOrderFeedback().isEmpty() && !controller.handOrderSplitAvailable(),
        "rejected contextual selection announces feedback and leaves no interval draft active");

    const QString invalid = QStringLiteral("(120){4}1~[(999){32}],2~l,E");
    model.rebuildFromText(invalid, 0.0);
    const int second = invalid.indexOf(QStringLiteral("2~l"));
    const auto isolated = copySelectedNotes(invalid, {{second, second + 1}}, model.noteSourceRanges());
    expect(isolated.startsWith(QStringLiteral("|| 4/4\n(120){4}<HS*1>2~l")),
        "invalid annotation contents never masquerade as BPM or subdivision context");
    const auto invalidParse = SimaiParser::parseForTimeline(invalid);
    expect(invalidParse.ok && !invalidParse.annotationErrors.isEmpty() && invalidParse.sourceTokens.size() == 2,
        "invalid annotations keep native tokens and separate diagnostics for recovery");
    const QString cluster = QStringLiteral("(120){4}12~l/3~r,4~l,E");
    model.rebuildFromText(cluster, 0.0);
    const int clusterStart = cluster.indexOf(QStringLiteral("12~l"));
    const auto recovered = deleteSelectedNotes(cluster, {{clusterStart, clusterStart + 1}}, model.noteSourceRanges());
    expect(recovered.contains(QStringLiteral("3~r,4~l")) && recovered.count(',') == cluster.count(',')
        && SimaiParser::parseForTimeline(recovered).annotationErrors.isEmpty(),
        "source-length recovery never deletes a legal neighbour's suffix or beat separator");
    return failures ? 1 : 0;
}
