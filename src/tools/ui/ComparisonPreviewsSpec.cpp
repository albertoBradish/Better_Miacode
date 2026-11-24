#include "app/ui/preview/ComparisonPreviews.h"
#include "app/ui/preview/PreviewModel.h"
#include "common/RuntimePaths.h"
#include "core/video/AssetPaths.h"
#include "tools/ui/FakePreviewBackend.h"
#include "tools/preview/NullPreviewAudioBackend.h"

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QVideoSink>
#include <QtQml/QQmlExtensionPlugin>
Q_IMPORT_QML_PLUGIN(MiaCode_PreviewPlugin)
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QFile>
#include <QTextStream>
#include <algorithm>

using namespace miacode;
using namespace miacode::ui;

namespace {
struct Rendered { std::atomic_bool masterDifficulty{false}, remasterDifficulty{false}; };
class AudioProbe final : public preview_audio::test::NullPreviewAudioBackend {
public:
    explicit AudioProbe(Rendered& log) : log_(log) {}
    void bindOutputRoute(std::shared_ptr<PreviewOutputRoute> route) override { route_ = std::move(route); }
    void configureTimeline(const QVector<TimelineNoteMarker>& markers, double, const PreviewTimingSettings&) override {
        notes_ = markers.size();
    }
    double startPreviewPlaybackTransaction(double second, bool, double) override {
        float samples[]{1, 1};
        if (route_) route_->process(samples, 2);
        if (notes_ == 3 && samples[0] > samples[1]) log_.masterDifficulty = true;
        if (notes_ == 2 && samples[1] > samples[0]) log_.remasterDifficulty = true;
        return second;
    }
private:
    Rendered& log_;
    int notes_ = 0;
    std::shared_ptr<PreviewOutputRoute> route_;
};
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir temp(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT "/comparisons-XXXXXX"));
    if (!temp.isValid()) return 2;
    QDir root(temp.path());
    root.mkpath("runtime");
    root.mkpath("app");
    const auto put = [&](const QString& relative, const QByteArray& bytes) {
        QFile file(root.filePath(relative));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", root.filePath("runtime").toUtf8());
    if (!put("runtime/.better-miacode-runtime", "BetterMiacode.runtime.v1")) return 2;
    QString error;
    if (!runtime_paths::initializeFromEnvironment(&error) || !runtime_paths::configureProcessEnvironment(&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }

    Rendered rendered;
    preview_audio::installPreviewAudioBackendProvider({[&] { return std::make_unique<AudioProbe>(rendered); }, {}});
    test::FakeBackendRate shared;
    root.mkpath("assets/background/outlines");
    assets::setAssetRoot(root.filePath("assets"));
    const auto writeImage = [&](const QString& relative, const QColor& color) {
        QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(color);
        return image.save(root.filePath(relative));
    };
    if (!writeImage("assets/background/outlines/1.maimai.png", Qt::red)
        || !writeImage("assets/background/outlines/8.DX.png", Qt::blue)) return 2;
    PreviewRuntime live;
    PreviewStageMediaHost primaryMedia;
    shared.runtime = &live;
    shared.mediaHost = &primaryMedia;
    shared.renderSettings.insert(QStringLiteral("outlineAutomatic"), true);
    test::FakeSurface surface(shared);
    test::FakeControl control(shared);
    PreviewSurface* surfaceSlot = &surface;
    PlaybackControl* controlSlot = &control;
    ShellNotifications notifications;
    PreviewModel main(notifications, surfaceSlot, controlSlot);
    ChartWorkspace workspace;
    ComparisonPreviews views(workspace, main);
    int failed = 0;
    const auto check = [&](bool ok, const char* message) {
        QTextStream(stdout) << (ok ? "PASS " : "FAIL ") << message << Qt::endl;
        if (!ok) ++failed;
    };
    const auto get = [&](int index) {
        return qobject_cast<ComparisonPreview*>(views.data(views.index(index), Qt::UserRole + 1).value<QObject*>());
    };
    const auto settled = [](auto predicate) {
        return QTest::qWaitFor(predicate, 10000);
    };
    {
        root.mkpath("video");
        // The FFmpeg probe reads the stream header. A raw clip keeps the test
        // independent of external encoders and of GPU decoder availability.
        QByteArray clip = "YUV4MPEG2 W16 H16 F30:1 Ip A1:1 C420jpeg\n";
        for (int i = 0; i < 30; ++i) {
            clip += "FRAME\n";
            clip += QByteArray(16 * 16, char(16 + i * 4));
            clip += QByteArray(16 * 16 / 2, char(128));
        }
        if (!put("video/test.mp4", clip) || !put("video/maidata.txt", "&title=Video\n")) return 2;
        QVideoSink firstSink, secondSink;
        QObject firstOutput, secondOutput;
        firstOutput.setProperty("videoSink", QVariant::fromValue(static_cast<QObject*>(&firstSink)));
        secondOutput.setProperty("videoSink", QVariant::fromValue(static_cast<QObject*>(&secondSink)));
        PreviewStageMediaHost video;
        video.setVideoDecodePreference(true);
        video.attachVideoOutputObject(&firstOutput);
        video.attachVideoOutputObject(&secondOutput);
        video.setChartPath(root.filePath("video/maidata.txt"), QStringLiteral("test.mp4"));
        video.startPlayback(0);
        check(settled([&] { return firstSink.videoFrame().isValid() && secondSink.videoFrame().isValid(); }),
            "one decoder supplies the same video to both preview sinks");
        video.detachVideoOutputObject(&firstOutput);
        check(!firstSink.videoFrame().isValid() && secondSink.videoFrame().isValid(),
            "detaching one output leaves the other consumer attached");
        check(video.releaseDecoderForFileReplace() && !secondSink.videoFrame().isValid(),
            "releasing a video clears retained frames on every sink");
    }
    const QString chart = "&title=Twin\n&first=1.25\n&lv_5=12\n&inote_5=(120){4}1,2,3,4,E\n&lv_6=13+\n&inote_6=(120){4}5,C,E\n";
    const auto open = [&](const QString& folder, const QString& text, int difficulty) {
        root.mkpath(folder);
        put(folder + "/maidata.txt", text.toUtf8());
        return workspace.openSource(text, root.filePath(folder + "/maidata.txt"), difficulty).accepted;
    };
    PreviewOutputRoute left, right;
    float aSamples[]{4.f, 4.f, -4.f, -4.f}, bSamples[]{4.f, 4.f, -4.f, -4.f};
    left.set(0.45, -1); right.set(0.45, 1);
    left.process(aSamples, 4); right.process(bSamples, 4);
    check(aSamples[0] > 0.44f && aSamples[1] == 0 && bSamples[0] == 0 && bSamples[1] > 0.44f,
        "independent stereo feeds pan to opposite speakers");
    left.set(0.45, 0); right.set(0.45, 0);
    float peaks[]{3, -3}, otherPeaks[]{3, -3};
    left.process(peaks, 2); right.process(otherPeaks, 2);
    check(peaks[0] + otherPeaks[0] <= 0.901f && peaks[1] + otherPeaks[1] >= -0.901f,
        "correlated over-range signals stay within the shared headroom");
    PreviewOutputRoute handoff(true);
    handoff.attach(1000, 0.004);
    handoff.set(0, 0);
    float preload[8] = {};
    handoff.process(preload, 8);
    check(handoff.drained(), "a muted mixer acknowledges its buffered tail");
    const auto superseded = handoff.fadeIn(1, 0);
    handoff.set(0, 0);
    const auto fresh = handoff.fadeIn(1, 0);
    handoff.releaseFade(superseded);
    float oldFrames[]{0.5f, 0.5f, 0.5f, 0.5f};
    handoff.process(oldFrames, 4);
    check(oldFrames[0] == 0 && oldFrames[2] == 0, "a superseded start cannot pass old audio");
    handoff.releaseFade(fresh);
    float leadingSilence[64] = {};
    handoff.process(leadingSilence, 64);
    float newFrames[48];
    std::ranges::fill(newFrames, 0.5f);
    handoff.process(newFrames, 48);
    check(newFrames[0] > 0 && newFrames[0] < 0.03f && std::abs(newFrames[46] - 0.5f) < 0.001f,
        "fresh PCM fades in over 24 ms after the decoder starts producing signal");
    handoff.set(0, 0);
    float outgoing[48];
    std::ranges::fill(outgoing, 0.5f);
    handoff.process(outgoing, 48);
    check(outgoing[0] > 0.47f && outgoing[0] < 0.5f && outgoing[46] == 0 && !handoff.drained(),
        "the outgoing mix reaches silence before the playback queue has drained");
    float queuedTail[6];
    std::ranges::fill(queuedTail, 0.5f);
    handoff.process(queuedTail, 6);
    check(handoff.drained() && queuedTail[0] == 0 && queuedTail[4] == 0,
        "the next source can open after the rendered buffer tail drains");
    handoff.set(1, 0);
    float refocused[]{0.5f, 0.5f};
    handoff.process(refocused, 2);
    handoff.set(0, 0);
    float retiring[]{0.5f, 0.5f};
    handoff.process(retiring, 2);
    handoff.set(1, 0);
    float reversed[]{0.5f, 0.5f};
    handoff.process(reversed, 2);
    check(retiring[0] < refocused[0] && reversed[0] > retiring[0] && reversed[0] < 0.06f,
        "rapid refocus reverses the current envelope without replaying an old ramp");
    double masterGain = -1, masterPan = 0;
    QObject::connect(&notifications, &ShellNotifications::previewOutputRouteChanged, &main,
        [&](double gain, double pan) { masterGain = gain; masterPan = pan; });
    views.addCurrent();
    check(!views.count(), "an empty editor adds nothing");
    open("A", chart, 5);
    if (!writeImage("A/bg.png", Qt::black)) return 2;
    primaryMedia.setChartPath(root.filePath("A/maidata.txt"));
    views.addCurrent();
    workspace.selectDifficulty(6);
    views.addCurrent();
    auto* a = get(0);
    auto* b = get(1);
    if (!a || !b) return 1;
    views.focusPanelAudio(a->panelId());
    check(masterGain == 0, "focusing a child mutes the main preview");
    views.focusPanelAudio(QStringLiteral("editor"));
    check(masterGain == 0, "focusing an editor preserves the selected child");
    views.focusPanelAudio(QStringLiteral("preview"));
    check(masterGain == 1, "focusing the main preview restores its renderer");
    views.setMixAudio(true); views.setMainPan(-0.8); a->setScreenPan(0.7);
    check(std::abs(masterGain - 0.3) < 0.001 && masterPan == -0.8, "three windows reserve headroom and follow their screen positions");
    views.focusPanelAudio(b->panelId());
    check(std::abs(masterGain - 0.3) < 0.001 && masterPan == -0.8, "focus leaves mixed gain and pan intact");
    views.setMixAudio(false);
    check(masterGain == 0, "leaving mix mode restores the last focused child");
    b->setSyncEnabled(false);
    check(!b->linked() && a->linked(), "individual sync icon detaches a single transport");
    b->setSyncEnabled(true);
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral(MIACODE_QML_SPEC_IMPORT_ROOT));
    engine.rootContext()->setContextProperty("testComparisons", &views);
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
import QtQuick.Window
import MiaCode.UI
Window {
    id: host
    width: 700; height: 500; visible: true
    Item { id: mainPane; objectName: "mainPane"; anchors.fill: parent }
    Item { id: panels; objectName: "dock"; property string activePanel: ""; function hostFor(id) { return mainPane } }
    ComparisonPreviewPanels { dock: panels; comparisons: testComparisons; workspaceWindow: host; primaryPane: mainPane }
}
)qml", QUrl());
    std::unique_ptr<QObject> ui(component.create());
    if (!ui) { QTextStream(stderr) << component.errorString(); return 1; }
    auto* pane = ui->findChild<QQuickItem*>("mainPane");
    check(pane && settled([&] { return pane->childItems().size()==2; }), "both child previews use workspace panel hosts");
    auto* dock = ui->findChild<QQuickItem*>("dock");
    dock->setProperty("activePanel", QStringLiteral("preview"));
    auto* window = qobject_cast<QQuickWindow*>(ui.get());
    QTest::mouseMove(window, QPoint(310, 150));
    QCoreApplication::processEvents();
    check(masterGain == 1, "moving over a child leaves the main preview selected");
    dock->setProperty("activePanel", b->panelId());
    check(masterGain == 0, "activating a child panel selects its audio");
    check(a->workspaceLabel() == "A" && b->workspaceLabel() == "B"
        && a->title().contains(QStringLiteral("Twin")), "workspace labels are separate from chart titles");
    const auto closeButtons = ui->findChildren<QQuickItem*>("closeComparisonPreviewButton");
    check(closeButtons.size() == 2 && closeButtons.front()->isVisible(), "children expose visible close buttons");
    check(settled([&] { return a->mediaHost() == &primaryMedia && b->mediaHost() == &primaryMedia; }),
        "linked children share the primary media host for the same file");
    b->setSyncEnabled(false);
    auto* local = qobject_cast<PreviewStageMediaHost*>(b->mediaHost());
    check(local && local != &primaryMedia && local->imageSource() == primaryMedia.imageSource(),
        "an independent clock keeps its own media route for the same background");
    b->setSyncEnabled(true);
    check(b->mediaHost() == &primaryMedia && local && !local->hasResolvedMedia(),
        "rejoining a shared clock releases the redundant media route");
    ui.reset();
    a->setSurfaceVisible(true);
    b->setSurfaceVisible(true);
    views.setPresentationActive(true);
    auto* aScene = qobject_cast<PreviewRuntime*>(a->runtime());
    auto* bScene = qobject_cast<PreviewRuntime*>(b->runtime());
    check(aScene && bScene && aScene != bScene && bScene != &live, "each difficulty owns its own render state");
    check(a->linked() && b->linked() && settled([&] { return aScene->frameState().noteMarkers.size() == 4
        && bScene->frameState().noteMarkers.size() == 2; }), "pins keep distinct difficulty data");
    check(settled([&] {
        const auto& firstOutline = aScene->frameState().assets.outlineImage;
        const auto& secondOutline = bScene->frameState().assets.outlineImage;
        return !firstOutline.isNull() && !secondOutline.isNull()
            && firstOutline.pixelColor(0, 0) == Qt::red && secondOutline.pixelColor(0, 0) == Qt::blue;
    }), "automatic judgement rings use each pinned difficulty's own note types");
    check(!aScene->frameState().noteMarkers.isEmpty() && std::abs(aScene->frameState().noteMarkers.first().second - 1.25) < 0.001, "first offset is included once");
    workspace.replaceDifficultyChart(5, "(120){4}1,2,3,E");
    check(settled([&] { return aScene->frameState().noteMarkers.size() == 3; })
        && bScene->frameState().noteMarkers.size() == 2, "edits refresh only the pinned difficulty that changed");
    live.setPlayheadSeconds(2.5);
    live.setTapFlowSpeed(5);
    check(a->position() == 2.5 && b->position() == 2.5
        && bScene->frameState().render.tapFlowSpeed == live.frameState().render.tapFlowSpeed, "time and appearance follow main");
    views.setMixAudio(true);
    a->setScreenPan(-0.9); b->setScreenPan(0.9);
    shared.playing = true;
    emit notifications.presentationChanged();
    check(settled([&] { return rendered.masterDifficulty.load() && rendered.remasterDifficulty.load(); }),
          "both linked audio workers receive their own difficulty markers and stereo route");
    shared.playing = false;
    emit notifications.presentationChanged();
    views.setMixAudio(false);
    b->seek(3.0);
    check(shared.position == 3.0, "seeking a linked tile addresses the master");
    a->togglePlayback();
    check(shared.playing, "linked play addresses the master");
    shared.playing = false;
    views.setSynchronize(false);
    a->seek(1.0);
    b->seek(2.0);
    a->togglePlayback();
    check(settled([&] { return a->position() > 1.1; }) && b->position() == 2.0 && !b->playing(), "independent clocks and seeks");
    a->togglePlayback();
    views.setSynchronize(true);
    const auto tabA = workspace.captureSong();
    open("B", QString(chart).replace(QStringLiteral("Twin"), QStringLiteral("Other")), 5);
    primaryMedia.setChartPath(root.filePath("B/maidata.txt"));
    check(a->mediaHost() && a->mediaHost() != &primaryMedia && b->mediaHost() == a->mediaHost(),
        "an independent child group shares one media host without the current document");
    a->seek(1.5);
    check(a->position() == 1.5 && b->position() == 1.5 && shared.position == 3.0, "same-song pins still share a clock after switching documents");
    a->togglePlayback();
    a->close();
    check(views.count() == 1 && b->playing() && settled([&] { return b->position() > 1.55; }), "closing the independent group leader preserves playback");
    views.setPresentationActive(false);
    check(!b->playing(), "hiding comparisons parks local playback");
    views.setPresentationActive(true);
    views.addCurrent();
    auto* other = get(1);
    check(other && other->linked() && !b->linked(), "different songs stay independent");
    const auto tabB = workspace.captureSong();
    workspace.restoreSong(tabA);
    check(b->linked() && !other->linked() && views.count()==2,
          "returning to A reconnects only the preview pinned to A");
    workspace.restoreSong(tabB);
    check(!b->linked() && other->linked() && views.count()==2,
          "returning to B keeps both previews and reconnects B");
    b->close();
    if (!other) return 1;
    other->close();

    // Different physical paths are common after importing the same song into two packs.
    open("Copy1", chart, 5);
    put("Copy1/track.wav", "matching audio contents");
    views.addCurrent();
    auto* copy = get(0);
    if (!copy) return 1;
    root.mkpath("Copy2");
    put("Copy2/track.wav", "matching audio contents");
    open("Copy2", chart, 6);
    check(settled([&] { return copy->linked(); }), "identical tracks in different folders synchronize");
    root.mkpath("Different");
    put("Different/track.wav", "unrelated audio contents");
    open("Different", chart, 6);
    check(!copy->linked(), "matching titles do not link different tracks");
    copy->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThreadPool::globalInstance()->waitForDone();
    return failed ? 1 : 0;
}
