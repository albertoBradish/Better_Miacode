#pragma once

#include "core/chart/IntroConfig.h"

#include <QDir>
#include <QCryptographicHash>
#include <QRegularExpression>
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

inline QString companionCoverOutputPath(const QString& videoPath, const QString& token, bool multiple)
{
    const QFileInfo info(videoPath);
    QString name = info.completeBaseName() + QStringLiteral("_cover");
    if (multiple) {
        QString label = token.mid(token.indexOf(QLatin1Char(':')) + 1);
        if (token.startsWith(QLatin1String("file:"))) label = QFileInfo(label).completeBaseName();
        label.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
        name += QLatin1Char('_') + label.left(60) + QLatin1Char('_')
            + QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
    }
    QString path = info.dir().filePath(name + QStringLiteral(".png"));
    for (int index = 2; QFileInfo::exists(path); ++index)
        path = info.dir().filePath(name + QStringLiteral("_%1.png").arg(index));
    return path;
}

} // namespace miacode::cover_export
