#include "core/scene/PreviewSlideTiming.h"
#include "core/chart/model/TimelineData.h"

#include <QLineF>
#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::preview::scene {
namespace {
QVector<double> distances(const QVector<QPointF>& path)
{
    if (path.size() < 2) return {};
    QVector<double> result(path.size(), 0.0);
    for (int i = 1; i < path.size(); ++i) {
        const double length = QLineF(path[i-1], path[i]).length();
        if (!std::isfinite(length)) return {};
        result[i] = result[i-1] + length;
    }
    return result.constLast() > 0.0 ? result : QVector<double>();
}

PreviewSlideArrowDistance project(const QPointF& point, const QPointF& direction,
    const QVector<QPointF>& path, const QVector<double>& lengths)
{
    PreviewSlideArrowDistance result;
    double best = std::numeric_limits<double>::infinity();
    for (int i = 1; i < path.size(); ++i) {
        const QPointF delta = path[i] - path[i-1];
        const double length = lengths[i] - lengths[i-1];
        if (length <= 1e-9) continue;
        const QPointF tangent = delta / length;
        // Arrow direction disambiguates coincident out-and-back paths (v1)
        // and intersections. Arrays contain reversed arrows within an area,
        // so iteration order alone cannot identify arc length.
        const double alignment = QPointF::dotProduct(tangent, direction);
        if (!direction.isNull() && alignment < 0.0) continue;
        const double u = std::clamp(QPointF::dotProduct(point - path[i-1], delta) / (length * length), 0.0, 1.0);
        const QPointF error = point - (path[i-1] + u * delta);
        const double score = QPointF::dotProduct(error, error) + (1.0 - alignment) * 1e-6;
        if (score < best) {
            best = score;
            result.distance = lengths[i-1] + u * length;
            result.tangent = tangent;
        }
    }
    return result;
}
}

PreviewSlideTimingGeometry buildPreviewSlideTimingGeometry(const TimelineNoteMarker& marker)
{
    PreviewSlideTimingGeometry result;
    result.timing = miacode::slide_judge::timingForMarker(marker);
    if (!result.timing.valid) return result;
    if (marker.type == QLatin1String("wifi")) {
        if (marker.wifiLanePoints.size() != 3 || marker.wifiTrackAreaPoints.isEmpty()) return {};
        const auto& path = marker.wifiLanePoints[1];
        const auto lengths = distances(path);
        if (lengths.isEmpty()) return {};
        result.totalLength = lengths.constLast();
        for (const auto& points : marker.wifiTrackAreaPoints) {
            QVector<PreviewSlideArrowDistance> area;
            for (const auto& point : points) {
                const auto arrow = project(point, {}, path, lengths);
                if (arrow.tangent.isNull()) return {};
                area.append(arrow);
            }
            result.wifiArrows.append(area);
        }
        return result;
    }
    if (marker.slideSegmentPoints.size() != marker.slideSegmentKeys.size()
        || marker.slideTrackAreaPoints.size() != marker.slideSegmentKeys.size()) return {};
    for (int segment = 0; segment < marker.slideSegmentPoints.size(); ++segment) {
        const auto& path = marker.slideSegmentPoints[segment];
        const auto lengths = distances(path);
        if (lengths.isEmpty()) return {};
        QVector<QVector<PreviewSlideArrowDistance>> areas;
        for (int area = 0; area < marker.slideTrackAreaPoints[segment].size(); ++area) {
            const auto& points = marker.slideTrackAreaPoints[segment][area];
            const auto rotations = marker.slideTrackAreaRotations.value(segment).value(area);
            if (rotations.size() != points.size()) return {};
            QVector<PreviewSlideArrowDistance> arrows;
            for (int index = 0; index < points.size(); ++index) {
                const double radians = rotations[index] * 3.14159265358979323846 / 180.0;
                auto arrow = project(points[index], QPointF(-std::cos(radians), std::sin(radians)), path, lengths);
                if (arrow.tangent.isNull()) return {};
                arrow.distance += result.totalLength;
                arrows.append(arrow);
            }
            areas.append(arrows);
        }
        result.slideArrows.append(areas);
        result.totalLength += lengths.constLast();
    }
    return result;
}

void applyPreviewSlideTimingBrightness(PreviewSpriteDescriptor* sprite,
    const PreviewSlideTimingGeometry& geometry, const PreviewSlideArrowDistance& arrow,
    double chartSecond, double canvasScale)
{
    if (sprite == nullptr || !geometry.isValid() || canvasScale <= 0.0) return;
    constexpr double lowBrightness = 0.25;
    const double progress = miacode::slide_judge::brightnessProgress(geometry.timing, chartSecond);
    if (progress <= 0.0) {
        sprite->brightness = lowBrightness;
    } else if (progress < 1.0) {
        sprite->unrevealedBrightness = lowBrightness;
        sprite->revealOffset = arrow.distance / geometry.totalLength - progress;
        sprite->revealDirection = arrow.tangent / (geometry.totalLength * canvasScale);
    }
    // At the earliest Perfect instant every still-visible pixel is normal,
    // including artwork overhang. Native judgement erasure controls visibility.
}

} // namespace miacode::preview::scene
