#pragma once

#include "export/video_export/VideoExportEncoding.h"
#include <QJsonObject>
#include <QStringList>
#include <cmath>

namespace miacode::video_export {

// CLI spelling of the official three-strategy model; no legacy size presets.
inline bool readVideoEncodingRequest(const QJsonObject& values, VideoEncodingSettings* settings, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (values.contains("size-preset"))
        return fail(QStringLiteral("--size-preset is retired; use --rate-mode file_size --size-limit-mb N."));
    QString mode = values.value("rate-mode").toString();
    if (values.contains("rate-mode") && mode.isEmpty())
        return fail(QStringLiteral("--rate-mode must be quality, file_size or bitrate."));
    if (mode.isEmpty()) {
        if (values.contains("size-limit-mb") && values.contains("video-bitrate"))
            return fail(QStringLiteral("Specify --rate-mode when providing both size and bitrate settings."));
        mode = values.contains("size-limit-mb") ? "file_size" : values.contains("video-bitrate") ? "bitrate" : "quality";
    }
    if (!QStringList{"quality", "file_size", "bitrate"}.contains(mode))
        return fail(QStringLiteral("--rate-mode must be quality, file_size or bitrate."));
    const QString quality = values.value("quality").toString("standard");
    if (!QStringList{"high", "standard", "small"}.contains(quality))
        return fail(QStringLiteral("--quality must be high, standard or small."));
    settings->mode = mode == "file_size" ? VideoRateMode::FileSize : mode == "bitrate" ? VideoRateMode::Bitrate : VideoRateMode::Quality;
    settings->quality = quality == "high" ? VideoQualityLevel::High : quality == "small" ? VideoQualityLevel::Small : VideoQualityLevel::Standard;
    for (const QString& key : {QStringLiteral("size-limit-mb"), QStringLiteral("video-bitrate")}) {
        if (!values.contains(key)) continue;
        const auto value = values.value(key);
        bool valid = false;
        double number = 0.0;
        if (value.isDouble()) { number = value.toDouble(); valid = true; }
        else if (value.isString()) number = value.toString().toDouble(&valid);
        if (!valid || !std::isfinite(number)) return fail(key + " must be a finite number.");
        if (key == "size-limit-mb") {
            if (number < kVideoSizeLimitMinMb || number > kVideoSizeLimitMaxMb || number != std::floor(number))
                return fail(QStringLiteral("--size-limit-mb must be an integer between 5 and 1000."));
            settings->sizeLimitMb = static_cast<int>(number);
        } else {
            if (number * 1000 < kVideoBitrateMinKbps || number * 1000 > kVideoBitrateMaxKbps)
                return fail(QStringLiteral("--video-bitrate must be between 0.1 and 100 Mbps."));
            settings->videoBitrateKbps = qRound(number * 1000);
        }
    }
    return true;
}

} // namespace miacode::video_export
