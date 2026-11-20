#pragma once

#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/chart/model/TimelineData.h"

namespace miacode::hand_order {

// Community-reference computation profile, not an official cabinet specification.
// Slide queues: local MuriSlideWifiJudge and MaiMuriDX 0ebab7ff6853a159ef6af76d9e3c43b23157bb5e.
// Hold body windows/debounce follow that commit's development notes; the Python
// detector itself only models hold heads. Button and screen remain independent.
// This profile dispatches a tick's button DOWN batch before screen DOWNs, so
// screen-only Touch cannot lose its event to a Tap already served by a button.
inline constexpr int fullReplayProfileRevision = 1;
struct FullReplayProfile {
    static constexpr double ticksPerSecond = 180.0;
    static constexpr double tapCriticalSeconds = 3.0 / ticksPerSecond;
    static constexpr double tapAvailableSeconds = 27.0 / ticksPerSecond;
    static constexpr double touchCriticalSeconds = 27.0 / ticksPerSecond;
    static constexpr double touchAvailableSeconds = 27.0 / ticksPerSecond;
    static constexpr double slideCriticalSeconds = 42.0 / ticksPerSecond;
    static constexpr double slideDeltaShiftSeconds = 9.0 / ticksPerSecond;
    static constexpr double slideLeadingSeconds = 15.0 / ticksPerSecond;
    static constexpr double slideAvailableSeconds = 108.0 / ticksPerSecond;
    static constexpr double holdHeadIgnoreSeconds = 6.0 / 60.0;
    static constexpr double touchHoldHeadIgnoreSeconds = 15.0 / 60.0;
    static constexpr double holdTailIgnoreSeconds = 12.0 / 60.0;
    static constexpr double holdReleaseDebounceSeconds = 3.0 / 60.0;
    static constexpr double maximumPalmRadius = 50.0 / 240.0;
    static constexpr qint64 maximumEventEntries = 262144;
    static constexpr bool buttonDownBeforeScreen = true;
};

struct ChartInputAction {
    ContactAction contact;
    bool button = false;
    int lane = 0; // Explicit button channel 1..8; ignored for screen contacts.
    int sourceParseOrder = -1; // Optional attribution; never changes judgement.
};

struct FullReplayOptions {
    int profileRevision = fullReplayProfileRevision;
    bool wifiNeedC = false; // DX reference default; true explicitly selects the older C-release rule.
    bool requireHighQuality = true;
    // Finite horizons return unfinished notes as pending, never as successful.
    double throughSecond = std::numeric_limits<double>::infinity();
    // Counts distinct tick nodes plus their physical-input transitions. May be
    // lowered by the caller; raising the audited profile limit is not supported.
    qint64 maxEventEntries = FullReplayProfile::maximumEventEntries;
};

struct FullReplayArea {
    bool hit = false, skipped = false;
    double second = 0;
    QString pad;
    QVector<BasicCause> causes;
};

struct FullReplayJudgement {
    int markerIndex = -1, parseOrder = -1;
    bool supported = true, judged = false, completed = false, highQuality = false;
    bool meetsTarget = false, sideEffect = false;
    bool headRequired = false, headJudged = false, headCompleted = false, headHighQuality = false;
    bool trackCompleted = false, trackHighQuality = false;
    int handMask = 0;
    double second = 0, margin = 0;
    double hitDelta = 0, headSecond = 0, headMargin = 0, headHitDelta = 0;
    // Effective body duration excludes the profile's head/tail ignore windows.
    // heldSeconds credits short OFF runs; rawHeldSeconds records actual ON time.
    double heldSeconds = 0, requiredHoldSeconds = 0, heldRatio = 0;
    double rawHeldSeconds = 0, releasedSeconds = 0;
    QString kind, quality, reason, inputChannel;
    QVector<BasicCause> causes;
    QVector<BasicCause> headCauses;
    QVector<int> sourceParseOrders, headSourceParseOrders;
    QVector<FullReplayArea> areas;
    QVector<QVector<FullReplayArea>> wifiAreas;
    QVector<int> segmentStartAreas, segmentEndAreas, wifiProgress;
    QVector<double> segmentCompletedSeconds;
    double wifiPadCSecond = std::numeric_limits<double>::quiet_NaN();
    // For completed critical Hold/Touch Hold only: each bit identifies a hand
    // sufficient for the actual consumed head and the whole effective body.
    // Actual contributors remain in handMask/causes, including redundant ones.
    int singleHandCompletionMask = 0;
};

struct FullReplay {
    QVector<FullReplayJudgement> judgements; // Exactly one entry per input marker.
    QVector<QVector<int>> touchGroups; // Marker indices.
    bool exhausted = false, valid = true, allCompleted = false, meetsTarget = false;
    QString error;
    int profileRevision = fullReplayProfileRevision;
    qint64 workUsed = 0;
};

struct BasicReplayEvidence {
    QVector<ChartInputAction> inputs;
    FullReplay replay;
};

// Replays only caller-supplied physical inputs. It never creates autoplay inputs
// from markers or uses marker.onSlide as evidence. Contact positions use a field
// radius of 1 (native 240) about the origin, matching ContactAction.
// ContactAction.radius is the sole screen-radius authority. Simultaneous screen
// actions assigned to one hand must describe the same physical palm center.
// The caller validates button reach and any button+screen sharing template;
// this judge validates channel state, not human reach or template feasibility.
FullReplay replayHandOrderChart(const QVector<TimelineNoteMarker>& markers,
    const QVector<ChartInputAction>& actions, const FullReplayOptions& options = {},
    qint64* workRemaining = nullptr);

} // namespace miacode::hand_order
