#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/transform/ChartNoteSelection.h"

namespace miacode::chart_transform {
HandOrderEditPlan alternateHandOrderInSelection(const QString& text,
    const miacode::chart_selection::NoteRanges& selected,
    const miacode::simai::SimaiTimingMetadata& metadata = {});
}
