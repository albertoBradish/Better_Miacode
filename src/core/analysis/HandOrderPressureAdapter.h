#pragma once

#include "core/chart/model/TimelineData.h"

namespace miacode::chart_structure {

struct HandOrderPressureFrame {
    double leftOccupancy = 0.0;
    double rightOccupancy = 0.0;
    double leftMovement = 0.0;
    double rightMovement = 0.0;
    double alternation = 0.0;
};

struct HandOrderPressureCurve {
    double durationSeconds = 0.0;
    double secondsPerFrame = 0.25;
    QVector<HandOrderPressureFrame> frames;
};

// Summarize already applied left/right annotations for inspection. The main
// pressure estimator uses explicit head context when available. An empty
// curve means there was no usable assignment in the displayed time range.
HandOrderPressureCurve estimateHandOrderPressure(const QVector<TimelineNoteMarker>& notes,
    double firstSeconds, double durationSeconds, double secondsPerFrame, int frameCount);

} // namespace miacode::chart_structure
