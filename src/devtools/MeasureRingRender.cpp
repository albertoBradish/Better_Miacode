#include "export/session/PreviewQuickExportSession.h"
#include "core/scene/PreviewSceneConstants.h"

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickWindow>
#include <QTextStream>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(MiaCode_PreviewPlugin)

// Isolated synthetic chart: production note guide and measure guide at the same
// timestamp. It needs no user chart, preferences, audio, skin installation or UI.
int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"guide", "Native note-guide PNG for comparison.", "path"});
    parser.addOption({"output", "Frame output directory.", "path"});
    parser.addOption({"sizes", "Comma-separated square frame sides.", "pixels", "360,540,1080,2160"});
    parser.addOption({"times", "Comma-separated explicit frame times; guides hit at 2 seconds.", "seconds", "1.9,2.0,2.0001"});
    parser.process(app);
    const QString output = parser.value("output");
    const QImage guide(parser.value("guide"));
    if (guide.isNull() || output.isEmpty() || !QDir().mkpath(output)) return 1;

    using namespace miacode::preview::scene;
    PreviewFrameState state;
    // Match the production asset loader's normalization before comparing radii.
    state.skin.noteGuideNormalImage = guide.scaled(
        qMax(1, qRound(guide.width() * kSkinAssetScale)),
        qMax(1, qRound(guide.height() * kSkinAssetScale)),
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    state.render.showMeasureLines = true;
    state.measureLineSeconds = {2.0};
    TimelineNoteMarker tap;
    tap.type = QStringLiteral("tap");
    tap.lane = 1;
    tap.second = tap.endSecond = 2.0;
    state.noteMarkers = {tap};
    state.sceneContentRevision = 1;
    QString error;
    for (const auto& sizeText : parser.value("sizes").split(',')) {
        bool valid = false;
        const int side = sizeText.toInt(&valid);
        if (!valid || side <= 0 || side > 8192) return 1;
        PreviewQuickExportSession renderer;
        renderer.setFrameSize(QSize(side, side));
        renderer.setFrameState(state);
        if (!renderer.initialize(format, nullptr, &error)) {
            QTextStream(stderr) << error << Qt::endl;
            return 1;
        }
        for (const auto& timeText : parser.value("times").split(',')) {
            const double second = timeText.toDouble(&valid);
            if (!valid || !qIsFinite(second)) return 1;
            renderer.applyExportFrameTick(second, false, false, true, 0, 60);
            QImage comparison(side * 2, side, QImage::Format_RGB32);
            comparison.fill(QColor(30, 32, 36));
            QPainter painter(&comparison);
            int column = 0;
            for (const auto layer : {GuideLayer, VisualAidLayer}) {
                renderer.setLayerFlags(layer);
                const QImage frame = renderer.renderFrame(&error);
                if (frame.isNull()) {
                    QTextStream(stderr) << error << Qt::endl;
                    return 1;
                }
                const QString label = column == 0 ? QStringLiteral("native") : QStringLiteral("measure");
                const QString name = QStringLiteral("%1-%2-%3.png").arg(side).arg(timeText).arg(label);
                if (!frame.save(QDir(output).filePath(name))) return 1;
                painter.drawImage(column++ * side, 0, frame);
            }
            painter.end();
            const QString name = QStringLiteral("%1-%2-native-left-measure-right.png").arg(side).arg(timeText);
            if (!comparison.save(QDir(output).filePath(name))) return 1;
        }
    }
    QTextStream(stdout) << output << Qt::endl;
    return 0;
}
