#include "core/analysis/HandOrderPressureAdapter.h"

#include "common/TaskCancellation.h"

#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace miacode::chart_structure {
namespace {

constexpr double kTapOccupancySeconds = 0.085;
constexpr double kMovementWindowSeconds = 0.75;
constexpr double kAlternationWindowSeconds = 0.55;

int sideIndex(HandOrderHand hand)
{
    switch (hand) {
    case HandOrderHand::Left: return 0;
    case HandOrderHand::Right: return 1;
    default: return -1;
    }
}

double laneDistance(int first, int second)
{
    const int difference = std::abs(std::clamp(first, 1, 8) - std::clamp(second, 1, 8));
    return std::min(difference, 8 - difference) / 4.0;
}

struct HeadAction {
    double second = 0.0;
    int side = -1;
    int lane = 0;
    QPointF touch;
    bool isTouch = false;
};

double movementBetween(const HeadAction& first, const HeadAction& second)
{
    if (first.isTouch && second.isTouch) {
        const QPointF delta = first.touch - second.touch;
        return std::min(1.0, std::hypot(delta.x(), delta.y()) / 540.0);
    }
    if (first.isTouch != second.isTouch && (first.lane == 0 || second.lane == 0)) return 0.5;
    return laneDistance(first.lane, second.lane);
}

// Add a constant load to whole bins in O(1), keeping fractional edges.
void addSpan(QVector<double>& delta, double start, double end, double duration, double step, double load)
{
    if (!std::isfinite(start) || !std::isfinite(end) || end <= 0.0 || start >= duration || end <= start) return;
    const double begin = std::max(0.0, start) / step;
    const double finish = std::min(duration, end) / step;
    const int lastFrame = static_cast<int>(delta.size()) - 2;
    const int first = static_cast<int>(std::clamp(std::floor(begin), 0.0, static_cast<double>(lastFrame)));
    const int last = static_cast<int>(std::clamp(std::ceil(finish) - 1, static_cast<double>(first),
        static_cast<double>(lastFrame)));
    if (first == last) {
        const double weight = load * std::min(1.0, finish - begin);
        delta[first] += weight;
        delta[first + 1] -= weight;
        return;
    }
    const double firstWeight = load * (first + 1 - begin);
    const double lastWeight = load * (finish - last);
    delta[first] += firstWeight;
    delta[first + 1] += load - firstWeight;
    delta[last] += lastWeight - load;
    delta[last + 1] -= lastWeight;
}

} // namespace

