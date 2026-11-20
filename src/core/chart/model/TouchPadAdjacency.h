#pragma once

#include <QString>
#include <cstdlib>
#include <utility>

namespace miacode::chart {

inline int touchPadRingIndex(const QString& pad)
{
    if (pad == QLatin1String("C")) return 4;
    if (pad.size() != 2 || pad.at(1) < QLatin1Char('1') || pad.at(1) > QLatin1Char('8')) return -1;
    switch (pad.at(0).toUpper().toLatin1()) {
    case 'A': return 0;
    case 'B': return 1;
    case 'D': return 2;
    case 'E': return 3;
    default: return -1;
    }
}

inline bool touchPadsAreAdjacent(const QString& a, const QString& b)
{
    if (a == b) return false;
    int firstRing = touchPadRingIndex(a);
    int secondRing = touchPadRingIndex(b);
    if (firstRing < 0 || secondRing < 0) return false;
    int firstLane = firstRing == 4 ? -1 : a.at(1).digitValue() - 1;
    int secondLane = secondRing == 4 ? -1 : b.at(1).digitValue() - 1;
    if (firstRing > secondRing) {
        std::swap(firstRing, secondRing);
        std::swap(firstLane, secondLane);
    }
    if (secondRing == 4) return firstRing == 1;
    if ((firstRing == 0 && secondRing == 0)
        || (firstRing == 2 && secondRing == 2)
        || (firstRing == 3 && secondRing == 3)
        || (firstRing == 1 && secondRing == 2)) return false;
    if (firstRing == 1 && secondRing == 1)
        return secondLane == ((firstLane + 1) & 7) || secondLane == ((firstLane + 7) & 7);
    if ((firstRing == 0 && secondRing == 1) || (firstRing == 2 && secondRing == 3))
        return firstLane == secondLane;
    return secondLane == firstLane || secondLane == ((firstLane + 1) & 7);
}

// Sequential sensors on the same ring are swipe neighbours even when
// they do not form a simultaneous Touchgroup.
inline bool touchPadsCanSweep(const QString& a, const QString& b)
{
    if (touchPadsAreAdjacent(a, b)) return true;
    const int firstRing = touchPadRingIndex(a);
    if (firstRing != touchPadRingIndex(b)
        || (firstRing != 0 && firstRing != 2 && firstRing != 3)) return false;
    const int distance = std::abs(a.at(1).digitValue() - b.at(1).digitValue());
    return distance == 1 || distance == 7;
}

} // namespace miacode::chart
