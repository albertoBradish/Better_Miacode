#include "export/session/ExportQuickWatermarkOverlay.h"

#include "export/video_export/VideoWatermarkSettings.h"

#include <QFontMetricsF>
#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QPainterPath>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTransform>
#include <QtMath>

#include <memory>
#include <utility>

namespace miacode::video_export {
namespace {

constexpr qreal kWatermarkAngle = -30.0;

QImage renderWatermarkLabel(const QString& text, int pixelSize, qreal maxWidth)
{
    QFont font;
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font);
    const QString label = metrics.elidedText(text, Qt::ElideRight, maxWidth);
    if (label.isEmpty()) return {};

    const qreal strokeWidth = qMax(1.0, pixelSize / 16.0);
    const qreal padding = strokeWidth + 2.0;
    const QRectF bounds = metrics.boundingRect(label).adjusted(-padding, -padding, padding, padding);
    QImage image(QSize(qCeil(bounds.width()), qCeil(bounds.height())), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(font);
    const QPointF baseline = -bounds.topLeft();
    QPainterPath outline;
    outline.addText(baseline, font, label);
    painter.strokePath(outline, QPen(QColor(0, 0, 0, 85), strokeWidth));
    painter.setPen(QColor(255, 255, 255, 80));
    painter.drawText(baseline, label);
    return image;
}

class WatermarkPatternNode final : public QSGTransformNode
{
public:
    explicit WatermarkPatternNode(std::unique_ptr<QSGTexture> texture)
        : texture_(std::move(texture))
    {
    }

    ~WatermarkPatternNode() override
    {
        // The pattern shares one texture across its label nodes.
        while (QSGNode* child = firstChild()) delete child;
    }

    void addLabel(const QRectF& rect)
    {
        auto* node = new QSGSimpleTextureNode;
        node->setTexture(texture_.get());
        node->setFiltering(QSGTexture::Linear);
        node->setRect(rect);
        appendChildNode(node);
    }

private:
    std::unique_ptr<QSGTexture> texture_;
};

class WatermarkOverlay final : public QQuickItem
{
public:
    WatermarkOverlay(QQuickItem* parent, QString text, int fontSize)
        : QQuickItem(parent), text_(std::move(text)), fontSize_(fontSize)
    {
        setFlag(ItemHasContents);
        setZ(3.0);
        setSize(parent->size());
        connect(parent, &QQuickItem::widthChanged, this, [this, parent] { setWidth(parent->width()); });
        connect(parent, &QQuickItem::heightChanged, this, [this, parent] { setHeight(parent->height()); });
    }

protected:
    void geometryChange(const QRectF& geometry, const QRectF& previous) override
    {
        QQuickItem::geometryChange(geometry, previous);
        if (geometry.size() != previous.size()) update();
    }

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override
    {
        delete oldNode;
        const qreal shortSide = qMin(width(), height());
        if (shortSide <= 0.0) return nullptr;

        const int pixelSize = qMax(1, qRound(fontSize_ * shortSide / 1080.0));
        const QImage label = renderWatermarkLabel(text_, pixelSize, width() * 0.82);
        if (label.isNull()) return nullptr;

        std::unique_ptr<QSGTexture> texture(window()->createTextureFromImage(label));
        if (!texture) return nullptr;
        auto* pattern = new WatermarkPatternNode(std::move(texture));
        QMatrix4x4 transform;
        transform.translate(width() / 2.0, height() / 2.0);
        transform.rotate(kWatermarkAngle, 0.0, 0.0, 1.0);
        pattern->setMatrix(transform);

        // Lay out staggered rows in the rotated coordinate space.
        const QRectF view(-width() / 2.0, -height() / 2.0, width(), height());
        const QRectF bounds = QTransform().rotate(-kWatermarkAngle).mapRect(view);
        const qreal spacingBase = qMax(shortSide, qMax(width(), height()) / 4.0);
        const qreal columnSpacing = qMax(label.width() + 2.0 * pixelSize, spacingBase * 0.65);
        const qreal rowSpacing = qMax(label.height() + 2.0 * pixelSize, spacingBase * 0.27);
        const int firstRow = qFloor(bounds.top() / rowSpacing);
        const int lastRow = qCeil(bounds.bottom() / rowSpacing);
        for (int row = firstRow; row <= lastRow; ++row) {
            const qreal stagger = row % 2 == 0 ? 0.0 : columnSpacing / 2.0;
            const int firstColumn = qFloor((bounds.left() - stagger) / columnSpacing);
            const int lastColumn = qCeil((bounds.right() - stagger) / columnSpacing);
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const QPointF center(column * columnSpacing + stagger, row * rowSpacing);
                const QRectF rect(center - QPointF(label.width() / 2.0, label.height() / 2.0), label.size());
                if (bounds.intersects(rect)) pattern->addLabel(rect);
            }
        }
        return pattern;
    }

private:
    QString text_;
    int fontSize_;
};

}  // namespace

void addVideoWatermarkOverlay(QQuickItem* parent, const VideoWatermarkSettings& settings)
{
    if (!settings.enabled) return;
    QString text = settings.text.trimmed();
    if (text.isEmpty()) return;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    const int fontSize = qBound(VideoWatermarkSettings::kMinimumFontSize, settings.fontSize,
                               VideoWatermarkSettings::kMaximumFontSize);
    new WatermarkOverlay(parent, std::move(text), fontSize);
}

}  // namespace miacode::video_export
