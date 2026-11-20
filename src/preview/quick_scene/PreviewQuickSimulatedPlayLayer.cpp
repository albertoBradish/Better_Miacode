#include "preview/quick_scene/PreviewQuickSimulatedPlayLayer.h"

#include "core/analysis/hand_order/HandPlaySimulation.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneMath.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace {
using namespace miacode::hand_order;

const QColor leftColor(78, 179, 255);
const QColor rightColor(255, 104, 134);
const QColor unknownColor(232, 232, 232);
const QColor conflictColor(255, 200, 58);
const QColor liftColor(117, 233, 219);
const QColor shadow(11, 20, 34, 216);

QColor faded(QColor color, double alpha)
{
    color.setAlphaF(color.alphaF() * qBound(0.0, alpha, 1.0));
    return color;
}

class PlayGeometry
{
public:
    explicit PlayGeometry(const QRectF& playfield)
        : center_(playfield.center()), scale_(playfield.width() * playFieldRadius / miacode::preview::scene::kLogicalCanvasSize)
    {
        vertices.reserve(4096);
    }

    void disk(const QPointF& center, double radius, const QColor& color)
    {
        constexpr int segments = 24;
        QPointF prior = center + QPointF(radius, 0);
        for (int i = 1; i <= segments; ++i) {
            const QPointF next = center + polar(radius, i, segments);
            triangle(center, prior, next, color);
            prior = next;
        }
    }

    void ring(const QPointF& center, double radius, double width, const QColor& color, bool dashed = false)
    {
        constexpr int segments = 32;
        const double inner = qMax(0.0, radius - width);
        for (int i = 0; i < segments; ++i) {
            if (dashed && (i / 2) % 2) continue;
            const QPointF a = center + polar(radius, i, segments);
            const QPointF b = center + polar(radius, i + 1, segments);
            const QPointF c = center + polar(inner, i, segments);
            const QPointF d = center + polar(inner, i + 1, segments);
            triangle(a, b, c, color);
            triangle(c, b, d, color);
        }
    }

    void stroke(const QPointF& from, const QPointF& to, double radius, const QColor& color)
    {
        const auto delta = to - from;
        const double length = std::hypot(delta.x(), delta.y());
        if (length < 1e-6) return;
        const QPointF normal(-delta.y() * radius / length, delta.x() * radius / length);
        triangle(from + normal, from - normal, to + normal, color);
        triangle(to + normal, from - normal, to - normal, color);
    }

    void arm(const PlayArmPose& arm, const PlayHandMotion& hand, QColor color, int index)
    {
        const QPointF shoulder = arm.shoulder.toVector2D().toPointF();
        const QPointF elbow = arm.elbow.toVector2D().toPointF();
        const QPointF wrist = arm.wrist.toVector2D().toPointF();
        const QPointF palm = arm.palm.toVector2D().toPointF();
        const double emphasis = hand.touching || std::isfinite(hand.targetSecond) ? 1.0 : 0.65;
        const QColor sleeve = faded(color.darker(155), 0.59 * emphasis);
        const QColor forearm = faded(color, (arm.lifted ? 0.76 : 0.55) * emphasis);
        stroke(shoulder, elbow, playUpperArmRadius + 0.008, faded(shadow, 0.76));
        stroke(elbow, wrist, playForearmRadius + 0.008, faded(shadow, 0.76));
        disk(elbow, playUpperArmRadius + 0.008, faded(shadow, 0.76));
        stroke(shoulder, elbow, playUpperArmRadius, sleeve);
        disk(elbow, playUpperArmRadius, sleeve);
        stroke(elbow, wrist, playForearmRadius, forearm);
        disk(wrist, playForearmRadius, forearm);
        stroke(wrist, palm, arm.palmRadius * 0.76, forearm);

        const auto delta = palm - wrist;
        const double length = std::hypot(delta.x(), delta.y());
        const QPointF along = length > 1e-6 ? delta / length : QPointF(0, -1);
        const QPointF inward = (index == 0 ? 1 : -1) * QPointF(-along.y(), along.x());
        const double radius = arm.palmRadius;
        const auto position = [&](double across, double forward) { return palm + radius * (inward * across + along * forward); };
        // The contact is at the fingertips. Keep the body of the glove behind
        // the cue, so even a small preview shows which note is being touched.
        const std::array<QPointF, 9> outline{position(-0.43, -1.30), position(-0.82, -0.82),
            position(-0.74, -0.28), position(-0.51, 0.24), position(0.20, 0.32), position(0.48, 0.08),
            position(0.86, -0.03), position(0.93, -0.38), position(0.48, -1.22)};
        const QColor glove = faded(color, hand.touching ? 0.91 : 0.75);
        for (int i = 0; i < int(outline.size()); ++i) {
            const auto& first = outline[i];
            const auto& last = outline[(i + 1) % outline.size()];
            triangle(position(0.0, -0.64), first, last, glove);
            stroke(first, last, 0.0045, shadow);
        }
        for (double finger : {-0.42, -0.14, 0.14})
            stroke(position(finger, 0.20), position(finger, -0.24), 0.0035, faded(shadow, 0.56));
        disk(palm, radius * 0.17, faded(Qt::white, hand.touching ? 0.94 : 0.58));
        if (arm.lifted) ring(palm, arm.palmRadius + 0.027, 0.012, liftColor, true);
        if (hand.blocked) ring(palm, arm.palmRadius + 0.034, 0.015, conflictColor);
    }

