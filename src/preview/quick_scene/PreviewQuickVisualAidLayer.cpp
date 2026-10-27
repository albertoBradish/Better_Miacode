#include "preview/quick_scene/PreviewQuickVisualAidLayer.h"
#include "preview/quick_scene/PreviewQuickCircleNodes.h"
#include "core/scene/PreviewVisualAidLayerState.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "common/DebugLog.h"

#include <QSGGeometryNode>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>

QSGNode* PreviewQuickVisualAidLayer::updateNode(QSGNode* oldNode,
    const miacode::preview::scene::PreviewFrameState& state,
    const miacode::preview::scene::PreviewPreparedSceneCache* cache,
    const miacode::preview::scene::PreviewLayerWindowCursor* cursor, const QSize& renderSize, bool measuresOnly)
{
    using namespace miacode::preview::scene;
    if (oldNode == nullptr) {
        oldNode = new QSGNode();
        oldNode->appendChildNode(new QSGNode());
        auto* lines = new QSGGeometryNode();
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        lines->setGeometry(geometry);
        lines->setFlag(QSGNode::OwnsGeometry);
        lines->setMaterial(new QSGVertexColorMaterial());
        lines->setFlag(QSGNode::OwnsMaterial);
        oldNode->appendChildNode(lines);
    }
    const PreviewActiveMarkerView markers = cache != nullptr && cursor != nullptr
        ? PreviewActiveMarkerView(state.noteMarkers, cache->slideLikeLayer(), *cursor)
        : PreviewActiveMarkerView(state.noteMarkers);
    const auto layer = buildPreviewVisualAidLayerState(state, markers,
        playfieldRectForStage(stageRectForSize(renderSize), state.render.layoutSquareScale), measuresOnly, !measuresOnly);
    QSGNode* previous = oldNode->firstChild();
    QSGNode* circles = buildPreviewCircleNodeTree(previous, layer.circles);
    if (circles != previous) {
        oldNode->removeChildNode(previous);
        delete previous;
        oldNode->prependChildNode(circles != nullptr ? circles : new QSGNode());
    }
    auto* lines = static_cast<QSGGeometryNode*>(oldNode->lastChild());
    auto* geometry = lines->geometry();
    geometry->allocate(layer.lines.size() * 6);
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int v = 0;
    for (const auto& line : layer.lines) {
        const QPointF delta = line.line.p2() - line.line.p1();
        const qreal length = line.line.length();
        const QPointF normal(-delta.y() / length * line.width * 0.5, delta.x() / length * line.width * 0.5);
        const QPointF a = line.line.p1() + normal, b = line.line.p1() - normal;
        const QPointF c = line.line.p2() + normal, d = line.line.p2() - normal;
        const int alpha = line.color.alpha();
        for (const auto& point : {a, b, c, c, b, d})
            vertices[v++].set(point.x(), point.y(), line.color.red() * alpha / 255,
                line.color.green() * alpha / 255, line.color.blue() * alpha / 255, alpha);
    }
    geometry->markVertexDataDirty();
    lines->markDirty(QSGNode::DirtyGeometry);
    if (layer.missingJudgeDataCount > 0 && lastDiagnosedRevision_ != state.sceneContentRevision) {
        lastDiagnosedRevision_ = state.sceneContentRevision;
        miacode::debug_log::appendLine(miacode::debug_log::Channel::Runtime,
            QStringLiteral("preview/visual_aids"), QStringLiteral("action=skip_missing_judge_data count=%1")
                .arg(layer.missingJudgeDataCount));
    }
    return oldNode;
}
