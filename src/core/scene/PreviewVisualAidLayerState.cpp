#include "core/scene/PreviewVisualAidLayerState.h"
#include "core/scene/PreviewOpacityCurves.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneMath.h"
#include "core/scene/PreviewTrackShared.h"
#include "core/analysis/MuriConfig.h"

#include <algorithm>
#include <cmath>

namespace miacode::preview::scene {
PreviewVisualAidLayerState buildPreviewVisualAidLayerState(const PreviewFrameState& state,
    const PreviewActiveMarkerView& markers, const QRectF& playfieldRect,
    bool includeMeasures, bool includeSkeleton)
{
    PreviewVisualAidLayerState result;
    const qreal scale = playfieldRect.width() / kLogicalCanvasSize;
    const double now = state.playheadSeconds;
    if (!qIsFinite(now)) return result;
    if (includeMeasures && state.render.showMeasureLines) {
        const auto timing = previewTapTimingForFlowSpeed(state.render.tapFlowSpeed);
        const auto begin = std::lower_bound(state.measureLineSeconds.begin(), state.measureLineSeconds.end(), now);
        for (auto it = begin; it != state.measureLineSeconds.end(); ++it) {
            const double delta = now - *it;
            if (delta < -timing.lifecycleDurationSeconds) break;
            const auto approach = sampleTapApproach(delta, timing.lifecycleDurationSeconds,
                timing.spawnDurationSeconds, timing.flyDurationSeconds, timing.unitsPerSecond,
                kLogicalDistanceTap, kLogicalDistanceEdge);
            if (approach.scale <= 0.0) continue;
            PreviewCircleDescriptor circle;
            circle.center = playfieldRect.center();
            circle.radiusX = circle.radiusY = approach.distance * scale;
            circle.fillColor = Qt::transparent;
            circle.strokeColor = QColor(220, 225, 230, qRound(155 * approach.scale));
            circle.strokeWidth = qMax<qreal>(0.7, 1.15 * scale);
            result.circles.append(circle);
        }
    }
    if (!includeSkeleton || !state.render.showJudgementSkeleton) return result;

    const auto validAreas = [](const QVector<QStringList>& areas) {
        if (areas.isEmpty()) return false;
        for (const auto& group : areas) {
            if (group.isEmpty()) return false;
            for (const auto& pad : group) if (!miacode::muri::padTokenIsValid(pad)) return false;
        }
        return true;
    };
    for (int index = 0; index < markers.size(); ++index) {
        const auto& marker = markers.markerAt(index);
        const bool wifi = marker.type == QLatin1String("wifi");
        if (!wifi && marker.type != QLatin1String("slide")) continue;
        const auto timing = previewSlideTrackTimingForEffectiveFlowSpeed(
            state.render.tapFlowSpeed * marker.hsMultiplier);
        if (now < marker.second - timing.appearLeadInSeconds || now >= marker.endSecond) continue;
        qreal opacity = now < marker.slideTraceSecond
            ? sampleSlideTrackPreTraceOpacity(marker.second, now,
                timing.appearLeadInSeconds, timing.fullBrightLeadInSeconds) : 1.0;
        if (opacity <= 0.0) continue;
        const MarkerMuriState* analysis = nullptr;
        if (state.muriRenderOptions.renderMode == RenderMode::MaimuriDxStyle) {
            const auto found = state.muriAnalysisReport.markerStates.constFind(makeMarkerAnalysisKey(marker));
            if (found != state.muriAnalysisReport.markerStates.constEnd()) analysis = &found.value();
        }
        const auto appendAreas = [&](const QVector<QStringList>& areas, int consumed, int segment, int lane) {
            // Every member of a pad group is an alternative. Connect alternatives
            // only across adjacent groups; never turn one group into a serial path.
            for (int area = qMax(0, consumed); area + 1 < areas.size(); ++area) {
                for (const QString& from : areas.at(area)) {
                    for (const QString& to : areas.at(area + 1)) {
                        if (!miacode::muri::padTokenIsValid(from) || !miacode::muri::padTokenIsValid(to)) continue;
                        const QPointF a = mapLogicalPointToRect(miacode::muri::padCenter(from), playfieldRect);
                        const QPointF b = mapLogicalPointToRect(miacode::muri::padCenter(to), playfieldRect);
                        if (QLineF(a, b).length() < 0.01) continue;
                        result.lines.append({QLineF(a, b), QColor(177, 249, 255, qRound(210 * opacity)),
                            qMax<qreal>(0.8, 1.35 * scale), markers.sourceIndexAt(index), segment, lane, area});
                    }
                }
            }
        };
        if (wifi) {
            if (marker.wifiLaneJudgeAreas.size() != 3
                || !validAreas(marker.wifiLaneJudgeAreas.value(0))
                || !validAreas(marker.wifiLaneJudgeAreas.value(1))
                || !validAreas(marker.wifiLaneJudgeAreas.value(2))
                || marker.wifiLaneJudgeAreas.at(0).size() != marker.wifiLaneJudgeAreas.at(1).size()
                || marker.wifiLaneJudgeAreas.at(2).size() != marker.wifiLaneJudgeAreas.at(1).size()) {
                ++result.missingJudgeDataCount; continue;
            }
            const qreal progress = qBound(0.0, (now - marker.slideTraceSecond)
                / qMax(0.001, marker.endSecond - marker.slideTraceSecond), 1.0);
            for (int lane = 0; lane < 3; ++lane) {
                const auto& areas = marker.wifiLaneJudgeAreas.at(lane);
                int consumed = now < marker.slideTraceSecond ? 0
                    : currentAreaIndexForProportion(marker.wifiTrackAreaThresholds, progress, areas.size());
                if (state.muriRenderOptions.renderMode == RenderMode::EraseByArea && now >= marker.slideTraceSecond)
                    consumed = qBound(0, qFloor(areas.size() * progress / qMax(0.001, marker.wifiCriticalProportion)), areas.size());
                if (analysis != nullptr) consumed = currentWifiLaneAreaIndexAt(
                    analysis->wifiLaneProgressSeconds.value(lane), analysis->wifiLaneAreas.value(lane), now);
                appendAreas(areas, consumed, 0, lane);
            }
            continue;
        }
        if (marker.slideSegmentJudgeAreas.isEmpty()
            || marker.slideSegmentJudgeAreas.size() != marker.slideSegmentShootSeconds.size()
            || marker.slideSegmentJudgeAreas.size() != marker.slideSegmentDurations.size()) {
            ++result.missingJudgeDataCount; continue;
        }
        for (int segment = 0; segment < marker.slideSegmentJudgeAreas.size(); ++segment) {
            const auto& areas = marker.slideSegmentJudgeAreas.at(segment);
            if (!validAreas(areas)) { ++result.missingJudgeDataCount; continue; }
            const double start = marker.slideSegmentShootSeconds.at(segment);
            const double duration = marker.slideSegmentDurations.at(segment);
            if (now >= start + duration) continue;
            const qreal progress = qBound(0.0, (now - start) / qMax(0.001, duration), 1.0);
            int consumed = now < start ? 0 : currentAreaIndexForProportion(
                marker.slideTrackAreaThresholds.value(segment), progress, areas.size());
            if (state.muriRenderOptions.renderMode == RenderMode::EraseByArea && now >= start) {
                const double critical = segment + 1 == marker.slideSegmentJudgeAreas.size()
                    ? marker.slideSegmentCriticalProportions.value(segment, 1.0) : 1.0;
                consumed = qBound(0, qFloor(areas.size() * progress / qMax(0.001, critical)), areas.size());
            }
            if (analysis != nullptr && segment < analysis->slideSegments.size()) {
                const auto& judged = analysis->slideSegments.at(segment);
                if (judged.completedSecond >= 0 && now >= judged.completedSecond) continue;
                consumed = 0;
                for (const auto& checkpoints : judged.areaCheckpoints) {
                    if (checkpoints.isEmpty() || passedCheckpointCount(checkpoints, now) < checkpoints.size()) break;
                    ++consumed;
                }
            }
            appendAreas(areas, consumed, segment, 0);
        }
    }
    return result;
}
}
