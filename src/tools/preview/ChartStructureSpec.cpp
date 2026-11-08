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
    ok &= expect(curve.frames.at(18).density > 0.0 && curve.frames.at(18).held == 0.0,
        "the scoring event at a hold release is not another press");
    const auto shifted = estimatePressure(chart, -0.5, 0.0);
    ok &= expect(shifted.frames.at(6).simultaneous == 1.0, "negative first applies once");
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
    bool ok = expect(writeDropSelection(document, 6, detected), "store a difficulty-specific auto drop");
    ok &= expect(!writeDropSelection(document, 6, detected), "the same metadata is a no-op");
    ok &= expect(dropInputFingerprint(document, 6, audio) == fingerprint, "metadata write does not invalidate its input");
    ok &= expect(readDropSelection(document, 6, fingerprint, 30.0) == detected, "current auto drop resolves");
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
    const bool document = testDocumentRegion();
    return audio && pressure && document ? 0 : 1;
}
