#pragma once

#include "core/chart/ChartTiming.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/parser/SimaiParser.h"
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <utility>

namespace miacode::chart {
struct BpmImportPoint {
    double second = 0;
    double bpm = 120;
    int numerator = 4;
    int denominator = 4;
    bool explicitMeter = false;
};
struct BpmImportResult {
    QString chart;
    QString error;
    int changes = 0;
    int notes = 0;
    QStringList nonStandardSubdivisions;
    bool ok() const { return error.isEmpty() && !chart.isEmpty(); }
};

// Uses only existing simai syntax. Durations become tempo-independent; rests
// use short sums of reciprocal subdivisions instead of millions of empty slots.
inline BpmImportResult importChartBpm(const QString& input, const QString& original,
    const miacode::simai::SimaiTimingMetadata& metadata, double first)
{
    BpmImportResult result;
    const auto fail = [&result](const char* code) { result.chart.clear(); result.error = QString::fromLatin1(code); return result; };
    if (!std::isfinite(first) || input.size() > 4 * 1024 * 1024) return fail("tools.bpm.import_invalid");
    QVector<BpmImportPoint> points;
    double importedEnd = 0;
    if (input.contains(QRegularExpression(QStringLiteral("(?m)^\\s*&inote_\\d+\\s*=")))) {
        const auto document = SimaiDocument::fromText(input);
        const auto ids = document.difficultyIds();
        if (ids.size() != 1) return fail("tools.bpm.import_one_difficulty");
        bool validFirst = document.first.trimmed().isEmpty();
        const double importedFirst = validFirst ? 0 : document.first.toDouble(&validFirst);
        const auto parsed = SimaiParser::parseForTimeline(document.difficulty(ids.first())->chart, miacode::simai::buildTimingMetadata(document));
        if (!validFirst || !std::isfinite(importedFirst) || !parsed.ok || !parsed.noteMarkers.isEmpty()
            || parsed.timingPoints.isEmpty()) return fail("tools.bpm.import_empty_required");
        for (const auto& point : parsed.timingPoints)
            points.append({point.second + importedFirst - first, point.bpm,
                point.meterNumerator, point.meterDenominator, point.meterExplicit});
        importedEnd = parsed.endSecond + importedFirst - first;
    } else {
        static const QRegularExpression item(QStringLiteral(R"((-?)(\d+):(\d{2}):(\d{3})\s*/\s*(\d+(?:\.\d+)?))"));
        auto matches = item.globalMatch(input.trimmed());
        const QString text = input.trimmed();
        qsizetype end = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString gap = text.mid(end, match.capturedStart() - end).trimmed();
            if ((end == 0 && !gap.isEmpty()) || (end != 0 && gap != QLatin1String("-"))) return fail("tools.bpm.import_invalid");
            bool minutesOk = false, bpmOk = false;
            const double minutes = match.captured(2).toDouble(&minutesOk);
            const int seconds = match.captured(3).toInt(), millis = match.captured(4).toInt();
            const double bpm = match.captured(5).toDouble(&bpmOk);
            if (!minutesOk || !bpmOk || !std::isfinite(minutes) || !std::isfinite(bpm)
                || bpm <= 0 || bpm > 100000 || seconds > 59) return fail("tools.bpm.import_invalid");
            const double time = (minutes * 60 + seconds + millis / 1000.0) * (match.captured(1).isEmpty() ? 1 : -1) - first;
            if (!std::isfinite(time) || (!points.isEmpty() && time < points.last().second)) return fail("tools.bpm.import_invalid");
            if (!points.isEmpty() && time == points.last().second) points.last() = {time, bpm};
            else points.append({time, bpm});
            end = match.capturedEnd();
        }
        if (!text.mid(end).trimmed().isEmpty()) return fail("tools.bpm.import_invalid");
    }
    if (points.isEmpty() || points.size() > 10000) return fail("tools.bpm.import_invalid");
    const auto parsed = SimaiParser::parseForTimeline(original, metadata);
    if (!parsed.ok || !SimaiParser::validateSyntax(original, metadata).ok) return fail("tools.bpm.import_chart_invalid");
    result.notes = parsed.noteMarkers.size();
    result.changes = points.size();
    const bool replaceMeters = std::any_of(points.cbegin(), points.cend(), [](const auto& point) { return point.explicitMeter; });
    QMap<double, QStringList> payloads;
    QVector<int> commas;
    for (const auto& span : miacode::simai::chartContentSpans(original, 0, original.size()))
        for (int i = span.start; i < span.end; ++i) if (original[i] == QLatin1Char(',')) commas.append(i);
    if (commas.size() != parsed.beatMarkers.size()) return fail("tools.bpm.import_chart_invalid");
    const auto cleanSlot = [&](const QString& slot, double oldBpm) {
        QString cleaned;
        int cursor = 0;
        for (const auto& span : miacode::simai::chartContentSpans(slot, 0, slot.size())) {
            QString code = slot.mid(span.start, span.end - span.start);
            static const QRegularExpression directives(QStringLiteral(R"(\([^()]*\)|\{[^{}]*\}|\[[^\[\]]*\])"));
            QString converted;
            qsizetype offset = 0;
            auto matches = directives.globalMatch(code);
            while (matches.hasNext()) {
                const auto match = matches.next();
                converted += code.mid(offset, match.capturedStart() - offset);
                QString token = match.captured();
                if (token.startsWith(QLatin1Char('['))) {
                    QString duration = token.mid(1, token.size() - 2);
                    const int doubleHash = duration.indexOf(QStringLiteral("##"));
                    if (doubleHash >= 0) {
                        const QString tail = duration.mid(doubleHash + 2);
                        if (tail.contains(QLatin1Char(':')) && !tail.contains(QLatin1Char('#')))
                            duration = duration.left(doubleHash + 2) + QString::number(oldBpm, 'g', 17) + QLatin1Char('#') + tail;
                    } else if (!duration.contains(QLatin1Char('#'))) {
                        duration.prepend(QString::number(oldBpm, 'g', 17) + QLatin1Char('#'));
                    }
                    converted += QLatin1Char('[') + duration + QLatin1Char(']');
                }
                offset = match.capturedEnd();
            }
            converted += code.mid(offset);
            converted.remove(QRegularExpression(QStringLiteral(R"((?<![A-Za-z0-9])E(?![1-8]))")));
            cleaned += slot.mid(cursor, span.start - cursor) + converted;
            cursor = span.end;
        }
        cleaned += slot.mid(cursor);
        if (replaceMeters)
            cleaned.remove(QRegularExpression(QStringLiteral(R"(\|\|\s*\d+\s*/\s*\d+[^\n]*)")));
        return cleaned;
    };
    int start = 0;
    for (int i = 0; i <= commas.size(); ++i) {
        const int end = i < commas.size() ? commas[i] : original.size();
        const double time = i < parsed.beatMarkers.size() ? parsed.beatMarkers[i].second : parsed.endSecond;
        const auto slot = cleanSlot(original.mid(start, end - start), timingAt(parsed.timingPoints, time).bpm);
        if (!slot.trimmed().isEmpty()) payloads[time].append(slot);
        start = end + 1;
    }
    const double end = qMax(qMax(parsed.endSecond, importedEnd), points.last().second);
    if (!std::isfinite(end) || end > 86400) return fail("tools.bpm.import_invalid");
    payloads[0]; payloads[qMax(0.0, end)];
    for (const auto& point : points) if (point.second >= 0) payloads[point.second];
    qsizetype timingIndex = 0;
    BpmImportPoint state = points.first();
    while (timingIndex < points.size() && points[timingIndex].second <= 0) state = points[timingIndex++];
    QString output = QStringLiteral("(%1){4}\n").arg(QString::number(state.bpm, 'g', 17));
    if (state.explicitMeter) output.prepend(QStringLiteral("|| %1/%2\n").arg(state.numerator).arg(state.denominator));
    double clock = 0;
    QString pendingPayload;
    const auto advance = [&](double target) {
        // Prefer a normal musical subdivision before using reciprocal rests.
        const double whole = (target - clock) * state.bpm / 240.0;
        int bestDivision = 0, bestCount = 0, bestCost = 100000;
        for (int division = 1; division <= 384 && whole > 1e-10; ++division) {
            if (whole * division > 512.5) continue;
            const int count = qRound(whole * division);
            if (count < 1 || count > 512) continue;
            if (std::abs(240.0 * count / (state.bpm * division) - (target - clock)) > 1e-8) continue;
            const int cost = QString::number(division).size() + count;
            if (cost < bestCost) { bestCost = cost; bestDivision = division; bestCount = count; }
        }
        if (bestDivision) {
            output += QStringLiteral("{%1}\n").arg(bestDivision) + std::exchange(pendingPayload, QString{});
            output += QString(bestCount, QLatin1Char(','));
            clock += 240.0 * bestCount / (state.bpm * bestDivision);
        }
        for (int step = 0; target - clock > 1e-8 && step < 10000; ++step) {
            const double whole = (target - clock) * state.bpm / 240.0;
            double divisor = std::ceil(1.0 / whole - 1e-10);
            divisor = qBound(1.0, divisor, static_cast<double>(std::numeric_limits<int>::max()));
            const double duration = 240.0 / (state.bpm * divisor);
            if (duration > target - clock + 1e-7) return false;
            output += QStringLiteral("{%1}\n").arg(static_cast<int>(divisor))
                + std::exchange(pendingPayload, QString{}) + QLatin1Char(',');
            clock += duration;
        }
        output += QLatin1Char('\n');
        return std::abs(clock - target) < 1e-6;
    };
    for (auto it = payloads.cbegin(); it != payloads.cend(); ++it) {
        if (!advance(it.key())) return fail("tools.bpm.import_precision");
        bool changed = false;
        while (timingIndex < points.size() && points[timingIndex].second <= it.key() + 1e-9) { state = points[timingIndex++]; changed = true; }
        if (changed) {
            if (state.explicitMeter) output += QStringLiteral("|| %1/%2\n").arg(state.numerator).arg(state.denominator);
            output += QStringLiteral("(%1){4}\n").arg(QString::number(state.bpm, 'g', 17));
        }
        for (const auto& slot : it.value()) pendingPayload += slot;
        if (output.size() > 8 * 1024 * 1024) return fail("tools.bpm.import_precision");
    }
    output += pendingPayload + QStringLiteral("\nE\n");
    const auto verified = SimaiParser::parseForTimeline(output, metadata);
    if (!verified.ok || !SimaiParser::validateSyntax(output, metadata).ok
        || verified.noteMarkers.size() != parsed.noteMarkers.size()) return fail("tools.bpm.import_precision");
    const auto close = [](double a, double b) { return std::abs(a - b) <= 1e-6; };
    for (int i = 0; i < parsed.noteMarkers.size(); ++i) {
        const auto& a = parsed.noteMarkers[i]; const auto& b = verified.noteMarkers[i];
        if (a.type != b.type || a.lane != b.lane || a.endLane != b.endLane || !close(a.second, b.second)
            || !close(a.endSecond, b.endSecond) || !close(a.slideTraceSecond, b.slideTraceSecond)
            || a.isBreak != b.isBreak || a.isEx != b.isEx || a.isMine != b.isMine || a.hsMultiplier != b.hsMultiplier
            || a.slideSegmentDurations.size() != b.slideSegmentDurations.size()) return fail("tools.bpm.import_precision");
        for (int j = 0; j < a.slideSegmentDurations.size(); ++j)
            if (!close(a.slideSegmentDurations[j], b.slideSegmentDurations[j])
                || !close(a.slideSegmentShootSeconds[j], b.slideSegmentShootSeconds[j])) return fail("tools.bpm.import_precision");
    }
    for (const auto& point : points)
        if (point.second >= 0 && std::abs(timingAt(verified.timingPoints, point.second + 2e-6).bpm - point.bpm) > 1e-9)
            return fail("tools.bpm.import_precision");
    QSet<int> nonStandard;
    for (const auto& beat : verified.beatMarkers)
        if (beat.subdivision > 0 && 384 % beat.subdivision != 0) nonStandard.insert(beat.subdivision);
    auto divisions = nonStandard.values();
    std::sort(divisions.begin(), divisions.end());
    for (int division : divisions) result.nonStandardSubdivisions.append(QString::number(division));
    result.chart = output;
    return result;
}
} // namespace miacode::chart
