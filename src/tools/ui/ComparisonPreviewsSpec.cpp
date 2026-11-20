#include "app/ui/preview/ComparisonPreviews.h"
#include "app/ui/preview/PreviewModel.h"
#include "common/RuntimePaths.h"
#include "tools/ui/FakePreviewBackend.h"
#include "tools/preview/NullPreviewAudioBackend.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QtQml/QQmlExtensionPlugin>
Q_IMPORT_QML_PLUGIN(MiaCode_PreviewPlugin)
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QFile>
#include <QTextStream>

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
    PreviewRuntime live;
    shared.runtime = &live;
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
    const QString chart = "&title=Twin\n&first=1.25\n&lv_5=12\n&inote_5=(120){4}1,2,3,4,E\n&lv_6=13+\n&inote_6=(120){4}5,6,E\n";
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
    double masterGain = -1, masterPan = 0;
    QObject::connect(&notifications, &ShellNotifications::previewOutputRouteChanged, &main,
        [&](double gain, double pan) { masterGain = gain; masterPan = pan; });
    views.addCurrent();
    check(!views.count(), "an empty editor adds nothing");
    open("A", chart, 5);
    views.addCurrent();
    workspace.selectDifficulty(6);
    views.addCurrent();
    auto* a = get(0);
    auto* b = get(1);
    if (!a || !b) return 1;
    a->focusAudio(true);
    check(masterGain == 0, "pointing at a pin mutes master audio without changing transport");
    views.focusMainAudio();
    check(masterGain == 1, "pointing at main restores the master renderer");
    views.setMixAudio(true); views.setMainPan(-0.8); a->setScreenPan(0.7);
    check(std::abs(masterGain - 0.3) < 0.001 && masterPan == -0.8, "three windows reserve headroom and follow their screen positions");
    views.setMixAudio(false);
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
    Item { id: panels; function hostFor(id) { return mainPane } }
    ComparisonPreviewPanels { dock: panels; comparisons: testComparisons; workspaceWindow: host; primaryPane: mainPane }
}
)qml", QUrl());
    std::unique_ptr<QObject> ui(component.create());
    if (!ui) { QTextStream(stderr) << component.errorString(); return 1; }
    auto* pane = ui->findChild<QQuickItem*>("mainPane");
    check(pane && settled([&] { return pane->childItems().size()==2; }), "both child previews use workspace panel hosts");
    check(qobject_cast<PreviewStageMediaHost*>(a->mediaHost())->hidePv(), "child preview suppresses video decoding");
    ui.reset();
    views.setPresentationActive(true);
    auto* aScene = qobject_cast<PreviewRuntime*>(a->runtime());
    auto* bScene = qobject_cast<PreviewRuntime*>(b->runtime());
    check(aScene && bScene && aScene != bScene && bScene != &live, "each difficulty owns its own render state");
    check(a->linked() && b->linked() && settled([&] { return aScene->frameState().noteMarkers.size() == 4
        && bScene->frameState().noteMarkers.size() == 2; }), "pins keep distinct difficulty data");
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
