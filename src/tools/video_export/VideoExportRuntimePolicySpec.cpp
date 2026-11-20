#include <QCoreApplication>
#include <QTextStream>

#include <optional>

#include "export/video_export/VideoExportRuntimePolicy.h"
#include "export/video_export/VideoExportSettings.h"
#include "export/video_export/VideoExportSnapshot.h"
#include "export/video_export/VideoExportAudioRenderPlan.h"

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << message << Qt::endl;
        return false;
    }
    return true;
}

bool verifyPolicy(QTextStream& err)
{
    using miacode::video_export::shouldRequestOffscreenPboReadback;
    using miacode::video_export::shouldRetryVideoExportWorkerAfterCrash;

    using namespace miacode::video_export;
    for (const auto preset : {VideoRateMode::Quality, VideoRateMode::FileSize, VideoRateMode::Bitrate}) {
        for (bool includePv : {true, false}) {
            VideoExportTask task;
            task.encoding.mode = preset;
            task.encoding.videoBitrateKbps = 7125;
            task.includePv = includePv;
            task.intro.pvPreview = true;
            task.intro.chartPreview = true;
            VideoExportTask restored;
            if (!require(applyVideoExportTemplateSettings(videoExportTemplateSettings(task), &restored)
                    && restored.encoding.mode == preset && restored.includePv == includePv
                    && restored.encoding.videoBitrateKbps == 7125 && restored.intro.chartPreview,
                QStringLiteral("the template preserves the PV toggle at every current preset"), err)) return false;
            VideoExportSnapshot snapshot;
            snapshot.chartTextUtf8 = QStringLiteral("&title=Codec spec\n&lv_5=13\n&inote_5=(120){4}1,\nE\n");
            snapshot.difficultyId = 5;
            snapshot.originalChartPath = QStringLiteral("C:/fixture/maidata.txt");
            snapshot.outputPath = QStringLiteral("C:/fixture/video.mp4");
            snapshot.contentDurationSeconds = 1.0;
            snapshot.encoding.mode = preset;
            snapshot.includePv = includePv;
            snapshot.encoding.videoBitrateKbps = 7125;
            VideoExportSnapshot wire;
            QString error;
            if (!require(VideoExportSnapshot::fromJson(snapshot.toJson(), &wire, &error)
                    && buildVideoExportTaskFromSnapshot(wire, &restored, &error)
                    && restored.encoding.mode == preset && restored.includePv == includePv
                    && restored.encoding.videoBitrateKbps == 7125,
                QStringLiteral("the worker receives size mode, target bitrate and independent PV: %1").arg(error), err))
                return false;
        }
    }
    VideoExportTask migrated;
    applyVideoExportPreferences({{"size_preset", "ultra_compact"}}, &migrated);
    if (!require(migrated.encoding.mode == VideoRateMode::Quality
            && migrated.encoding.quality == VideoQualityLevel::Small && migrated.includePv,
        QStringLiteral("old preferences follow the official quality migration without old PV or size behavior"), err)) return false;
    VideoExportSnapshot legacySnapshot;
    QJsonObject legacyWire{{"schema", "miacode_export_snapshot_v1"},
        {"chart", QJsonObject{{"difficulty_id", 5}, {"text_utf8", "&title=Legacy\n&lv_5=12\n&inote_5=(120){4}1,E\n"}}},
        {"export", QJsonObject{{"output_path", "C:/fixture/legacy.mp4"}, {"size_preset", "custom"}, {"target_video_bitrate_mbps", 6.25}}}};
    QString legacyError;
    if (!require(!VideoExportSnapshot::fromJson(legacyWire, &legacySnapshot, &legacyError),
        QStringLiteral("retired worker encoding fields fail explicitly rather than silently changing size semantics"), err)) return false;
    legacyWire.insert("export", QJsonObject{{"output_path", "C:/fixture/legacy.mp4"},
        {"rate_mode", "bitrate"}, {"video_bitrate_kbps", 6250}});
    if (!require(VideoExportSnapshot::fromJson(legacyWire, &legacySnapshot, &legacyError)
            && legacySnapshot.encoding.mode == VideoRateMode::Bitrate && legacySnapshot.encoding.videoBitrateKbps == 6250,
        QStringLiteral("container compatibility retains only the official current encoding fields"), err)) return false;
    legacyWire.insert("schema", "miacode_export_snapshot_v999");
    if (!require(!VideoExportSnapshot::fromJson(legacyWire, &legacySnapshot, &legacyError),
        QStringLiteral("unknown protocols remain rejected"), err)) return false;

    if (!require(
            shouldRequestOffscreenPboReadback(std::nullopt, std::nullopt),
            QStringLiteral("default export PBO policy should be enabled when no env override is set"),
            err)) {
        return false;
    }
    if (!require(
            shouldRequestOffscreenPboReadback(true, std::nullopt),
            QStringLiteral("ENABLE=1 should keep PBO requested"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRequestOffscreenPboReadback(false, std::nullopt),
            QStringLiteral("ENABLE=0 should disable PBO"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRequestOffscreenPboReadback(true, true),
            QStringLiteral("DISABLE=1 should override ENABLE=1"),
            err)) {
        return false;
    }
    if (!require(
            shouldRetryVideoExportWorkerAfterCrash(true, true, false, 1),
            QStringLiteral("CrashExit with PBO enabled should retry once"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRetryVideoExportWorkerAfterCrash(true, false, false, 1),
            QStringLiteral("CrashExit without PBO should not retry"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRetryVideoExportWorkerAfterCrash(false, true, false, 1),
            QStringLiteral("NormalExit failure should not retry"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRetryVideoExportWorkerAfterCrash(true, true, true, 1),
            QStringLiteral("Canceled crash should not retry"),
            err)) {
        return false;
    }
    if (!require(
            !shouldRetryVideoExportWorkerAfterCrash(true, true, false, 2),
            QStringLiteral("Crash retry should happen at most once"),
            err)) {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    QTextStream out(stdout);

    if (!verifyPolicy(err)) {
        return 1;
    }

    out << "video_export_runtime_policy_spec ok" << Qt::endl;
    return 0;
}
