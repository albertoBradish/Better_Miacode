#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/transform/ChartNoteSelection.h"

namespace miacode::chart_transform {

// Internal geometric tool: selected Slide starts must determine one common
// reflection axis. The real trajectory determines all hand-change progress.
HandOrderEditPlan screenSplitHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {});

// All selected Slides determine one axis. The replacement covers the first-to-
// last selected source span; gaps remain unchanged and never contribute starts.
HandOrderEditPlan screenSplitHandOrderInSelection(const QString& text,
    const miacode::chart_selection::NoteRanges& ranges,
    const miacode::simai::SimaiTimingMetadata& metadata = {});

} // namespace miacode::chart_transform
