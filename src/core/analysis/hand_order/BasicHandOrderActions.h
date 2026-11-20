#pragma once
#include "core/analysis/hand_order/HandOrderJudge.h"
#include <QSet>

namespace miacode::hand_order {
struct ScheduledAction {
    ContactAction action;
    bool button = false, motion = false;
    int lane = 0;
    bool committed = false, ordinary = false;
    int ordinal = -1;
    double softWeight = 1.0;
    int touchParseOrder = -1;
};
// Drops only points equal to their time-linear interpolation (round-off
// tolerance); collinearity alone is insufficient when velocity changes.
void compactContactAction(ContactAction& action);

// A single finite screen contact is constructed for each physical hand.
// Logical paths remain separate for attribution, but have identical physical
// centres during overlap. Replay must still prove every logical judgement.
bool resolveHandOrderActions(const QVector<ScheduledAction>& logical,
    QVector<ScheduledAction>* physical, qint64& work);

struct PhysicalCrossingEffort {
    int flowingConflicts = 0;
    double stationarySeconds = 0.0;
    bool valid = true;
};
// Input must have been resolved to one centre per hand. Attribute a crossing
// once per physical interval, independent of duplicated logical slide branches.
PhysicalCrossingEffort physicalHandCrossingEffort(const QVector<ScheduledAction>& physical,
    const QSet<int>& holdSources, const QSet<int>& sweepSources, qint64& work);

struct PhysicalHandTransfer {
    double second = 0;
    QPointF position;
    int beforeHand = 0, afterHand = 1, ordinal = 1;
};
double physicalHandTransferCharge(const QVector<PhysicalHandTransfer>& transfers);
} // namespace miacode::hand_order
