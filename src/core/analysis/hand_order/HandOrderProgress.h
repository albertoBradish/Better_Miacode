#pragma once

#include <functional>

namespace miacode::hand_order {
// Stage-local measured work. A new stage/attempt/candidate starts a new range;
// totals are actual decisions/windows, never the optional search work budget.
struct HandOrderProgress {
    enum Phase { Preparing, Windows, Decisions, Replay, Finished, Normalizing, Patterns, ConflictReview } phase = Preparing;
    int completed = 0, total = 0;
    int window = 0, windows = 0, attempt = 0;
    // -1: automatic Sticking, 0: optimization baseline, >0: candidate audit.
    int candidate = -1;
    // Fixed-assignment scans in a combined job have their own row.
    enum Pass { None, ContextSearch, VerifyAssignments } pass = None;
};
using HandOrderProgressCallback = std::function<void(const HandOrderProgress&)>;
} // namespace miacode::hand_order
