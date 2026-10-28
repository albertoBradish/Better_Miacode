#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector>

#include "core/scene/PreviewAnimatedSpriteHelpers.h"

class QImage;

namespace miacode::preview::scene {

struct PreviewSpriteDescriptor {
    const QImage* image = nullptr;
    QPointF center;
    qreal width = 0.0;
    qreal height = 0.0;
    qreal rotationDegrees = 0.0;
    qreal opacity = 1.0;
    // RGB modulation only; alpha and the source skin's transparent contour
    // remain unchanged. Reveal distance is interpolated per pixel in QSG.
    qreal brightness = 1.0;
    qreal unrevealedBrightness = 1.0;
    qreal revealOffset = 0.0;
    QPointF revealDirection;
    QRectF sourceRect;
    PreviewAnimatedSpriteEffect effect = PreviewAnimatedSpriteEffect::None;
    bool cacheable = true;
};

using PreviewSpriteDescriptors = QVector<PreviewSpriteDescriptor>;

}  // namespace miacode::preview::scene
