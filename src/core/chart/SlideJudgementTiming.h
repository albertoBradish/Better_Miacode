#pragma once

#include <QVector>
#include <array>

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
    double brightnessCompleteSecond = 0.0;
    double brightnessStartSecond = 0.0;
    bool usesLaunchBeat = false;
};

// Only playable note heads at the same A-pad and chart instant count as a
// launch beat. Hold tails, held bodies, inner touches and mines do not.
class LaunchBeatIndex {
public:
    explicit LaunchBeatIndex(const QVector<TimelineNoteMarker>& markers);
    double startSecondFor(const TimelineNoteMarker& marker) const;
private:
    std::array<QVector<double>,8> moments_;
};

Timing timingForMarker(const TimelineNoteMarker& marker, const LaunchBeatIndex* launchBeats = nullptr);
// Selected-start-to-earliest-Perfect progress. If Perfect has already begun
// at the selected start (star head or matching launch beat), light immediately.
double brightnessProgress(const Timing& timing, double chartSecond);

} // namespace miacode::slide_judge
