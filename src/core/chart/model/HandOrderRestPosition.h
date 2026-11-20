#pragma once

#include <QPointF>
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace miacode::chart {

inline constexpr double handOrderReturnDelaySeconds = 0.4;
inline constexpr double handOrderDefaultReturnSpeed = 3.0;
inline constexpr double handOrderMinReturnSpeed = 1.0;
inline constexpr double handOrderMaxReturnSpeed = 8.0;
// The native E ring is 205 * 3.1 / 4 units from the 540-square centre.
// Chart and analysis use the same field-radius (240) normalization.
inline constexpr double handOrderNaturalERadius = (205.0 * 3.1 / 4.0) / 240.0;

// Hand index 0 is left (E7); hand index 1 is right (E3).
inline QPointF handOrderNaturalRestPoint(int hand)
{
    return {hand == 0 ? -handOrderNaturalERadius : handOrderNaturalERadius, 0.0};
}

inline double handOrderFullyReturnedTime(const QPointF& lastPoint, double lastSecond, int hand,
                                        double speed = handOrderDefaultReturnSpeed)
{
    if (!std::isfinite(lastSecond)) return -std::numeric_limits<double>::infinity();
    const QPointF home = handOrderNaturalRestPoint(hand);
    const double distance = std::hypot(lastPoint.x() - home.x(), lastPoint.y() - home.y());
    return lastSecond + handOrderReturnDelaySeconds + distance / speed;
}

inline QPointF handOrderRestedPoint(const QPointF& lastPoint, double lastSecond, double now, int hand,
                                   double speed = handOrderDefaultReturnSpeed)
{
    const QPointF home = handOrderNaturalRestPoint(hand);
    if (!std::isfinite(lastSecond)) return home;
    const double distance = std::hypot(lastPoint.x() - home.x(), lastPoint.y() - home.y());
    if (distance <= 1e-9) return home;
    const double fraction = qBound(0.0,
        qMax(0.0, now - lastSecond - handOrderReturnDelaySeconds) * speed / distance, 1.0);
    return lastPoint * (1.0 - fraction) + home * fraction;
}

} // namespace miacode::chart