    void triangle(const QPointF& a, const QPointF& b, const QPointF& c, const QColor& color)
    {
        const int alpha = color.alpha();
        for (const auto& point : {a, b, c}) {
            const QPointF mapped = map(point);
            QSGGeometry::ColoredPoint2D vertex;
            vertex.set(mapped.x(), mapped.y(), color.red() * alpha / 255,
                color.green() * alpha / 255, color.blue() * alpha / 255, alpha);
            vertices.append(vertex);
        }
    }

    QVector<QSGGeometry::ColoredPoint2D> vertices;
    QPointF map(const QPointF& point) const { return center_ + scale_ * point; }

private:
    static QPointF polar(double radius, int index, int segments)
    {
        const double angle = 2 * std::numbers::pi * index / segments;
        return {radius * std::cos(angle), radius * std::sin(angle)};
    }

    QPointF center_;
    double scale_;
};

void drawTrajectory(PlayGeometry& geometry, const PlayHandMotion& hand, const QColor& color, double second)
{
    for (int i = 1; i < hand.trail.size(); ++i) {
        const double intensity = double(i) / (hand.trail.size() - 1);
        geometry.stroke(hand.trail.at(i - 1), hand.trail.at(i), 0.008 + 0.01 * intensity, faded(color, 0.67 * intensity));
    }
    const double ahead = hand.targetSecond - second;
    if (hand.touching || ahead <= 0.0 || ahead > 0.24) return;
    const QPointF line = hand.nextTarget - hand.contact;
    for (int i = 2; i < 8; ++i)
        geometry.disk(hand.contact + line * (i / 8.0), 0.0085, faded(color, 0.65));
}

void drawCue(PlayGeometry& geometry, const PlayContactCue& cue)
{
    const bool unassigned = cue.hand == HandOrderHand::Unspecified;
    const QColor color = cue.block != PlayBlock::None ? conflictColor : unassigned ? unknownColor
        : cue.hand == HandOrderHand::Left ? leftColor : rightColor;
    const bool pending = cue.timeToContact > 0.0;
    const double radius = cue.kind == PlayContactKind::Trace ? 0.10
        : 0.108 + (pending ? 0.48 * cue.timeToContact : 0.25 * qMin(0.15, -cue.timeToContact));
    geometry.ring(cue.target, radius + 0.006, 0.024, faded(shadow, cue.opacity));
    geometry.ring(cue.target, radius, 0.015, faded(color, cue.opacity), unassigned || pending);
    if (cue.strikes && !pending && cue.timeToContact >= -0.08)
        geometry.ring(cue.target, radius + 0.026, 0.008, faded(color, cue.opacity * 0.70));
}

void drawInterference(PlayGeometry& geometry, const PlayArmFrame& pose)
{
    if (pose.interference == PlayInterference::Clear) return;
    const auto point = pose.interferencePoint;
    if (pose.interference == PlayInterference::Lifted) {
        geometry.ring(point, 0.115, 0.015, liftColor, true);
        return;
    }
    geometry.disk(point, 0.10, shadow);
    geometry.ring(point, 0.102, 0.014, conflictColor);
    const QPointF a(0.041, 0.041), b(0.041, -0.041);
    geometry.stroke(point - a, point + a, 0.011, conflictColor);
    geometry.stroke(point - b, point + b, 0.011, conflictColor);
}

void drawButtons(PlayGeometry& geometry, const PlayContactFrame& frame, const PlayArmFrame& pose)
{
    for (int lane = 1; lane <= 8; ++lane) {
        const auto point = miacode::preview::scene::laneUnitVector(lane) * playExternalButtonRadius;
        QColor color(221, 222, 233, 180);
        bool pressed = false;
        for (int hand = 0; hand < 2; ++hand) {
            const auto& motion = frame.hands[hand];
            if (motion.buttonLane != lane || !playArmCanPress(pose, frame.hands, hand)) continue;
            color = hand == 0 ? leftColor : rightColor;
            pressed = true;
        }
        geometry.disk(point, 0.058, pressed ? color : shadow);
        geometry.ring(point, 0.059, pressed ? 0.012 : 0.008, color);
        if (pressed) geometry.disk(point, 0.020, faded(Qt::white, 0.85));
    }
}

class PlayLayerNode final : public QSGNode
{
public:
    QSGGeometryNode* geometry = nullptr;
    QSGSimpleTextureNode* caption = nullptr;
    QString captionKey;
    QSizeF captionSize;
};

