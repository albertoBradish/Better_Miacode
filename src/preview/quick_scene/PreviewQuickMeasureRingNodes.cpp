#include "preview/quick_scene/PreviewQuickMeasureRingNodes.h"

#include <QMatrix4x4>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QVector>

#include <array>
#include <cstring>
#include <functional>

namespace {

using miacode::preview::scene::PreviewCircleDescriptor;

class MeasureRingMaterial;

class MeasureRingShader final : public QSGMaterialShader
{
public:
    MeasureRingShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/src/preview/quick_scene/shaders/PreviewMeasureRing.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/src/preview/quick_scene/shaders/PreviewMeasureRing.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override;
};

class MeasureRingMaterial final : public QSGMaterial
{
public:
    MeasureRingMaterial()
    {
        setFlag(Blending);
        setFlag(RequiresFullMatrix);
        setFlag(NoBatching);
    }

    QSGMaterialType* type() const override
    {
        static QSGMaterialType materialType;
        return &materialType;
    }

    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override
    {
        return new MeasureRingShader();
    }

    int compare(const QSGMaterial* other) const override
    {
        if (this == other) return 0;
        return std::less<const QSGMaterial*>{}(this, other) ? -1 : 1;
    }

    float radius = 0.0f;
    float halfWidth = 0.0f;
    std::array<float, 4> color{};
};

bool MeasureRingShader::updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial)
{
    auto* material = static_cast<MeasureRingMaterial*>(newMaterial);
    QByteArray* buffer = state.uniformData();
    // std140: matrix 0, opacity 64, radius 68, halfWidth 72, color 80.
    Q_ASSERT(buffer->size() >= 96);
    if (state.isMatrixDirty() || oldMaterial == nullptr)
        std::memcpy(buffer->data(), state.combinedMatrix().constData(), 64);
    if (state.isOpacityDirty() || oldMaterial == nullptr) {
        const float opacity = state.opacity();
        std::memcpy(buffer->data() + 64, &opacity, sizeof(opacity));
    }
    std::memcpy(buffer->data() + 68, &material->radius, sizeof(float));
    std::memcpy(buffer->data() + 72, &material->halfWidth, sizeof(float));
    std::memcpy(buffer->data() + 80, material->color.data(), sizeof(material->color));
    return true;
}

class MeasureRingNode final : public QSGGeometryNode
{
public:
    MeasureRingNode()
    {
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        setGeometry(geometry);
        setFlag(OwnsGeometry);
        setMaterial(new MeasureRingMaterial());
        setFlag(OwnsMaterial);
    }

    void updateRing(const PreviewCircleDescriptor& circle)
    {
        auto* ringMaterial = static_cast<MeasureRingMaterial*>(material());
        ringMaterial->radius = static_cast<float>(circle.radiusX);
        ringMaterial->halfWidth = static_cast<float>(circle.strokeWidth * 0.5);
        const float alpha = static_cast<float>(circle.strokeColor.alphaF());
        ringMaterial->color = {static_cast<float>(circle.strokeColor.redF()) * alpha,
            static_cast<float>(circle.strokeColor.greenF()) * alpha,
            static_cast<float>(circle.strokeColor.blueF()) * alpha, alpha};

        // Leave room for both antialiased edges; coordinates remain fractional
        // as the ring moves. Local coordinates are unaffected by scene batching.
        const float extent = ringMaterial->radius + ringMaterial->halfWidth + 2.0f;
        const float x = static_cast<float>(circle.center.x());
        const float y = static_cast<float>(circle.center.y());
        auto* vertices = geometry()->vertexDataAsTexturedPoint2D();
        vertices[0].set(x - extent, y - extent, -extent, -extent);
        vertices[1].set(x + extent, y - extent, extent, -extent);
        vertices[2].set(x - extent, y + extent, -extent, extent);
        vertices[3].set(x + extent, y + extent, extent, extent);
        geometry()->markVertexDataDirty();
        markDirty(DirtyGeometry | DirtyMaterial);
    }
};

bool isRenderableRing(const PreviewCircleDescriptor& circle)
{
    return qIsFinite(circle.radiusX) && circle.radiusX > 0.001
        && qFuzzyCompare(circle.radiusX, circle.radiusY)
        && qIsFinite(circle.strokeWidth) && circle.strokeWidth > 0.001
        && circle.strokeColor.alpha() > 0;
}

} // namespace

QSGNode* buildPreviewMeasureRingNodeTree(QSGNode* oldNode,
    const miacode::preview::scene::PreviewCircleDescriptors& circles)
{
    if (circles.isEmpty()) return nullptr;
    auto* root = oldNode != nullptr ? oldNode : new QSGNode();
    QVector<QSGNode*> children;
    for (auto* child = root->firstChild(); child != nullptr; child = child->nextSibling())
        children.append(child);
    int index = 0;
    for (const auto& circle : circles) {
        if (!isRenderableRing(circle)) continue;
        auto* existing = index < children.size() ? children.at(index) : nullptr;
        auto* ring = dynamic_cast<MeasureRingNode*>(existing);
        if (ring == nullptr) {
            ring = new MeasureRingNode();
            if (existing != nullptr) {
                root->insertChildNodeBefore(ring, existing);
                root->removeChildNode(existing);
                delete existing;
                children[index] = ring;
            } else {
                root->appendChildNode(ring);
                children.append(ring);
            }
        }
        ring->updateRing(circle);
        ++index;
    }
    for (int i = children.size() - 1; i >= index; --i) {
        root->removeChildNode(children[i]);
        delete children[i];
    }
    if (index == 0) {
        if (root != oldNode) delete root;
        return nullptr;
    }
    return root;
}
