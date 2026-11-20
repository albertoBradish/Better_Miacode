#pragma once

#include "core/chart/model/TimelineData.h"

namespace miacode::hand_order {

struct InteractionStep {
    const TimelineNoteMarker* note = nullptr;
    bool opposite = false;
};

using InteractionSteps = QVector<InteractionStep>;

struct InteractionPhases {
    InteractionSteps steps;
    bool allowLeft = true;
    bool allowRight = true;
    HandOrderHand preferred = HandOrderHand::Unspecified;
};

// An ineligible or simultaneous moment is a null entry in the ordered stream.
QVector<InteractionSteps> findTriangleInteractions(const QVector<const TimelineNoteMarker*>& stream);
QVector<InteractionSteps> findTrillInteractions(const QVector<const TimelineNoteMarker*>& stream);

HandOrderHand interactionHand(HandOrderHand first, bool opposite);

QVector<const TimelineNoteMarker*> sustainedInteractionContext(const QVector<TimelineNoteMarker>& chart);

// Sustained contacts outside a group retain their current responsibilities.
bool interactionPhaseFits(const InteractionSteps& steps, HandOrderHand first,
    const QVector<const TimelineNoteMarker*>& sustainedContext);

// Validate the resolved phase against the assembled chart, including contacts
// after a Hold that starts inside this group.
bool interactionPhaseFitsCombined(const InteractionSteps& steps, HandOrderHand first,
    const QVector<TimelineNoteMarker>& chart, const QVector<const TimelineNoteMarker*>& sustainedContext);

// Groups are disjoint and ordered in time. Context contains the chart's current
// assignments, including any separate pattern defaults already selected.
QVector<HandOrderHand> reviewInteractionPhases(const QVector<InteractionPhases>& groups,
    const QVector<TimelineNoteMarker>& context, double returnSpeed);

} // namespace miacode::hand_order
