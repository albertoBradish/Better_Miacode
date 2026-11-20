#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/scene/PreviewHandOrderRendering.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "core/scene/PreviewSceneMath.h"
#include "export/session/PreviewQuickExportSession.h"
#include "export/video_export/VideoExportSnapshot.h"
#include "preview/runtime/PreviewSceneAssetLoader.h"

#include <QDir>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>

using namespace miacode::preview::scene;

int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& label) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << label << '\n';
        ok &= pass;
    };
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) expect(QDir().mkpath(output), QStringLiteral("render evidence directory exists"));
    const auto assets = miacode::preview::runtime::PreviewSceneAssetLoader::load(
        QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin/skinSD"), PreviewOutlineVariant::Line);
    PreviewFrameState state;
    state.skin = assets.skinAssets;
    state.render.layoutSquareScale = 1.0;
    state.render.showHandOrderAnnotations = true;
    const QString split = QStringLiteral("(120){4}1w5[2:1]~[track=split:0:l&0:r],E");
    const QString handoff = QStringLiteral("(120){4}1w5[2:1]~[track=split:0:l|0.5:r&0:r|0.75:l],E");
    const auto parsed = SimaiParser::validateSyntax(split);
    expect(parsed.ok && parsed.annotationErrors.isEmpty() && parsed.noteMarkers.size() == 1,
        QStringLiteral("synthetic Wifi parses with native geometry"));
    if (!ok) return 1;
    state.noteMarkers = parsed.noteMarkers;
    const auto marker = state.noteMarkers.constFirst();
    state.playheadSeconds = marker.slideTraceSecond;

    PreviewQuickExportSession renderer;
    renderer.setFrameSize(QSize(720, 720));
    renderer.setLayerFlags(TrackLayer | SlideMotionLayer);
    renderer.setFrameState(state);
    QString error;
    if (!renderer.initialize(QSurfaceFormat(), nullptr, &error)) {
        expect(false, QStringLiteral("real offscreen QSG export initialization: ") + error);
        return 1;
    }
    const auto render = [&](const PreviewFrameState& frame, const QString& name) {
        renderer.setFrameState(frame);
        const QImage image = renderer.renderFrame(&error);
        expect(!image.isNull(), name + QStringLiteral(" renders through shared QSG: ") + error);
        if (!image.isNull() && !output.isEmpty())
            expect(image.save(QDir(output).filePath(name + QStringLiteral(".png"))), name + QStringLiteral(" evidence saved"));
        return image;
    };
    const QImage splitImage = render(state, QStringLiteral("wifi-split"));
    // At movement start all three native stars overlap. Outer stars retain
    // their complete silhouettes, so their pixels can cross the centre axis.
    // Verify the track's geometric partition in its own real QSG render.
    renderer.setLayerFlags(TrackLayer);
    renderer.setFrameState(state);
    const QImage splitTrackImage = renderer.renderFrame(&error);
    expect(!splitTrackImage.isNull(), QStringLiteral("independent track-layer QSG raster: ") + error);
    renderer.setLayerFlags(TrackLayer | SlideMotionLayer);
    const QRectF rect = playfieldRectForStage(QRectF(0, 0, 720, 720), 1.0);
    const QPointF origin = marker.wifiLanePoints.at(1).constFirst();
    const QPointF clipOrigin = mapLogicalPointToRect(
        QPointF(kLogicalCanvasCenter + origin.x(), kLogicalCanvasCenter + origin.y()), rect);
    const QPointF normal = previewWifiSectorNormal(marker, 0);
    int blue = 0;
    int red = 0;
    int wrongSide = 0;
    for (int y = 0; y < splitTrackImage.height(); ++y) {
        for (int x = 0; x < splitTrackImage.width(); ++x) {
            const QColor pixel = splitTrackImage.pixelColor(x, y);
            if (pixel.alpha() < 100) continue;
            const bool isBlue = pixel.blue() > pixel.red() + 30;
            const bool isRed = pixel.red() > pixel.blue() + 30;
            blue += isBlue;
            red += isRed;
            const double side = QPointF::dotProduct(QPointF(x + 0.5, y + 0.5) - clipOrigin, normal);
            // A narrow margin excludes antialiasing exactly on the shared axis.
            wrongSide += (isBlue && side < -2) || (isRed && side > 2);
        }
    }
    expect(blue > 100 && red > 100 && wrongSide == 0,
        QStringLiteral("actual track raster colors both native sectors on complementary geometric sides (%1 blue, %2 red, %3 wrong)")
            .arg(blue).arg(red).arg(wrongSide));
    state.playheadSeconds = marker.slideTraceSecond + (marker.endSecond - marker.slideTraceSecond) * 0.5;
    state.noteMarkers = SimaiParser::validateSyntax(handoff).noteMarkers;
    const QImage handoffImage = render(state, QStringLiteral("wifi-split-handoff"));
    expect(handoffImage != splitImage, QStringLiteral("explicit movement time advances handoff and native erasure"));
    renderer.setLayerFlags(SlideMotionLayer);
    auto fixedHands = state;
    fixedHands.noteMarkers = parsed.noteMarkers;
    renderer.setFrameState(fixedHands);
    const QImage fixedMotion = renderer.renderFrame(&error);
    renderer.setFrameState(state);
    const QImage handoffMotion = renderer.renderFrame(&error);
    expect(!fixedMotion.isNull() && !handoffMotion.isNull(), QStringLiteral("exact-boundary moving-star rasters render"));
    const auto colorCounts = [&](const QImage& image, int nativeLane) {
        const auto& samples = marker.wifiLanePoints.at(nativeLane);
        const qreal sampleIndex = (samples.size() - 1) * 0.5;
        const int first = qFloor(sampleIndex);
        const qreal fraction = sampleIndex - first;
        const QPointF position = samples.at(first) * (1.0 - fraction)
            + samples.at(qMin(first + 1, int(samples.size() - 1))) * fraction;
        const QPointF centre = mapLogicalPointToRect(QPointF(kLogicalCanvasCenter + position.x(),
            kLogicalCanvasCenter + position.y()), rect);
        QPoint counts;
        // At 50% the native outer stars are separate; this disk stays clear
        // of the common centre star and checks actual colors, not erasure.
        for (int y = qMax(0, qFloor(centre.y() - 45)); y < qMin(image.height(), qCeil(centre.y() + 45)); ++y) {
            for (int x = qMax(0, qFloor(centre.x() - 45)); x < qMin(image.width(), qCeil(centre.x() + 45)); ++x) {
                const QPointF delta = QPointF(x + 0.5, y + 0.5) - centre;
                if (QPointF::dotProduct(delta, delta) > 45 * 45) continue;
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 100) continue;
                counts.rx() += pixel.blue() > pixel.red() + 30;
                counts.ry() += pixel.red() > pixel.blue() + 30;
            }
        }
        return counts;
    };
    const QPoint before0 = colorCounts(fixedMotion, 0);
    const QPoint after0 = colorCounts(handoffMotion, 0);
    const QPoint before1 = colorCounts(fixedMotion, 2);
    const QPoint after1 = colorCounts(handoffMotion, 2);
    expect(before0.x() > 100 && before0.y() == 0 && after0.y() > 100 && after0.x() == 0
        && before1.y() > 100 && before1.x() == 0 && after1.y() > 100 && after1.x() == 0,
        QStringLiteral("at exact 50% sector 0 changes blue to red while sector 1 remains red (%1/%2 to %3/%4; %5/%6 to %7/%8)")
            .arg(before0.x()).arg(before0.y()).arg(after0.x()).arg(after0.y())
            .arg(before1.x()).arg(before1.y()).arg(after1.x()).arg(after1.y()));
    renderer.setLayerFlags(TrackLayer | SlideMotionLayer);

    state.render.showHandOrderAnnotations = false;
    const QImage hidden = render(state, QStringLiteral("wifi-hidden"));
    auto original = state;
    original.noteMarkers = SimaiParser::parseForTimeline(miacode::hand_order::stripAnnotations(split)).noteMarkers;
    const QImage plain = render(original, QStringLiteral("wifi-original"));
    expect(hidden == plain, QStringLiteral("hidden explicit sectors are pixel-identical to unannotated native Wifi"));

    const QString single = QStringLiteral("(120){4}1w5[2:1]~[track=single:0:l|0.5:l],E");
    state.noteMarkers = SimaiParser::validateSyntax(single).noteMarkers;
    state.render.showHandOrderAnnotations = true;
    const QImage singleImage = render(state, QStringLiteral("wifi-single"));
    auto legacy = state;
    legacy.noteMarkers = SimaiParser::parseForTimeline(QStringLiteral("(120){4}1w5[2:1]~l,E")).noteMarkers;
    const QImage legacyImage = render(legacy, QStringLiteral("wifi-legacy"));
    expect(singleImage != legacyImage, QStringLiteral("explicit single mode has a visible circled-one marker beyond common hand color"));
    state.render.showHandOrderAnnotations = false;
    expect(render(state, QStringLiteral("wifi-single-hidden")) == plain,
        QStringLiteral("hiding single mode removes its badge and restores all native paths"));

    VideoExportSnapshot snapshot;
    snapshot.difficultyId = 5;
    snapshot.chartTextUtf8 = QStringLiteral("&first=0\n&inote_5=") + handoff;
    snapshot.originalChartPath = QStringLiteral(MIACODE_SOURCE_ROOT "/src/tools/video_export/synthetic_wifi.txt");
    snapshot.outputPath = QStringLiteral(MIACODE_SOURCE_ROOT "/synthetic_wifi_unused.mp4");
    snapshot.contentDurationSeconds = marker.endSecond;
    snapshot.showHandOrderAnnotations = true;
    VideoExportSnapshot restored;
    VideoExportTask task;
    expect(VideoExportSnapshot::fromJson(snapshot.toJson(), &restored, &error)
        && buildVideoExportTaskFromSnapshot(restored, &task, &error),
        QStringLiteral("snapshot reconstructs the captured synthetic Wifi: ") + error);
    if (task.noteMarkers.size() == 1) {
        state.noteMarkers = task.noteMarkers;
        state.render.showHandOrderAnnotations = task.showHandOrderAnnotations;
        expect(render(state, QStringLiteral("wifi-worker")) == handoffImage,
            QStringLiteral("worker snapshot produces pixel-identical split rendering at the same explicit frame time"));
    } else expect(false, QStringLiteral("worker retains one native Wifi"));
    renderer.invalidate();
    return ok ? 0 : 1;
}
