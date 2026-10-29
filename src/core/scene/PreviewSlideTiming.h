#pragma once

#include "core/chart/SlideJudgementTiming.h"
#include "core/scene/PreviewSpriteDescriptor.h"

struct TimelineNoteMarker;

namespace miacode::preview::scene {

struct PreviewSlideArrowDistance {
    double distance = 0.0;
    QPointF tangent;
};

struct PreviewSlideTimingGeometry {
    miacode::slide_judge::Timing timing;
    double totalLength = 0.0;
    QVector<QVector<QVector<PreviewSlideArrowDistance>>> slideArrows;
    QVector<QVector<PreviewSlideArrowDistance>> wifiArrows;
    bool isValid() const { return timing.valid && totalLength > 0.0; }
};

PreviewSlideTimingGeometry buildPreviewSlideTimingGeometry(const TimelineNoteMarker& marker,
    const miacode::slide_judge::LaunchBeatIndex* launchBeats = nullptr, bool earlyPerfectFromHead = false);
void applyPreviewSlideTimingBrightness(PreviewSpriteDescriptor* sprite,
    const PreviewSlideTimingGeometry& geometry, const PreviewSlideArrowDistance& arrow,
    double chartSecond, double canvasScale);

} // namespace miacode::preview::scene
