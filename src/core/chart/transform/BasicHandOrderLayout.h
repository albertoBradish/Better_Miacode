#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderSweepThreshold.h"
#include <QPair>
#include <QSet>

namespace miacode::hand_order { struct PatternSuggestion; }

namespace miacode::chart_transform {

// Points use native slide coordinates centred at (0, 0).
HandOrderHand basicLayoutHandForPoint(const QPointF& point);
HandOrderHand basicLayoutHeadHand(const TimelineNoteMarker& marker);
QPointF basicLayoutTrackStartPoint(const TimelineNoteMarker& marker);
HandOrderHand basicLayoutTrackHand(const TimelineNoteMarker& marker);
HandOrderHand basicLayoutWifiSectorHand(const TimelineNoteMarker& marker, int sector);

struct BasicHandOrderLayoutPlan {
    struct PhaseLink { int first, second; bool changesHand; };
    HandOrderEditPlan edit;
    int preserved = 0;
    int unresolvedColored = 0;
    // Parse-order pairs for statically recognized cross-token one-stroke traces.
    QVector<QPair<int, int>> oneStrokeLinks;
    // A moving hand follows these tap edges through repeated anchored Eachs.
    QVector<QPair<int, int>> windingLinks;
    // Short key sweeps that must retain their moving hand.
    QVector<QPair<int, int>> sweepLinks;
    // Regular small-axis taps prefer alternating hands.
    QVector<QPair<int, int>> interactionLinks;
    QVector<PhaseLink> phaseLinks;
    // A delayed same-key return after an Each is a new strike by that hand.
    QVector<QPair<int, int>> reboundLinks;
    // Local eighth-note returns; do not merge distant pairs into one search unit.
    QVector<QPair<int, int>> relaxedReboundLinks;
    // Two adjacent repetitions of the same short sweep alternate hands.
    QVector<QPair<int, int>> alternatingSweepLinks;
    // A slide trace uses the opposite hand while its head lane is tapped repeatedly.
    QSet<int> tapLaneReservedTracks;
    // Single/coincident traces whose head belongs to a key-sweep gesture.
    QSet<int> keySweepSplitTracks;
    QSet<int> keySweepKeepTracks;
    QSet<int> windingEachHeads;
    QSet<int> configuredHeads;
    QSet<int> gestureHeads, gestureTracks;
};

// An empty selection means the complete difficulty text. Existing suffixes,
// including malformed ones, remain untouched. No execution feasibility is inferred.
BasicHandOrderLayoutPlan basicHandOrderLayoutInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {}, double returnSpeed = chart::handOrderDefaultReturnSpeed,
    int sweepThreshold = chart::handOrderDefaultSweepThreshold,
    bool colorUnresolved = false,
    const QVector<hand_order::PatternSuggestion>* recognizedPatterns = nullptr);

} // namespace miacode::chart_transform