HandOrderPressureCurve estimateHandOrderPressure(const QVector<TimelineNoteMarker>& notes,
    double firstSeconds, double durationSeconds, double secondsPerFrame, int frameCount)
{
    if (!std::isfinite(firstSeconds) || !std::isfinite(durationSeconds) || !std::isfinite(secondsPerFrame)
        || durationSeconds <= 0.0 || secondsPerFrame <= 0.0 || frameCount <= 0) return {};
    HandOrderPressureCurve curve{durationSeconds, secondsPerFrame, QVector<HandOrderPressureFrame>(frameCount)};
    std::array<QVector<double>, 2> occupancy{QVector<double>(frameCount + 1), QVector<double>(frameCount + 1)};
    std::array<QVector<double>, 2> trackMovement{QVector<double>(frameCount + 1), QVector<double>(frameCount + 1)};
    QVector<HeadAction> heads;
    QSet<QString> slideHeads;
    bool covered = false;
    const auto frameAt = [&](double second) {
        return static_cast<int>(std::clamp(second / secondsPerFrame, 0.0, static_cast<double>(frameCount - 1)));
    };
    const auto inMedia = [&](double second) {
        return std::isfinite(second) && second >= 0.0 && second < durationSeconds;
    };
    for (const auto& note : notes) {
        task::CancellationScope::checkpoint();
        const bool slide = note.type == QLatin1String("slide") || note.type == QLatin1String("wifi");
        const double headSecond = note.second + firstSeconds;
        const double tailSecond = note.endSecond + firstSeconds;
        if (slide && !note.trackMine) {
            const double traceSecond = std::max(note.second, note.slideTraceSecond) + firstSeconds;
            const double duration = tailSecond - traceSecond;
            if (std::isfinite(duration) && duration > 0.0 && tailSecond > 0.0 && traceSecond < durationSeconds) {
                const double distance = note.slideRuntimeTrackLength > 0.0
                    ? std::clamp(note.slideRuntimeTrackLength / 20.0, 0.2, 6.0)
                    : std::max(0.25, laneDistance(note.lane, note.endLane));
                const auto addTrack = [&](const QVector<HandOrderTrackStop>& stops) {
                    int previous = -1;
                    for (qsizetype i = 0; i < stops.size(); ++i) {
                        const auto& stop = stops.at(i);
                        const int side = sideIndex(stop.hand);
                        const double start = traceSecond + duration * stop.progress;
                        const double end = i + 1 < stops.size()
                            ? traceSecond + duration * stops.at(i + 1).progress : tailSecond;
                        if (side >= 0 && std::isfinite(start) && std::isfinite(end)
                            && start < durationSeconds && end > std::max(0.0, start)) {
                            addSpan(occupancy[side], start, end, durationSeconds, secondsPerFrame, 1.0);
                            addSpan(trackMovement[side], start, end, durationSeconds, secondsPerFrame,
                                distance * secondsPerFrame / duration);
                            if (previous >= 0 && previous != side && inMedia(start)) ++curve.frames[frameAt(start)].alternation;
                            covered = true;
                        }
                        previous = side;
                    }
                };
                if (note.handOrderWifiMode == HandOrderWifiMode::Split) {
                    for (const auto& sector : note.handOrderWifiSectorStops) addTrack(sector);
                } else {
                    addTrack(note.handOrderTrackStops);
                }
            }
        }

        const int headSide = sideIndex(note.handOrderHead);
        if (headSide < 0 || !std::isfinite(headSecond) || (slide ? !note.hasHeadStar || note.headMine : note.isMine)) continue;
        if (slide) {
            const QString identity = QStringLiteral("%1|%2|%3|%4|%5").arg(note.second, 0, 'g', 17)
                .arg(note.sourceLine).arg(note.sourceCol).arg(note.lane).arg(note.eachGroupId);
            if (slideHeads.contains(identity)) continue;
            slideHeads.insert(identity);
        }
        const bool hold = note.type == QLatin1String("hold") || note.type == QLatin1String("touch_hold");
        const double end = hold && tailSecond > headSecond ? tailSecond : headSecond + kTapOccupancySeconds;
        if (headSecond < durationSeconds && end > 0.0) {
            addSpan(occupancy[headSide], headSecond, end, durationSeconds, secondsPerFrame, 1.0);
            covered = true;
        }
        if (inMedia(headSecond)) {
            const bool isTouch = note.type == QLatin1String("touch") || note.type == QLatin1String("touch_hold");
            const int lane = isTouch && note.touchPad == QLatin1String("C") ? 0 : note.lane;
            heads.append({headSecond, headSide, lane, note.touchPoint, isTouch});
        }
    }
    if (!covered) return {};

    std::stable_sort(heads.begin(), heads.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::array<HeadAction, 2> previousHand;
    HeadAction previous;
    for (const auto& action : heads) {
        task::CancellationScope::checkpoint();
        auto& frame = curve.frames[frameAt(action.second)];
        auto& sameHand = previousHand[action.side];
        if (sameHand.side >= 0 && action.second - sameHand.second < kMovementWindowSeconds) {
            const double distance = movementBetween(sameHand, action);
            if (action.side == 0) frame.leftMovement += distance;
            else frame.rightMovement += distance;
        }
        const double elapsed = action.second - previous.second;
        if (previous.side >= 0 && previous.side != action.side && elapsed > 0.0001 && elapsed < kAlternationWindowSeconds)
            frame.alternation += 1.0 - elapsed / kAlternationWindowSeconds;
        sameHand = action;
        previous = action;
    }

    std::array<double, 2> busy{}, moved{};
    for (int i = 0; i < frameCount; ++i) {
        auto& frame = curve.frames[i];
        for (int side = 0; side < 2; ++side) {
            busy[side] += occupancy[side].at(i);
            moved[side] += trackMovement[side].at(i);
        }
        frame.leftOccupancy = std::max(0.0, busy[0]);
        frame.rightOccupancy = std::max(0.0, busy[1]);
        frame.leftMovement += std::max(0.0, moved[0]);
        frame.rightMovement += std::max(0.0, moved[1]);
    }
    return curve;
}

} // namespace miacode::chart_structure
