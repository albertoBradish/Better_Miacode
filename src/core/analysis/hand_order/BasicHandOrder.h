#pragma once
#include "core/chart/transform/HandOrderPatternRules.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/HandOrderEdit.h"
#include "core/analysis/hand_order/HandOrderPlayerModel.h"
#include "core/chart/model/HandOrderSweepThreshold.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderVersion.h"
#include "core/analysis/hand_order/HandOrderProgress.h"
#include <QJsonObject>
#include <QMap>
#include <memory>
#include <limits>

namespace miacode::hand_order {
inline constexpr int basicAlgorithmVersion = chart::handOrderFeatureVersion;
inline constexpr int basicJudgeProfileRevision = 2;
struct BasicParameters {
    static constexpr double minReturnSpeed = 1.0;
    static constexpr double maxReturnSpeed = 8.0;
    static constexpr qint64 maxAllowedWorkUnits = 10000000000LL;
    int version = basicAlgorithmVersion;
    int judgeProfileRevision = basicJudgeProfileRevision;
    int beamWidth = 64;
    int maxTasks = 512;
    int counterfactualBudget = 64;
    double contextSeconds = 1.0;
    double pressSeconds = 0.025;
    double returnSpeed = chart::handOrderDefaultReturnSpeed;
    int sweepThreshold = chart::handOrderDefaultSweepThreshold;
    bool colorUnresolved = false;
    PlayerProfile player;
    qint64 maxWorkUnits = 300000000;
    bool overwriteExisting = false;
    bool highConfidenceOnly = false;
    // 0: exterior preferred, 1: screen preferred, 2: mixed.
    int inputPreference = 0;
    // Internal fixed-assignment audit used by the pattern postprocessor. Native
    // layout priors and a fresh full-stream charge make comparisons symmetric.
    bool evaluateCompleteAssignment = false;
    // Diagnostic compatibility switch for comparing the old fixed-label
    // lookahead expansion. Actual head/track locks are unchanged in both modes.
    bool legacyFixedTargetLookahead = false;
    // Bounded diagnostic evidence only; never enabled by GUI/default benchmarks.
    bool captureRejectedCandidates = false;
    // Request-local bounded memoization of complete deterministic attempts.
    bool reuseWindowSearch = true;
    // Diagnostic-only coherent schedule probe; NaN keeps normal proposals.
    double diagnosticSlideInputOffsetSeconds = std::numeric_limits<double>::quiet_NaN();
    std::shared_ptr<const QVector<PatternSuggestion>> recognizedPatterns;
    bool capturePhysicalReplay = false;
};
struct BasicReplayEvidence;
struct BasicAssignmentEvaluation {
    bool complete = false;
    double cost = 0, risk = 0, peakSpeed = 0, playerCost = 0;
    double peakTapRate = 0, peakLoad = 0;
    int ergonomicBreaks = 0;
    int interactionBreaks = 0, longitudinalBreaks = 0, sameKeyBreaks = 0;
};
struct BasicCause {
    int tokenStart = -1;
    int branch = 0;
    QString kind;
    QString pad;
    double second = 0;
    double progress = 0;
    int handMask = 0;
};
struct BasicHandoffEvidence {
    int branch = 0, handoffCount = 0;
    int sector = -1;
    bool baselineFound = false;
    double baselineCost = 0, baselinePeak = 0;
    QString reason;
};
struct BasicCounterfactualEvidence {
    QString component;
    double progress = 0;
    HandOrderHand alternativeHand = HandOrderHand::Unspecified;
    bool searched = false, found = false, limited = false;
    double signedCostGap = 0, score = 0;
    int signedRiskTierGap = 0, signedPeakTierGap = 0;
};
struct BasicRow {
    int start = 0, end = 0, line = 1, col = 1;
    bool context = false;
    QString originalAnnotation;
    QString status;
    QString reason;
    HandOrderHand head = HandOrderHand::Unspecified;
    HandOrderHand track = HandOrderHand::Unspecified;
    double cost = 0, peakSpeed = 0;
    double playerCost = 0, peakTapRate = 0, peakLoad = 0;
    QVector<int> lanes;
    QVector<HandOrderHand> noteHands;
    bool hasCounterfactual = false;
    QString counterfactualComponent;
    HandOrderHand counterfactualHand = HandOrderHand::Unspecified;
    double signedCostGap = 0;
    int signedPeakTierGap = 0;
    QVector<QVector<HandOrderTrackStop>> trackIntervals;
    QVector<HandOrderWifiMode> wifiModes;
    QVector<QVector<QVector<HandOrderTrackStop>>> wifiSectors;
    bool fullReplayVerified = false;
    bool hasCandidate = false;
    int dependencyUnit = -1;
    double confidence = 0;
    QString confidenceLevel = QStringLiteral("insufficient_evidence");
    QString pressureLevel;
    QString inputChannel;
    QVector<BasicCounterfactualEvidence> counterfactualEvidence;
    QVector<BasicCause> causes;
    QString judgementKind, judgementQuality, conditions, handoffReason;
    double judgementSecond = 0, judgementMargin = 0;
    int handoffCount = 0;
    double oneHandoffCost = 0, oneHandoffPeak = 0;
    QVector<BasicHandoffEvidence> handoffEvidence;
};
struct BasicWindowEvidence {
    int start = 0, end = 0, tasks = 0;
    int coreTokens = 0, plannedTokens = 0, protectedTargetImports = 0, protectedContextImports = 0;
    int decisionTasks = 0, touchTasks = 0, carriedActions = 0, searchAttempts = 0;
    int searchRequests = 0, searchCacheHits = 0, repeatedCompleteSearches = 0;
    qint64 repeatedCompleteWork = 0, reusedSearchWork = 0;
    bool confidenceIterationLimited = false;
    qint64 workUsed = 0;
    QString outcome;
};
struct BasicRejectionEvidence {
    QString category;
    int window = 0;
    QJsonObject detail;
};
struct BasicResult {
    QVector<BasicRow> rows;
    QVector<chart_transform::HandOrderEditPlan::Patch> patches;
    bool truncated = false;
    bool evidenceTruncated = false;
    QString error;
    int contextObjects = 0;
    int algorithmVersion = basicAlgorithmVersion;
    int judgeProfileRevision = basicJudgeProfileRevision;
    qint64 workUsed = 0;
    int includedObjects = 0, auditedObjects = 0, windowCount = 0, maxWindowTasks = 0;
    bool taskLimited = false, workLimited = false;
    QVector<BasicWindowEvidence> searchWindows;
    QMap<QString, qint64> rejectionCounts;
    QVector<BasicRejectionEvidence> rejectionSamples;
    BasicAssignmentEvaluation evaluation;
    bool patternOptimization = false;
    int patternSegments = 0, patternCandidates = 0, patternAccepted = 0;
    QString patternOutcome;
    QVector<PatternSuggestion> patternSuggestions;
    bool combinedOptimization = false;
    bool combinedNormalizationUsed = false;
    int conflictWindows = 0, confirmedConflictNotes = 0, conflictPatchesWithheld = 0;
    bool conflictReviewLimited = false;
    std::shared_ptr<const BasicReplayEvidence> physicalReplay;
    BasicAssignmentEvaluation originalEvaluation;
    QString contextGuardOutcome;
    BasicAssignmentEvaluation contextBaselineEvaluation;
    BasicAssignmentEvaluation contextInternalEvaluation;
    BasicAssignmentEvaluation contextSerializedEvaluation;
    qint64 contextBaselineWorkUsed = 0, contextSearchWorkUsed = 0, contextSerializedWorkUsed = 0;
};
double basicOrdinaryAssociationWeight(double seconds, int intermediateNotes);
// Additive cost over fixed-time motion segments. Coordinates use field-radius units.
double basicMotionCost(const QVector<QPointF>& points, const QVector<double>& segmentSeconds,
                       double* peakSpeed = nullptr);
BasicResult inferBasicHandOrder(const QString& text, const SimaiParseResult& parsed,
    int begin, int end, const BasicParameters& parameters = {},
    const miacode::simai::SimaiTimingMetadata& metadata = {},
    const HandOrderProgressCallback& progress = {});
} // namespace miacode::hand_order
