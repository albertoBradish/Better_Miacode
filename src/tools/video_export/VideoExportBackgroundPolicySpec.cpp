#include "core/scene/PreviewLayerOrder.h"
#include "export/session/PreviewQuickD3D11ExportSession.h"
#include "export/session/PreviewQuickExportSession.h"
#include "export/video_export/VideoExportQuickRenderBackend.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTemporaryDir>
#include <cmath>
#include <functional>

namespace {

using Step = std::function<bool(QImage*, bool*, bool, QString*)>;

bool require(bool condition, const QString& message)
{
    if (!condition) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return condition;
}

bool verifyBackgroundResolution()
{
    QTemporaryDir fixture(QDir::current().filePath(QStringLiteral("video-background-spec-XXXXXX")));
    if (!fixture.isValid()) return false;
    QImage skin(8, 8, QImage::Format_RGBA8888);
    skin.fill(Qt::white);
    for (const QString& name : {QStringLiteral("tap.png"), QStringLiteral("hold.png"), QStringLiteral("star.png")}) {
        if (!skin.save(fixture.filePath(name))) return false;
    }
    const QString badPvPath = fixture.filePath(QStringLiteral("pv.mp4"));
    QFile badPv(badPvPath);
    if (!badPv.open(QIODevice::WriteOnly) || badPv.write("corrupt PV") != 10) return false;
    badPv.close();

    VideoExportTask task;
    task.skinDirectory = fixture.path();
    task.chartPath = fixture.filePath(QStringLiteral("maidata.txt"));
    task.outlineVariant = PreviewOutlineVariant::None;
    const QSize size(96, 64);
    VideoExportQuickRenderBackend backend;
    QString error;
    bool ok = require(!backend.bootstrap(task, badPvPath, {}, {}, size, &error) && !error.isEmpty(),
        QStringLiteral("the invalid PV fails when the decoder opens it"));
    task.muriRenderOptions.renderMode = RenderMode::Debug;
    ok &= require(backend.bootstrap(task, badPvPath, {}, {}, size, &error),
        QStringLiteral("debug mode does not open the invalid PV: %1").arg(error));
    task.muriRenderOptions.renderMode = RenderMode::Native;
    task.sizePreset = VideoExportSizePreset::Target30Mb;
    task.includePv = false;
    ok &= require(backend.bootstrap(task, badPvPath, {}, {}, size, &error),
        QStringLiteral("the PV checkbox independently suppresses decoding at 30 MB: %1").arg(error));
    return ok;
}

bool verifiesTransparentPixels(const QImage& frame)
{
    if (frame.isNull() || frame.format() != QImage::Format_RGBA8888_Premultiplied) return false;
    const int x = frame.width() / 2;
    const auto* pixel = frame.constScanLine(frame.height() / 2) + x * 4;
    // Half-opacity red is stored premultiplied exactly once.
    return frame.pixelColor(0, 0).alpha() == 0
        && std::abs(pixel[0] - 110) <= 2 && std::abs(pixel[1] - 16) <= 2
        && std::abs(pixel[2] - 8) <= 2 && std::abs(pixel[3] - 128) <= 1;
}

template <typename Session>
bool verify(Session& session, const Step& step, bool pipelined)
{
    const QSize size(96, 64);
    session.setFrameSize(size);
    session.setLayerFlags(miacode::preview::scene::BackdropLayer);
    miacode::preview::scene::PreviewFrameState state;
    QImage sprite(8, 8, QImage::Format_RGBA8888_Premultiplied);
    sprite.fill(QColor(220, 32, 16, 128));
    state.assets.outlineImage = sprite;
    state.render.backgroundPolicy = BackgroundPolicy::Transparent;
    state.media.stageMediaAvailable = true;
    session.setFrameState(state);

    QImage background(size, QImage::Format_RGBA8888);
    background.fill(Qt::green);
    session.setBackgroundFrame(background, true, false, 1.0);
    QString error;
    const QImage transparent = session.renderFrame(&error);
    bool ok = require(verifiesTransparentPixels(transparent),
        QStringLiteral("the transparent framebuffer ignores stage media and keeps premultiplied pixels: %1").arg(error));
    if (pipelined) {
        QImage completed;
        bool ready = false;
        bool completedOk = step(&completed, &ready, false, &error);
        for (int n = 0; n < 4 && completedOk && !ready; ++n) {
            completedOk = step(&completed, &ready, true, &error);
        }
        ok &= require(completedOk && ready && verifiesTransparentPixels(completed),
            QStringLiteral("pipelined readback preserves alpha and premultiplication: %1").arg(error));
    }

    state.render.backgroundPolicy = BackgroundPolicy::OpaqueBlack;
    session.setFrameState(state);
    session.setBackgroundFrame(background, true, false, 1.0);
    const QImage black = session.renderFrame(&error);
    if (!require(!black.isNull(), QStringLiteral("the opaque framebuffer renders: %1").arg(error))) return false;
    const QColor composite = black.pixelColor(black.width() / 2, black.height() / 2);
    ok &= require(black.pixelColor(0, 0) == QColor(Qt::black) && composite.alpha() == 255
            && std::abs(composite.red() - 110) <= 2 && std::abs(composite.green() - 16) <= 2,
        QStringLiteral("debug black is opaque, discards media and composites the chart layer once"));
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication app(argc, argv);
    QString error;
    bool ok = false;
    bool testedPipeline = false;
    if (app.arguments().contains(QStringLiteral("--d3d11"))) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        PreviewQuickD3D11ExportSession session;
        if (!require(session.initialize(&error), error)) return 1;
        testedPipeline = session.supportsPipelinedReadback();
        ok = verify(session, [&](QImage* frame, bool* ready, bool drain, QString* detail) {
            return session.renderFramePipelinedStep(frame, ready, drain, detail);
        }, testedPipeline);
    } else {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        PreviewQuickExportSession session;
        if (!require(session.initialize({}, nullptr, &error), error)) return 1;
        testedPipeline = session.supportsOffscreenPboReadback();
        ok = verify(session, [&](QImage* frame, bool* ready, bool drain, QString* detail) {
            return session.renderFramePboStep(frame, ready, drain, detail);
        }, testedPipeline);
    }
    ok &= verifyBackgroundResolution();
    if (ok) QTextStream(stdout) << "video_export_background_policy_spec ok; pipelined readback: "
        << (testedPipeline ? "tested" : "unsupported") << Qt::endl;
    return ok ? 0 : 1;
}
