#pragma once

#include <optional>

#include <QString>
#include <QtGlobal>

namespace miacode::video_export {

enum class VideoExportSizePreset {
    Standard = 0,
    Compact = 1,
    UltraCompactWithPv = 2,
    UltraCompact = 3,
    Target30Mb = 4,
    Target100Mb = 5,
    Target200Mb = 6,
    Custom = 7,
};

constexpr int kVideoExportWorkerMaxCrashRetries = 1;

struct VideoExportSizePolicy {
    double bitrateCoefficient = 0.0;
    qint64 minBitrateKbps = 0;
    qint64 maxBitrateKbps = 0;
    double maxRateMultiplier = 1.0;
    double bufferMultiplier = 1.0;
    int gopSeconds = 2;
    int maxAudioBitrateKbps = 320;
    // A size preset adjusts the quality tier selected by Fast/HighQuality;
    // it never replaces that tier's base CRF.
    int x264CrfAdjustment = 0;
    bool usePeakConstrainedVbr = false;
};

inline constexpr double kCustomVideoBitrateMinMbps = 0.1;
inline constexpr double kCustomVideoBitrateMaxMbps = 100.0;
inline constexpr double kCustomVideoBitrateDefaultMbps = 8.0;

double normalizedCustomVideoBitrateMbps(double value);
double legacySizePresetVideoBitrateMbps(VideoExportSizePreset preset, int width, int height, int fps);
bool legacySizePresetOmitsPv(VideoExportSizePreset preset);
VideoExportSizePolicy videoExportSizePolicy(VideoExportSizePreset preset);
QString videoExportSizePresetToken(VideoExportSizePreset preset);
VideoExportSizePreset videoExportSizePresetFromToken(const QString& token,
    VideoExportSizePreset fallback = VideoExportSizePreset::Standard);
qint64 videoExportTargetBytes(VideoExportSizePreset preset);
qint64 videoExportTargetBitrateKbps(VideoExportSizePreset preset, double seconds, int audioKbps);
int effectiveVideoExportAudioBitrateKbps(VideoExportSizePreset preset, int requestedKbps);
int effectiveVideoExportX264Crf(VideoExportSizePreset preset, int baseCrf);

bool shouldRequestOffscreenPboReadback(
    const std::optional<bool>& enableOverride,
    const std::optional<bool>& disableOverride
);

bool shouldRetryVideoExportWorkerAfterCrash(
    bool crashExit,
    bool pboRequestedForAttempt,
    bool cancelRequested,
    int completedAttemptCount
);

}  // namespace miacode::video_export
