#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "export/session/PreviewQuickExportSession.h"
#include "preview/runtime/PreviewSceneAssetLoader.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QDir>

using namespace miacode::preview::scene;
int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc,argv); bool ok = true;
    const auto expect = [&](bool pass, const QString& message) { ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n'; };
    const QString source = "(150){8}1-5[4:1],3,1/5-1[4:1],7,5/1-5[4:1],3,1,,,,";
    const auto result = miacode::hand_order::optimizeHandOrderPatterns(source, SimaiParser::validateSyntax(source),0,source.size());
    if (result.patternSuggestions.isEmpty()) return 1;
    const auto assets = miacode::preview::runtime::PreviewSceneAssetLoader::load(
        QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin/skinSD"),PreviewOutlineVariant::Line);
    PreviewFrameState state; state.skin = assets.skinAssets; state.assets = assets.assetState;
    state.render.layoutSquareScale = 1; state.render.showHandOrderAnnotations = true; state.playheadSeconds = 0.48;
    PreviewQuickExportSession renderer; renderer.setFrameSize(QSize(720,720));
    renderer.setLayerFlags(BackdropLayer | GuideLayer | TrackLayer | SlideMotionLayer | HeadLayer | TouchLayer);
    renderer.setFrameState(state); QString error;
    if (!renderer.initialize(QSurfaceFormat(),nullptr,&error)) { expect(false,error); return 1; }
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) QDir().mkpath(output);
    QImage previous;
    for (int i = 0; i < result.patternSuggestions.first().variants.size(); ++i) {
        state.noteMarkers = result.patternSuggestions.first().variants.at(i).markers;
        ++state.sceneContentRevision; ++state.sceneStorageGeneration;
        renderer.setFrameState(state); const auto frame = renderer.renderFrame(&error);
        int blue = 0, red = 0;
        for (int y = 0; y < frame.height(); ++y) for (int x = 0; x < frame.width(); ++x) {
            const auto c = frame.pixelColor(x,y);
            blue += c.blue() > 100 && c.blue() > c.red()*1.4 && c.blue() > c.green()*1.1;
            red += c.red() > 100 && c.red() > c.blue()*1.4 && c.red() > c.green()*1.3;
        }
        expect(!frame.isNull() && blue > 100 && red > 100, QStringLiteral("candidate %1 renders both native hand colors through actual QSG (%2/%3)").arg(i).arg(blue).arg(red));
        if (!previous.isNull()) expect(frame != previous,"mirrored solution produces a different native rendered image at the same time");
        if (!output.isEmpty()) frame.save(output+QStringLiteral("/native-candidate-%1.png").arg(i));
        previous = frame;
    }
    renderer.invalidate(); return ok ? 0 : 1;
}
