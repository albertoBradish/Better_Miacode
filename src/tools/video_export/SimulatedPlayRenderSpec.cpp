#include "core/analysis/hand_order/HandPlayPose.h"
#include "core/analysis/hand_order/HandPlayReview.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/scene/PreviewPreparedSceneCache.h"
#include "export/session/PreviewQuickExportSession.h"
#include "export/video_export/VideoExportSnapshot.h"
#include "preview/runtime/PreviewSceneAssetLoader.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QLineF>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>

#include <algorithm>
#include <cmath>

using namespace miacode::hand_order;
using namespace miacode::preview::scene;
using Hand = HandOrderHand;

namespace {
bool near(const QPointF& a, const QPointF& b) { return QLineF(a, b).length() < 1e-5; }
}

int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    const QString output = app.arguments().value(1);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& name) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << name << '\n';
        ok &= pass;
    };

    const QString text = "(120){8}8~l/1~r,7h[4:1]~l/A3~r,,5,,2-4-6[4:1]~[head=r;track=0:r|0.5:l],,,,,,3~m,E";
    const auto parsed = SimaiParser::validateSyntax(text);
    expect(parsed.ok && parsed.annotationErrors.isEmpty() && parsed.noteMarkers.size() == 7,
        "native Tap, Hold, Touch, Slide hand interval and either-hand fixture parses");
    if (!ok) return 1;
    HandPlayContacts contacts;
    contacts.rebuild(parsed.noteMarkers, 0.08);
    const auto chord = contacts.sample(parsed.noteMarkers.first().second + 0.005);
    expect(chord.hands[0].touching && chord.hands[1].touching && chord.hands[0].contact.x() < 0.0
        && chord.hands[1].contact.x() > 0.0, "explicit left and right heads both press on the beat");
    const auto approaching = contacts.sample(parsed.noteMarkers.first().second - 0.075);
    expect(!approaching.hands[0].touching && approaching.hands[0].contact.x() < 0.0
        && approaching.hands[0].contact.y() < 0.0 && !approaching.cues.isEmpty() && approaching.cues.first().timeToContact > 0.0,
        "before the hit the palm is travelling and the cue leads the beat");
    const auto hold = contacts.sample(parsed.noteMarkers.at(2).second + 0.2);
    expect(hold.hands[0].touching && near(hold.hands[0].contact, QPointF(-0.92387953, -0.38268343)),
        "the left palm remains on the native Hold until release");
    const auto unlabelled = contacts.sample(parsed.noteMarkers.at(4).second + 0.005);
    expect(std::any_of(unlabelled.cues.cbegin(), unlabelled.cues.cend(), [](const auto& cue) {
            return cue.hand == Hand::Unspecified && cue.strikes && cue.timeToContact <= 0;
        }) && !unlabelled.hands[0].touching && !unlabelled.hands[1].touching,
        "missing Sticking remains unassigned with a neutral cue");
    const auto& slide = parsed.noteMarkers.at(5);
    expect(slide.slideSegmentPoints.size() >= 2, "the handoff fixture contains a native chained Slide");
    const double halfway = (slide.slideTraceSecond + slide.endSecond) / 2;
    const auto beforeTransfer = contacts.sample(halfway - 0.001);
    const auto afterTransfer = contacts.sample(halfway + 0.001);
    expect(beforeTransfer.hands[1].touching && !beforeTransfer.hands[0].touching
        && afterTransfer.hands[0].touching && !afterTransfer.hands[1].touching
        && QLineF(beforeTransfer.hands[1].contact, afterTransfer.hands[0].contact).length() < 0.01,
        "handoff follows movement progress and both hands meet on the native track");
    const auto rewind = contacts.sample(slide.slideTraceSecond + 0.02);
    contacts.sample(slide.endSecond + 1);
    const auto repeated = contacts.sample(slide.slideTraceSecond + 0.02);
    expect(near(rewind.hands[1].contact, repeated.hands[1].contact) && rewind.hands[1].touching == repeated.hands[1].touching,
        "seeking backward reproduces the same pose at the same chart second");
    const auto either = contacts.sample(parsed.noteMarkers.last().second + 0.005);
    expect(either.hands[0].touching != either.hands[1].touching, "explicit either-hand annotation picks one hand");
    auto synthetic = parsed.noteMarkers.at(3);
    synthetic.parseOrder = -1;
    contacts.rebuild({synthetic}, 0.08);
    expect(contacts.isEmpty(), "preview-only markers cannot become simulated chart actions");

    const auto blocked = SimaiParser::validateSyntax("(120){16}8h[4:1]~l,2~l,E");
    contacts.rebuild(blocked.noteMarkers, 0.08);
    const auto occupied = contacts.sample(blocked.noteMarkers.last().second + 0.005);
    expect(occupied.hands[0].blocked && near(occupied.hands[0].contact, chord.hands[0].contact)
        && std::any_of(occupied.cues.cbegin(), occupied.cues.cend(), [](const auto& cue) { return cue.block == PlayBlock::Occupied; }),
        "a same-hand strike cannot steal the palm from an incompatible active Hold");
    const auto hurried = SimaiParser::validateSyntax("(240){16}8~l,4~l,E");
    contacts.rebuild(hurried.noteMarkers, 0.08);
    const auto tooFast = contacts.sample(hurried.noteMarkers.last().second + 0.001);
    expect(tooFast.hands[0].blocked && !tooFast.hands[0].touching,
        "an unreachable transit stays short of the target and is marked blocked");

    const auto wifi = SimaiParser::validateSyntax("(120){4}1w5[4:1]~[head=r;track=split:0:l&0:r],E");
    expect(wifi.ok && wifi.annotationErrors.isEmpty() && wifi.noteMarkers.size() == 1, "split WiFi fixture parses");
    if (wifi.ok && wifi.annotationErrors.isEmpty() && wifi.noteMarkers.size() == 1) {
        const auto& fan = wifi.noteMarkers.first();
        contacts.rebuild(wifi.noteMarkers, 0.08);
        const auto sectors = contacts.sample(fan.slideTraceSecond + (fan.endSecond - fan.slideTraceSecond) * 0.70);
        expect(sectors.hands[0].touching && sectors.hands[1].touching
            && QLineF(sectors.hands[0].contact, sectors.hands[1].contact).length() > 0.1,
            "split WiFi uses both native palm tracks");
    }
    const auto singleWifi = SimaiParser::validateSyntax("(120){4}1w5[4:1]~[head=r;track=single:0:r],E");
    expect(singleWifi.ok && singleWifi.annotationErrors.isEmpty() && singleWifi.noteMarkers.size() == 1, "single WiFi fixture parses");
    if (singleWifi.ok && singleWifi.annotationErrors.isEmpty() && singleWifi.noteMarkers.size() == 1) {
        contacts.rebuild(singleWifi.noteMarkers, 0.08);
        const auto& fan = singleWifi.noteMarkers.first();
        const auto sweep = contacts.sample(fan.slideTraceSecond + (fan.endSecond - fan.slideTraceSecond) * 0.70);
        expect(!sweep.hands[0].touching && sweep.hands[1].touching, "single WiFi stays with its annotated hand");
    }
    const auto shared = SimaiParser::validateSyntax("(120){4}1-3[4:1]*-6[4:1]~[head=r;branch0=0:r;branch1=0:l],E");
    expect(shared.ok && shared.annotationErrors.isEmpty() && shared.noteMarkers.size() == 2, "shared Slide head fixture parses");
    if (shared.ok && shared.annotationErrors.isEmpty() && shared.noteMarkers.size() == 2) {
        contacts.rebuild(shared.noteMarkers, 0.08);
        const auto atHead = contacts.sample(shared.noteMarkers.first().second + 0.005);
        expect(std::count_if(atHead.cues.cbegin(), atHead.cues.cend(), [](const auto& cue) { return cue.strikes; }) == 1,
            "branches with one native star schedule one head strike");
    }

    std::array<PlayHandMotion, 2> motions;
    motions[0].source = 0;
    motions[1].source = 1;
    motions[0].contact = {-0.72, 0.18}; motions[1].contact = {0.72, 0.18};
    motions[0].touching = motions[1].touching = true;
    expect(resolvePlayArms(motions, 0.09).interference == PlayInterference::Clear, "separated arm volumes stay clear");
    motions[0].contact = {-0.075, 0.04}; motions[1].contact = {0.075, 0.04};
    expect(resolvePlayArms(motions, 0.09).interference == PlayInterference::Collision,
        "parallel centerlines still collide when two grounded palms overlap by radius");
    motions[1].touching = false;
    const auto lifted = resolvePlayArms(motions, 0.09);
    expect(lifted.interference == PlayInterference::Lifted && (lifted.arms[0].lifted || lifted.arms[1].lifted),
        "a free palm can lift above a close grounded palm");
    motions[1].contact = {4.0, -4.0};
    expect(resolvePlayArms(motions, 0.09).interference == PlayInterference::OutOfReach,
        "the arm length clamps an out-of-reach target");

    const PlayProfile defaultProfile;
    expect(std::abs(defaultProfile.shoulderHeightCm() - 139.4) < 1e-6
        && std::abs(defaultProfile.readyHandHeightCm() - 107.1) < 1e-6 && playReadyY(defaultProfile) > 0,
        "standing shoulder and hand start heights use the cabinet's floor coordinate");
    MuriRenderOptions crouching;
    crouching.playProfile.heightCm = 210;
    crouching.playProfile.standingDistanceCm = 48;
    auto fixedHeight = crouching;
    fixedHeight.playProfile.automaticHeight = false;
    const auto low = SimaiParser::validateSyntax("(120){4},,4h[4:1]~r,,,E");
    const double lowSecond = low.noteMarkers.first().second;
    const HandPlaySimulation movingBody(low.noteMarkers, crouching), fixedBody(low.noteMarkers, fixedHeight);
    expect(fixedBody.bodyOffsetCm(lowSecond) == 0 && movingBody.bodyOffsetCm(lowSecond) < -0.5
        && movingBody.bodyOffsetCm(lowSecond) >= -crouching.playProfile.lowerLimitCm,
        "a low contact lowers the body within the configured range");
    expect(movingBody.bodyOffsetCm(lowSecond - 1.0) == 0
        && std::abs(movingBody.bodyOffsetCm(lowSecond) - movingBody.bodyOffsetCm(lowSecond - 0.04))
            <= 0.04 * crouching.playProfile.verticalSpeedCmPerSecond + 1e-6,
        "body movement starts at standing height and respects speed during deterministic seeking");
    const HandPlaySimulation blockedSimulation(blocked.noteMarkers, {});
    const auto warnings = blockedSimulation.detectIssues(blocked.noteMarkers);
    bool detected = false;
    for (const auto& issue : warnings) {
        if (issue.kind != PlayIssueKind::Occupied) continue;
        const auto frame = blockedSimulation.sample(issue.second, false);
        detected |= issue.line == blocked.noteMarkers.last().sourceLine
            && issue.column == blocked.noteMarkers.last().sourceCol && issue.handMask == 1
            && std::any_of(frame.contacts.cues.cbegin(), frame.contacts.cues.cend(), [](const auto& cue) {
                return cue.block == PlayBlock::Occupied;
            });
    }
    expect(detected, "detected warning links to the blocked note and its visibly yellow frame");

    const HandPlaySimulation judgedBlocked(blocked.noteMarkers, {}, true);
    expect(judgedBlocked.replayVerified() && std::any_of(judgedBlocked.judgements().cbegin(),
        judgedBlocked.judgements().cend(), [&](const auto& judgement) {
            return judgement.source == blocked.noteMarkers.last().parseOrder
                && judgement.grade == PlayGrade::Miss && judgement.baseLoss == 500
                && std::abs(judgement.achievementLoss - 100.0 / 3.0) < 1e-6;
        }), "an occupied strike receives a real Miss against the chart score");
    const auto scoredWarnings = judgedBlocked.detectIssues(blocked.noteMarkers);
    expect(std::any_of(scoredWarnings.cbegin(), scoredWarnings.cend(), [](const auto& issue) {
        return issue.kind == PlayIssueKind::Occupied && issue.grade == PlayGrade::Miss && issue.judgeSecond >= issue.second;
    }), "the occupied cue and its corresponding lost judgement are in one detection row");

    auto single = SimaiParser::validateSyntax("(120){4},,1~r,E").noteMarkers;
    single.first().hsMultiplier = 4.0;
    MuriRenderOptions slow, fast;
    slow.playTapFlowSpeed = 2.0;
    fast.playTapFlowSpeed = 10.0;
    fast.playProfile.reactionMs = 200;
    HandPlayContacts slowContacts, fastContacts;
    slowContacts.rebuild(single, 0.08, playReadyY(slow.playProfile), &slow);
    fastContacts.rebuild(single, 0.08, playReadyY(fast.playProfile), &fast);
    const auto& slowHead = slowContacts.plannedContacts().first();
    const auto& fastHead = fastContacts.plannedContacts().first();
    expect(slowHead.appear < fastHead.appear && slowHead.ready < slowHead.nominal && fastHead.ready > fastHead.nominal
        && !fastContacts.handsAt(fastHead.nominal)[1].touching,
        "flow speed and per-note HS shrink available reaction time");
    const double beat = single.first().second;
    const auto renderedButton = slowContacts.handsAt(beat + 0.006)[1];
    expect(renderedButton.buttonLane == 1 && !renderedButton.screen
        && std::abs(QLineF(QPointF{}, renderedButton.contact).length() - playExternalButtonRadius) < 1e-6,
        "a tap goes to the external button by default");
    slow.playProfile.preferButtons = false;
    slowContacts.rebuild(single, 0.08, playReadyY(slow.playProfile), &slow);
    const auto renderedTouch = slowContacts.handsAt(beat + 0.006)[1];
    expect(renderedTouch.buttonLane == 0 && renderedTouch.screen, "screen input uses a separate channel when requested");

    const auto noLaunch = SimaiParser::validateSyntax("(120){4},1-3[4:1]~r,,,E");
    const auto withLaunch = SimaiParser::validateSyntax("(120){4},1-3[4:1]~r,1~r,,E");
    const auto traceStart = [&](const SimaiParseResult& track) {
        MuriRenderOptions options;
        HandPlayContacts plan;
        plan.rebuild(track.noteMarkers, 0.08, 0.0, &options);
        for (const auto& contact : plan.plannedContacts())
            if (contact.kind == PlayContactKind::Trace) return contact.path.start;
        return std::numeric_limits<double>::infinity();
    };
    if (noLaunch.noteMarkers.isEmpty() || withLaunch.noteMarkers.isEmpty()) return 1;
    expect(traceStart(noLaunch) < noLaunch.noteMarkers.first().slideTraceSecond - 0.05
        && std::abs(traceStart(withLaunch) - withLaunch.noteMarkers.first().slideTraceSecond) < 1e-6,
        "free hands may advance a slide without a launch beat and retain an explicit launch");

    const auto overlap = SimaiParser::validateSyntax("(120){4},A1~l/1?-3[4:1]~r,,,E");
    const auto track = std::find_if(overlap.noteMarkers.cbegin(), overlap.noteMarkers.cend(),
        [](const auto& note) { return note.type == "slide"; });
    expect(overlap.ok && overlap.annotationErrors.isEmpty() && track != overlap.noteMarkers.cend(),
        "a preceding Touch that consumes a live slide area parses");
    if (track != overlap.noteMarkers.cend()) {
        const HandPlaySimulation fullPath(overlap.noteMarkers, {});
        const HandPlaySimulation remainingPath(overlap.noteMarkers, {}, true);
        const double when = track->slideTraceSecond - 0.025;
        const auto before = fullPath.sample(when, false).contacts.hands[1];
        const auto after = remainingPath.sample(when, false).contacts.hands[1];
        const auto state = remainingPath.report().markerStates.value(makeMarkerAnalysisKey(*track));
        const auto entry = state.slideSegments.value(0).areaCheckpoints.value(0).value(0);
        expect(remainingPath.replayVerified() && before.touching && !after.touching
            && entry.cause == AreaJudgeCause::Other && entry.second < when && state.slideSegments.value(0).completedSecond >= 0.0,
            "an externally cleared prefix is skipped while the remaining slide still completes");
    }

    MuriRenderOptions adaptive;
    const auto doubleSlide = SimaiParser::validateSyntax("(120){4},1?-3[4:1]*-4[4:1]~[branch0=0:r;branch1=0:r],,,E");
    const auto trident = SimaiParser::validateSyntax("(120){4},1?-3[4:1]*-4[4:1]*-7[4:1]~[branch0=0:r;branch1=0:r;branch2=0:l],,,E");
    expect(doubleSlide.ok && doubleSlide.annotationErrors.isEmpty() && doubleSlide.noteMarkers.size() == 2
        && trident.ok && trident.annotationErrors.isEmpty() && trident.noteMarkers.size() == 3,
        "adjacent double slide and trident fixtures parse");
    if (ok) {
        HandPlayContacts plan;
        plan.rebuild(doubleSlide.noteMarkers, 0.08, 0.0, &adaptive);
        const auto expanded = plan.handsAt(doubleSlide.noteMarkers.first().slideTraceSecond + 0.035)[1];
        expect(std::abs(expanded.palmRadius - FullReplayProfile::maximumPalmRadius) < 1e-6,
            "one hand covers two nearby traces with its maximum palm");
        plan.rebuild(trident.noteMarkers, 0.08, 0.0, &adaptive);
        const auto narrow = plan.handsAt(trident.noteMarkers.first().slideTraceSecond + 0.035)[0];
        expect(narrow.palmRadius > 0.0 && narrow.palmRadius < 0.08,
            "a nearby independent trace triggers fingertip contact in a trident");
    }

    MuriRenderOptions longReach;
    longReach.playProfile.standingDistanceCm = 60;
    longReach.playProfile.automaticHeight = false;
    const HandPlaySimulation adjustsDistance(low.noteMarkers, longReach);
    longReach.playProfile.automaticDistance = false;
    const HandPlaySimulation fixedDistance(low.noteMarkers, longReach);
    expect(adjustsDistance.distanceOffsetCm(lowSecond) < -0.5 && fixedDistance.distanceOffsetCm(lowSecond) == 0.0,
        "an extended reach causes a bounded forward change in standing distance");

    const auto crossingText = QStringLiteral("(120){4},,3~l/6~r,,,,E");
    const auto crossing = SimaiParser::validateSyntax(crossingText);
    const auto suggestion = reviewHandPlay(crossingText, crossing, 0, crossingText.size(), {});
    QString corrected = crossingText;
    for (auto it = suggestion.patches.crbegin(); it != suggestion.patches.crend(); ++it)
        corrected.replace(it->start, it->end - it->start, it->text);
    expect(corrected.contains("3~r/6~l") && stripAnnotations(corrected) == stripAnnotations(crossingText)
        && suggestion.rows.size() == 2, "the fourth pass corrects a reversed Each and preserves the native notes");
    const auto lockedText = QStringLiteral("(120){4},,3h[2:1]~l/6~r,,,,E");
    const auto locked = reviewHandPlay(lockedText, SimaiParser::validateSyntax(lockedText), 0, lockedText.size(), {});
    expect(locked.patches.isEmpty(), "the fourth pass keeps a forced hold-hand exception");
    const auto phasedText = QStringLiteral("(185){16},,2~r,1~l,3~r,,,,E");
    const auto phased = reviewHandPlay(phasedText, SimaiParser::validateSyntax(phasedText), 0, phasedText.size(), {});
    QString retainedPhase = phasedText;
    for (auto it = phased.patches.crbegin(); it != phased.patches.crend(); ++it)
        retainedPhase.replace(it->start, it->end - it->start, it->text);
    expect(retainedPhase.contains("2~r,1~l,3~r") || retainedPhase.contains("2~l,1~r,3~l"),
        "the fourth pass preserves a recognized triangle's guided relative phase");

    VideoExportSnapshot snapshot, decoded;
    snapshot.chartTextUtf8 = "&inote_5=" + text;
    snapshot.difficultyId = 5;
    snapshot.outputPath = "simulated-play.mp4";
    snapshot.muriRenderOptions.renderMode = RenderMode::SimulatedPlay;
    snapshot.muriRenderOptions.playProfile = crouching.playProfile;
    expect(VideoExportSnapshot::fromJson(snapshot.toJson(), &decoded) && decoded.muriRenderOptions.renderMode == RenderMode::SimulatedPlay
        && decoded.muriRenderOptions.playProfile == crouching.playProfile, "video snapshot retains play mode and body parameters");

    PreviewFrameState state;
    state.render.layoutSquareScale = 0.87;
    state.muriRenderOptions.renderMode = RenderMode::SimulatedPlay;
    state.noteMarkers = parsed.noteMarkers;
    state.sceneContentRevision = 1;
    state.playheadSeconds = parsed.noteMarkers.first().second + 0.01;
    PreviewQuickExportSession renderer;
    renderer.setFrameSize({720, 720});
    renderer.setLayerFlags(SimulatedPlayLayer);
    renderer.setFrameState(state);
    QString error;
    if (!renderer.initialize(QSurfaceFormat(), nullptr, &error)) {
        expect(false, QStringLiteral("shared QSG session initializes: ") + error);
        return 1;
    }
    const auto render = [&] {
        renderer.setFrameState(state);
        return renderer.renderFrame(&error);
    };
    const QImage first = render();
    PreviewPreparedSceneCache prepared;
    prepared.sync(state);
    expect(prepared.playSimulation() && prepared.playSimulation()->replayVerified()
        && prepared.playSimulation()->report().markerStates.contains(makeMarkerAnalysisKey(slide)),
        "simulated playback prepares native Maimuri track checkpoints");
    state.playheadSeconds = halfway + 0.05;
    const QImage later = render();
    state.playheadSeconds = parsed.noteMarkers.first().second + 0.01;
    expect(!first.isNull() && !later.isNull() && first != later && first == render(),
        "shared QSG paints both frames and repeats the first frame after a seek");
    state.muriRenderOptions.renderMode = RenderMode::Native;
    const QImage native = render();
    expect(!native.isNull() && native != first, "switching to Native clears the simulation layer");
    state.muriRenderOptions.renderMode = RenderMode::SimulatedPlay;
    state.render.showHandOrderAnnotations = false;
    expect(render() == native, "original rendering suppresses the simulated hands");
    state.render.showHandOrderAnnotations = true;
    for (auto& marker : state.noteMarkers) {
        if (marker.handOrderHead == Hand::Left) marker.handOrderHead = Hand::Right;
        else if (marker.handOrderHead == Hand::Right) marker.handOrderHead = Hand::Left;
    }
    ++state.sceneContentRevision;
    expect(render() != first, "editing the Sticking revision changes the same frame");

    if (!output.isEmpty()) {
        expect(QDir().mkpath(output), "image output directory is available");
        const auto assets = miacode::preview::runtime::PreviewSceneAssetLoader::load(
            QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin/skinSD"), PreviewOutlineVariant::Line);
        state.skin = assets.skinAssets;
        state.assets = assets.assetState;
        state.noteMarkers = parsed.noteMarkers;
        ++state.sceneContentRevision;
        renderer.setLayerFlags(kPreviewAllRenderLayers);
        state.playheadSeconds = halfway + 0.05;
        expect(render().save(QDir(output).filePath("slide-handoff.png")), "saves a handoff frame with the native skin");
        state.noteMarkers = blocked.noteMarkers;
        ++state.sceneContentRevision;
        state.playheadSeconds = blocked.noteMarkers.last().second + 0.005;
        expect(render().save(QDir(output).filePath("blocked-hold.png")), "saves a frame with an incompatible strike");
        state.playheadSeconds += FullReplayProfile::tapAvailableSeconds + 0.03;
        expect(render().save(QDir(output).filePath("miss-score.png")), "saves a frame with the missed note and score deduction");
    }
    return ok ? 0 : 1;
}
