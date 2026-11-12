#pragma once

#include <QString>
#include "app/services/ChartWorkspace.h"
#include "app/services/ChartWorkspaceFileService.h"

namespace miacode::automatic_chart {
enum class Trigger { FirstOpen, Zip, Render };
struct Result {
    bool accepted = true;
    bool changed = false;
    QString error;
};
bool normalizationPreservesGameplay(const SimaiParseResult& before, const SimaiParseResult& after);
Result prepare(ChartWorkspace& workspace, ChartWorkspaceFileService& files, Trigger trigger);
} // namespace miacode::automatic_chart
