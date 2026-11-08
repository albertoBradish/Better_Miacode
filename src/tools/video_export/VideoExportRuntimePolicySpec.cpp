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
    using miacode::video_export::effectiveVideoExportAudioBitrateKbps;
    using miacode::video_export::effectiveVideoExportX264Crf;
    using miacode::video_export::VideoExportSizePreset;
    using miacode::video_export::shouldRequestOffscreenPboReadback;
    using miacode::video_export::shouldRetryVideoExportWorkerAfterCrash;
    using miacode::video_export::videoExportSizePolicy;
    using miacode::video_export::videoExportSizePresetToken;

    using namespace miacode::video_export;
    if (!require(videoExportTargetBytes(VideoExportSizePreset::Target30Mb) == 30000000
        && videoExportTargetBytes(VideoExportSizePreset::Target100Mb) == 100000000
        && videoExportTargetBytes(VideoExportSizePreset::Target200Mb) == 200000000
        && videoExportTargetBitrateKbps(VideoExportSizePreset::Target30Mb, 180, 128) == 1156
        && videoExportTargetBitrateKbps(VideoExportSizePreset::Target30Mb, 0, 128) == 0,
        QStringLiteral("target sizes reserve audio and container bytes"), err)) return false;
    const auto standard = videoExportSizePolicy(VideoExportSizePreset::Standard);
    const auto compact = videoExportSizePolicy(VideoExportSizePreset::Compact);
    const auto ultraWithPv = videoExportSizePolicy(VideoExportSizePreset::UltraCompactWithPv);
    const auto ultra = videoExportSizePolicy(VideoExportSizePreset::UltraCompact);
    if (!require(standard.gopSeconds > 0 && compact.gopSeconds > 0
                     && ultra.gopSeconds > 0 && ultra.minBitrateKbps > 0
                     && compact.minBitrateKbps <= compact.maxBitrateKbps
                     && ultra.minBitrateKbps <= ultra.maxBitrateKbps
                     && ultraWithPv.minBitrateKbps <= ultraWithPv.maxBitrateKbps
                     && videoExportSizePresetToken(VideoExportSizePreset::UltraCompactWithPv)
                         != videoExportSizePresetToken(VideoExportSizePreset::UltraCompact),
                 QStringLiteral("size presets expose valid budgets and distinct PV behavior"), err)) {
        return false;
    }

    for (const auto preset : {VideoExportSizePreset::Standard, VideoExportSizePreset::Compact,
                              VideoExportSizePreset::UltraCompactWithPv, VideoExportSizePreset::UltraCompact}) {
        const auto policy = videoExportSizePolicy(preset);
        if (!require(effectiveVideoExportX264Crf(preset, 22) == 22 + policy.x264CrfAdjustment
                         && effectiveVideoExportAudioBitrateKbps(preset, 320)
                             == qMin(320, policy.maxAudioBitrateKbps),
                     QStringLiteral("encoder settings apply the selected size policy"), err)) return false;
    }

    for (const auto preset : {VideoExportSizePreset::Standard, VideoExportSizePreset::Target30Mb,
                              VideoExportSizePreset::Target100Mb, VideoExportSizePreset::Target200Mb,
                              VideoExportSizePreset::Custom}) {
        for (bool includePv : {true, false}) {
            VideoExportTask task;
            task.sizePreset = preset;
            task.targetVideoBitrateMbps = 7.125;
            task.includePv = includePv;
            task.intro.pvPreview = true;
            task.intro.chartPreview = true;
            VideoExportTask restored;
            if (!require(applyVideoExportTemplateSettings(videoExportTemplateSettings(task), &restored)
                    && restored.sizePreset == preset && restored.includePv == includePv
                    && restored.targetVideoBitrateMbps == 7.125 && restored.intro.chartPreview,
                QStringLiteral("the template preserves the PV toggle at every current preset"), err)) return false;
            VideoExportSnapshot snapshot;
            snapshot.chartTextUtf8 = QStringLiteral("&title=Codec spec\n&lv_5=13\n&inote_5=(120){4}1,\nE\n");
            snapshot.difficultyId = 5;
            snapshot.originalChartPath = QStringLiteral("C:/fixture/maidata.txt");
            snapshot.outputPath = QStringLiteral("C:/fixture/video.mp4");
            snapshot.contentDurationSeconds = 1.0;
            snapshot.sizePreset = preset;
            snapshot.includePv = includePv;
            snapshot.targetVideoBitrateMbps = 7.125;
            VideoExportSnapshot wire;
            QString error;
            if (!require(VideoExportSnapshot::fromJson(snapshot.toJson(), &wire, &error)
                    && buildVideoExportTaskFromSnapshot(wire, &restored, &error)
                    && restored.sizePreset == preset && restored.includePv == includePv
                    && restored.targetVideoBitrateMbps == 7.125,
                QStringLiteral("the worker receives size mode, target bitrate and independent PV: %1").arg(error), err))
                return false;
        }
    }
    VideoExportTask migrated;
    QJsonObject legacy{{QStringLiteral("size_preset"), QStringLiteral("ultra_compact")}};
    applyVideoExportPreferences(legacy, &migrated);
    if (!require(migrated.sizePreset == VideoExportSizePreset::Custom && !migrated.includePv
            && migrated.targetVideoBitrateMbps == 4.0 && migrated.audioBitrateKbps == 128,
        QStringLiteral("the legacy compact preset retains its bitrate and PV setting"), err)) return false;
    legacy.insert(QStringLiteral("size_preset"), QStringLiteral("target_30mb"));
    legacy.insert(QStringLiteral("include_pv"), true);
    applyVideoExportPreferences(legacy, &migrated);
    if (!require(migrated.sizePreset == VideoExportSizePreset::Target30Mb && migrated.includePv,
        QStringLiteral("an explicit PV selection takes precedence over the legacy 30 MB default"), err)) return false;

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
