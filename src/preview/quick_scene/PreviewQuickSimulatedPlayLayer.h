#pragma once

#include <QRectF>

class QSGNode;
class QQuickWindow;

namespace miacode::hand_order { class HandPlaySimulation; }

QSGNode* updatePreviewSimulatedPlayLayer(QSGNode* oldNode, const miacode::hand_order::HandPlaySimulation& simulation,
                                       double second, const QRectF& playfield, QQuickWindow* window);
