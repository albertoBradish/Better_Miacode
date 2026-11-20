#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/analysis/hand_order/HandOrderConflictReview.h"
#include "core/analysis/hand_order/HandPlayContacts.h"
#include "core/analysis/hand_order/HandPlayPose.h"
#include "core/analysis/MuriSlideReferenceData.h"
#include "core/chart/SlideReferenceData.h"
#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QTextStream>
#include <cmath>
#include <tuple>

using namespace miacode::hand_order;
namespace {
TimelineNoteMarker note(QString type, double second, int order = 0, int lane = 1, QString pad = {}) {
    TimelineNoteMarker marker; marker.type = type; marker.second = second; marker.parseOrder = order;
    marker.lane = lane; marker.touchPad = pad; marker.endSecond = second;
    return marker;
}
ChartInputAction screen(QString pad, double start, double end, int hand = 0, int token = 1, QString kind = "touch") {
    ChartInputAction input;
    auto& action = input.contact; action.tokenStart = token; action.hand = hand; action.kind = kind;
    action.start = start; action.end = end; action.motionStart = start; action.motionEnd = end;
    action.points = {touchPadPoint(pad)}; action.times = {start}; return input;
}
ChartInputAction button(int lane, double start, double end, int hand = 0) {
    ChartInputAction input; input.button = true; input.lane = lane;
    input.contact.start = start; input.contact.end = end; input.contact.hand = hand; input.contact.kind = "button";
    return input;
}
TimelineNoteMarker slide(QString key = "1-5", int order = 0) {
    auto marker = note("slide", 0, order);
    marker.hasHeadStar = false; marker.slideTrackKey = key; marker.slideSegmentKeys = {key};
    marker.slideTraceSecond = 0; marker.endSecond = 2;
    marker.slideSegmentDurations = {2}; marker.slideSegmentCriticalProportions = {1}; return marker;
}
QVector<ChartInputAction> straight(double final = 2) {
    return {screen("A1", 0, .02, 0, 1, "slide"), screen("B1", .2, .22, 0, 1, "slide"),
        screen("C", .4, .42, 0, 1, "slide"), screen("B5", final - .2, final - .18, 0, 1, "slide"),
        screen("A5", final, final + .02, 0, 1, "slide")};
}
TimelineNoteMarker wifi() {
    auto marker = note("wifi", 0); marker.hasHeadStar = false; marker.slideTrackKey = "1w5";
    marker.slideTraceSecond = 0; marker.endSecond = 2; marker.wifiCriticalProportion = 1; return marker;
}
QVector<ChartInputAction> wifiInputs(bool includeC = true) {
    QVector<ChartInputAction> actions{screen("A1", 0, .02), screen("B2", .2, .22), screen("B1", .3, .32),
        screen("B8", .4, .42), screen("B3", .6, .62), screen("B7", .7, .72),
        screen("A4", 1.9, 1.92), screen("A6", 1.95, 1.97), screen("A5", 2, 2.02)};
    if (includeC) actions.append(screen("C", .5, .52)); return actions;
}
QVector<ChartInputAction> nativeInputs(const TimelineNoteMarker& marker, bool truncate = false, bool referenceRealPath = false) {
    QVector<ChartInputAction> inputs;
    if (marker.hasHeadStar) inputs.append(button(marker.lane, marker.second, marker.second + .025, 1));
    if (marker.type == "wifi") {
        const auto paths = miacode::muri::detail::loadRuntimeWifiActionPaths(marker.slideTrackKey);
        for (int hand = 0; hand < paths.size(); ++hand) {
            ChartInputAction input; auto& action = input.contact;
            action.hand = hand; action.kind = "wifi"; action.start = action.motionStart = marker.slideTraceSecond;
            action.end = action.motionEnd = marker.endSecond; action.radius = 50.0 / 240.0;
            for (int i = 0; i < paths.at(hand).size(); ++i) {
                action.points.append(paths.at(hand).at(i) / 240.0);
                action.times.append(action.start + (marker.endSecond - action.start) * i / (paths.at(hand).size() - 1));
            }
            if (truncate) action.end = action.start + (marker.endSecond - action.start) * .1;
            inputs.append(input);
        }
    } else {
        ChartInputAction input; auto& action = input.contact;
        action.kind = "slide"; action.start = action.motionStart = marker.slideTraceSecond;
        action.end = action.motionEnd = marker.endSecond;
        for (int segment = 0; segment < marker.slideSegmentPoints.size(); ++segment) {
            const auto points = referenceRealPath
                ? miacode::muri::detail::loadRuntimeSlideActionPath(marker.slideSegmentKeys.at(segment))
                : marker.slideSegmentPoints.at(segment);
            const double first = marker.slideSegmentShootSeconds.at(segment), duration = marker.slideSegmentDurations.at(segment);
            for (int i = 0; i < points.size(); ++i) {
                const double second = first + duration * i / (points.size() - 1);
                if (!action.times.isEmpty() && second <= action.times.last() + 1e-8) {
                    action.points.last() = points.at(i) / 240.0; continue;
                }
                action.points.append(points.at(i) / 240.0); action.times.append(second);
            }
        }
        if (truncate) action.end = action.start + (marker.endSecond - action.start) * .1;
        inputs.append(input);
    }
    return inputs;
}
void dumpNativeFailure(const QString& token, const FullReplay& replay, bool realPath) {
    QTextStream out(stderr);
    out << "  native=" << token << " real_path=" << realPath << " valid=" << replay.valid << " error=" << replay.error << '\n';
    for (const auto& judgement : replay.judgements) {
        out << "    " << judgement.parseOrder << ' ' << judgement.reason << " track=" << judgement.trackCompleted
            << " head=" << judgement.headCompleted << " delta=" << judgement.hitDelta << " margin=" << judgement.margin << " missed=";
        for (int i = 0; i < judgement.areas.size(); ++i) if (!judgement.areas.at(i).hit) out << i << ',';
        out << '\n';
    }
}
bool sameNumber(double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::abs(a - b) < 1e-8; }
bool sameCauses(const QVector<BasicCause>& a, const QVector<BasicCause>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        const auto& x = a.at(i); const auto& y = b.at(i);
        if (std::tie(x.tokenStart, x.branch, x.kind, x.pad, x.handMask) != std::tie(y.tokenStart, y.branch, y.kind, y.pad, y.handMask)
            || !sameNumber(x.second, y.second) || !sameNumber(x.progress, y.progress)) return false;
    }
    return true;
}
bool sameAreas(const QVector<FullReplayArea>& a, const QVector<FullReplayArea>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        const auto& x = a.at(i); const auto& y = b.at(i);
        if (std::tie(x.hit, x.skipped, x.pad) != std::tie(y.hit, y.skipped, y.pad)
            || !sameNumber(x.second, y.second) || !sameCauses(x.causes, y.causes)) return false;
    }
    return true;
}
bool sameJudgement(const FullReplayJudgement& a, const FullReplayJudgement& b) {
    if (std::tie(a.parseOrder, a.supported, a.judged, a.completed, a.highQuality, a.meetsTarget, a.sideEffect,
            a.headRequired, a.headJudged, a.headCompleted, a.headHighQuality, a.trackCompleted, a.trackHighQuality,
            a.handMask, a.singleHandCompletionMask, a.kind, a.quality, a.reason, a.inputChannel)
        != std::tie(b.parseOrder, b.supported, b.judged, b.completed, b.highQuality, b.meetsTarget, b.sideEffect,
            b.headRequired, b.headJudged, b.headCompleted, b.headHighQuality, b.trackCompleted, b.trackHighQuality,
            b.handMask, b.singleHandCompletionMask, b.kind, b.quality, b.reason, b.inputChannel)) return false;
    for (const auto& values : {std::pair{a.second, b.second}, {a.margin, b.margin}, {a.hitDelta, b.hitDelta},
            {a.headSecond, b.headSecond}, {a.headMargin, b.headMargin}, {a.headHitDelta, b.headHitDelta},
            {a.heldSeconds, b.heldSeconds}, {a.requiredHoldSeconds, b.requiredHoldSeconds}, {a.heldRatio, b.heldRatio},
            {a.rawHeldSeconds, b.rawHeldSeconds}, {a.releasedSeconds, b.releasedSeconds}, {a.wifiPadCSecond, b.wifiPadCSecond}})
        if (!sameNumber(values.first, values.second)) return false;
    if (!sameCauses(a.causes, b.causes) || !sameCauses(a.headCauses, b.headCauses) || !sameAreas(a.areas, b.areas)
        || a.sourceParseOrders != b.sourceParseOrders || a.headSourceParseOrders != b.headSourceParseOrders
        || a.segmentStartAreas != b.segmentStartAreas || a.segmentEndAreas != b.segmentEndAreas
        || a.wifiProgress != b.wifiProgress || a.wifiAreas.size() != b.wifiAreas.size()
        || a.segmentCompletedSeconds.size() != b.segmentCompletedSeconds.size()) return false;
    for (int i = 0; i < a.wifiAreas.size(); ++i) if (!sameAreas(a.wifiAreas.at(i), b.wifiAreas.at(i))) return false;
    for (int i = 0; i < a.segmentCompletedSeconds.size(); ++i)
        if (!sameNumber(a.segmentCompletedSeconds.at(i), b.segmentCompletedSeconds.at(i))) return false;
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass;
    };
    const auto tap = note("tap", 0);
    const auto independent = replayHandOrderChart({tap, note("tap", 0, 1)},
        {button(1, 0, .03), screen("A1", 0, .03, 1)});
    expect(independent.meetsTarget && independent.judgements.size() == 2,
        "button and A screen are independent consumable channels");
    auto touchA = note("touch", 0, 1, 1, "A1");
    const QVector<ChartInputAction> twoChannels{button(1, 0, .03), screen("A1", 0, .03, 1)};
    const auto buttonThenScreen = replayHandOrderChart({tap, touchA}, twoChannels);
    expect(buttonThenScreen.meetsTarget && buttonThenScreen.judgements.first().inputChannel == "Button1"
        && buttonThenScreen.judgements.last().inputChannel == "A1",
        "same-tick button batch serves Tap before the separate A DOWN serves Touch");
    auto laterTapOrder = tap; laterTapOrder.parseOrder = 1; touchA.parseOrder = 0;
    const auto reversedChannels = replayHandOrderChart({touchA, laterTapOrder}, twoChannels);
    expect(reversedChannels.meetsTarget && reversedChannels.judgements.first().inputChannel == "A1"
        && reversedChannels.judgements.last().inputChannel == "Button1",
        "Tap and same-lane Touch both complete when their source order is reversed");
    const auto onlyTap = replayHandOrderChart({tap}, twoChannels);
    expect(onlyTap.meetsTarget && onlyTap.judgements.first().causes.size() == 1
        && onlyTap.judgements.first().inputChannel == "Button1",
        "one Tap consumes its first matching channel and cannot be judged twice by button plus A");
    auto duplicateA = touchA; duplicateA.parseOrder = 1;
    const auto repeatedPad = replayHandOrderChart({touchA, duplicateA}, twoChannels);
    expect(repeatedPad.judgements.first().completed && !repeatedPad.judgements.last().completed
        && !repeatedPad.meetsTarget,
        "one A DOWN cannot directly judge duplicate Touch members and button cannot donate another screen event");
    const auto unionOn = replayHandOrderChart({tap, note("tap", .1, 1)},
        {button(1, 0, .12), button(1, .1, .3, 1)});
    expect(unionOn.judgements.first().highQuality && !unionOn.judgements.last().completed,
        "a second hand joining an ON button cannot manufacture a fresh DOWN");
    auto touch = note("touch", 0, 0, 1, "B1");
    expect(!replayHandOrderChart({touch}, {button(1, 0, .03)}).meetsTarget,
        "exterior button inputs never create screen coverage");
    const auto inconsistent = replayHandOrderChart({touch},
        {screen("B1", 0, .03), screen("B5", 0, .03)});
    expect(!inconsistent.valid && inconsistent.error == "inconsistent_same_hand_screen_contacts",
        "one hand cannot supply simultaneous contacts at two different palm centers");
    auto broadPalm = screen("C", 0, .03);
    broadPalm.contact.points = {touchPadPoint("B1") * .75};
    const auto centerTouch = note("touch", 0, 0, 1, "C");
    const bool normalMisses = !replayHandOrderChart({centerTouch}, {broadPalm}).meetsTarget;
    broadPalm.contact.radius = 50.0 / 240.0;
    expect(normalMisses && replayHandOrderChart({centerTouch}, {broadPalm}).meetsTarget,
        "each actual contact uses its explicit finite palm radius, including Wifi templates");
    broadPalm.contact.radius = 1;
    expect(!replayHandOrderChart({centerTouch}, {broadPalm}).valid,
        "an entire-screen circle is outside the audited one-palm templates");
    const auto badTap = replayHandOrderChart({note("tap", 1)}, {button(1, .9, .93)});
    expect(badTap.judgements.first().completed && badTap.judgements.first().quality == "bad"
        && std::abs(badTap.judgements.first().hitDelta + .1) < 1e-8 && !badTap.meetsTarget,
        "accepted early tap returns actual delta and bad community grade");
    auto hold = note("hold", 0); hold.endSecond = 1;
    const auto held = replayHandOrderChart({hold}, {button(1, 0, 1)});
    expect(held.meetsTarget && std::abs(held.judgements.first().heldRatio - 1) < 1e-8
        && held.judgements.first().singleHandCompletionMask == 1,
        "hold validates the full sustained channel rather than only its head");
    const auto gap = replayHandOrderChart({hold}, {button(1, 0, .3), button(1, .6, 1)});
    expect(!gap.meetsTarget && gap.judgements.first().reason == "hold_contact_gap"
        && gap.judgements.first().heldRatio > .56 && gap.judgements.first().heldRatio < .58,
        "hold gaps remain actual failures despite a valid critical head");
    expect(replayHandOrderChart({hold}, {button(1, 0, .8)}).meetsTarget
        && replayHandOrderChart({hold}, {button(1, 0, .03), button(1, .1, .8)}).meetsTarget,
        "Hold ignores the first six and last twelve 60Hz frames of its body");
    const auto shortOff = replayHandOrderChart({hold}, {button(1, 0, .3), button(1, .3 + 8.0 / 180.0, 1)});
    expect(shortOff.meetsTarget && shortOff.judgements.first().releasedSeconds == 0
        && shortOff.judgements.first().rawHeldSeconds < shortOff.judgements.first().heldSeconds,
        "an OFF run shorter than three frames remains pressed in the Hold state machine");
    const auto exactOff = replayHandOrderChart({hold}, {button(1, 0, .3), button(1, .35, 1)});
    expect(!exactOff.meetsTarget && std::abs(exactOff.judgements.first().releasedSeconds - .05) < 1e-8,
        "three full OFF frames count the complete release run including its debounce prefix");
    const auto channelTransfer = replayHandOrderChart({hold}, {button(1, 0, .5), screen("A1", .4, 1, 1)});
    expect(channelTransfer.meetsTarget && channelTransfer.judgements.first().handMask == 3
        && channelTransfer.judgements.first().singleHandCompletionMask == 0,
        "actual complementary Hold handoff passes gameplay but neither hand can own its whole annotation");
    const auto redundantBrush = replayHandOrderChart({hold}, {button(1, 0, 1), screen("A1", .3, .5, 1)});
    expect(redundantBrush.meetsTarget && redundantBrush.judgements.first().handMask == 3
        && redundantBrush.judgements.first().singleHandCompletionMask == 1
        && redundantBrush.judgements.first().causes.size() >= 2,
        "redundant opposite-hand brush retains all causes while the original hand alone completes Hold");
    const auto bothSufficient = replayHandOrderChart({hold}, {screen("A1", 0, 1), screen("A1", 0, 1, 1)});
    expect(bothSufficient.meetsTarget && bothSufficient.judgements.first().headCauses.size() == 2
        && bothSufficient.judgements.first().singleHandCompletionMask == 3,
        "simultaneous real head contributors can each independently satisfy the complete Hold body");
    const auto noHeadResurrection = replayHandOrderChart({hold}, {screen("A1", -.2, 1, 1), screen("A1", 0, 1)});
    expect(!noHeadResurrection.meetsTarget && !noHeadResurrection.judgements.first().headCompleted
        && noHeadResurrection.judgements.first().singleHandCompletionMask == 0,
        "single-hand body shadows cannot resurrect a head DOWN suppressed by the real prior ON state");
    auto shortHold = hold; shortHold.endSecond = .2;
    expect(replayHandOrderChart({shortHold}, {button(1, 0, .03)}).meetsTarget,
        "short Hold with no effective body follows its head judgement");
    auto shortBodyHold = hold; shortBodyHold.endSecond = .32;
    const auto untouchedShortBody = replayHandOrderChart({shortBodyHold}, {});
    expect(!untouchedShortBody.judgements.first().completed && untouchedShortBody.judgements.first().quality == "miss"
        && untouchedShortBody.judgements.first().heldSeconds == 0,
        "initial OFF in a never-touched short effective body cannot fabricate a held Late Good result");
    expect(replayHandOrderChart({shortBodyHold}, {button(1, 0, .03)}).meetsTarget,
        "a real critical head seeds the held state before its sub-three-frame effective-body release");
    auto exTap = note("tap", 1); exTap.isEx = true;
    expect(replayHandOrderChart({exTap}, {button(1, .9, .93)}).meetsTarget,
        "EX protection promotes every accepted tap head within the available window");
    auto exHold = hold; exHold.isEx = true;
    expect(replayHandOrderChart({exHold}, {button(1, .1, 1)}).meetsTarget
        && !replayHandOrderChart({exHold}, {button(1, .1, .3), button(1, .5, 1)}).meetsTarget,
        "EX Hold protects its head and never protects a released effective body");
    const auto missedHoldHead = replayHandOrderChart({hold}, {button(1, .2, 1)});
    expect(missedHoldHead.judgements.first().completed && !missedHoldHead.judgements.first().headCompleted
        && missedHoldHead.judgements.first().quality == "bad" && !missedHoldHead.meetsTarget,
        "a missed Hold head with a held body gets an actual non-Miss bad result");
    auto touchHold = note("touch_hold", 0, 0, 1, "B1"); touchHold.endSecond = 1;
    expect(replayHandOrderChart({touchHold}, {screen("B1", 0, 1)}).meetsTarget
        && !replayHandOrderChart({touchHold}, {screen("B1", 0, .03)}).meetsTarget,
        "Touch Hold requires sustained screen contact and is not a group member");
    const auto touchHoldBrush = replayHandOrderChart({touchHold}, {screen("B1", 0, 1), screen("B1", .3, .5, 1)});
    const auto touchHoldRelay = replayHandOrderChart({touchHold}, {screen("B1", 0, .5), screen("B1", .4, 1, 1)});
    expect(touchHoldBrush.meetsTarget && touchHoldBrush.judgements.first().handMask == 3
        && touchHoldBrush.judgements.first().singleHandCompletionMask == 1
        && touchHoldRelay.meetsTarget && touchHoldRelay.judgements.first().singleHandCompletionMask == 0,
        "Touch Hold shadows distinguish redundant sensor contributors from a necessary complementary relay");
    expect(replayHandOrderChart({touchHold}, {screen("B1", 0, .03), screen("B1", .25, .8)}).meetsTarget,
        "Touch Hold ignores fifteen head frames and twelve tail frames");
    auto shortTouchHold = touchHold; shortTouchHold.endSecond = .4;
    expect(replayHandOrderChart({shortTouchHold}, {screen("B1", 0, .03)}).meetsTarget,
        "short Touch Hold with no effective body follows its head judgement");
    auto shortTouchBody = touchHold; shortTouchBody.endSecond = .47;
    expect(replayHandOrderChart({shortTouchBody}, {}).judgements.first().quality == "miss"
        && replayHandOrderChart({shortTouchBody}, {screen("B1", 0, .03)}).meetsTarget,
        "Touch Hold distinguishes initial OFF from a short release after an actual head input");
    FullReplayOptions prefix; prefix.throughSecond = .5;
    const auto partialHold = replayHandOrderChart({hold}, {button(1, 0, 1)}, prefix);
    expect(!partialHold.judgements.first().judged && partialHold.judgements.first().quality == "pending"
        && std::abs(partialHold.judgements.first().heldSeconds - .4) < 1e-8,
        "finite prefix returns pending hold and accumulated duration");
    for (int n = 2; n <= 5; ++n) {
        QVector<TimelineNoteMarker> markers; QVector<ChartInputAction> actions;
        for (int i = 0; i < n; ++i) {
            auto member = note("touch", 0, i, 1, QStringLiteral("B%1").arg(i + 1)); member.eachGroupId = 1;
            markers.append(member);
        }
        const int threshold = int(std::ceil(.51 * n));
        for (int i = 0; i < threshold; ++i) actions.append(screen(markers.at(i).touchPad, .02 * i, .02 * i + .01, i % 2, i));
        const auto group = replayHandOrderChart(markers, actions);
        expect(group.meetsTarget && group.touchGroups.size() == 1,
            "N=2..5 Touch groups require 2,2,3,3 distinct real judgements");
        if (n > threshold) expect(group.judgements.last().kind == "group_carry"
            && group.judgements.last().handMask == 3 && group.judgements.last().causes.size() == threshold,
            "group carry retains all real contributors and no synthetic screen event");
    }
    const auto line = replayHandOrderChart({slide()}, straight());
    expect(line.meetsTarget && line.judgements.first().areas.size() == 5
        && line.judgements.first().areas.last().hit,
        "ordinary slide advances every native queue area from supplied screen contacts");
    const auto early = replayHandOrderChart({slide()}, straight(.8));
    expect(early.judgements.first().trackCompleted && !early.meetsTarget
        && early.judgements.first().reason == "slide_outside_target_window",
        "self-sourced early slide clear has no Muri autoplay quality exemption");
    const auto skipped = replayHandOrderChart({slide()}, {screen("B1", .2, .22), screen("B5", 1.8, 1.82), screen("A5", 2, 2.02)});
    expect(skipped.meetsTarget && skipped.judgements.first().areas.first().skipped
        && skipped.judgements.first().areas.at(2).skipped,
        "long slide skip rules preserve explicit skipped-area audit records");
    const auto noRelease = replayHandOrderChart({slide("1-3")}, {screen("A1", 0, 3), screen("A3", 2, 2.02, 1)});
    expect(!noRelease.meetsTarget && !noRelease.judgements.first().trackCompleted,
        "short slide cannot skip its unhit protected penultimate area");
    auto chain = slide(); chain.slideSegmentKeys = {"1-5", "5-1"};
    chain.slideSegmentDurations = {1, 1}; chain.slideSegmentCriticalProportions = {1, 1};
    auto chainInputs = straight(1);
    chainInputs += {screen("B5", 1.2, 1.22), screen("C", 1.4, 1.42), screen("B1", 1.8, 1.82), screen("A1", 2, 2.02)};
    const auto chained = replayHandOrderChart({chain}, chainInputs);
    expect(chained.meetsTarget && chained.judgements.first().areas.size() == 9
        && chained.judgements.first().segmentStartAreas == QVector<int>({0, 4})
        && chained.judgements.first().segmentEndAreas == QVector<int>({4, 8}),
        "native chain shares its common area and preserves both segment boundaries");
    auto headed = slide(); headed.hasHeadStar = true;
    auto duplicate = headed; duplicate.parseOrder = 1; duplicate.slideBranchIndex = 1;
    auto headedActions = straight(); headedActions.append(button(1, 0, .03));
    const auto sharedHead = replayHandOrderChart({headed, duplicate}, headedActions);
    expect(sharedHead.meetsTarget && sharedHead.judgements.last().headHighQuality,
        "same native star head consumes one button event for all its logical branches");
    // Remove initial A1 too, since an intentional screen A1 can judge the star.
    auto missedHeadActions = straight(); missedHeadActions.removeFirst();
    const auto missedHead = replayHandOrderChart({headed}, missedHeadActions);
    expect(missedHead.judgements.first().trackCompleted && !missedHead.judgements.first().headCompleted
        && !missedHead.meetsTarget,
        "completed slide queues cannot conceal a missed independent star head");
    auto future = slide(); future.second = 1; future.slideTraceSecond = 1.5; future.endSecond = 3;
    FullReplayOptions earlyPrefix; earlyPrefix.throughSecond = 1;
    const auto priorOn = replayHandOrderChart({future}, {screen("A1", -1000, 2)}, earlyPrefix);
    expect(priorOn.judgements.first().areas.first().hit && !priorOn.judgements.first().judged
        && priorOn.judgements.first().areas.first().second >= 1 - FullReplayProfile::slideLeadingSeconds - 1e-8,
        "slide availability restores genuine pre-existing ON state and retains pending progress");
    FullReplayOptions olderWifi; olderWifi.wifiNeedC = true;
    const auto w = replayHandOrderChart({wifi()}, wifiInputs(), olderWifi);
    expect(w.meetsTarget && w.judgements.first().wifiAreas.size() == 3
        && w.judgements.first().wifiProgress == QVector<int>({4, 4, 4})
        && std::isfinite(w.judgements.first().wifiPadCSecond),
        "Wifi has three independent queues, OR alternatives, and an actual C release");
    const auto noC = replayHandOrderChart({wifi()}, wifiInputs(false), olderWifi);
    expect(!noC.meetsTarget && noC.judgements.first().reason == "wifi_c_not_released",
        "Wifi C rule cannot be replaced by completion of its three visible lanes");
    expect(replayHandOrderChart({wifi()}, wifiInputs(false)).meetsTarget,
        "default DX profile does not impose the older Wifi C release requirement");
    const auto withinGrace = replayHandOrderChart({slide()}, straight(2.5));
    const auto beyondGrace = replayHandOrderChart({slide()}, straight(2.7));
    expect(withinGrace.judgements.first().trackCompleted && !withinGrace.meetsTarget
        && !beyondGrace.judgements.first().trackCompleted,
        "community slide grace is six tenths of a second and never a 24-hour autoplay exemption");
    // These inputs are explicit caller-created physical paths from native
    // parser data. They do not call the Muri autoplay action generator.
    const auto reference = miacode::slide_reference::root();
    const auto nativeSlides = reference.value("slides").toObject();
    int nativeCount = 0, nativeFailures = 0, nativeTruncations = 0;
    for (auto it = nativeSlides.constBegin(); it != nativeSlides.constEnd(); ++it) {
        const QString token = it.key() + QStringLiteral("[2:1]");
        const auto parsed = SimaiParser::validateSyntax(QStringLiteral("(120){4}") + token + ',');
        if (!parsed.ok || parsed.noteMarkers.size() != 1) { ++nativeFailures; QTextStream(stderr) << "  native parser fixture failed: " << token << '\n'; continue; }
        const auto& marker = parsed.noteMarkers.first(); ++nativeCount;
        const auto replay = replayHandOrderChart(parsed.noteMarkers, nativeInputs(marker));
        if (!replay.meetsTarget) {
            ++nativeFailures; dumpNativeFailure(token, replay, false);
            // Diagnose reference real-path differences without silently changing
            // the asserted native trajectory or weakening its target quality.
            dumpNativeFailure(token, replayHandOrderChart(parsed.noteMarkers, nativeInputs(marker, false, true)), true);
        }
        if (!replayHandOrderChart(parsed.noteMarkers, nativeInputs(marker, true)).meetsTarget) ++nativeTruncations;
    }
    expect(nativeCount == nativeSlides.size() && nativeFailures == 0,
        "every bundled native slide key replays its parsed physical trajectory with an independent button head");
    expect(nativeTruncations == nativeSlides.size(),
        "cutting every native trajectory after its first tenth cannot pass the target");
    QTextStream(stdout) << "MATRIX: " << nativeCount << " native slide keys; " << nativeFailures << " failures; " << nativeTruncations << " negative truncations\n";
    const QStringList chains{"1-3-7", "8-6-2", "1^2^3^4", "8<7<6<5", "1p3q5pp7qq1", "1V73q4", "8V26p5"};
    for (const auto& shape : chains) {
        const QString token = shape + QStringLiteral("[2:1]");
        const auto parsed = SimaiParser::validateSyntax(QStringLiteral("(120){4}") + token + ',');
        bool complete = parsed.ok && parsed.noteMarkers.size() == 1, cutFails = false;
        if (complete) {
            const auto replay = replayHandOrderChart(parsed.noteMarkers, nativeInputs(parsed.noteMarkers.first()));
            complete = replay.meetsTarget;
            if (!complete) dumpNativeFailure(token, replay, false);
            cutFails = !replayHandOrderChart(parsed.noteMarkers, nativeInputs(parsed.noteMarkers.first(), true)).meetsTarget;
        } else QTextStream(stderr) << "  native chain parser fixture failed: " << token << '\n';
        expect(complete && cutFails, "mixed native chain direction preserves real geometry, full queues and truncation rejection");
    }
    int nativeWifiCount = 0;
    const auto nativeWifis = reference.value("wifi").toObject();
    for (auto it = nativeWifis.constBegin(); it != nativeWifis.constEnd(); ++it) {
        const QString token = it.key() + QStringLiteral("[2:1]");
        const auto parsed = SimaiParser::validateSyntax(QStringLiteral("(120){4}") + token + ',');
        bool complete = parsed.ok && parsed.noteMarkers.size() == 1;
        if (complete) {
            const auto replay = replayHandOrderChart(parsed.noteMarkers, nativeInputs(parsed.noteMarkers.first()));
            complete = replay.meetsTarget && !replayHandOrderChart(parsed.noteMarkers, nativeInputs(parsed.noteMarkers.first(), true)).meetsTarget;
            if (!complete) dumpNativeFailure(token, replay, true);
        }
        nativeWifiCount += complete;
    }
    expect(nativeWifiCount == nativeWifis.size(), "all native Wifi directions complete with two finite real palms and reject truncated input");
    for (const QString token : {QStringLiteral("1-5b[2:1]"), QStringLiteral("1@bx-4b[2:1]")}) {
        const auto parsed = SimaiParser::validateSyntax(QStringLiteral("(120){4}") + token + ',');
        expect(parsed.ok && parsed.noteMarkers.size() == 1
            && replayHandOrderChart(parsed.noteMarkers, nativeInputs(parsed.noteMarkers.first())).meetsTarget,
            "native Break track and Break/EX head modifiers retain the same physical queue semantics");
    }
    for (const QString token : {QStringLiteral("1h[2:1]"), QStringLiteral("1xh[2:1]"), QStringLiteral("Ch[2:1]")}) {
        const auto parsed = SimaiParser::validateSyntax(QStringLiteral("(120){4}") + token + ',');
        bool complete = parsed.ok && parsed.noteMarkers.size() == 1;
        if (complete) {
            const auto& marker = parsed.noteMarkers.first();
            const auto input = marker.type == "touch_hold" ? screen(marker.touchPad, marker.second, marker.endSecond)
                : button(marker.lane, marker.second, marker.endSecond);
            complete = replayHandOrderChart(parsed.noteMarkers, {input}).meetsTarget;
        }
        expect(complete, "native Hold, EX Hold and Touch Hold parser durations feed sustained physical replay");
    }
    auto shiftedSlide = slide(); shiftedSlide.second -= 10; shiftedSlide.slideTraceSecond -= 10; shiftedSlide.endSecond -= 10;
    auto shiftedActions = straight();
    for (auto& input : shiftedActions) {
        auto& action = input.contact; action.start -= 10; action.end -= 10; action.motionStart -= 10; action.motionEnd -= 10;
        for (auto& time : action.times) time -= 10;
    }
    const auto shifted = replayHandOrderChart({shiftedSlide}, shiftedActions);
    expect(shifted.meetsTarget && std::abs(shifted.judgements.first().second - line.judgements.first().second + 10) < 1e-8,
        "negative-time replay and uniform chart/action shifts preserve judgement");
    auto lateTouch = note("touch", 1e9, 0, 1, "B1"); qint64 sparseWork = 10000;
    const auto sparse = replayHandOrderChart({lateTouch}, {screen("B1", 0, 1e9 + 1)}, {}, &sparseWork);
    expect(!sparse.exhausted && sparseWork > 9000 && !sparse.meetsTarget,
        "billion-second sustained input uses sparse events and cannot retrigger at a window boundary");
    QVector<TimelineNoteMarker> longMarkers; QVector<ChartInputAction> longInputs;
    for (int i = 0; i < 1200; ++i) {
        longMarkers.append(note("tap", i * .5, i, i % 8 + 1));
        auto input = button(i % 8 + 1, i * .5, i * .5 + .025, i % 2); input.contact.tokenStart = i;
        longInputs.append(input);
    }
    qint64 activeWork = 300000;
    const auto manyCompleted = replayHandOrderChart(longMarkers, longInputs, {}, &activeWork);
    expect(!manyCompleted.exhausted && manyCompleted.meetsTarget && activeWork > 0 && manyCompleted.judgements.size() == 1200,
        "twelve hundred independent native consumers fit a fixed budget through active-window scheduling without dropping any marker");
    for (auto& marker : longMarkers) marker.lane = 1;
    qint64 unresolvedWork = 300000;
    const auto manyUnresolved = replayHandOrderChart(longMarkers, {button(1, 0, 1000)}, {}, &unresolvedWork);
    bool trueMisses = !manyUnresolved.exhausted && !manyUnresolved.meetsTarget && manyUnresolved.judgements.size() == 1200
        && manyUnresolved.judgements.first().meetsTarget && unresolvedWork > 0;
    for (int i = 1; i < manyUnresolved.judgements.size(); ++i) trueMisses &= manyUnresolved.judgements.at(i).judged
        && !manyUnresolved.judgements.at(i).completed && manyUnresolved.judgements.at(i).reason == "no_down_in_window";
    expect(trueMisses, "all eleven hundred ninety-nine unresolved native notes expire honestly while a continuous prior ON never becomes another DOWN");
    for (bool needC : {false, true}) {
        FullReplayOptions batchOptions; batchOptions.wifiNeedC = needC;
        QVector<TimelineNoteMarker> batchMarkers; QVector<ChartInputAction> batchInputs;
        QVector<FullReplayJudgement> expected;
        const auto appendFixture = [&](QVector<TimelineNoteMarker> markers, QVector<ChartInputAction> inputs) {
            const double shift = batchMarkers.size() * 5.0; const int base = batchMarkers.size();
            for (int i = 0; i < markers.size(); ++i) {
                auto& marker = markers[i]; marker.parseOrder = base + i; marker.second += shift;
                marker.endSecond += shift; marker.slideTraceSecond += shift;
                for (auto& time : marker.slideSegmentShootSeconds) time += shift;
            }
            for (auto& input : inputs) {
                auto& action = input.contact; action.start += shift; action.end += shift;
                action.motionStart += shift; action.motionEnd += shift;
                for (auto& time : action.times) time += shift;
            }
            const auto isolated = replayHandOrderChart(markers, inputs, batchOptions);
            expected += isolated.judgements; batchMarkers += markers; batchInputs += inputs;
        };
        for (int repetition = 0; repetition < 4; ++repetition) {
            appendFixture({tap}, {button(1, 0, .03)});
            appendFixture({touch}, {screen("B1", 0, .03)});
            appendFixture({hold}, {button(1, 0, 1), screen("A1", .3, .5, 1)});
            appendFixture({touchHold}, {screen("B1", 0, 1)});
            QVector<TimelineNoteMarker> members;
            for (int i = 0; i < 3; ++i) { auto member = note("touch", 0, i, 1, QStringLiteral("B%1").arg(i + 1)); member.eachGroupId = 17; members.append(member); }
            appendFixture(members, {screen("B1", 0, .01), screen("B2", .02, .03, 1)});
            appendFixture({slide()}, straight()); appendFixture({wifi()}, wifiInputs());
            appendFixture({hold}, {button(1, 0, .3), button(1, .3 + 8.0 / 180.0, 1)});
            appendFixture({shortHold}, {});
        }
        const auto batch = replayHandOrderChart(batchMarkers, batchInputs, batchOptions);
        bool equal = batch.valid && !batch.exhausted && batch.judgements.size() == expected.size();
        for (int i = 0; equal && i < expected.size(); ++i) equal &= sameJudgement(batch.judgements.at(i), expected.at(i));
        expect(equal, "active scheduling preserves complete isolated Tap/Touch/Hold/TouchHold/group/Slide/Wifi states, grades, body shadows and causes in a separated batch");
    }
    qint64 tiny = 1;
    expect(replayHandOrderChart({tap}, {button(1, 0, .03)}, {}, &tiny).exhausted,
        "all preprocessing, geometry and state updates share the caller's work budget");
    auto cancellation = std::make_shared<std::atomic_bool>(true); bool cancelled = false;
    try { miacode::task::CancellationScope scope(cancellation); replayHandOrderChart({tap}, {}); }
    catch (const miacode::task::Cancelled&) { cancelled = true; }
    expect(cancelled, "worker cancellation reaches full replay preprocessing");
    auto dense = screen("C", 0, 2); dense.contact.points.clear(); dense.contact.times.clear();
    for (int i = 0; i <= 200; ++i) {
        dense.contact.points.append(QPointF(i % 2 ? 1 : -1, 0)); dense.contact.times.append(i / 100.0);
    }
    FullReplayOptions memoryBound; memoryBound.maxEventEntries = 64;
    qint64 denseWork = 1000000;
    const auto memoryStop = replayHandOrderChart({centerTouch}, {dense}, memoryBound, &denseWork);
    expect(memoryStop.exhausted && !memoryStop.valid && memoryStop.error == "event_memory_limit" && denseWork > 0,
        "dense crossing paths hit the event memory cap before exhausting the shared work budget");
    FullReplayOptions stale; stale.profileRevision = fullReplayProfileRevision - 1;
    expect(!replayHandOrderChart({tap}, {}, stale).valid,
        "an incompatible external profile revision cannot silently enter replay");
    PlayProfile conflictProfile;
    conflictProfile.automaticHeight = conflictProfile.automaticDistance = false;
    const auto contactA = screen("C", 0, .8, 0, 10, "touch_hold");
    auto contactB = screen("C", .35, .875, 1, 11, "touch");
    std::array<PlayHandMotion, 2> hands;
    for (int i = 0; i < 2; ++i) {
        hands[i].source = i;
        hands[i].screen = true;
        hands[i].touching = true;
        hands[i].contact = touchPadPoint("C");
        hands[i].palmRadius = contactA.contact.radius;
    }
    expect(resolvePlayArms(hands, .08, conflictProfile).interference == PlayInterference::Collision,
        "physical review fixture has a resolved two-arm collision");
    auto requiredHold = note("touch_hold", 0, 0, 9, "C");
    requiredHold.endSecond = .8;
    // The Hold and the later input are different channels. Their contacts
    // overlap at C in the motion model but the button's DOWN is still judged.
    contactB.button = true;
    contactB.lane = 2;
    contactB.contact.start = .77;
    contactB.contact.end = .8;
    const auto strike = note("tap", .77, 1, 2);
    const QVector<TimelineNoteMarker> conflicted{requiredHold, strike};
    QVector<ChartInputAction> withOverlap{contactA, contactB};
    qint64 conflictWork = 1000000;
    const auto plain = replayHandOrderChart(conflicted, withOverlap, {}, &conflictWork);
    const auto constrained = reviewPhysicalConflicts(conflicted, {withOverlap, plain}, conflictProfile, false, conflictWork);
    expect(plain.meetsTarget && constrained.complete && constrained.regressedOrders.contains(1)
        && constrained.affectedSources.contains(10) && constrained.affectedSources.contains(11),
        "a collision is confirmed when suppressing an overlapping contact loses the actual DOWN");
    auto separatedTap = strike;
    separatedTap.second = separatedTap.endSecond = .81;
    withOverlap[1].contact.start = .81;
    withOverlap[1].contact.end = .85;
    const QVector<TimelineNoteMarker> separated{requiredHold, separatedTap};
    const auto safe = replayHandOrderChart(separated, withOverlap, {}, &conflictWork);
    const auto clean = reviewPhysicalConflicts(separated, {withOverlap, safe}, conflictProfile, false, conflictWork);
    expect(safe.meetsTarget && clean.complete && clean.regressedOrders.isEmpty(),
        "the same contacts outside the shared judgement interval do not fail physical review");
    return ok ? 0 : 1;
}
