#pragma once

#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/MuriRenderOptions.h"
#include <QSet>

namespace miacode::hand_order {

QString handOrderPatternFingerprint(const PatternSuggestion& suggestion);

BasicResult optimizeHandOrder(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const BasicParameters& parameters, const simai::SimaiTimingMetadata& metadata,
    const MuriRenderOptions& playOptions = {}, const QSet<QString>& ignoredPatterns = {},
    const HandOrderProgressCallback& progress = {});

} // namespace miacode::hand_order
