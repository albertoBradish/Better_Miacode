#include "core/chart/parser/SimaiParser.h"
#include "core/chart/SlideJudgementTiming.h"
#include "core/scene/PreviewSceneConstants.h"
#include "export/session/PreviewQuickD3D11ExportSession.h"
#include "export/session/PreviewQuickExportSession.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QQuickWindow>
#include <QTextStream>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(MiaCode_PreviewPlugin)

int main(int argc, char** argv)
{
    const bool d3d = [&] {
        for (int i=1;i<argc;++i) if (QByteArray(argv[i]) == "d3d11") return true;
        return false;
    }();
    QQuickWindow::setGraphicsApi(d3d ? QSGRendererInterface::Direct3D11 : QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3,3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc,argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"backend","opengl or d3d11", "backend", "opengl"});
    parser.addOption({"skin","Native skin directory", "path"});
    parser.addOption({"output","Output directory", "path"});
    parser.process(app);
    const QDir skin(parser.value("skin"));
    const QString output = parser.value("output");
    if (output.isEmpty() || !QDir().mkpath(output)) return 1;
    using namespace miacode::preview::scene;
    PreviewFrameState state;
    const auto image = [&](const QString& name) {
        return QImage(skin.filePath(name));
    };
    state.skin.slideTrackImage = image(QStringLiteral("slide.png"));
    state.skin.slideTrackEachImage = image(QStringLiteral("slide_each.png"));
    state.skin.slideTrackBreakImage = image(QStringLiteral("slide_break.png"));
    for (int i=0;i<11;++i) {
        state.skin.wifiImages.append(image(QStringLiteral("wifi_%1.png").arg(i)));
        state.skin.wifiEachImages.append(image(QStringLiteral("wifi_each_%1.png").arg(i)));
        state.skin.wifiBreakImages.append(image(QStringLiteral("wifi_break_%1.png").arg(i)));
    }
    if (state.skin.slideTrackImage.isNull()) return 1;
    state.render.showTimestamp = false;
    state.render.showSlideJudgementTiming = true;
    QJsonArray results;
    QString error;
    int index=0;
    for (const auto& token : {QStringLiteral("1-5[4:1]"),QStringLiteral("1>1[2:1]"),
             QStringLiteral("1pp5[4:1]"),QStringLiteral("1v1[4:1]"),QStringLiteral("1-6-4[2:1]"),
             QStringLiteral("1-5[0##0.125]"),QStringLiteral("1-5b[4:1]"),
             QStringLiteral("1s5[4:1]"),QStringLiteral("1V35[4:1]"),QStringLiteral("1w5[4:1]")}) {
        const auto parsed = SimaiParser::parseForTimeline(QStringLiteral("(120){8}")+token+QStringLiteral(",\nE"));
        state.noteMarkers.clear();
        for (const auto& marker : parsed.noteMarkers)
            if (marker.type==QLatin1String("slide") || marker.type==QLatin1String("wifi")) state.noteMarkers.append(marker);
        if (state.noteMarkers.size()!=1) return 1;
        if (index==7) state.noteMarkers[0].slideEach=true;
        const auto timing=miacode::slide_judge::timingForMarker(state.noteMarkers[0]);
        if (!timing.valid) return 1;
        ++state.sceneContentRevision;
        PreviewQuickExportSession gl;
        PreviewQuickD3D11ExportSession dx;
        if (d3d) {dx.setFrameSize(QSize(540,540));dx.setFrameState(state);dx.setLayerFlags(TrackLayer);}
        else {gl.setFrameSize(QSize(540,540));gl.setFrameState(state);gl.setLayerFlags(TrackLayer);}
        if (!(d3d ? dx.initialize(&error) : gl.initialize(format,nullptr,&error))) {
            QTextStream(stderr)<<error<<Qt::endl;return 1;
        }
        const auto render=[&](double time,const PreviewFrameState& frame) {
            if(d3d) {dx.setFrameState(frame);dx.applyExportFrameTick(time,false,false,true,0,60);return dx.renderFrame(&error);}
            gl.setFrameState(frame);gl.applyExportFrameTick(time,false,false,true,0,60);return gl.renderFrame(&error);
        };
        QVector<QImage> frames;
        const QVector<double> times{timing.headSecond, timing.headSecond+(timing.judgementSecond-timing.headSecond)*.25,
            timing.headSecond+(timing.judgementSecond-timing.headSecond)*.5,timing.judgementSecond,
            timing.headSecond+(timing.judgementSecond-timing.headSecond)*.25};
        for(int i=0;i<times.size();++i) {
            const auto frame=render(times[i],state);
            if(frame.isNull()) {QTextStream(stderr)<<error<<Qt::endl;return 1;}
            frame.save(QDir(output).filePath(QStringLiteral("%1-%2.png").arg(index).arg(i)));
            frames.append(frame.copy());
        }
        // Native full-track control at the same explicit instant. Delaying the
        // guide suppresses only native trimming; colors/flash/alpha stay equal.
        PreviewFrameState control=state;
        control.render.showSlideJudgementTiming=false;
        control.noteMarkers[0].slideTraceSecond=timing.endSecond+1;
        ++control.sceneContentRevision;
        const QImage full=render(timing.judgementSecond,control);
        const bool completion=frames[3]==full;
        const bool seek=frames[1]==frames[4];
        if(!completion || !seek) {QTextStream(stderr)<<"pixel contract failed: "<<token<<" completion="<<completion<<" seek="<<seek<<Qt::endl;return 1;}
        QImage strip(540*4,570,QImage::Format_RGB32);strip.fill(QColor(24,26,30));
        QPainter painter(&strip);
        painter.setPen(Qt::white);
        for(int i=0;i<4;++i) {painter.drawImage(i*540,30,frames[i]);painter.drawText(i*540+8,20,QStringLiteral("%1 | %2 ms").arg(token).arg(times[i]*1000,0,'f',1));}
        painter.end();strip.save(QDir(output).filePath(QStringLiteral("%1-strip.png").arg(index)));
        results.append(QJsonObject{{"shape",token},{"judgementSecond",timing.judgementSecond},
            {"perfectStartSecond",timing.perfectStartSecond},{"perfectEndSecond",timing.perfectEndSecond},
            {"completeMatchesNativePixels",completion},{"seekIdentical",seek}});
        ++index;
    }
    QFile report(QDir(output).filePath(QStringLiteral("report.json")));
    if(!report.open(QIODevice::WriteOnly)) return 1;
    report.write(QJsonDocument(QJsonObject{{"backend",d3d?"d3d11":"opengl"},{"cases",results}}).toJson());
    QTextStream(stdout)<<"slide timing GPU frames verified: "<<index<<Qt::endl;
    return 0;
}
