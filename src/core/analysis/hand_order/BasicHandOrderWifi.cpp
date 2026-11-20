#include "core/analysis/hand_order/BasicHandOrderWifi.h"
#include "core/chart/SlideReferenceData.h"
#include "core/analysis/MuriConfig.h"
#include "common/TaskCancellation.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <algorithm>
#include <limits>

namespace miacode::hand_order {
namespace {
ContactAction baseAction(const TimelineNoteMarker& marker)
{
    ContactAction action;
    action.kind = QStringLiteral("slide"); action.branch = marker.slideBranchIndex;
    action.start = action.motionStart = marker.slideTraceSecond;
    action.end = action.motionEnd = marker.endSecond;
    // Existing native Wifi palm template: a finite 50px contact circle on a
    // 240px field. It is never expanded to encompass an entire Wifi fan.
    action.radius = muri::kHandRadiusWifi / 240.0;
    return action;
}
}
bool buildWifiHandOrderActions(const TimelineNoteMarker& marker,
    QVector<ContactAction>* sectors, QVector<ContactAction>* singleHand)
{
    sectors->clear(); singleHand->clear();
    const auto entry = slide_reference::root().value(QStringLiteral("wifi")).toObject()
        .value(marker.slideTrackKey).toObject();
    const auto paths = entry.value(QStringLiteral("di_real_path_samples")).toArray();
    if (paths.size() != 2 || marker.wifiLanePoints.size() != 3 || marker.endSecond <= marker.slideTraceSecond) return false;
    for (const auto& value : paths) {
        task::CancellationScope::checkpoint();
        const auto points = value.toArray(); if (points.size() < 2) return false;
        auto action = baseAction(marker);
        // The bundled two-palm paths are straight. Preserve each kink if a
        // later reference contains one, while collapsing collinear samples.
        for (int i = 0; i < points.size(); ++i) {
            const auto point = points.at(i).toObject();
            const QPointF position(point.value("x").toDouble() / 240.0, point.value("y").toDouble() / 240.0);
            const double second = action.start + (action.end - action.start) * i / (points.size() - 1);
            if (action.points.size() >= 2) {
                const auto a = action.points.at(action.points.size() - 2), b = action.points.last();
                const auto ab = b - a, bc = position - b;
                if (std::abs(ab.x() * bc.y() - ab.y() * bc.x()) < 1e-9
                    && QPointF::dotProduct(ab, bc) >= 0) {
                    action.points.last() = position; action.times.last() = second; continue;
                }
            }
            action.points.append(position); action.times.append(second);
        }
        sectors->append(action);
    }
    const auto outer0 = marker.wifiLanePoints.first().last() / 240.0;
    if (QLineF(sectors->at(0).points.last(), outer0).length() > QLineF(sectors->at(1).points.last(), outer0).length())
        std::swap((*sectors)[0], (*sectors)[1]);

    const auto queues = entry.value(QStringLiteral("tri_judge_sequence")).toArray();
    if (queues.size() != 3) return false;
    const int count = queues.at(0).toArray().size(); if (count < 2) return false;
    for (int direction : {1, -1}) {
        auto action = baseAction(marker);
        for (int area = 0; area < count; ++area) {
            QVector<QPointF> points;
            for (int side = 0; side < 3; ++side) {
                task::CancellationScope::checkpoint();
                const int lane = ((area % 2 == 0) == (direction > 0)) ? side : 2 - side;
                const auto queue = queues.at(lane).toArray(); if (queue.size() != count) return false;
                const auto pads = queue.at(area).toArray(); if (pads.isEmpty()) return false;
                QPointF selected; double nearest = std::numeric_limits<double>::infinity();
                const auto previous = !points.isEmpty() ? points.last() : !action.points.isEmpty() ? action.points.last() : QPointF();
                for (const auto& pad : pads) {
                    const auto point = touchPadPoint(pad.toString()); const double distance = QLineF(previous, point).length();
                    if (distance < nearest) { nearest = distance; selected = point; }
                }
                if (points.isEmpty() || QLineF(points.last(), selected).length() > 1e-8) points.append(selected);
            }
            const double stage = area == count - 1 ? qBound(0.72, marker.wifiCriticalProportion + 0.025, 0.92)
                : double(area) / (count - 1) * 0.78;
            for (int i = 0; i < points.size(); ++i) {
                const double fraction = area == 0 ? 0 : stage + (double(i) / qMax(1, points.size() - 1) - 0.5) * 0.09;
                const double second = action.start + fraction * (action.end - action.start);
                if (!action.times.isEmpty() && second <= action.times.last() + 1e-8) continue;
                action.points.append(points.at(i)); action.times.append(second);
            }
        }
        if (action.points.size() < 2) return false;
        action.points.append(action.points.last()); action.times.append(action.end);
        singleHand->append(action);
    }
    return true;
}
} // namespace miacode::hand_order
