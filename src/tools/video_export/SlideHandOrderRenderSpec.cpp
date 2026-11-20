#include "core/chart/transform/ChartBatchTransform.h"
#include "core/chart/transform/HandOrderScreenSplit.h"
#include "core/scene/PreviewHandOrderRendering.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "core/scene/PreviewTrackLayerState.h"
#include "export/session/PreviewQuickExportSession.h"
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
    const auto expect = [&](bool pass, const QString& name) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << name << '\n';
        ok &= pass;
    };
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) expect(QDir().mkpath(output), QStringLiteral("render evidence directory exists"));

    TimelineNoteMarker retrace;
    retrace.slideSegmentPoints = {{{0, 0}, {100, 0}, {0, 0}, {100, 0}, {0, 0}}};
    retrace.slideTrackAreaPoints = {{{{50, 0}, {50, 0}, {50, 0}, {50, 0}}}};
    retrace.slideTrackAreaThresholds = {{0.0}};
    bool correctTurns = true;
    for (int arrow = 0; arrow < 4; ++arrow)
        correctTurns &= qAbs(previewSlideArrowSegmentProgress(retrace, 0, 0, arrow) - (0.875 - arrow * 0.25)) < 1e-12;
    expect(correctTurns, QStringLiteral("checkpoint prior disambiguates four coinciding points on different retraced turns"));
    TimelineNoteMarker bounded;
    QVector<QPointF> boundedPoints;
    for (int point = 0; point <= 5; ++point) boundedPoints.append({point * 20.0, 1.0});
    for (int point = 6; point < 15; ++point) boundedPoints.append({100.0, 100.0});
    for (int point = 15; point <= 20; ++point) boundedPoints.append({(20 - point) * 20.0, 0.0});
    bounded.slideSegmentPoints = {boundedPoints};
    bounded.slideTrackAreaPoints = {{{{50, 0}}}};
    bounded.slideTrackAreaThresholds = {{0.0, 0.25}};
    expect(qAbs(previewSlideArrowSegmentProgress(bounded, 0, 0, 0) - 0.125) < 1e-12,
        QStringLiteral("the bounded prior excludes a geometrically closer point on a distant loop turn"));
    TimelineNoteMarker straight = retrace;
    straight.slideSegmentPoints = {{{0, 0}, {10, 0}, {100, 0}}};
    straight.slideTrackAreaPoints = {{{{10, 0}}}};
    expect(qAbs(previewSlideArrowSegmentProgress(straight, 0, 0, 0) - 0.5) < 1e-12,
        QStringLiteral("projection uses native sample time rather than polyline arc length"));
    TimelineNoteMarker missing = retrace;
    missing.slideSegmentPoints.clear();
    expect(previewSlideArrowSegmentProgress(missing, 0, 0, 2) == previewSlideArrowCheckpointProgress(missing, 0, 0, 2),
        QStringLiteral("missing native samples retain the previous checkpoint fallback"));
    TimelineNoteMarker chained = retrace;
    chained.slideSegmentDurations = {1.0, 3.0};
    expect(qAbs(previewSlideMovementProgress(chained, 1, 0.5) - 0.625) < 1e-12,
        QStringLiteral("chain movement progress retains the existing duration semantics"));

    const auto assets = miacode::preview::runtime::PreviewSceneAssetLoader::load(
        QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin/skinSD"), PreviewOutlineVariant::Line);
    PreviewFrameState state;
    state.skin = assets.skinAssets;
    state.render.layoutSquareScale = 1.0;
    state.render.showHandOrderAnnotations = true;
    state.muriRenderOptions.showSlideTracks = true;
    const QRectF rect = playfieldRectForStage(QRectF(0, 0, 720, 720), 1.0);
    PreviewQuickExportSession renderer;
    renderer.setFrameSize(QSize(720, 720));
    renderer.setLayerFlags(TrackLayer);
    renderer.setFrameState(state);
    QString error;
    if (!renderer.initialize(QSurfaceFormat(), nullptr, &error)) {
        expect(false, QStringLiteral("real QSG renderer initializes: ") + error);
        return 1;
    }
    const auto render = [&](const QString& name) {
        renderer.setFrameState(state);
        const QImage image = renderer.renderFrame(&error);
        expect(!image.isNull(), name + QStringLiteral(" renders through shared QSG: ") + error);
        if (!image.isNull() && !output.isEmpty())
            expect(image.save(QDir(output).filePath(name + QStringLiteral(".png"))), name + QStringLiteral(" evidence saved"));
        return image;
    };
    const QString source = QStringLiteral("(120){4}1p4[8:1]/8q5[8:1],E");
    for (int rotation = 0; rotation < 2; ++rotation) {
        const QString native = rotation == 0 ? source : miacode::chart_transform::transformChartText(
            source, miacode::chart_transform::ChartTransformOp::Rotate45Clockwise);
        const auto split = miacode::chart_transform::screenSplitHandOrderInSelection(native, 0, native.size());
        const auto parsed = SimaiParser::validateSyntax(split.replacement);
        expect(split.diagnostics.isEmpty() && parsed.ok && parsed.annotationErrors.isEmpty() && parsed.noteMarkers.size() == 2,
            QStringLiteral("synthetic screen-split pair parses for rotation %1").arg(rotation));
        if (parsed.noteMarkers.size() != 2) continue;
        state.noteMarkers = parsed.noteMarkers;
        state.playheadSeconds = state.noteMarkers.constFirst().second;
        state.render.showHandOrderAnnotations = true;
        const QPointF normal = rotation == 0 ? QPointF(1, 0) : QPointF(std::sqrt(0.5), std::sqrt(0.5));
        int blue = 0, red = 0, wrong = 0, previousWrong = 0;
        for (const auto& marker : state.noteMarkers) {
            for (int segment = 0; segment < marker.slideTrackAreaPoints.size(); ++segment) {
                const auto& areas = marker.slideTrackAreaPoints.at(segment);
                for (int area = 0; area < areas.size(); ++area) {
                    for (int arrow = 0; arrow < areas.at(area).size(); ++arrow) {
                        const double side = QPointF::dotProduct(areas.at(area).at(arrow), normal);
                        if (qAbs(side) < 1e-5) continue;
                        const auto expected = side > 0 ? HandOrderHand::Right : HandOrderHand::Left;
                        const auto actual = previewHandOrderAtProgress(marker.handOrderTrackStops,
                            previewSlideMovementProgress(marker, segment, previewSlideArrowSegmentProgress(marker, segment, area, arrow)));
                        const auto previous = previewHandOrderAtProgress(marker.handOrderTrackStops,
                            previewSlideMovementProgress(marker, segment, previewSlideArrowCheckpointProgress(marker, segment, area, arrow)));
                        wrong += actual != expected;
                        previousWrong += previous != expected;
                        blue += actual == HandOrderHand::Left;
                        red += actual == HandOrderHand::Right;
                    }
                }
            }
        }
        expect(blue > 0 && red > 0 && wrong == 0 && previousWrong > 0,
            QStringLiteral("rotation %1: all native arrow centres follow screen side (%2 blue, %3 red, %4 wrong; old mapping %5 wrong)")
                .arg(rotation).arg(blue).arg(red).arg(wrong).arg(previousWrong));
        auto descriptorState = state;
        // Artwork resolution consumes the hand field after choosing the native
        // colored image. Keep it here to inspect the shared layer's assignment.
        descriptorState.skin.handOrderVariants.clear();
        const auto layer = buildPreviewTrackLayerState(descriptorState, PreviewActiveMarkerView(descriptorState.noteMarkers), rect);
        int spriteWrong = 0;
        for (const auto& sprite : layer.sprites) {
            const double side = QPointF::dotProduct(sprite.center - rect.center(), normal);
            if (qAbs(side) < 1e-5) continue;
            spriteWrong += sprite.handOrderHand != (side > 0 ? HandOrderHand::Right : HandOrderHand::Left);
        }
        expect(!layer.sprites.isEmpty() && spriteWrong == 0,
            QStringLiteral("rotation %1: shared preview/export layer assigns every arrow sprite by its centre (%2 sprites, %3 wrong)")
                .arg(rotation).arg(layer.sprites.size()).arg(spriteWrong));
        const QImage colored = render(QStringLiteral("slide-screen-split-%1").arg(rotation));
        int bluePixels = 0, redPixels = 0;
        for (int y = 0; y < colored.height(); ++y) {
            for (int x = 0; x < colored.width(); ++x) {
                const QColor pixel = colored.pixelColor(x, y);
                if (pixel.alpha() < 100) continue;
                bluePixels += pixel.blue() > pixel.red() + 30;
                redPixels += pixel.red() > pixel.blue() + 30;
            }
        }
        expect(bluePixels > 100 && redPixels > 100,
            QStringLiteral("rotation %1: real QSG raster retains both hand colors (%2 blue, %3 red)")
                .arg(rotation).arg(bluePixels).arg(redPixels));
        state.render.showHandOrderAnnotations = false;
        const QImage hidden = render(QStringLiteral("slide-screen-hidden-%1").arg(rotation));
        state.noteMarkers = SimaiParser::parseForTimeline(native).noteMarkers;
        expect(render(QStringLiteral("slide-screen-original-%1").arg(rotation)) == hidden,
            QStringLiteral("rotation %1: hiding hand colors is pixel-identical to the native track").arg(rotation));
    }
    renderer.invalidate();
    return ok ? 0 : 1;
}
