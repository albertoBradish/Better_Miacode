#include "core/scene/PreviewVisualAidTiming.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace miacode::preview::scene {
QVector<double> buildPreviewMeasureLineSeconds(const SimaiParseResult& parsed,
    int chartClockCount, PreviewMeasureLineMode mode, int customBeats, double firstSeconds)
{
    QVector<double> result;
    if (!qIsFinite(firstSeconds)) return result;
    const auto append = [&](double second) {
        const double shifted = second + firstSeconds;
        if (qIsFinite(shifted) && (result.isEmpty() || qAbs(result.constLast() - shifted) > 1e-7))
            result.append(shifted);
    };
    if (mode == PreviewMeasureLineMode::TextLines) {
        for (double second : parsed.textLineStartSeconds) append(second);
        return result;
    }
    const int step = mode == PreviewMeasureLineMode::CustomBeats
        ? (customBeats > 0 ? customBeats : 4) : (chartClockCount > 0 ? chartClockCount : 4);
    // Each comma supplies one constant-tempo interval. The parser's beat
    // position is independent of its legacy measure phase/time signature.
    double nextBeat = 0.0;
    for (const TimelineBeatMarker& span : parsed.beatMarkers) {
        if (!qIsFinite(span.quarterBeat) || !qIsFinite(span.quarterBeatLength)
            || !qIsFinite(span.durationSeconds) || span.quarterBeatLength <= 0.0
            || span.durationSeconds <= 0.0) continue;
        const double endBeat = span.quarterBeat + span.quarterBeatLength;
        if (nextBeat + 1e-7 < span.quarterBeat)
            nextBeat = std::ceil((span.quarterBeat - 1e-7) / step) * step;
        while (nextBeat <= endBeat + 1e-7) {
            const double fraction = qBound(0.0, (nextBeat - span.quarterBeat) / span.quarterBeatLength, 1.0);
            append(span.second + fraction * span.durationSeconds);
            nextBeat += step;
        }
    }
    return result;
}
}
