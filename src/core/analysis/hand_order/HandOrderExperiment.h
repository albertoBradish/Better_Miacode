#pragma once

#include "core/analysis/hand_order/BasicHandOrder.h"
#include <QJsonObject>
#include <QStringList>

namespace miacode::hand_order::experiment {
inline constexpr int version = 1;

struct Segment {
    double startSecond = 0, endSecond = 0;
    int sourceStart = 0, sourceEnd = 0, line = 1, column = 1;
    int events = 0, unresolved = 0, contactCandidates = 0;
    double effortCost = 0, peakSpeed = 0, peakTapRate = 0, peakLoad = 0, candidateSupport = 0;
    QStringList reasons;
};

struct Analysis {
    bool valid = false, limited = false;
    QString error;
    QString inputSha256;
    int algorithmVersion = basicAlgorithmVersion;
    int judgeProfileRevision = basicJudgeProfileRevision;
    qint64 workUsed = 0;
    int unlocatedRows = 0;
    PlayerProfile profile;
    QVector<Segment> segments;
};

struct SimulationParameters {
    double reactionMs = 80.0;
    double movementSpeed = 7.0;
    double stamina = 1.0;
    double judgementJitterMs = 15.0;
    double referenceWindowMs = 150.0;
    int trials = 64;
    quint32 seed = 20261112;
};

struct SimulationSegment {
    int index = 0;
    qint64 samples = 0, outsideWindow = 0;
    double meanAbsOffsetMs = 0, p95AbsOffsetMs = 0;
};

struct Simulation {
    bool valid = false;
    QString error;
    SimulationParameters parameters;
    qint64 samples = 0, outsideWindow = 0;
    QVector<SimulationSegment> segments;
};

inline constexpr qsizetype maximumInputUtf8Bytes = 256 * 1024;
bool withinInputLimit(const QString& text);
bool validParameters(const SimulationParameters& parameters);
Analysis analyze(const QString& text, const SimaiParseResult& parsed,
                 const simai::SimaiTimingMetadata& timing, const PlayerProfile& profile = {});
Simulation simulate(const Analysis& analysis, const SimulationParameters& parameters);
QJsonObject toJson(const Analysis& analysis);
QJsonObject toJson(const Simulation& simulation);
} // namespace miacode::hand_order::experiment
