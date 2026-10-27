#pragma once

#include "core/scene/PreviewActiveMarkerView.h"
#include "core/scene/PreviewCircleDescriptor.h"
#include "core/scene/PreviewFrameState.h"
#include <QLineF>

namespace miacode::preview::scene {
struct PreviewVisualAidLine {
    QLineF line;
    QColor color;
    qreal width = 1.0;
    int markerIndex = -1;
    int segmentIndex = 0;
    int laneIndex = 0;
    int areaIndex = 0;
};
struct PreviewVisualAidLayerState {
    PreviewCircleDescriptors circles;
    QVector<PreviewVisualAidLine> lines;
    int missingJudgeDataCount = 0;
};

PreviewVisualAidLayerState buildPreviewVisualAidLayerState(const PreviewFrameState& state,
    const PreviewActiveMarkerView& markers, const QRectF& playfieldRect,
    bool includeMeasures = true, bool includeSkeleton = true);
}
