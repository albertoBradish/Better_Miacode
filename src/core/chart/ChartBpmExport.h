#pragma once
#include "core/chart/parser/SimaiParser.h"
#include <QStringList>
#include <QtMath>
#include <cmath>

namespace miacode::chart {
enum class BpmExportFormat { Sequence, AudioTime, EmptyMaidata };
inline QString bpmAudioTimestamp(double second)
{
    const qint64 ms = qRound64(second * 1000.0);
    const qint64 absolute = qAbs(ms);
    return QStringLiteral("%1%2:%3:%4").arg(ms < 0 ? QStringLiteral("-") : QString())
        .arg(absolute / 60000, 2, 10, QLatin1Char('0'))
        .arg(absolute / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(absolute % 1000, 3, 10, QLatin1Char('0'));
}
inline QString exportChartBpm(const SimaiParseResult& parsed, double first, BpmExportFormat format)
{
    if (parsed.timingPoints.isEmpty() || !std::isfinite(first)) return {};
    QVector<TimelineTimingPoint> changes;
    for (const auto& point : parsed.timingPoints) {
        if (changes.isEmpty() || changes.last().bpm != point.bpm) changes.append(point);
    }
    if (format != BpmExportFormat::EmptyMaidata) {
        QStringList lines;
        for (const auto& point : changes) {
            const QString bpm = QString::number(point.bpm, 'f', 2);
            lines.append(format == BpmExportFormat::Sequence ? bpm
                : bpmAudioTimestamp(point.second + first) + QLatin1Char('/') + bpm);
        }
        return lines.join(format == BpmExportFormat::Sequence ? QStringLiteral("-") : QStringLiteral(" - "));
    }
    QString output = QStringLiteral("&title=BPM timing\n&first=%1\n&inote_1=\n")
        .arg(QString::number(first, 'g', 17));
    double bpm = -1.0;
    int subdivision = -1;
    int numerator = 4, denominator = 4;
    bool explicitMeter = false;
    qsizetype timingIndex = 0;
    const auto emitTiming = [&](const TimelineTimingPoint& point) {
        if (point.meterExplicit && (!explicitMeter || numerator != point.meterNumerator
            || denominator != point.meterDenominator)) {
            output += QStringLiteral("\n|| %1/%2\n").arg(point.meterNumerator).arg(point.meterDenominator);
            numerator = point.meterNumerator;
            denominator = point.meterDenominator;
            explicitMeter = true;
        }
        if (bpm != point.bpm) {
            output += QStringLiteral("(%1)").arg(QString::number(point.bpm, 'g', 17));
            bpm = point.bpm;
            subdivision = -1;
        }
    };
    for (const auto& beat : parsed.beatMarkers) {
        while (timingIndex < parsed.timingPoints.size()
            && parsed.timingPoints[timingIndex].second <= beat.second) emitTiming(parsed.timingPoints[timingIndex++]);
        const int beats = beat.subdivision;
        if (subdivision != beats) {
            output += QStringLiteral("{%1}").arg(beats);
            subdivision = beats;
        }
        output += QLatin1Char(',');
    }
    while (timingIndex < parsed.timingPoints.size()) emitTiming(parsed.timingPoints[timingIndex++]);
    if (subdivision < 0) output += QStringLiteral("{4}");
    return output + QStringLiteral("\nE\n");
}
} // namespace miacode::chart
