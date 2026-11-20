#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include <QStringList>

namespace miacode::chart_transform {

struct NativeSlideSplitItem {
    int start = 0, end = 0, line = 1, col = 1;
    QString before, after, status, reason;
    QStringList changes;
    int beforeTracks = 0, afterTracks = 0, beforeHeads = 0, afterHeads = 0;
};

struct NativeSlideSplitPlan {
    bool canApply = false;
    // Replacement for [begin,end). On a blocked batch this remains the original
    // selection and patches is empty; item drafts remain available for preview.
    QString replacement;
    QVector<HandOrderEditPlan::Patch> patches;
    QVector<NativeSlideSplitItem> items;
    QVector<SimaiMessage> diagnostics;
    QStringList blockers;
    int changed = 0, partial = 0, unsupported = 0, selected = 0, ignored = 0;
    int beforeTracks = 0, afterTracks = 0;
    int beforeHeads = 0, afterHeads = 0;
};

// Only existing native chain segments are split. Geometry and actual movement
// intervals are strictly reparsed and compared, but independent judgement,
// counting, Each, disappearance, SFX and Muri semantics can change.
NativeSlideSplitPlan planNativeSlideSplit(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {});

} // namespace miacode::chart_transform
