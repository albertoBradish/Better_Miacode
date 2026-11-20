#pragma once

#include "core/chart/model/TimelineData.h"

#include <QtMath>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <limits>

namespace miacode::preview::scene {

inline HandOrderHand previewHandOrderAtProgress(const QVector<HandOrderTrackStop>& stops, qreal progress)
{
    HandOrderHand hand = HandOrderHand::Unspecified;
    for (const HandOrderTrackStop& stop : stops) {
        if (progress < stop.progress) {
            break;
        }
        hand = stop.hand;
    }
    return hand;
}

inline HandOrderHand previewWifiHandAtProgress(const TimelineNoteMarker& marker, int sector, qreal progress)
{
    if (marker.handOrderWifiMode == HandOrderWifiMode::Split) {
        if (sector < 0 || sector >= marker.handOrderWifiSectorStops.size()) return HandOrderHand::Unspecified;
        return previewHandOrderAtProgress(marker.handOrderWifiSectorStops.at(sector), progress);
    }
    return previewHandOrderAtProgress(marker.handOrderTrackStops, progress);
}

inline QPointF previewWifiSectorNormal(const TimelineNoteMarker& marker, int sector)
{
    if (marker.wifiLanePoints.size() < 3 || marker.wifiLanePoints.at(0).isEmpty()
        || marker.wifiLanePoints.at(1).isEmpty()) return {};
    const auto& centre = marker.wifiLanePoints.at(1);
    const QPointF direction = centre.constLast() - centre.constFirst();
    QPointF normal(-direction.y(), direction.x());
    if (QPointF::dotProduct(normal, marker.wifiLanePoints.at(0).constLast() - centre.constLast()) < 0)
        normal = -normal;
    const qreal length = std::hypot(normal.x(), normal.y());
    return length > 0 ? normal * ((sector == 0 ? 1.0 : -1.0) / length) : QPointF();
}

// The circled 1 is an explicit execution-mode badge, independent of hand color.
inline const QImage* previewWifiSingleBadgeImage()
{
    static const QImage image = [] {
        QImage result(36, 36, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(24, 24, 24, 230));
        painter.drawEllipse(QRectF(2, 2, 32, 32));
        painter.drawLine(QPointF(14, 13), QPointF(18, 10));
        painter.drawLine(QPointF(18, 10), QPointF(18, 26));
        painter.drawLine(QPointF(13, 26), QPointF(23, 26));
        return result;
    }();
    return &image;
}

inline qreal previewSlideMovementProgress(
    const TimelineNoteMarker& marker,
    int segmentIndex,
    qreal segmentProgress)
{
    qreal totalDuration = 0.0;
    qreal precedingDuration = 0.0;
    for (int index = 0; index < marker.slideSegmentDurations.size(); ++index) {
        const qreal duration = qMax<qreal>(0.0, marker.slideSegmentDurations.at(index));
        totalDuration += duration;
        if (index < segmentIndex) {
            precedingDuration += duration;
        }
    }
    if (totalDuration <= 0.0 || segmentIndex < 0 || segmentIndex >= marker.slideSegmentDurations.size()) {
        return qBound<qreal>(0.0, segmentProgress, 1.0);
    }
    const qreal duration = qMax<qreal>(0.0, marker.slideSegmentDurations.at(segmentIndex));
    return qBound<qreal>(
        0.0,
        (precedingDuration + qBound<qreal>(0.0, segmentProgress, 1.0) * duration) / totalDuration,
        1.0);
}

// Erasure checkpoints provide a coarse ordering prior and the no-sample fallback.
inline qreal previewSlideArrowCheckpointProgress(
    const TimelineNoteMarker& marker,
    int segmentIndex,
    int areaIndex,
    int arrowIndex)
{
    if (segmentIndex < 0 || segmentIndex >= marker.slideTrackAreaPoints.size()) {
        return 0.0;
    }
    const auto& areas = marker.slideTrackAreaPoints.at(segmentIndex);
    if (areaIndex < 0 || areaIndex >= areas.size()) {
        return 0.0;
    }
    const QVector<QPointF>& arrows = areas.at(areaIndex);
    if (arrows.isEmpty()) {
        return 0.0;
    }
    const QVector<double>* thresholds = segmentIndex < marker.slideTrackAreaThresholds.size()
        ? &marker.slideTrackAreaThresholds.at(segmentIndex) : nullptr;
    const qreal areaStart = qBound<qreal>(0.0, thresholds != nullptr ? thresholds->value(areaIndex, 0.0) : 0.0, 1.0);
    const qreal areaEnd = thresholds != nullptr && areaIndex + 1 < thresholds->size()
        ? qBound<qreal>(areaStart, thresholds->at(areaIndex + 1), 1.0)
        : 1.0;
    const auto* checkpointAreas = segmentIndex < marker.slideTrackAreaCheckpoints.size()
        ? &marker.slideTrackAreaCheckpoints.at(segmentIndex) : nullptr;
    const auto* cutAreas = segmentIndex < marker.slideTrackAreaCutIndices.size()
        ? &marker.slideTrackAreaCutIndices.at(segmentIndex) : nullptr;
    const QVector<double>* checkpoints = checkpointAreas != nullptr && areaIndex < checkpointAreas->size()
        ? &checkpointAreas->at(areaIndex) : nullptr;
    const QVector<int>* cuts = cutAreas != nullptr && areaIndex < cutAreas->size()
        ? &cutAreas->at(areaIndex) : nullptr;
    // Source arrow rows run from the area end back toward its start; track
    // erasure removes the array tail first for the same reason.
    const qreal arrowCentre = qBound<qreal>(0.5, arrows.size() - arrowIndex - 0.5, arrows.size() - 0.5);

    int previousCut = 0;
    qreal previousProgress = areaStart;
    for (int index = 0; checkpoints != nullptr && index < checkpoints->size(); ++index) {
        const int cut = qBound(
            previousCut,
            cuts != nullptr && cuts->size() == checkpoints->size()
                ? cuts->at(index)
                : qRound(static_cast<qreal>(index + 1) * arrows.size() / checkpoints->size()),
            arrows.size());
        const qreal checkpoint = qBound<qreal>(previousProgress, checkpoints->at(index), areaEnd);
        if (arrowCentre < cut) {
            return previousProgress + (checkpoint - previousProgress)
                * (arrowCentre - previousCut) / (cut - previousCut);
        }
        previousCut = cut;
        previousProgress = checkpoint;
    }
    if (previousCut >= arrows.size()) {
        return previousProgress;
    }
    return previousProgress + (areaEnd - previousProgress)
        * (arrowCentre - previousCut) / (arrows.size() - previousCut);
}

inline qreal previewSlideArrowSegmentProgress(
    const TimelineNoteMarker& marker,
    int segmentIndex,
    int areaIndex,
    int arrowIndex)
{
    const qreal estimate = previewSlideArrowCheckpointProgress(marker, segmentIndex, areaIndex, arrowIndex);
    if (segmentIndex < 0 || segmentIndex >= marker.slideSegmentPoints.size()
        || segmentIndex >= marker.slideTrackAreaPoints.size()) return estimate;
    const auto& points = marker.slideSegmentPoints.at(segmentIndex);
    const auto& areas = marker.slideTrackAreaPoints.at(segmentIndex);
    if (points.size() < 2 || areaIndex < 0 || areaIndex >= areas.size()
        || arrowIndex < 0 || arrowIndex >= areas.at(areaIndex).size()) return estimate;
    const QPointF arrow = areas.at(areaIndex).at(arrowIndex);
    if (!std::isfinite(arrow.x()) || !std::isfinite(arrow.y())) return estimate;

    const auto thresholds = marker.slideTrackAreaThresholds.value(segmentIndex);
    const qreal areaStart = qBound<qreal>(0.0, thresholds.value(areaIndex, 0.0), 1.0);
    const qreal areaEnd = areaIndex + 1 < thresholds.size()
        ? qBound<qreal>(areaStart, thresholds.at(areaIndex + 1), 1.0) : 1.0;
    // Erasure can lead the arrow centre by part of an area. Keep a full area's
    // allowance around its prior, while excluding distant turns of a loop.
    const qreal radius = qMax<qreal>(areaEnd - areaStart, 2.0 / (points.size() - 1));
    const qreal first = qMax<qreal>(0.0, estimate - radius);
    const qreal last = qMin<qreal>(1.0, estimate + radius);
    qreal bestDistanceSquared = std::numeric_limits<qreal>::infinity();
    qreal bestProgress = estimate;
    constexpr qreal distanceTieSquared = 1e-8;
    for (int index = qMax(0, qFloor(first * (points.size() - 1)));
         index + 1 < points.size() && qreal(index) / (points.size() - 1) <= last; ++index) {
        const QPointF start = points.at(index);
        const QPointF displacement = points.at(index + 1) - start;
        const qreal lengthSquared = QPointF::dotProduct(displacement, displacement);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0) continue;
        const qreal lower = qMax<qreal>(0.0, first * (points.size() - 1) - index);
        const qreal upper = qMin<qreal>(1.0, last * (points.size() - 1) - index);
        const qreal fraction = qBound(lower,
            QPointF::dotProduct(arrow - start, displacement) / lengthSquared, upper);
        const QPointF delta = arrow - (start + displacement * fraction);
        const qreal distanceSquared = QPointF::dotProduct(delta, delta);
        const qreal progress = (index + fraction) / (points.size() - 1);
        if (!std::isfinite(distanceSquared)) continue;
        if (distanceSquared + distanceTieSquared < bestDistanceSquared
            || (qAbs(distanceSquared - bestDistanceSquared) <= distanceTieSquared
                && qAbs(progress - estimate) < qAbs(bestProgress - estimate))) {
            bestDistanceSquared = qMin(bestDistanceSquared, distanceSquared);
            bestProgress = progress;
        }
    }
    // Samples, like the moving star, are uniformly spaced in segment time.
    // The caller still maps this through the unchanged chain-duration helper.
    return qBound<qreal>(0.0, bestProgress, 1.0);
}

// Wifi arrow rows lie on the centre lane. Its samples are linear in movement
// time, so projection places each row at the same progress as the moving stars.
inline qreal previewWifiArrowMovementProgress(const TimelineNoteMarker& marker, const QPointF& point)
{
    if (marker.wifiLanePoints.isEmpty()) {
        return 0.0;
    }
    const QVector<QPointF>& centreLane = marker.wifiLanePoints.at(marker.wifiLanePoints.size() / 2);
    if (centreLane.size() < 2) {
        return 0.0;
    }
    const QPointF start = centreLane.constFirst();
    const QPointF displacement = centreLane.constLast() - start;
    const qreal lengthSquared = displacement.x() * displacement.x() + displacement.y() * displacement.y();
    if (lengthSquared <= 0.0) {
        return 0.0;
    }
    const QPointF relative = point - start;
    return qBound<qreal>(
        0.0,
        (relative.x() * displacement.x() + relative.y() * displacement.y()) / lengthSquared,
        1.0);
}

}  // namespace miacode::preview::scene
