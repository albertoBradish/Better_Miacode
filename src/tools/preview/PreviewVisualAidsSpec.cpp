#include "core/chart/parser/SimaiParser.h"
#include "core/chart/SlideJudgeData.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/scene/PreviewVisualAidTiming.h"
#include "core/scene/PreviewVisualAidLayerState.h"
#include "core/scene/PreviewOpacityCurves.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/analysis/MuriConfig.h"

#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

using namespace miacode::preview::scene;
namespace {
bool require(bool value, const char* message)
{
    if (!value) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return value;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
PreviewVisualAidLayerState sample(PreviewFrameState& state, double second)
{
    state.playheadSeconds = second;
    return buildPreviewVisualAidLayerState(state, PreviewActiveMarkerView(state.noteMarkers), QRectF(0, 0, 540, 540));
}
TimelineNoteMarker slide(const QString& token)
{
    const auto parsed = SimaiParser::parseForTimeline(QStringLiteral("(120){8}") + token + QStringLiteral(",\nE"));
    for (const auto& marker : parsed.noteMarkers)
        if (marker.type == QLatin1String("slide") || marker.type == QLatin1String("wifi")) return marker;
    return {};
}
bool timing()
{
    bool ok = true;
    const auto parsed = SimaiParser::parseForTimeline(QStringLiteral(
        "(120){8}1,1,1,\n(240){8}1,1,1,1,1,1,1,1,1,\nE"));
    const auto defaults = buildPreviewMeasureLineSeconds(parsed, 0, PreviewMeasureLineMode::ChartClockCount, 4, 1.25);
    ok &= require(defaults.size() == 2 && near(defaults[0], 1.25) && near(defaults[1], 2.625),
        "quarter-note grid survives an off-grid BPM change and applies &first once");
    const auto negative = buildPreviewMeasureLineSeconds(parsed, -4, PreviewMeasureLineMode::ChartClockCount, 4, -0.5);
    ok &= require(negative.size() == defaults.size() && near(negative[1], 0.875), "invalid chart clock_count falls back to four");
    const auto three = buildPreviewMeasureLineSeconds(parsed, 3, PreviewMeasureLineMode::ChartClockCount, 9, 0.0);
    ok &= require(three.size() == 3 && near(three[1], 1.125) && near(three[2], 1.875),
        "positive chart clock_count determines measure length independently of custom X");
    const auto custom = buildPreviewMeasureLineSeconds(parsed, 3, PreviewMeasureLineMode::CustomBeats, 2, 0.0);
    ok &= require(custom.size() == 4 && near(custom[1], 0.875) && near(custom[2], 1.375),
        "custom positive X counts quarter notes across tempo changes");
    const auto badCustom = buildPreviewMeasureLineSeconds(parsed, 3, PreviewMeasureLineMode::CustomBeats, 0, 0.0);
    ok &= require(badCustom.size() == 2 && near(badCustom[1], 1.375), "invalid custom X safely defaults to four");
    const auto lines = SimaiParser::parseForTimeline(QStringLiteral("|| comment\n(120)\n{4}\n1,\n\n|| another\n2,\n|| trailing\n\nE\n"));
    const auto lineTimes = buildPreviewMeasureLineSeconds(lines, 7, PreviewMeasureLineMode::TextLines, 5, -0.25);
    ok &= require(lineTimes.size() == 3 && near(lineTimes[0], -0.25) && near(lineTimes[1], 0.25) && near(lineTimes[2], 0.75),
        "text-line mode uses all row entry times, deduplicates equal times and excludes terminal rows");
    const auto unchanged = SimaiParser::parseForTimeline(QStringLiteral("(120){8}1,1,1,(120){8}1,1,1,1,1,\nE"));
    const auto unchangedTimes = buildPreviewMeasureLineSeconds(unchanged, 0, PreviewMeasureLineMode::ChartClockCount, 4, 0.0);
    ok &= require(unchangedTimes.size() == 2 && near(unchangedTimes[1], 2.0), "repeated BPM directive does not restart the aid grid");
    return ok;
}
bool rings()
{
    bool ok = true;
    PreviewFrameState state;
    state.measureLineSeconds = {2.0};
    ok &= require(sample(state, 1.9).circles.isEmpty(), "measure lines default off");
    state.render.showMeasureLines = true;
    const auto timing = previewTapTimingForFlowSpeed(state.render.tapFlowSpeed);
    ok &= require(sample(state, 2.0 - timing.lifecycleDurationSeconds - .001).circles.isEmpty(), "ring waits for base tap appearance");
    const double mid = 2.0 - timing.flyDurationSeconds / 2;
    const auto before = sample(state, mid);
    const auto hit = sample(state, 2.0);
    ok &= require(before.circles.size() == 1 && hit.circles.size() == 1
        && before.circles[0].radiusX < hit.circles[0].radiusX
        && near(hit.circles[0].radiusX, kLogicalDistanceEdge)
        && near(hit.circles[0].radiusY, kLogicalDistanceEdge)
        && hit.circles[0].fillColor.alpha() == 0, "entire thin hollow ring approaches and reaches the judge circle");
    ok &= require(sample(state, 2.00001).circles.isEmpty(), "ring disappears immediately after its timestamp");
    const auto seek = sample(state, mid);
    ok &= require(seek.circles.size() == before.circles.size() && near(seek.circles[0].radiusX, before.circles[0].radiusX),
        "explicit-time replay and backward seek are deterministic");
    return ok;
}
bool skeletons()
{
    bool ok = true;
    PreviewFrameState state;
    state.noteMarkers = {slide(QStringLiteral("1V35[4:1]"))};
    const auto marker = state.noteMarkers[0];
    ok &= require(marker.type == QLatin1String("slide") && marker.slideSegmentJudgeAreas.size() == 1,
        "parser exposes real judgement data through shared chart data");
    const double waiting = marker.slideTraceSecond - .001;
    ok &= require(sample(state, waiting).lines.isEmpty(), "judgement skeleton defaults off");
    state.render.showJudgementSkeleton = true;
    const auto initial = sample(state, waiting);
    ok &= require(initial.lines.size() == 8 && state.muriAnalysisReport.isEmpty(),
        "all eight candidate connections of a V slide are shown without requiring Muri analysis");
    bool padCenters = true, serializedAlternatives = false;
    const QPointF a2 = miacode::muri::padCenter(QStringLiteral("A2"));
    const QPointF b2 = miacode::muri::padCenter(QStringLiteral("B2"));
    for (const auto& line : initial.lines) {
        const auto& areas = marker.slideSegmentJudgeAreas[0];
        bool source = false, target = false;
        for (const auto& pad : areas[line.areaIndex]) source |= QLineF(line.line.p1(), miacode::muri::padCenter(pad)).length() < 1e-6;
        for (const auto& pad : areas[line.areaIndex + 1]) target |= QLineF(line.line.p2(), miacode::muri::padCenter(pad)).length() < 1e-6;
        padCenters &= source && target;
        serializedAlternatives |= (line.line.p1() == a2 && line.line.p2() == b2) || (line.line.p1() == b2 && line.line.p2() == a2);
    }
    ok &= require(padCenters && !serializedAlternatives, "skeleton joins pad centres and preserves alternative groups without serializing them");
    const auto middle = sample(state, marker.slideTraceSecond + .7 * (marker.endSecond - marker.slideTraceSecond));
    ok &= require(middle.lines.size() < initial.lines.size() && !middle.lines.isEmpty(), "consumed judge areas disappear while the remaining slide stays visible");
    ok &= require(sample(state, marker.endSecond).lines.isEmpty(), "skeleton finishes with its slide");
    state.noteMarkers = {slide(QStringLiteral("1^3-5[4:1]"))};
    const auto chain = state.noteMarkers[0];
    const auto chainBefore = sample(state, chain.slideTraceSecond - .001);
    bool first = false, second = false;
    for (const auto& line : chainBefore.lines) { first |= line.segmentIndex == 0; second |= line.segmentIndex == 1; }
    ok &= require(first && second, "connected slide keeps both judgement segments");
    const auto chainAfter = sample(state, chain.slideSegmentShootSeconds.value(1) + .001);
    bool onlySecond = !chainAfter.lines.isEmpty();
    for (const auto& line : chainAfter.lines) onlySecond &= line.segmentIndex == 1;
    ok &= require(onlySecond, "completed chain segment disappears independently");
    state.noteMarkers = {slide(QStringLiteral("1w5[4:1]"))};
    const auto wifi = state.noteMarkers[0];
    const auto fan = sample(state, wifi.slideTraceSecond - .001);
    bool lanes[3] = {false, false, false};
    for (const auto& line : fan.lines) lanes[line.laneIndex] = true;
    ok &= require(fan.lines.size() == 12 && lanes[0] && lanes[1] && lanes[2], "Wifi preserves three distinct judge lanes and alternative endpoints");
    state.noteMarkers[0].wifiLaneJudgeAreas.clear();
    const auto missing = sample(state, wifi.slideTraceSecond);
    ok &= require(missing.lines.isEmpty() && missing.missingJudgeDataCount == 1, "missing judge data is skipped and diagnosed");
    state.noteMarkers[0] = wifi;
    state.noteMarkers[0].wifiLaneJudgeAreas[1][0] = {QStringLiteral("invalid")};
    const auto malformed = sample(state, wifi.slideTraceSecond);
    ok &= require(malformed.lines.isEmpty() && malformed.missingJudgeDataCount == 1, "malformed judge data does not fabricate geometry");
    ok &= require(miacode::slide_judge::slideAreas(QStringLiteral("not-a-shape")).isEmpty()
        && miacode::slide_judge::wifiLanes(QStringLiteral("not-a-shape")).isEmpty(), "shared judge loader rejects missing reference keys");
    return ok;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool ok = timing() & rings() & skeletons();
    if (ok) QTextStream(stdout) << "preview_visual_aids_spec ok" << Qt::endl;
    return ok ? 0 : 1;
}
