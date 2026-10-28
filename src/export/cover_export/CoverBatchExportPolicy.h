#pragma once

#include "core/chart/IntroConfig.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QtGlobal>

namespace miacode::cover_export {

// The export audition uses a negative intro axis, while note markers already
// include #first. PV-preview's hold samples a separate positive media segment.
struct CoverPreviewFrameTime {
    double chartSeconds = 0.0;
    double pvSeconds = 0.0;
    bool hideChart = false;
};

inline CoverPreviewFrameTime coverPreviewFrameTime(double previewSeconds,
    bool introEnabled, bool pvPreview, double pvPreviewStartSeconds)
{
    CoverPreviewFrameTime time{previewSeconds, qMax(0.0, previewSeconds), false};
    if (introEnabled && pvPreview && previewSeconds < 0.0) {
        const double elapsed = previewSeconds + miacode::intro::introDurationSeconds(true);
        if (elapsed >= 0.0 && elapsed < miacode::intro::kPvPreviewHoldSeconds) {
            time.pvSeconds = qMax(0.0, pvPreviewStartSeconds) + elapsed;
            time.hideChart = true;
        }
    }
    return time;
}

inline double boundedBatchCoverFrameSeconds(double requested, double chartDuration,
                                            double pvSeekEndSeconds = -1.0)
{
    const double end = pvSeekEndSeconds >= 0.0
        ? qMin(chartDuration, pvSeekEndSeconds) : chartDuration;
    return qBound(0.0, requested, qMax(0.0, end));
}

inline QString batchCoverOutputPath(const QString& outputDirectory, const QString& exportPath)
{
    return QDir(outputDirectory).filePath(QFileInfo(exportPath).completeBaseName()
                                        + QStringLiteral("_cover.png"));
}

} // namespace miacode::cover_export
