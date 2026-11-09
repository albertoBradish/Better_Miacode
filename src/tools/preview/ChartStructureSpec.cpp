#include "core/analysis/ChartStructureAnalysis.h"
#include "core/chart/document/SimaiDocument.h"

#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace miacode::chart_preview;
using namespace miacode::chart_structure;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return condition;
}

bool testAudioStructure()
{
    constexpr int sampleRate = 8000;
    QVector<float> audio(sampleRate * 24);
    for (qsizetype index = 0; index < audio.size(); ++index) {
        const double second = static_cast<double>(index) / sampleRate;
        const bool chorus = second >= 8.0 && second < 16.0;
        const double beat = 0.7 + 0.3 * std::cos(4.0 * std::numbers::pi * second);
        audio[index] = static_cast<float>(chorus
            ? beat * (0.38 * std::sin(2.0 * std::numbers::pi * 82 * second)
                + 0.17 * std::sin(2.0 * std::numbers::pi * 1771 * second))
            : 0.018 * std::sin(2.0 * std::numbers::pi * 440 * second));
    }
    const auto features = analyzeAudio(audio, sampleRate);
    const auto drops = recommendDrops(features, {});
    bool ok = expect(features.frames.size() == 48, "audio feature cadence");
    ok &= expect(features.frames.at(20).lowBand > features.frames.at(2).lowBand,
        "spectral feature detects the chorus bass band");
    ok &= expect(!drops.isEmpty() && drops.first().confident
        && std::abs(drops.first().range.startSeconds - 8.0) <= 1.0,
        "the abrupt sustained chorus ranks first");
    ok &= expect(recommendDrops(analyzeAudio(QVector<float>(sampleRate * 6), sampleRate), {}).isEmpty(),
        "silent PCM never produces a drop");
    ok &= expect(recommendDrops(analyzeAudio(audio.sliced(0, sampleRate * 2), sampleRate), {}).isEmpty(),
        "very short audio has no drop");
    const auto steady = recommendDrops(analyzeAudio(audio.sliced(0, sampleRate * 7), sampleRate), {});
    ok &= expect(std::none_of(steady.begin(), steady.end(), [](const auto& candidate) { return candidate.confident; }),
        "steady audio cannot be a confident drop");
    PressureCurve intense;
    intense.frames.resize(96);
    for (auto& frame : intense.frames) frame.strength = 1.0;
    const auto quiet = recommendDrops(analyzeAudio(audio.sliced(0, sampleRate * 7), sampleRate), intense);
    ok &= expect(std::none_of(quiet.begin(), quiet.end(), [](const auto& candidate) { return candidate.confident; }),
        "high chart pressure does not make steady audio a confident drop");
    const std::atomic_bool cancelled{true};
    ok &= expect(analyzeAudio(audio, sampleRate, &cancelled).frames.isEmpty(), "audio DSP honors cancellation");
    return ok;
}

bool testPressure()
{
    SimaiParseResult chart;
    chart.durationSeconds = 10.0;
    TimelineNoteMarker hold;
    hold.type = QStringLiteral("hold");
    hold.second = 2.0;
    hold.endSecond = 4.0;
    TimelineNoteMarker tap;
    tap.type = QStringLiteral("tap");
    tap.second = 2.0;
    tap.lane = 5;
    TimelineNoteMarker slide;
    slide.type = QStringLiteral("slide");
    slide.second = 2.0;
    slide.slideTraceSecond = 2.5;
    slide.endSecond = 4.0;
    slide.hasHeadStar = false;
    chart.noteMarkers = {hold, tap, slide};
    const auto curve = estimatePressure(chart, 0.5, 12.0);
    bool ok = expect(curve.frames.size() == 48, "pressure spans the media duration");
    ok &= expect(curve.frames.at(10).simultaneous == 1.0 && curve.frames.at(10).movement > 0.0,
        "a distant chord is one simultaneous load in offset media time");
    ok &= expect(curve.frames.at(14).held == 1.0 && curve.frames.at(14).slides == 1.0,
        "hold and slide occupations overlap separately");
    ok &= expect(curve.frames.at(14).hybrid > 0.0,
        "a concurrent hold and headless slide require additional effort");
    ok &= expect(curve.frames.at(18).density > 0.0 && curve.frames.at(18).held == 0.0,
        "the scoring event at a hold release is not another press");
    const auto shifted = estimatePressure(chart, -0.5, 0.0);
    ok &= expect(shifted.frames.at(6).simultaneous == 1.0, "negative first applies once");
    return ok;
}

