#include "core/chart/transform/HandOrderScreenSplit.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <QLineF>
#include <algorithm>
#include <array>
#include <cmath>

namespace miacode::chart_transform {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double sideTolerance = 1e-5;

QPointF lanePoint(int lane)
{
    const double angle = (lane - 0.5) * pi / 4.0;
    return {std::sin(angle), -std::cos(angle)};
}

double sideOf(const QPointF& axis, const QPointF& point)
{
    return axis.x() * point.y() - axis.y() * point.x();
}

HandOrderHand sideHand(double side)
{
    return std::abs(side) < sideTolerance ? HandOrderHand::Unspecified
        : side > 0 ? HandOrderHand::Right : HandOrderHand::Left;
}

bool commonAxis(const std::array<bool, 8>& starts, QPointF* axis)
{
    int candidates = 0;
    // These are all reflection axes of the eight native button positions.
    for (int index = 0; index < 8; ++index) {
        QPointF candidate(std::sin(index * pi / 8.0), -std::cos(index * pi / 8.0));
        if (candidate.y() > sideTolerance) candidate = -candidate;
        bool matches = true, hasOffAxis = false;
        for (int lane = 1; lane <= 8; ++lane) {
            if (!starts.at(lane - 1)) continue;
            const auto point = lanePoint(lane);
            hasOffAxis |= std::abs(sideOf(candidate, point)) > sideTolerance;
            const auto reflected = 2.0 * QPointF::dotProduct(point, candidate) * candidate - point;
            bool found = false;
            for (int peer = 1; peer <= 8; ++peer)
                if (starts.at(peer - 1) && QLineF(reflected, lanePoint(peer)).length() < sideTolerance) found = true;
            if (!found) { matches = false; break; }
        }
        // An axis containing all starts cannot separate them into two sides.
        if (matches && hasOffAxis) { ++candidates; *axis = candidate; }
    }
    return candidates == 1;
}

QVector<HandOrderTrackStop> splitTrack(const TimelineNoteMarker& marker, const QPointF& axis)
{
    struct Sample { double progress; double side; };
    QVector<Sample> samples;
    const double total = marker.endSecond - marker.slideTraceSecond;
    if (!std::isfinite(total) || total <= 0 || marker.slideSegmentPoints.isEmpty()
        || marker.slideSegmentPoints.size() != marker.slideSegmentShootSeconds.size()) return {};
    for (int segment = 0; segment < marker.slideSegmentPoints.size(); ++segment) {
        const auto& points = marker.slideSegmentPoints.at(segment);
        const double first = marker.slideSegmentShootSeconds.at(segment);
        const double last = segment + 1 < marker.slideSegmentShootSeconds.size()
            ? marker.slideSegmentShootSeconds.at(segment + 1) : marker.endSecond;
        if (points.size() < 2 || !std::isfinite(first) || !std::isfinite(last) || last <= first) return {};
        for (int point = 0; point < points.size(); ++point) {
            const double progress = ((first - marker.slideTraceSecond)
                + (last - first) * point / (points.size() - 1)) / total;
            const auto position = points.at(point);
            const double rawSide = sideOf(axis, position);
            // Native artwork samples use float coordinates. Scale the axis
            // tolerance to their radius so a star on the axis stays on it.
            const double side = std::abs(rawSide) <= sideTolerance * qMax(1.0, std::hypot(position.x(), position.y()))
                ? 0.0 : rawSide;
            if (!std::isfinite(progress) || !std::isfinite(side)) return {};
            samples.append({progress, side});
        }
    }
    QVector<HandOrderTrackStop> stops;
    Sample previous{0.0, 0.0};
    double firstOnAxis = -1.0;
    for (const auto& sample : samples) {
        const auto hand = sideHand(sample.side);
        if (hand == HandOrderHand::Unspecified) {
            if (!stops.isEmpty() && firstOnAxis < 0) firstOnAxis = sample.progress;
            continue;
        }
        if (stops.isEmpty()) stops.append({0.0, hand});
        else if (hand != stops.constLast().hand) {
            // The preview interpolates native samples linearly in segment time.
            // Exact axis runs belong to the arriving side until a real crossing.
            const double crossing = firstOnAxis >= 0 ? firstOnAxis : previous.progress
                + (sample.progress - previous.progress) * std::abs(previous.side)
                    / (std::abs(previous.side) + std::abs(sample.side));
            if (crossing > stops.constLast().progress && crossing < 1.0)
                stops.append({crossing, hand});
        }
        firstOnAxis = -1.0;
        previous = sample;
    }
    return stops;
}
} // namespace

HandOrderEditPlan screenSplitHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    begin = qBound(0, begin, int(text.size()));
    end = qBound(begin, end, int(text.size()));
    return screenSplitHandOrderInSelection(text,
        miacode::chart_selection::NoteRanges{{begin, end}}, metadata);
}

