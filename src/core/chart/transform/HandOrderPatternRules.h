#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/model/HandOrderSweepThreshold.h"
#include <QMetaType>
#include <memory>

namespace miacode::hand_order {

enum class PatternRecognitionMode { ExistingRules, Prioritized };
enum class InteractionPhaseMode { None, Alternating, Triangle };

struct PatternVariant {
    QString strategy;
    QString choiceKey;
    QString syntax;
    QVector<chart_transform::HandOrderEditPlan::Patch> patches;
    QVector<TimelineNoteMarker> markers;
};
struct PatternPreviewData { QVector<TimelineNoteMarker> markers; };
using PatternPreview = std::shared_ptr<const PatternPreviewData>;

struct PatternSuggestion {
    QString registryId;
    QString name;
    QString instanceKey;
    QString similarityKey;
    int start = 0, end = 0, line = 1, col = 1;
    double second = 0, endSecond = 0;
    // Structural certainty, not a calibrated probability or a physical proof.
    bool definite = false;
    int preferred = -1;
    int solverPriority = 0;
    QVector<PatternVariant> variants;
    InteractionPhaseMode interactionPhase = InteractionPhaseMode::None;
    bool prioritizesTrack = false;
};

// Named rules use the IDs in tools/hand_order_patterns/registry.csv. Only an
// observed structure produces a match; unknown names never get a generic label.
QVector<PatternSuggestion> recognizeHandOrderPatterns(const QString& text,
    const SimaiParseResult& parsed, int begin, int end,
    PatternRecognitionMode mode = PatternRecognitionMode::ExistingRules,
    int sweepThreshold = chart::handOrderDefaultSweepThreshold);

} // namespace miacode::hand_order

Q_DECLARE_METATYPE(QVector<TimelineNoteMarker>)
Q_DECLARE_METATYPE(miacode::hand_order::PatternPreview)
