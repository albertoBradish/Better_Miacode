#pragma once

#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/MuriRenderOptions.h"

namespace miacode::hand_order {

BasicResult reviewHandPlay(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const MuriRenderOptions& options, const HandOrderProgressCallback& progress = {}, double timelineOffset = 0.0);

} // namespace miacode::hand_order
