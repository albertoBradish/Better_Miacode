#pragma once
#include "core/analysis/hand_order/HandOrderJudge.h"

namespace miacode::hand_order {
bool buildWifiHandOrderActions(const TimelineNoteMarker& marker,
    QVector<ContactAction>* sectors, QVector<ContactAction>* singleHand);
} // namespace miacode::hand_order
