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

struct ArrowPlacement {
    int area = 0;
    int index = 0;
    QPointF point;
    QPointF direction;
};

struct ProjectionCandidate {
    PreviewSlideArrowDistance arrow;
    double score = std::numeric_limits<double>::infinity();
};

ProjectionCandidate candidateFor(const ArrowPlacement& placement, int segment,
    const QVector<QPointF>& path, const QVector<double>& lengths)
{
    ProjectionCandidate result;
    result.arrow.distance = lengths[segment-1];
    const double length = lengths[segment]-lengths[segment-1];
    if (length <= 1e-9) return result;
    const QPointF delta = path[segment]-path[segment-1];
    const QPointF tangent = delta/length;
    const double u = std::clamp(QPointF::dotProduct(placement.point-path[segment-1],delta)/(length*length),0.0,1.0);
    result.arrow.distance += u*length;
    result.arrow.tangent = tangent;
    const double alignment = QPointF::dotProduct(tangent,placement.direction);
    if (alignment < 0.0) return result;
    const QPointF error = placement.point-(path[segment-1]+u*delta);
    result.score = QPointF::dotProduct(error,error)+(1.0-alignment)*1e-6;
    return result;
}

// Coincident stretches in pp/qq can have the same position AND tangent.
// Find the best ordered association over the whole arrow sequence, instead
// of letting independent nearest-point queries jump between laps. Prefix
// minima keep the dynamic program linear in arrows times path samples.
bool projectInPathOrder(const QVector<ArrowPlacement>& placements,
    const QVector<QPointF>& path, const QVector<double>& lengths,
    QVector<PreviewSlideArrowDistance>* resolved)
{
    const int count = path.size()-1;
    QVector<double> previousCosts, previousDistances;
    QVector<QVector<int>> parents;
    parents.reserve(placements.size());
    for (int arrow=0; arrow<placements.size(); ++arrow) {
        QVector<double> costs(count,std::numeric_limits<double>::infinity()), positions(count);
        QVector<int> row(count,-1);
        int prefix=0, bestIndex=-1;
        double bestCost=std::numeric_limits<double>::infinity();
        for (int sample=0; sample<count; ++sample) {
            const auto candidate=candidateFor(placements[arrow],sample+1,path,lengths);
            positions[sample]=candidate.arrow.distance;
            if (arrow==0) {
                costs[sample]=candidate.score;
                continue;
            }
            while (prefix<count && previousDistances[prefix]<=candidate.arrow.distance+1e-7) {
                if (previousCosts[prefix]<bestCost) {
                    bestCost=previousCosts[prefix];
                    bestIndex=prefix;
                }
                ++prefix;
            }
            if (std::isfinite(candidate.score) && bestIndex>=0) {
                costs[sample]=bestCost+candidate.score;
                row[sample]=bestIndex;
            }
        }
        previousCosts=std::move(costs);
        previousDistances=std::move(positions);
        parents.append(std::move(row));
    }
    if (placements.isEmpty()) return true;
    const auto best=std::min_element(previousCosts.cbegin(),previousCosts.cend());
    if (best==previousCosts.cend() || !std::isfinite(*best)) return false;
    int selected=static_cast<int>(best-previousCosts.cbegin());
    resolved->resize(placements.size());
    for (int arrow=placements.size()-1; arrow>=0; --arrow) {
        (*resolved)[arrow]=candidateFor(placements[arrow],selected+1,path,lengths).arrow;
        selected=parents[arrow][selected];
        if (arrow>0 && selected<0) return false;
    }
    return true;
}
}

PreviewSlideTimingGeometry buildPreviewSlideTimingGeometry(const TimelineNoteMarker& marker,
    const miacode::slide_judge::LaunchBeatIndex* launchBeats)
{
    PreviewSlideTimingGeometry result;
    result.timing = miacode::slide_judge::timingForMarker(marker,launchBeats);
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
        QVector<ArrowPlacement> placements;
        for (int area = 0; area < marker.slideTrackAreaPoints[segment].size(); ++area) {
            const auto& points = marker.slideTrackAreaPoints[segment][area];
            const auto rotations = marker.slideTrackAreaRotations.value(segment).value(area);
            if (rotations.size() != points.size()) return {};
            areas.append(QVector<PreviewSlideArrowDistance>(points.size()));
            // The reference artwork stores each area in reverse draw order.
            for (int index = points.size()-1; index>=0; --index) {
                if (!std::isfinite(points[index].x()) || !std::isfinite(points[index].y())
                    || !std::isfinite(rotations[index])) return {};
                const double radians = rotations[index] * 3.14159265358979323846 / 180.0;
                placements.append({area,index,points[index],QPointF(-std::cos(radians),std::sin(radians))});
            }
        }
        QVector<PreviewSlideArrowDistance> resolved;
        if (!projectInPathOrder(placements,path,lengths,&resolved)) return {};
        for (int i=0; i<placements.size(); ++i) {
            auto arrow=resolved[i];
            arrow.distance+=result.totalLength;
            areas[placements[i].area][placements[i].index]=arrow;
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
