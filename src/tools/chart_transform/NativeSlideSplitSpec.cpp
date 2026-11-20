#include "core/chart/transform/NativeSlideSplit.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/scene/PreviewSfxTimeline.h"
#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>
#include <cmath>

using namespace miacode::chart_transform;
namespace {
SimaiParseResult parseNative(const QString& text) {
    auto parsed = SimaiParser::validateSyntax(text);
    std::sort(parsed.noteMarkers.begin(), parsed.noteMarkers.end(), [](const auto& a, const auto& b) {
        return a.parseOrder < b.parseOrder;
    });
    return parsed;
}
NativeSlideSplitPlan split(const QString& text, int begin = 0, int end = -1) {
    return planNativeSlideSplit(text, begin, end < 0 ? text.size() : end);
}
QString applied(const QString& text, const NativeSlideSplitPlan& plan) {
    QString result = text;
    for (int i = plan.patches.size() - 1; i >= 0; --i) {
        const auto& patch = plan.patches.at(i); result.replace(patch.start, patch.end - patch.start, patch.text);
    }
    return result;
}
void details(const NativeSlideSplitPlan& plan) {
    QTextStream out(stderr); out << "  blockers=" << plan.blockers.join(',') << '\n';
    for (const auto& item : plan.items) out << "  " << item.line << ':' << item.col << ' ' << item.reason
        << " before=" << item.before << " after=" << item.after << '\n';
    for (const auto& error : plan.diagnostics) out << "  diagnostic " << error.line << ':' << error.col << ' ' << error.message << '\n';
}
HandOrderHand at(const TimelineNoteMarker& marker, double second) {
    HandOrderHand result = HandOrderHand::Unspecified;
    const double progress = (second - marker.slideTraceSecond) / (marker.endSecond - marker.slideTraceSecond);
    for (const auto& stop : marker.handOrderTrackStops) if (stop.progress <= progress) result = stop.hand;
    return result;
}
miacode::hand_order::ChartInputAction touch(const QString& pad, double second) {
    miacode::hand_order::ChartInputAction input;
    auto& action = input.contact; action.kind = "slide"; action.start = second; action.end = second + .03;
    action.points = {miacode::hand_order::touchPadPoint(pad)}; action.times = {second}; return input;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message, const NativeSlideSplitPlan& plan = {}) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass;
        if (!pass) details(plan);
    };
    const QString chain = "(120){4}1-3-5[1:1],";
    const auto simple = split(chain); const auto result = applied(chain, simple);
    const auto before = parseNative(chain), after = parseNative(result);
    expect(simple.canApply && simple.changed == 1 && simple.patches.size() == 1
        && before.noteMarkers.size() == 1 && after.ok && after.annotationErrors.isEmpty() && after.noteMarkers.size() == 2,
        "a native two-segment chain becomes two strictly reparsed independent tracks", simple);
    if (after.noteMarkers.size() == 2) {
        const auto& old = before.noteMarkers.first();
        expect(after.noteMarkers.first().hasHeadStar && !after.noteMarkers.last().hasHeadStar
            && std::abs(after.noteMarkers.first().slideTraceSecond - old.slideTraceSecond) < 1e-8
            && std::abs(after.noteMarkers.last().slideTraceSecond - old.slideSegmentShootSeconds.at(1)) < 1e-8
            && std::abs(after.noteMarkers.last().endSecond - old.endSecond) < 1e-8
            && after.noteMarkers.first().slideSegmentPoints.first() == old.slideSegmentPoints.first()
            && after.noteMarkers.last().slideSegmentPoints.first() == old.slideSegmentPoints.last(),
            "first head/wait and both native geometries/absolute movement intervals are preserved without an extra beat", simple);
    }
    const QString shared = "(120){4}1-3-5[1:1]*-7-1[1:1],";
    const auto sharedPlan = split(shared); const auto sharedAfter = parseNative(applied(shared, sharedPlan));
    expect(sharedPlan.canApply && sharedPlan.beforeTracks == 2 && sharedPlan.afterTracks == 4
        && sharedPlan.beforeHeads == 1 && sharedPlan.afterHeads == 1 && sharedAfter.noteMarkers.size() == 4
        && sharedAfter.noteMarkers.at(0).hasHeadStar && sharedAfter.noteMarkers.at(1).hasHeadStar
        && !sharedAfter.noteMarkers.at(2).hasHeadStar && !sharedAfter.noteMarkers.at(3).hasHeadStar,
        "same-head branches keep one shared first-head group and add no later heads", sharedPlan);
    const QString mixed = "(120){4}1-3-5[1:1]*-7[2:1],";
    const auto mixedPlan = split(mixed);
    expect(mixedPlan.canApply && mixedPlan.beforeTracks == 2 && mixedPlan.afterTracks == 3
        && parseNative(applied(mixed, mixedPlan)).noteMarkers.size() == 3,
        "a single-segment sibling remains in the first shared group while its chained sibling is split", mixedPlan);
    const QString annotation = "(120){4}1-3-5[1:1]~[head=r;track=0:l|0.25:r|0.5:m|0.75:l],";
    const auto annotatedPlan = split(annotation); const auto annotationAfter = parseNative(applied(annotation, annotatedPlan));
    expect(annotatedPlan.canApply && annotationAfter.annotationErrors.isEmpty()
        && annotationAfter.noteMarkers.size() == 2 && annotationAfter.noteMarkers.first().handOrderHead == HandOrderHand::Right
        && annotationAfter.noteMarkers.first().handOrderTrackStops.size() == 2
        && annotationAfter.noteMarkers.first().handOrderTrackStops.last().progress == .5
        && annotationAfter.noteMarkers.last().handOrderTrackStops.first().hand == HandOrderHand::Either
        && annotationAfter.noteMarkers.last().handOrderTrackStops.last().progress == .5,
        "annotation nodes use absolute movement times, boundary-zero inheritance and internal remapping", annotatedPlan);
    const QString nearBoundary = "(120){4}1-3-5[1:1]~[track=0:l|0.49999999999999994:r|0.50000000000000011:m],";
    const auto nearPlan = split(nearBoundary); const auto nearAfter = parseNative(applied(nearBoundary, nearPlan));
    bool nearPreserved = nearPlan.canApply && nearAfter.annotationErrors.isEmpty() && nearAfter.noteMarkers.size() == 2;
    if (nearPreserved) {
        const auto& first = nearAfter.noteMarkers.first().handOrderTrackStops;
        const auto& second = nearAfter.noteMarkers.last().handOrderTrackStops;
        nearPreserved = first.size() == 2 && first.last().progress < 1 && first.last().progress > .999999
            && second.size() == 2 && second.first().hand == HandOrderHand::Right && second.last().progress > 0 && second.last().progress < 1e-10;
    }
    expect(nearPreserved, "adjacent representable nodes on either side of a native boundary survive without illegal u=1", nearPlan);
    const QString unequal = "(120){4}1-3-7[1:1]~[track=0:l|0.5:r|0.8:m],";
    const auto unequalPlan = split(unequal); const auto unequalBefore = parseNative(unequal);
    const auto unequalAfter = parseNative(applied(unequal, unequalPlan));
    bool sameHands = unequalPlan.canApply && unequalAfter.noteMarkers.size() == 2;
    if (sameHands) for (const auto& marker : unequalAfter.noteMarkers)
        for (double u : {.05, .4, .9}) {
            const double second = marker.slideTraceSecond + u * (marker.endSecond - marker.slideTraceSecond);
            sameHands &= at(marker, second) == at(unequalBefore.noteMarkers.first(), second);
        }
    expect(sameHands, "unequal native segment durations retain the original time-dependent hand function", unequalPlan);
    const QString tiny = "(120){4}1-3-7[0##0.0002],";
    const auto tinyPlan = split(tiny); const auto tinyBefore = parseNative(tiny);
    const auto tinyAfter = parseNative(applied(tiny, tinyPlan));
    expect(tinyPlan.canApply && tinyAfter.noteMarkers.size() == 2
        && tinyBefore.noteMarkers.first().slideSegmentDurations.first() < .001
        && std::abs(tinyAfter.noteMarkers.first().endSecond - tinyAfter.noteMarkers.first().slideTraceSecond - .001) < 1e-10
        && std::abs(tinyAfter.noteMarkers.last().endSecond - tinyBefore.noteMarkers.first().endSecond) < 1e-10,
        "tiny segments preserve real shoot/end intervals including the parser's minimum-duration clamp", tinyPlan);
    for (const QString prefix : {QStringLiteral("1@bx"), QStringLiteral("1m"), QStringLiteral("1?"), QStringLiteral("1!")}) {
        const QString text = "(120){4}" + prefix + "-3-5mb[1:1],";
        const auto plan = split(text); const auto parsed = parseNative(applied(text, plan));
        const auto original = parseNative(text);
        bool preserved = plan.canApply && parsed.noteMarkers.size() == 2 && original.noteMarkers.size() == 1;
        if (preserved) {
            const auto& old = original.noteMarkers.first(); const auto& first = parsed.noteMarkers.first(); const auto& last = parsed.noteMarkers.last();
            preserved &= first.headBreak == old.headBreak && first.headEx == old.headEx && first.headMine == old.headMine
                && first.slideHeadUsesTapMaterial == old.slideHeadUsesTapMaterial && first.hasHeadStar == old.hasHeadStar
                && first.headlessImmediate == old.headlessImmediate && last.headlessImmediate == old.headlessImmediate
                && first.trackBreak && last.trackBreak && first.trackMine && last.trackMine
                && !last.hasHeadStar && !last.headBreak && !last.headEx && !last.headMine;
        }
        expect(preserved, "head material/Break/EX/Mine and original ?/! style survive once while track Break/Mine are inherited", plan);
    }
    const QString timing = "(90){8},,<HS*2>1-3-5[0.25##1.2345]`2,(180)4,";
    const auto timingPlan = split(timing);
    expect(timingPlan.canApply && timingPlan.ignored == 2
        && parseNative(applied(timing, timingPlan)).noteMarkers.first().hsMultiplier == 2,
        "BPM, leading time offset, HS and backtick-Each context strictly reparse without changing surrounding notes", timingPlan);
    const QString unicode = QStringLiteral("(120){4}|| 中文😀\r\n1-3-5[1:1]~l,\r\nE");
    const int begin = unicode.indexOf("1-3"), end = unicode.indexOf(',', begin);
    const auto unicodePlan = split(unicode, begin, end); const auto unicodeAfter = applied(unicode, unicodePlan);
    expect(unicodePlan.canApply && unicodePlan.items.first().line == 2 && unicodePlan.items.first().col == 1
        && unicodeAfter.left(begin) == unicode.left(begin) && unicodeAfter.endsWith(",\r\nE")
        && unicodeAfter.count("\r\n") == unicode.count("\r\n"),
        "UTF-16 selection coordinates retain Unicode comments, CRLF and all unselected bytes", unicodePlan);
    const QString partial = "(120){4}1-3-5[1:1]/2-4-6[1:1],";
    const int partialEnd = partial.indexOf("2-4") + 3;
    const auto partialPlan = split(partial, 0, partialEnd);
    expect(partialPlan.canApply && partialPlan.partial == 1 && partialPlan.changed == 1
        && applied(partial, partialPlan).contains("/2-4-6[1:1],"),
        "a partially selected token is reported and skipped while complete reliable chains can be applied", partialPlan);
    const auto withTap = split("(120){4}2/1-3-5[1:1],");
    expect(withTap.canApply && withTap.ignored == 1 && withTap.changed == 1,
        "ordinary selected notes are untouched non-targets rather than blocking a selected chain", withTap);
    for (const QString text : {QStringLiteral("(120){4}1-3-5[1:1]/1-5[1:1],"),
            QStringLiteral("(120){4}1-3-5[1:1]/1w5[1:1],"), QStringLiteral("(120){4}1-3-5[1:1]*w5[1:1],"),
            QStringLiteral("(120){4}1-3-5[1:1]~[track=0:l|1:r],"), QStringLiteral("(120){4}1-3-5[1:1],9,")}) {
        const auto plan = split(text);
        expect(!plan.canApply && plan.patches.isEmpty() && plan.replacement == text && !plan.blockers.isEmpty(),
            "a selected single slide, Wifi, invalid annotation or native syntax error blocks the entire batch", plan);
    }
    const QString distantError = "(120){4}1-3-5[1:1],2~z,";
    const int chainEnd = distantError.indexOf(','); const auto distantPlan = split(distantError, 0, chainEnd);
    const auto distantResult = parseNative(applied(distantError, distantPlan));
    expect(distantPlan.canApply && applied(distantError, distantPlan).endsWith(",2~z,")
        && distantResult.errors.isEmpty() && distantResult.annotationErrors.size() == 1,
        "an existing unselected annotation error is preserved and position-remapped without becoming a target failure", distantPlan);
    const QString semantic = "(120){4}1-3-5b[1:2],";
    const auto semanticPlan = split(semantic); const auto semanticBefore = parseNative(semantic);
    const auto semanticAfter = parseNative(applied(semantic, semanticPlan));
    QVector<miacode::preview_sfx_timeline::Event> beforeSfx, afterSfx;
    QVector<miacode::preview_sfx_timeline::TouchholdSpan> beforeSpans, afterSpans;
    miacode::preview_sfx_timeline::buildTimeline(semanticBefore.noteMarkers, 1, {}, &beforeSfx, &beforeSpans);
    miacode::preview_sfx_timeline::buildTimeline(semanticAfter.noteMarkers, 1, {}, &afterSfx, &afterSpans);
    const auto countKind = [](const auto& events, const QString& kind) { int count = 0; for (const auto& event : events) count += event.kind == kind; return count; };
    expect(semanticPlan.canApply && semanticPlan.items.first().changes.contains("break_count_scoring_changes")
        && countKind(beforeSfx, "break_slide_start") == 1 && countKind(afterSfx, "break_slide_start") == 2
        && countKind(beforeSfx, "judge_break_slide") == 1 && countKind(afterSfx, "judge_break_slide") == 2,
        "identical movement geometry adds an independent Break start/tail SFX and exposes changed count/scoring risk", semanticPlan);
    if (semanticPlan.canApply) {
        const auto& marker = semanticBefore.noteMarkers.first();
        const double duration = marker.slideSegmentShootSeconds.at(1) - marker.slideTraceSecond;
        const double lastDuration = marker.endSecond - marker.slideSegmentShootSeconds.at(1);
        const double critical = marker.endSecond - (1 - marker.slideSegmentCriticalProportions.last()) * lastDuration;
        QVector<miacode::hand_order::ChartInputAction> inputs;
        miacode::hand_order::ChartInputAction head; head.button = true; head.lane = 1; head.contact.hand = 1;
        head.contact.start = marker.second; head.contact.end = marker.second + .03; inputs.append(head);
        inputs += {touch("A1", marker.slideTraceSecond), touch("B2", marker.slideTraceSecond + duration * .1),
            touch("A3", marker.slideTraceSecond + duration * .2), touch("B4", critical - .2), touch("A5", critical)};
        const auto originalJudge = miacode::hand_order::replayHandOrderChart(semanticBefore.noteMarkers, inputs);
        const auto splitJudge = miacode::hand_order::replayHandOrderChart(semanticAfter.noteMarkers, inputs);
        expect(originalJudge.meetsTarget && !splitJudge.meetsTarget && splitJudge.judgements.size() == 2,
            "same real contacts can critically finish the original chain while early-clearing a new independent first segment", semanticPlan);
    }
    const auto cancelled = std::make_shared<std::atomic_bool>(true); bool stopped = false;
    try { miacode::task::CancellationScope scope(cancelled); split(chain); }
    catch (const miacode::task::Cancelled&) { stopped = true; }
    expect(stopped, "native split planning participates in worker cancellation before parsing");
    return ok ? 0 : 1;
}
