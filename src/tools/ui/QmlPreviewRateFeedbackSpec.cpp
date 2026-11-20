// The rate commands are the one transport pair the backend applies WITHOUT
// announcing. PlaybackCoordinator::applyPreviewPlaybackRate writes the new rate
// and drives audio and the media route, but emits no shell notification; while
// the preview is paused no playhead tick arrives either. A model that only
// listens therefore keeps reporting the rate the session started with, and
// everything downstream reads from it — the menu's tick, the transport label,
// the rate HUD — so the rate menu and Ctrl+O / Ctrl+P look completely dead from
// the outside while the backend is in fact obeying them.
//
// This pins the read-back, not the notification: the shell must end a rate
// command knowing what the backend actually settled on.

#include "app/ui/preview/PreviewModel.h"
#include "tools/ui/FakePreviewBackend.h"
#include "app/services/PreviewSurface.h"
#include "app/services/ShellNotifications.h"
#include "common/DebugLog.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTextStream>

namespace {

bool expect(bool condition, const QString& message, QTextStream& out, int* failed)
{
    out << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
    out.flush();
    if (!condition) ++*failed;
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    int failed = 0;

    using namespace miacode::test;
    FakeBackendRate backend;
    FakeSurface surface(backend);
    FakeControl control(backend);
    miacode::ShellNotifications notifications;
    miacode::PreviewSurface* surfaceSlot = &surface;
    miacode::PlaybackControl* controlSlot = &control;
    miacode::ui::PreviewModel model(notifications, surfaceSlot, controlSlot);

    expect(qFuzzyCompare(model.rate(), 1.0),
           QStringLiteral("the model starts on the backend's rate"), out, &failed);

    // Nothing pushes while paused, so the read-back is the model's only chance.
    QSignalSpy transportSpy(&model, &miacode::ui::PreviewModel::transportChanged);
    model.setRate(1.25);
    expect(backend.setCount == 1, QStringLiteral("a rate pick reaches the backend"), out, &failed);
    expect(qFuzzyCompare(model.rate(), 1.25),
           QStringLiteral("the model reports the rate the backend settled on"), out, &failed);
    expect(transportSpy.count() >= 1,
           QStringLiteral("the rate change is announced to QML"), out, &failed);

    model.adjustRate(-1);
    expect(backend.nudgeCount == 1, QStringLiteral("a speed shortcut reaches the backend"), out, &failed);
    expect(qFuzzyCompare(model.rate(), 1.0),
           QStringLiteral("a nudge is visible to QML without a playhead tick"), out, &failed);

    // The clamp lives in the backend, and the shell must report the clamped
    // value rather than the one it asked for.
    model.setRate(0.1);
    expect(qFuzzyCompare(model.rate(), 0.25),
           QStringLiteral("a clamped request reports the clamped rate, not the request"),
           out, &failed);

    model.setHandOrderBreakdown();
    expect(!model.handOrderFeaturesEnabled() && !model.handOrderBreakdown(),
           QStringLiteral("breakdown mode requires explicit opt-in"), out, &failed);
    model.setHandOrderFeaturesEnabled(true);
    QSignalSpy modeSpy(&model, &miacode::ui::PreviewModel::renderModeChanged);
    model.setHandOrderBreakdown();
    expect(model.handOrderBreakdown() && !model.simulatedPlay() && !model.debugRendering()
           && model.renderMode() == QStringLiteral("hand_order_breakdown") && modeSpy.count() == 1,
           QStringLiteral("breakdown command and notification reach QML while paused"), out, &failed);
    model.setSimulatedPlay();
    expect(!model.handOrderBreakdown() && model.simulatedPlay(),
           QStringLiteral("simulated play hides breakdown steps"), out, &failed);
    model.setDebugRendering();
    expect(!model.handOrderBreakdown() && model.debugRendering(),
           QStringLiteral("debug rendering hides breakdown steps"), out, &failed);
    model.setMuriCheckEnabled(true);
    expect(!model.handOrderBreakdown() && model.muriCheckEnabled(),
           QStringLiteral("Muri rendering hides breakdown steps"), out, &failed);
    model.setMuriCheckEnabled(false);
    expect(!model.handOrderBreakdown() && model.renderMode() == QStringLiteral("native"),
           QStringLiteral("normal rendering hides breakdown steps"), out, &failed);

    // The model logs through the async writer. Join it while QCoreApplication is
    // still alive, as the product entry does; joining it from static teardown
    // after the application object is gone intermittently hangs the process.
    miacode::debug_log::shutdownAsyncLogWriter();

    if (failed != 0) {
        out << "QmlPreviewRateFeedback spec failed: " << failed << '\n';
        return 1;
    }
    out << "QmlPreviewRateFeedback spec passed.\n";
    return 0;
}
