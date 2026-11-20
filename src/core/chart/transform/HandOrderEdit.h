#pragma once

#include "core/chart/parser/SimaiParser.h"

namespace miacode::chart_transform {

struct HandOrderTokenEdit {
    int start = 0;
    int end = 0;
    int line = 1;
    int col = 1;
    QString baseToken;
    bool hasHead = false;
    HandOrderHand head = HandOrderHand::Unspecified;
    QVector<QVector<HandOrderTrackStop>> tracks;
    QVector<HandOrderWifiMode> wifiModes;
    QVector<QVector<QVector<HandOrderTrackStop>>> wifiSectors;
    QVector<bool> wifiBranches;
};

struct HandOrderEditPlan {
    QString replacement;
    int changed = 0;
    int partial = 0;
    int unsupported = 0;
    QVector<SimaiMessage> diagnostics;
    // Split drafts use the resulting document's UTF-16 coordinates.
    QVector<HandOrderTokenEdit> intervals;
    struct Patch { int start; int end; QString text; };
    QVector<Patch> patches;
};

QVector<double> handOrderSplitProgresses(const QVector<double>& movementDurations);
QString handOrderSuffix(const HandOrderTokenEdit& token);
bool handOrderTokenComplete(const HandOrderTokenEdit& token);
HandOrderEditPlan setHandOrderInSelection(const QString& text, int begin, int end, HandOrderHand hand,
    const miacode::simai::SimaiTimingMetadata& metadata = {});
HandOrderEditPlan splitHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {});
// The first bisection assigns L/R. Further bisections retain each interval's hand.
HandOrderEditPlan bisectHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {});
HandOrderEditPlan swapHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {});

} // namespace miacode::chart_transform