HandOrderEditPlan screenSplitHandOrderInSelection(const QString& text,
    const miacode::chart_selection::NoteRanges& selected,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    HandOrderEditPlan plan;
    const auto ranges = miacode::chart_selection::normalizedNoteRanges(selected, text.size());
    if (ranges.isEmpty()) return plan;
    const int begin = ranges.first().start;
    const int end = ranges.last().end;
    plan.replacement = text.mid(begin, end - begin);
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    if (!parsed.errors.isEmpty() || !parsed.annotationErrors.isEmpty()) {
        plan.diagnostics = parsed.errors + parsed.annotationErrors;
        return plan;
    }
    QVector<const TimelineNoteMarker*> byParseOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byParseOrder.size()) byParseOrder[marker.parseOrder] = &marker;
    QVector<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n')) lineStarts.append(i + 1);
    struct Target { const SimaiSourceToken* source; int start; HandOrderTokenEdit token; };
    QVector<Target> targets;
    std::array<bool, 8> starts{};
    int slideCount = 0;
    for (const auto& source : parsed.sourceTokens) {
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        const auto range = std::lower_bound(ranges.cbegin(), ranges.cend(), start,
            [](const miacode::chart_selection::NoteRange& candidate, int position) {
                return candidate.end <= position;
            });
        if (range == ranges.cend() || range->start >= finish) continue;
        if (start < range->start || finish > range->end) { ++plan.partial; continue; }
        if (!source.markerCount) continue;
        bool supported = true;
        HandOrderTokenEdit token;
        token.baseToken = hand_order::splitToken(text.mid(start, source.length)).baseToken;
        for (int branch = 0; branch < source.markerCount; ++branch) {
            const auto* marker = byParseOrder.value(source.firstParseOrder + branch);
            if (!marker || marker->type != QStringLiteral("slide") || marker->slideBranchIndex != branch) {
                supported = false; break;
            }
            token.tracks.append(QVector<HandOrderTrackStop>{});
            token.hasHead |= marker->hasHeadStar;
            token.head = marker->handOrderHead;
        }
        if (!supported) { ++plan.unsupported; continue; }
        for (int branch = 0; branch < source.markerCount; ++branch) {
            const auto* marker = byParseOrder.at(source.firstParseOrder + branch);
            starts.at(marker->lane - 1) = true;
            ++slideCount;
        }
        targets.append({&source, start, token});
    }
    QPointF axis;
    const bool singleHead = slideCount >= 2 && std::count(starts.cbegin(), starts.cend(), true) == 1;
    if (singleHead) {
        const int lane = int(std::find(starts.cbegin(), starts.cend(), true) - starts.cbegin()) + 1;
        axis = lanePoint(lane);
        if (axis.y() > sideTolerance) axis = -axis;
    }
    if (slideCount < 2 || (!singleHead && !commonAxis(starts, &axis))) {
        plan.diagnostics.append({1, 1, 1,
            QStringLiteral("Screen split requires at least two Slides whose start positions determine one reflection axis.")});
        return plan;
    }
    for (auto& target : targets) {
        for (int branch = 0; branch < target.source->markerCount; ++branch) {
            const auto& marker = *byParseOrder.at(target.source->firstParseOrder + branch);
            target.token.tracks[branch] = splitTrack(marker, axis);
            const auto head = sideHand(sideOf(axis, lanePoint(marker.lane)));
            if (head != HandOrderHand::Unspecified) target.token.head = head;
            else if (target.token.head == HandOrderHand::Unspecified)
                target.token.head = marker.lane >= 5 ? HandOrderHand::Left : HandOrderHand::Right;
        }
        if (!handOrderTokenComplete(target.token)) {
            plan.diagnostics.append({target.source->line, target.source->col, target.source->col + target.source->length - 1,
                QStringLiteral("Screen split cannot resolve this Slide's native trajectory relative to the common axis.")});
            return plan;
        }
    }
    int delta = 0;
    for (const auto& target : targets) {
        const auto& source = *target.source;
        const QString replacement = target.token.baseToken + handOrderSuffix(target.token);
        if (replacement == text.mid(target.start, source.length)) continue;
        plan.replacement.replace(target.start - begin + delta, source.length, replacement);
        plan.patches.append({target.start, target.start + source.length, replacement});
        ++plan.changed;
        delta += replacement.size() - source.length;
    }
    const auto verified = SimaiParser::validateSyntax(text.left(begin) + plan.replacement + text.mid(end), metadata);
    if (!verified.annotationErrors.isEmpty()) {
        plan.replacement = text.mid(begin, end - begin);
        plan.changed = 0;
        plan.patches.clear();
        plan.diagnostics += verified.annotationErrors;
    }
    return plan;
}
} // namespace miacode::chart_transform
