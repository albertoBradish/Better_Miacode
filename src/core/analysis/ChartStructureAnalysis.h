#pragma once

#include "core/chart/ChartPreviewRegion.h"
#include "core/chart/parser/SimaiParser.h"

#include <QVector>
#include <atomic>

namespace miacode::chart_structure {

struct AudioFrame {
    double energy = 0.0;
    double flux = 0.0;
    double lowBand = 0.0;
    double highBand = 0.0;
};

struct AudioFeatures {
    double durationSeconds = 0.0;
    double secondsPerFrame = 0.5;
    QVector<AudioFrame> frames;
};

struct PressureFrame {
    double density = 0.0;
    double simultaneous = 0.0;
    double held = 0.0;
    double slides = 0.0;
    double movement = 0.0;
    double rhythm = 0.0;
    double strength = 0.0;
};

struct PressureCurve {
    double durationSeconds = 0.0;
    double secondsPerFrame = 0.25;
    QVector<PressureFrame> frames;
};

struct DropCandidate {
    chart_preview::MediaRange range;
    double score = 0.0;
    bool confident = false;
};

AudioFeatures analyzeAudio(const QVector<float>& samples, int sampleRate,
    const std::atomic_bool* cancelled = nullptr);
PressureCurve estimatePressure(const SimaiParseResult& chart, double firstSeconds, double mediaDuration);
QVector<DropCandidate> recommendDrops(const AudioFeatures& audio, const PressureCurve& pressure);

} // namespace miacode::chart_structure
