#pragma once

struct TimelineNoteMarker;

namespace miacode::slide_judge {

// Continuous chart seconds. This is a reference timing model, not a replay of
// sensor input or the Muri detector's relaxed completion/expiry policy.
struct Timing {
    bool valid = false;
    bool wifiReference = false;
    double headSecond = 0.0;
    double traceSecond = 0.0;
    double endSecond = 0.0;
    double judgementSecond = 0.0;
    double lastAreaDuration = 0.0;
    double perfectStartSecond = 0.0;
    double perfectEndSecond = 0.0;
};

Timing timingForMarker(const TimelineNoteMarker& marker);
// Head-to-judgement progress, including the wait before the guiding star moves.
double brightnessProgress(const Timing& timing, double chartSecond);

} // namespace miacode::slide_judge
