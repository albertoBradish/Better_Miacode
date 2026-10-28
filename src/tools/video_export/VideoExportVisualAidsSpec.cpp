#include "export/video_export/VideoExportSnapshot.h"
#include "export/video_export/VideoExportSettings.h"
#include "export/session/PreviewQuickExportSession.h"
#include "export/session/PreviewQuickD3D11ExportSession.h"
#include "core/scene/PreviewVisualAidLayerState.h"

#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

using namespace miacode::preview::scene;
namespace {
bool require(bool condition, const char* message)
{
    if (!condition) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return condition;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    VideoExportSnapshot source;
    source.chartTextUtf8 = QStringLiteral("&title=Visual Aid Spec\n&first=1.25\n&clock_count=3\n&lv_5=12\n"
        "&inote_5=(120){8}1,1,1,\n(240){8}1,1,1,1,1,1,1,1,1,\nE\n");
    source.difficultyId = 5;
    source.originalChartPath = QStringLiteral("C:/synthetic/maidata.txt");
    source.outputPath = QStringLiteral("C:/synthetic/out.mp4");
    source.contentDurationSeconds = 2.0;
    source.exportStartSeconds = 1.5;
    source.fullRangeExport = false;
    source.showMeasureLines = true;
    source.showJudgementSkeleton = true;
    source.showSlideJudgementTiming = true;
    source.measureLineMode = 0;
    source.measureLineCustomBeats = 9;
    source.clockCountEnabled = false;
    VideoExportSnapshot restored;
    QString error;
    ok &= require(VideoExportSnapshot::fromJson(source.toJson(), &restored, &error), "snapshot restores");
    ok &= require(restored.showMeasureLines && restored.showJudgementSkeleton && restored.showSlideJudgementTiming
        && restored.measureLineMode == 0 && restored.measureLineCustomBeats == 9,
        "both independent visibility flags and measure settings survive snapshot transport");
    VideoExportTask task;
    ok &= require(buildVideoExportTaskFromSnapshot(restored, &task, &error), "worker rebuilds visual-aid task");
    ok &= require(task.measureLineSeconds.size() == 3 && near(task.measureLineSeconds[0], 1.25)
        && near(task.measureLineSeconds[1], 2.375) && near(task.measureLineSeconds[2], 3.125),
        "partial export retains chart-zero beat phase, offset and BPM integration");
    ok &= require(task.clockCount == 3 && !task.clockCountEnabled,
        "visual measure length leaves the existing count-in value and audio gate intact");
    VideoExportTask other;
    other.showMeasureLines = false;
    other.showJudgementSkeleton = false;
    miacode::video_export::copyVideoExportUserSettings(task, &other);
    ok &= require(other.showMeasureLines && other.showJudgementSkeleton && other.showSlideJudgementTiming && other.measureLineCustomBeats == 9,
        "difficulty reseeding retains explicit export toggles");

    QJsonObject legacyJson = source.toJson();
    QJsonObject render = legacyJson.value(QStringLiteral("render")).toObject();
    for (const char* key : {"show_measure_lines", "show_judgement_skeleton", "show_slide_judgement_timing", "measure_line_mode", "measure_line_custom_beats"})
        render.remove(QLatin1String(key));
    legacyJson.insert(QStringLiteral("render"), render);
    VideoExportSnapshot legacy;
    ok &= require(VideoExportSnapshot::fromJson(legacyJson, &legacy, &error)
        && !legacy.showMeasureLines && !legacy.showJudgementSkeleton && !legacy.showSlideJudgementTiming && legacy.measureLineCustomBeats == 4,
        "legacy snapshots default visual aids off and custom X to four");
    render.insert(QStringLiteral("measure_line_custom_beats"), -8);
    render.insert(QStringLiteral("measure_line_mode"), 85);
    legacyJson.insert(QStringLiteral("render"), render);
    ok &= require(VideoExportSnapshot::fromJson(legacyJson, &legacy, &error)
        && legacy.measureLineCustomBeats == 4 && legacy.measureLineMode == 0,
        "invalid wire measure settings normalize before reaching the worker");

    PreviewFrameState frame;
    frame.measureLineSeconds = task.measureLineSeconds;
    frame.render.showMeasureLines = task.showMeasureLines;
    frame.render.showJudgementSkeleton = task.showJudgementSkeleton;
    frame.render.showSlideJudgementTiming = task.showSlideJudgementTiming;
    PreviewQuickExportSession gl;
    PreviewQuickD3D11ExportSession d3d;
    gl.setFrameState(frame);
    d3d.setFrameState(frame);
    // A live edit after capture must not alter either export session.
    frame.render.showMeasureLines = false;
    frame.render.showJudgementSkeleton = false;
    frame.render.showSlideJudgementTiming = false;
    frame.measureLineSeconds.clear();
    for (double second : {2.2, 2.375, 2.4, 2.2}) {
        gl.applyExportFrameTick(second, false, false, false, 0, 60);
        d3d.applyExportFrameTick(second, false, false, false, 0, 60);
        for (const auto* captured : {&gl.frameState(), &d3d.frameState()}) {
            ok &= require(captured->render.showMeasureLines && captured->render.showJudgementSkeleton && captured->render.showSlideJudgementTiming
                && captured->measureLineSeconds == task.measureLineSeconds && near(captured->playheadSeconds, second),
                "both explicit-frame export sessions retain captured aid settings through ticks and backward seeks");
        }
    }
    if (!ok && !error.isEmpty()) QTextStream(stderr) << error << Qt::endl;
    if (ok) QTextStream(stdout) << "video_export_visual_aids_spec ok" << Qt::endl;
    return ok ? 0 : 1;
}