SimaiParseResult repeatLanes(const QVector<int>& lanes, double start, double interval, int count)
{
    SimaiParseResult chart;
    chart.durationSeconds = 20.0;
    for (int i = 0; i < count; ++i) {
        TimelineNoteMarker note;
        note.type = QStringLiteral("tap");
        note.second = start + interval * i;
        note.lane = lanes.at(i % lanes.size());
        chart.noteMarkers.append(note);
    }
    return chart;
}

const PressureFrame& frameAt(const PressureCurve& curve, double second)
{
    return curve.frames.at(static_cast<qsizetype>(second / curve.secondsPerFrame));
}

bool testMovementAndEndurance()
{
    const auto near = estimatePressure(repeatLanes({1, 2}, 0.0, 0.125, 64), 0.0, 0.0);
    const auto far = estimatePressure(repeatLanes({1, 5}, 0.0, 0.125, 64), 0.0, 0.0);
    const auto rotated = estimatePressure(repeatLanes({4, 8}, 0.0, 0.125, 64), 0.0, 0.0);
    const auto isolated = estimatePressure(repeatLanes({1, 5}, 7.0, 0.125, 8), 0.0, 0.0);
    const auto& sustained = frameAt(far, 7.5);
    const auto& nearby = frameAt(near, 7.5);
    const auto& recovered = frameAt(far, 16.0);
    bool ok = expect(nearby.density == sustained.density && nearby.movement < sustained.movement
        && nearby.strength + 0.08 < sustained.strength, "equal TPS with longer reaches produces more pressure");
    ok &= expect(std::abs(frameAt(rotated, 7.5).strength - sustained.strength) < 1e-12,
        "rotating all button contacts preserves their effort");
    ok &= expect(frameAt(isolated, 7.5).density == sustained.density
        && frameAt(isolated, 7.5).endurance + 0.3 < sustained.endurance,
        "sustained exertion increases pressure above an isolated copy of the same pattern");
    ok &= expect(recovered.density == 0.0 && recovered.endurance < sustained.endurance * 0.1,
        "endurance load recovers during a rest");

    const auto alternating = estimatePressure(repeatLanes({1, 4}, 0.0, 0.125, 64), 0.0, 0.0);
    const auto circling = estimatePressure(repeatLanes({1, 4, 7, 2, 5, 8, 3, 6}, 0.0, 0.125, 64), 0.0, 0.0);
    ok &= expect(frameAt(alternating, 4.0).movement < frameAt(circling, 4.0).movement,
        "alternating revisits are easier than a series of equally long new reaches");

    const QString phrase = QStringLiteral("{16}") + QStringLiteral("1,5,").repeated(32) + QStringLiteral("E");
    const auto slowChart = SimaiParser::parseForTimeline(QStringLiteral("(120)") + phrase);
    const auto fastChart = SimaiParser::parseForTimeline(QStringLiteral("(240)") + phrase);
    const auto slow = estimatePressure(slowChart, 0.0, 20.0);
    const auto fast = estimatePressure(fastChart, 0.0, 20.0);
    ok &= expect(slowChart.ok && fastChart.ok && frameAt(fast, 1.25).strength > frameAt(slow, 1.25).strength,
        "a higher BPM raises exertion when the same subdivision reduces contact spacing in time");
    return ok;
}