void updateCaption(PlayLayerNode& node, const HandPlaySimulation& simulation, double second,
    const QRectF& playfield, QQuickWindow* window)
{
    const PlayJudgement* judgement = nullptr;
    const auto& events = simulation.judgements();
    const auto after = std::upper_bound(events.cbegin(), events.cend(), second,
        [](double time, const auto& event) { return time < event.second; });
    const double accumulated = after == events.cbegin() ? 0.0 : (after - 1)->cumulativeLoss;
    int shown = 0;
    for (auto it = after; it != events.cbegin() && shown < 12; ++shown) {
        --it;
        if (second - it->second > 0.78) break;
        if (it->baseLoss || it->breakBonusLoss) { judgement = &*it; break; }
    }
    if (!judgement) {
        if (node.caption) {
            node.removeChildNode(node.caption);
            delete node.caption;
            node.caption = nullptr;
        }
        return;
    }
    const QString title = QStringLiteral("%1  −%2%").arg(playGradeName(judgement->grade),
        QString::number(judgement->achievementLoss, 'f', 3));
    const QString cumulative = QStringLiteral("Σ −%1%").arg(QString::number(accumulated, 'f', 3));
    const int pixels = qBound(10, qRound(playfield.width() * 0.045), 46);
    const QString key = title + cumulative + QString::number(pixels);
    if (key != node.captionKey || !node.caption) {
        QFont font(QStringLiteral("Sans Serif"));
        font.setBold(true);
        font.setPixelSize(pixels);
        QFontMetrics metrics(font);
        const int width = qMax(metrics.horizontalAdvance(title), metrics.horizontalAdvance(cumulative)) + pixels;
        const int height = 2 * metrics.height() + pixels / 2;
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(16, 18, 26, 224));
        painter.drawRoundedRect(image.rect(), pixels / 3.0, pixels / 3.0);
        painter.setFont(font);
        painter.setPen(conflictColor);
        painter.drawText(QRect(0, pixels / 8, width, metrics.height()), Qt::AlignCenter, title);
        painter.setPen(unknownColor);
        painter.drawText(QRect(0, metrics.height() + pixels / 8, width, metrics.height()), Qt::AlignCenter, cumulative);
        painter.end();
        auto* texture = window->createTextureFromImage(image);
        if (!texture) return;
        if (node.caption) {
            node.removeChildNode(node.caption);
            delete node.caption;
        }
        node.caption = new QSGSimpleTextureNode;
        node.caption->setOwnsTexture(true);
        node.caption->setTexture(texture);
        node.caption->setFiltering(QSGTexture::Linear);
        node.appendChildNode(node.caption);
        node.captionKey = key;
        node.captionSize = QSizeF(width, height);
    }
    node.caption->setRect(QRectF(playfield.center().x() - node.captionSize.width() / 2,
        playfield.top() + playfield.height() * 0.16, node.captionSize.width(), node.captionSize.height()));
}

void drawMissedTargets(PlayGeometry& geometry, const QVector<PlayJudgement>& events, double second)
{
    const auto after = std::upper_bound(events.cbegin(), events.cend(), second,
        [](double time, const auto& event) { return time < event.second; });
    for (auto it = after; it != events.cbegin();) {
        --it;
        const double opacity = 1.0 - (second - it->second) / 0.62;
        if (opacity <= 0.0) break;
        if (it->baseLoss || it->breakBonusLoss)
            geometry.ring(it->position, 0.102, 0.013, faded(conflictColor, opacity));
    }
}

} // namespace

QSGNode* updatePreviewSimulatedPlayLayer(QSGNode* oldNode, const HandPlaySimulation& simulation,
                                       double second, const QRectF& playfield, QQuickWindow* window)
{
    if (simulation.isEmpty() || !std::isfinite(second)) return nullptr;
    const auto [frame, pose] = simulation.sample(second);
    PlayGeometry drawing(playfield);
    drawButtons(drawing, frame, pose);
    drawTrajectory(drawing, frame.hands[0], leftColor, second);
    drawTrajectory(drawing, frame.hands[1], rightColor, second);
    const auto drawArm = [&](int hand) {
        drawing.arm(pose.arms[hand], frame.hands[hand], hand == 0 ? leftColor : rightColor, hand);
    };
    const int behind = pose.arms[0].lifted ? 1 : 0;
    drawArm(behind);
    drawArm(1 - behind);
    for (const auto& cue : frame.cues) drawCue(drawing, cue);
    drawInterference(drawing, pose);
    drawMissedTargets(drawing, simulation.judgements(), second);

    auto* node = static_cast<PlayLayerNode*>(oldNode);
    if (node == nullptr) {
        node = new PlayLayerNode;
        node->geometry = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->geometry->setGeometry(geometry);
        node->geometry->setFlag(QSGNode::OwnsGeometry);
        node->geometry->setMaterial(new QSGVertexColorMaterial);
        node->geometry->setFlag(QSGNode::OwnsMaterial);
        node->appendChildNode(node->geometry);
    }
    auto* geometry = node->geometry->geometry();
    geometry->allocate(drawing.vertices.size());
    std::copy(drawing.vertices.cbegin(), drawing.vertices.cend(), geometry->vertexDataAsColoredPoint2D());
    geometry->markVertexDataDirty();
    node->geometry->markDirty(QSGNode::DirtyGeometry);
    if (window) updateCaption(*node, simulation, second, playfield, window);
    return node;
}
