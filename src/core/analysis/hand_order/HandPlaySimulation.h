#pragma once

#include "core/analysis/MuriTypes.h"
#include "core/analysis/hand_order/HandPlayContacts.h"
#include "core/analysis/hand_order/HandPlayPose.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"

namespace miacode::hand_order {

enum class PlayIssueKind { ArmCollision, OutOfReach, Occupied, TooFast, CrossedHands, JudgementLoss };
enum class PlayGrade { None, Critical, Perfect, Great, Good, Miss };

struct PlayJudgement {
    int source = -1;
    bool slideBody = false;
    double second = 0.0;
    QPointF position;
    PlayGrade grade = PlayGrade::None;
    int baseLoss = 0;
    int breakBonusLoss = 0;
    double achievementLoss = 0.0;
    double cumulativeLoss = 0.0;
};

QString playGradeName(PlayGrade grade);

struct PlayIssue {
    PlayIssueKind kind;
    double second = 0.0;
    double endSecond = 0.0;
    int line = 1;
    int column = 1;
    int endColumn = 1;
    // 1 = left, 2 = right, 3 = both.
    int handMask = 0;
    int source = -1;
    int otherSource = -1;
    double judgeSecond = 0.0;
    PlayGrade grade = PlayGrade::None;
    int baseLoss = 0;
    double achievementLoss = 0.0;
};

struct PlaySimulationFrame {
    PlayContactFrame contacts;
    PlayArmFrame arms;
};

class HandPlaySimulation
{
public:
    HandPlaySimulation(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options,
        bool computeJudgements = false);
    bool isEmpty() const { return contacts_.isEmpty(); }
    double bodyOffsetCm(double second) const;
    double distanceOffsetCm(double second) const;
    PlaySimulationFrame sample(double second, bool withTrail = true) const;
    QVector<PlayIssue> detectIssues(const QVector<TimelineNoteMarker>& markers) const;
    const QVector<PlayJudgement>& judgements() const { return judgements_; }
    const MuriAnalysisReport& report() const { return report_; }
    bool replayVerified() const { return replayVerified_; }

private:
    HandPlayContacts contacts_;
    PlayProfile profile_;
    double palmRadius_ = 0.0;
    QVector<double> reviewTimes_;
    QVector<double> bodyOffsets_;
    QVector<double> distanceOffsets_;
    QVector<PlayJudgement> judgements_;
    MuriAnalysisReport report_;
    bool replayVerified_ = false;

    void prepareMotion(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options,
        const PlayTraceShortcuts& shortcuts = {});
    FullReplay replay(const QVector<TimelineNoteMarker>& markers, const MuriRenderOptions& options) const;
    void buildJudgements(const QVector<TimelineNoteMarker>& markers, const FullReplay& replay);
    void buildReport(const QVector<TimelineNoteMarker>& markers, const FullReplay& replay);
};

} // namespace miacode::hand_order
