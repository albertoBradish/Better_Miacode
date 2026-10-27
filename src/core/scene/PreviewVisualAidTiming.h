#pragma once

#include "core/chart/parser/SimaiParser.h"

namespace miacode::preview::scene {

enum class PreviewMeasureLineMode { ChartClockCount = 0, CustomBeats = 1, TextLines = 2 };

inline PreviewMeasureLineMode normalizedMeasureLineMode(int value)
{
    return value == 1 ? PreviewMeasureLineMode::CustomBeats
        : value == 2 ? PreviewMeasureLineMode::TextLines : PreviewMeasureLineMode::ChartClockCount;
}

// Quarter-note beats accumulate from chart zero; BPM directives never reset
// this grid. firstSeconds is the same chart-to-media offset used by notes.
QVector<double> buildPreviewMeasureLineSeconds(const SimaiParseResult& parsed,
    int chartClockCount, PreviewMeasureLineMode mode, int customBeats, double firstSeconds);

}
