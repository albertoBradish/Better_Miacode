#pragma once

#include "core/scene/PreviewCircleDescriptor.h"

class QSGNode;

// Measure guides are full circles. Their thin edges use analytic pixel coverage
// independently of the polygon-based diagnostic circles elsewhere in the scene.
QSGNode* buildPreviewMeasureRingNodeTree(
    QSGNode* oldNode,
    const miacode::preview::scene::PreviewCircleDescriptors& circles
);