bool testTouchAndTraceGeometry()
{
    auto chart = repeatLanes({1}, 0.0, 0.125, 16);
    for (qsizetype i = 0; i < chart.noteMarkers.size(); ++i) {
        auto& note = chart.noteMarkers[i];
        note.type = QStringLiteral("touch");
        note.touchPoint = i % 2 ? QPointF(300.0, 195.0) : QPointF(100.0, 315.0);
    }
    auto transformed = chart;
    for (auto& note : transformed.noteMarkers) {
        const auto p = note.touchPoint;
        note.touchPoint = QPointF(540.0 - p.y() + 21.0, p.x() - 17.0);
    }
    const auto originalCurve = estimatePressure(chart, 0.0, 0.0);
    const auto movedCurve = estimatePressure(transformed, 0.0, 0.0);
    bool ok = expect(frameAt(originalCurve, 1.0).movement > 0.0
        && std::abs(frameAt(originalCurve, 1.0).strength - frameAt(movedCurve, 1.0).strength) < 1e-12,
        "touch displacement is measured and is invariant to rigid transforms");

    TimelineNoteMarker trace;
    trace.type = QStringLiteral("slide");
    trace.second = 1.0;
    trace.slideTraceSecond = 1.0;
    trace.endSecond = 1.125;
    trace.hasHeadStar = false;
    trace.slideSegmentPoints = {{QPointF(0, 0), QPointF(240, 0)}};
    TimelineNoteMarker hold;
    hold.type = QStringLiteral("hold");
    hold.second = 1.125;
    hold.endSecond = 1.25;
    chart.noteMarkers = {trace, hold};
    const auto separate = estimatePressure(chart, 0.0, 0.0);
    ok &= expect(frameAt(separate, 1.0).slides == 0.5 && frameAt(separate, 1.0).held == 0.5
        && frameAt(separate, 1.0).hybrid == 0.0, "separate occupations within the same frame do not overlap");
    ok &= expect(frameAt(separate, 1.0).movement > 0.0, "a headless trace contributes path movement");
    chart.noteMarkers[0].type = QStringLiteral("wifi");
    chart.noteMarkers[0].wifiLanePoints = trace.slideSegmentPoints;
    const auto wifi = estimatePressure(chart, 0.0, 0.0);
    ok &= expect(frameAt(wifi, 1.0).hybrid > frameAt(separate, 1.0).hybrid
        && frameAt(wifi, 1.0).strength > frameAt(separate, 1.0).strength, "Wi-Fi traces add concurrent load");
    chart.noteMarkers[0].trackMine = true;
    const auto mine = estimatePressure(chart, 0.0, 0.0);
    ok &= expect(frameAt(mine, 1.0).slides == 0.0, "a headless mine trace adds no occupation");
    return ok;
}

