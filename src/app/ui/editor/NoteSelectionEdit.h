#pragma once

#include "core/chart/transform/ChartNoteSelection.h"
#include "editor/SimaiTextEditPolicy.h"

namespace miacode::ui {
inline bool noteCommandRequiresContiguousSelection(const QString& operation)
{
    return operation == QLatin1String("hand_order.basic_layout")
        || operation == QLatin1String("hand_order.split_annotation")
        || operation == QLatin1String("hand_order.split_native_slide");
}

// Collect independent object edits before applying one native editor transaction.
template <typename EditRange>
miacode::editor::SimaiTextEditResult editSelectedNoteRanges(const QString& original,
    const miacode::chart_selection::NoteRanges& selected, const QString& operation, EditRange editRange)
{
    miacode::editor::SimaiTextEditResult result;
    const auto ranges = miacode::chart_selection::normalizedNoteRanges(selected, original.size());
    if (ranges.isEmpty()) return result;
    if (ranges.size() > 1 && noteCommandRequiresContiguousSelection(operation)) {
        result.error = QStringLiteral("hand_order.object_selection_requires_contiguous");
        return result;
    }
    QString transformed = original;
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
        const auto part = editRange(transformed, it->start, it->end);
        if (!part.error.isEmpty() || !part.consumed) {
            result.error = part.error;
            return result;
        }
        const auto& edit = part.transaction;
        if (edit.hasEdit) transformed.replace(edit.replacementStart,
            edit.replacementEnd - edit.replacementStart, edit.replacementText);
    }
    result.consumed = true;
    result.transaction.hasEdit = transformed != original;
    result.transaction.replacementStart = 0;
    result.transaction.replacementEnd = original.size();
    result.transaction.replacementText = transformed;
    result.transaction.anchor = result.transaction.position = ranges.first().start;
    return result;
}
} // namespace miacode::ui
