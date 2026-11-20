#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

// Pad entry time along a slide or wifi track, as a proportion of the track.
struct MuriPadTimeEntry {
    QString pad;
    double proportion = 0.0;
};

// Authoritative parser state in chart time. Same-time directives are folded in source order.
struct TimelineTimingPoint {
    double second = 0.0;
    double quarterBeat = 0.0;
    double bpm = 120.0;
    int meterNumerator = 4;
    int meterDenominator = 4;
    bool meterExplicit = false;
};

struct TimelineBeatMarker {
    double second = 0.0;
    bool major = false;
    // Constant-tempo comma interval, for the independent quarter-note aid grid.
    double quarterBeat = 0.0;
    double quarterBeatLength = 0.0;
    int subdivision = 4;
    double durationSeconds = 0.0;
    int sourceLine = 1;
    int sourceCol = 1;
};

enum class HandOrderHand {
    Unspecified,
    Left,
    Right,
    Either,
};

// Track movement progress excludes the wait before slide tracing. Stops are
// left-inclusive; the final stop also owns progress 1.0.
struct HandOrderTrackStop {
    double progress = 0.0;
    HandOrderHand hand = HandOrderHand::Unspecified;
};

// Split sectors follow the native outer lane indices 0 and 2, not screen left/right.
// Single requires one selected hand for the entire native Wifi.
enum class HandOrderWifiMode { Legacy, Split, Single };

struct TimelineNoteMarker {
    // Source column of this branch's path, separate from the token/head column.
    int slideTrackSourceCol = 0;
    double second = 0.0;
    // Tempo at this note's head, used for local 1/64-note timing comparisons.
    double tempoBpm = 120.0;
    // Simai {N} subdivision in effect when this note was emitted.
    int subdivision = 4;
    double endSecond = -1.0;
    double slideTraceSecond = -1.0;
    double availableSecond = -1.0;
    int parseOrder = -1;
    int eachGroupId = -1;
    int sourceLine = 1;
    int sourceCol = 1;
    // The annotation range is one-based and inclusive in the original line.
    int handOrderAnnotationCol = 0;
    int handOrderAnnotationEndCol = 0;
    // Source-order branch index for a '*' slide group; zero for other notes.
    int slideBranchIndex = 0;
    int slideBranchSourceCol = 0;
    int slideBranchSourceEndCol = 0;
    HandOrderHand handOrderHead = HandOrderHand::Unspecified;
    QVector<HandOrderTrackStop> handOrderTrackStops;
    HandOrderWifiMode handOrderWifiMode = HandOrderWifiMode::Legacy;
    // The two sides follow native wifi lane order (outer lanes 0 and 2).
    QVector<QVector<HandOrderTrackStop>> handOrderWifiSectorStops;
    int lane = 1;
    int endLane = 1;
    QString type;
    QString slideDisplayKey;
    QString slideTrackKey;
    QStringList slideSegmentKeys;
    // Judge-pad alternatives, not artwork samples. Populated by the parser.
    QVector<QVector<QStringList>> slideSegmentJudgeAreas;
    QVector<QVector<QStringList>> wifiLaneJudgeAreas;
    QVector<double> slideSegmentShootSeconds;
    QVector<double> slideSegmentDurations;
    QVector<QVector<MuriPadTimeEntry>> slideSegmentPadEnterTimes;
    QVector<double> slideSegmentCriticalProportions;
    QVector<QVector<QPointF>> slideSegmentPoints;
    QVector<QVector<double>> slideSegmentAngles;
    QVector<QVector<QPointF>> wifiLanePoints;
    QVector<QVector<double>> wifiLaneAngles;
    QVector<QVector<QVector<QPointF>>> slideTrackAreaPoints;
    QVector<QVector<QVector<double>>> slideTrackAreaRotations;
    QVector<QVector<double>> slideTrackAreaThresholds;
    QVector<QVector<QVector<double>>> slideTrackAreaCheckpoints;
    QVector<QVector<QVector<int>>> slideTrackAreaCutIndices;
    QVector<QVector<QPointF>> wifiTrackAreaPoints;
    QVector<QVector<double>> wifiTrackAreaRotations;
    QVector<QVector<int>> wifiTrackAreaImageIndices;
    QVector<double> wifiTrackAreaThresholds;
    QVector<QVector<double>> wifiTrackAreaCheckpoints;
    QVector<MuriPadTimeEntry> wifiPadEnterTimes;
    double wifiCriticalProportion = 1.0;
    double slideNativeTrackLength = 0.0;
    double slideRuntimeTrackLength = 0.0;
    QPointF touchPoint;
    QString touchPad;
    bool tapUsesStarMaterial = false;
    bool tapStarDouble = false;
    bool isEach = false;
    bool isBreak = false;
    bool isEx = false;
    bool isFirework = false;
    // Mine note (simai `m` suffix). A mine is an "avoid" note: MiaCode is an
    // autoplay simulator, so a mine is always perfectly dodged for judge
    // effects and Muri analysis, while preview/export still play its type SFX.
    // `isMine` covers
    // tap/hold/touch/touch_hold; slides use trackMine below.
    bool isMine = false;
    bool onSlide = false;
    bool slideHead = false;
    bool tailOnSlideHead = false;
    bool slideEach = false;
    bool sameHeadSlide = false;
    bool beforeSlide = false;
    bool afterSlide = false;
    bool headEach = false;
    bool headBreak = false;
    bool headEx = false;
    bool slideHeadUsesTapMaterial = false;
    bool trackBreak = false;
    // Slide mine components are independent: `8m-4` sets `headMine`, while
    // `8-4m[...]` sets `trackMine`; both modifiers may be present together.
    bool trackMine = false;
    bool headMine = false;
    bool hasHeadStar = true;
    bool headlessImmediate = false;
    // HS (hi-speed) multiplier in effect at the moment this note was emitted.
    // Frozen at emission — subsequent <HS*N> directives only affect notes
    // emitted after, so hold bodies retain their start-time HS even if a
    // later directive crosses the body. Effective fall speed = userFlowSpeed
    // * hsMultiplier. Default 1.0 = current/legacy behavior.
    double hsMultiplier = 1.0;
};
