#pragma once

#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/analysis/hand_order/HandPlaySettings.h"
#include <QHash>
#include <QSet>

namespace miacode::hand_order {

struct HandOrderConflictReview {
    bool complete = true;
    int intervals = 0;
    QSet<int> regressedOrders;
    QSet<int> affectedSources;
    QHash<int, FullReplayJudgement> affectedJudgements;
};

HandOrderConflictReview reviewPhysicalConflicts(const QVector<TimelineNoteMarker>& markers,
    const BasicReplayEvidence& evidence, const PlayProfile& profile, bool wifiNeedC, qint64& work,
    const HandOrderProgressCallback& progress = {});

} // namespace miacode::hand_order
