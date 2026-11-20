#pragma once

#include "core/analysis/hand_order/BasicHandOrder.h"

namespace miacode::hand_order {

struct PatternOptimizationParameters {
    // Annotation provenance is not stored in Simai. This explicit option locks
    // every existing label; labels outside the requested range are always locked.
    bool preserveExisting = false;
    int maximumCandidates = 4096;
    int maximumMarkers = 100000;
    PatternRecognitionMode recognition = PatternRecognitionMode::ExistingRules;
};

// Registry-backed structural shortcuts and alternatives. No full-chart baseline
// search is required; definite defaults remain reviewable annotation edits.
BasicResult optimizeHandOrderPatterns(const QString& text, const SimaiParseResult& parsed,
    int begin, int end, const BasicParameters& parameters = {},
    const PatternOptimizationParameters& optimization = {},
    const miacode::simai::SimaiTimingMetadata& metadata = {},
    const HandOrderProgressCallback& progress = {});

} // namespace miacode::hand_order