bool testDocumentRegion()
{
    auto document = SimaiDocument::createEmpty();
    document.ensureDifficulty(6).chart = QStringLiteral("(120){4}1,2,3,4,\nE");
    document.ensureDifficulty(5).chart = QStringLiteral("(120){4}1,1,1,1,\nE");
    const QString audio = QStringLiteral("synthetic-audio-sha256");
    const QString fingerprint = dropInputFingerprint(document, 6, audio);
    const DropSelection detected{{12.0, 23.0}, DropSource::Auto, fingerprint};
    const qsizetype originalFields = document.extraFields.size();
    bool ok = expect(writeDropSelection(document, 6, detected), "store a difficulty-specific auto drop");
    ok &= expect(document.extraFields.size() == originalFields + 5
        && document.difficulty(6)->chart == QStringLiteral("(120){4}1,2,3,4,\nE"),
        "only the five drop metadata fields are written; the chart contains no generated annotations");
    ok &= expect(!writeDropSelection(document, 6, detected), "the same metadata is a no-op");
    ok &= expect(dropInputFingerprint(document, 6, audio) == fingerprint, "metadata write does not invalidate its input");
    ok &= expect(readDropSelection(document, 6, fingerprint, 30.0) == detected, "current auto drop resolves");
    auto oldAlgorithm = document;
    for (auto& field : oldAlgorithm.extraFields) {
        if (field.key == QStringLiteral("miacode_drop_version_6")) field.value = QStringLiteral("1");
    }
    ok &= expect(!readDropSelection(oldAlgorithm, 6, fingerprint, 30.0),
        "the previous pressure algorithm's auto drop is invalidated");
    ok &= expect(!readDropSelection(document, 5, fingerprint, 30.0), "drop does not cross difficulty");
    ok &= expect(!readDropSelection(document, 6, QStringLiteral("stale"), 30.0), "stale audio or chart invalidates auto drop");
    auto changedChart = document;
    changedChart.first = QStringLiteral("-0.3");
    ok &= expect(dropInputFingerprint(changedChart, 6, audio) != fingerprint, "first changes the chart input fingerprint");
    ok &= expect(seedDemoWindow(document, detected.range, 30.0), "seed a portable demo when absent");
    ok &= expect(resolveIntroWindow(document, 6, {}, IntroWindowMode::Automatic, 30.0, fingerprint).source
        == IntroWindowSource::Drop, "generated demo never hides the current drop");
    document.extraFields.append({QStringLiteral("miacode_demo_source"), QStringLiteral("manual")});
    document.extraFields.append({QStringLiteral("demo_seek"), QStringLiteral("3")});
    document.extraFields.append({QStringLiteral("demo_len"), QStringLiteral("4")});
    ok &= expect(!seedDemoWindow(document, detected.range, 30.0), "retain the saved demo");
    ok &= expect(resolveIntroWindow(document, 6, {}, IntroWindowMode::Automatic, 30.0, fingerprint).range
        == MediaRange{3.0, 7.0}, "the saved demo is independent of the drop");
    ok &= expect(resolveIntroWindow(document, 6, {}, IntroWindowMode::FollowDrop, 30.0, fingerprint).range
        == MediaRange{12.0, 20.5}, "follow drop bypasses demo and retains the intro contract");
    ok &= expect(resolveIntroWindow(document, 6, MediaRange{27.0, 37.0}, IntroWindowMode::Automatic, 30.0, fingerprint).range
        == MediaRange{27.0, 30.0}, "explicit start near the end clips to available media");
    const DropSelection manual{{8.0, 14.0}, DropSource::Manual, audio};
    ok &= expect(writeDropSelection(document, 6, manual), "manual replaces auto");
    const auto reopened = SimaiDocument::fromText(document.toText());
    ok &= expect(readDropSelection(reopened, 6, {}, 30.0) == manual, "manual round-trips without auto fingerprint");
    ok &= expect(!readDropSelection(reopened, 6, {}, 12.0), "manual past media duration requires review");
    ok &= expect(resolveIntroWindow(reopened, 6, {}, IntroWindowMode::FollowDrop, 30.0, {}, {}, true).source
        == IntroWindowSource::Default, "authoritative session clear suppresses an on-disk selection");
    ok &= expect(!MediaRange{2.0, std::numeric_limits<double>::infinity()}.isValid(), "reject non-finite times");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const QCoreApplication app(argc, argv);
    const bool audio = testAudioStructure();
    const bool pressure = testPressure();
    const bool movement = testMovementAndEndurance();
    const bool traces = testTouchAndTraceGeometry();
    const bool document = testDocumentRegion();
    const QString lf = QStringLiteral("&lv_5=12\n&inote_5=(120)\n{4}1,2,3,4,E\n");
    QString crlf = lf; crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const bool newlineIdentity = expect(dropChartFingerprint(SimaiDocument::fromText(lf), 5)
        == dropChartFingerprint(SimaiDocument::fromText(crlf), 5),
        "logical LF/CRLF equivalents share a portable drop fingerprint");
    return audio && pressure && movement && traces && document && newlineIdentity ? 0 : 1;
}
