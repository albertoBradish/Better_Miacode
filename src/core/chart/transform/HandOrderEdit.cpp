#include "core/chart/transform/HandOrderEdit.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::chart_transform {
namespace {
enum class EditMode { Assign, Split, Bisect, Swap };

QChar handGlyph(HandOrderHand hand)
{
    switch (hand) {
    case HandOrderHand::Left: return QLatin1Char('l');
    case HandOrderHand::Right: return QLatin1Char('r');
    case HandOrderHand::Either: return QLatin1Char('m');
    default: return {};
    }
}

QString progressText(double progress)
{
    QString text = QString::number(progress, 'g', std::numeric_limits<double>::max_digits10);
    const int exponentAt = text.indexOf(QLatin1Char('e'));
    if (exponentAt >= 0) {
        const int exponent = text.mid(exponentAt + 1).toInt();
        QString digits = text.left(exponentAt);
        int point = digits.indexOf(QLatin1Char('.'));
        if (point < 0) point = digits.size();
        digits.remove(QLatin1Char('.'));
        point += exponent;
        if (point <= 0) text = QStringLiteral("0.") + QString(-point, QLatin1Char('0')) + digits;
        else if (point >= digits.size()) text = digits + QString(point - digits.size(), QLatin1Char('0'));
        else text = digits.left(point) + QLatin1Char('.') + digits.mid(point);
    }
    return text;
}

HandOrderEditPlan planSelection(const QString& text, int begin, int end,
                               HandOrderHand hand, EditMode mode, const miacode::simai::SimaiTimingMetadata& metadata)
{
    const bool split = mode == EditMode::Split;
    const bool bisect = mode == EditMode::Bisect;
    HandOrderEditPlan plan;
    begin = qBound(0, begin, int(text.size()));
    end = qBound(begin, end, int(text.size()));
    plan.replacement = text.mid(begin, end - begin);
    if (begin == end) return plan;
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    if ((split || bisect) && !parsed.errors.isEmpty()) {
        // Native errors can invalidate downstream BPM/duration interpretation.
        plan.diagnostics = parsed.errors;
        return plan;
    }
    QVector<const TimelineNoteMarker*> byParseOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byParseOrder.size())
            byParseOrder[marker.parseOrder] = &marker;
    QVector<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n')) lineStarts.append(i + 1);
    int delta = 0;
    for (const auto& source : parsed.sourceTokens) {
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        if (start >= end || finish <= begin) continue;
        if (start < begin || finish > end) { ++plan.partial; continue; }
        bool invalid = !source.nativeValid;
        for (const auto& error : parsed.annotationErrors) {
            if (error.line == source.line && error.col >= source.col
                && error.col < source.col + source.length) {
                plan.diagnostics.append(error);
                invalid = true;
            }
        }
        if (invalid) {
            for (const auto& error : parsed.errors)
                if (error.line == source.line && error.col >= source.col
                    && error.col < source.col + source.length) plan.diagnostics.append(error);
            continue;
        }
        if (source.markerCount == 0) continue;
        const QString original = text.mid(start, source.length);
        const auto parts = hand_order::splitToken(original);
        QString replacement;
        HandOrderTokenEdit token;
        if (mode == EditMode::Swap) {
            replacement = original;
            // The suffix is already validated. Only hand values follow these
            // delimiters, so spelling, progress precision and modes stay exact.
            for (int i = parts.suffixOffset + 1; parts.suffixOffset >= 0 && i < replacement.size(); ++i) {
                const QChar previous = replacement.at(i - 1);
                if (previous != QLatin1Char('~') && previous != QLatin1Char('=') && previous != QLatin1Char(':')) continue;
                if (replacement.at(i) == QLatin1Char('l')) replacement[i] = QLatin1Char('r');
                else if (replacement.at(i) == QLatin1Char('r')) replacement[i] = QLatin1Char('l');
            }
        } else if (mode == EditMode::Assign) {
            bool explicitWifi = false;
            token.baseToken = parts.baseToken;
            for (int i = 0; i < source.markerCount; ++i) {
                const auto* marker = byParseOrder.value(source.firstParseOrder + i);
                if (!marker || (marker->type != QStringLiteral("slide") && marker->type != QStringLiteral("wifi"))) continue;
                token.hasHead |= marker->hasHeadStar;
                token.head = hand;
                token.tracks.append(marker->handOrderTrackStops);
                token.wifiModes.append(marker->handOrderWifiMode);
                token.wifiSectors.append(marker->handOrderWifiSectorStops);
                token.wifiBranches.append(marker->type == QStringLiteral("wifi"));
                explicitWifi |= marker->handOrderWifiMode != HandOrderWifiMode::Legacy;
                auto assign = [hand](auto& stops) {
                    if (stops.isEmpty()) stops.append({0.0, hand});
                    for (auto& stop : stops) stop.hand = hand;
                };
                if (marker->handOrderWifiMode == HandOrderWifiMode::Split) {
                    for (auto& sector : token.wifiSectors.last()) assign(sector);
                } else assign(token.tracks.last());
            }
            bool cluster = parts.baseToken.size() > 1;
            for (QChar ch : parts.baseToken)
                cluster = cluster && ch >= QLatin1Char('1') && ch <= QLatin1Char('8');
            if (explicitWifi) {
                replacement = token.baseToken + handOrderSuffix(token);
            } else if (cluster) {
                QStringList notes;
                for (QChar ch : parts.baseToken)
                    notes.append(QString(ch) + QLatin1Char('~') + handGlyph(hand));
                replacement = notes.join(QLatin1Char('/'));
            } else {
                replacement = parts.baseToken + QLatin1Char('~') + handGlyph(hand);
            }
        } else {
            token = {start + delta, finish + delta, source.line, source.col,
                     parts.baseToken, false, HandOrderHand::Unspecified, {}};
            bool supported = true;
            for (int i = 0; i < source.markerCount; ++i) {
                const auto* parsedMarker = byParseOrder.value(source.firstParseOrder + i);
                if (!parsedMarker) { supported = false; break; }
                const auto& marker = *parsedMarker;
                if (marker.type != QStringLiteral("slide") && marker.type != QStringLiteral("wifi")) {
                    supported = false;
                    break;
                }
                if (bisect && marker.type != QStringLiteral("slide")) { supported = false; break; }
                if (marker.slideBranchIndex != token.tracks.size()) { supported = false; break; }
                token.hasHead = token.hasHead || marker.hasHeadStar;
                token.head = marker.handOrderHead;
                auto stops = marker.handOrderTrackStops;
                auto sectors = marker.handOrderWifiSectorStops;
                QVector<double> durations;
                // Native tracing clamps each segment to its actual shoot/end timeline.
                for (int segment = 0; segment < marker.slideSegmentShootSeconds.size(); ++segment) {
                    const double finish = segment + 1 < marker.slideSegmentShootSeconds.size()
                        ? marker.slideSegmentShootSeconds.at(segment + 1) : marker.endSecond;
                    durations.append(finish - marker.slideSegmentShootSeconds.at(segment));
                }
                const auto boundaries = handOrderSplitProgresses(durations);
                if (boundaries.isEmpty() || marker.endSecond <= marker.slideTraceSecond) supported = false;
                const auto insertBoundaries = [&](QVector<HandOrderTrackStop>& nodes) {
                  if (nodes.isEmpty()) nodes.append({0.0, HandOrderHand::Unspecified});
                  for (double progress : boundaries) {
                    const auto at = std::lower_bound(nodes.begin(), nodes.end(), progress,
                        [](const auto& stop, double value) { return stop.progress < value; });
                    const bool exists = (at != nodes.end() && std::abs(at->progress - progress) < 1e-12)
                        || (at != nodes.begin() && std::abs((at - 1)->progress - progress) < 1e-12);
                    if (!exists) {
                        const auto inherited = at == nodes.begin() ? nodes.constFirst().hand : (at - 1)->hand;
                        nodes.insert(at, {progress, inherited});
                    }
                  }
                };
                if (bisect) {
                    if (stops.size() < 2) {
                        stops = {{0.0, HandOrderHand::Left}, {0.5, HandOrderHand::Right}};
                    } else {
                        const auto previous = stops;
                        for (int stop = 0; stop < previous.size(); ++stop) {
                            const double first = previous.at(stop).progress;
                            const double last = stop + 1 < previous.size() ? previous.at(stop + 1).progress : 1.0;
                            const double middle = first + (last - first) * 0.5;
                            // A representable midpoint is required for tiny intervals.
                            if (middle > first && middle < last) {
                                const auto at = std::lower_bound(stops.begin(), stops.end(), middle,
                                    [](const auto& node, double value) { return node.progress < value; });
                                stops.insert(at, {middle, previous.at(stop).hand});
                            }
                        }
                    }
                } else if (marker.handOrderWifiMode == HandOrderWifiMode::Split) {
                    for (auto& sector : sectors) insertBoundaries(sector);
                } else {
                    insertBoundaries(stops);
                }
                token.tracks.append(stops);
                token.wifiModes.append(marker.handOrderWifiMode);
                token.wifiSectors.append(sectors);
                token.wifiBranches.append(marker.type == QStringLiteral("wifi"));
            }
            if (!supported || token.tracks.isEmpty()) { ++plan.unsupported; continue; }
            replacement = parts.suffix.isEmpty() && !bisect ? original : token.baseToken + handOrderSuffix(token);
        }
        if (replacement != original) {
            plan.replacement.replace(start - begin + delta, source.length, replacement);
            plan.patches.append({start, finish, replacement});
            ++plan.changed;
        }
        if (split) {
            token.end = token.start + replacement.size();
            plan.intervals.append(token);
        }
        delta += replacement.size() - source.length;
    }
    const auto verified = SimaiParser::validateSyntax(text.left(begin) + plan.replacement + text.mid(end), metadata);
    if (verified.annotationErrors.size() > parsed.annotationErrors.size()) {
        plan.replacement = text.mid(begin, end - begin);
        plan.changed = 0;
        plan.intervals.clear();
        plan.patches.clear();
        plan.diagnostics += verified.annotationErrors;
    }
    return plan;
}
} // namespace

QVector<double> handOrderSplitProgresses(const QVector<double>& movementDurations)
{
    double total = 0;
    for (double duration : movementDurations) {
        if (!std::isfinite(duration) || duration <= 0) return {};
        total += duration;
    }
    if (movementDurations.size() < 2) return {0.5};
    if (!std::isfinite(total) || total <= 0) return {};
    QVector<double> boundaries;
    double elapsed = 0;
    for (int segment = 0; segment + 1 < movementDurations.size(); ++segment) {
        elapsed += movementDurations.at(segment);
        boundaries.append(elapsed / total);
    }
    return boundaries;
}

bool handOrderTokenComplete(const HandOrderTokenEdit& token)
{
    if (token.tracks.isEmpty()) return false;
    if ((!token.wifiModes.isEmpty() && token.wifiModes.size() != token.tracks.size())
        || (!token.wifiSectors.isEmpty() && token.wifiSectors.size() != token.tracks.size())
        || (!token.wifiBranches.isEmpty() && token.wifiBranches.size() != token.tracks.size())) return false;
    const auto valid = [](const QVector<HandOrderTrackStop>& stops) {
        if (stops.isEmpty() || stops.constFirst().progress != 0.0) return false;
        double previous = -1.0;
        for (const auto& stop : stops) {
            if (!std::isfinite(stop.progress) || stop.progress <= previous || stop.progress >= 1.0
                || stop.hand < HandOrderHand::Left || stop.hand > HandOrderHand::Either) return false;
            previous = stop.progress;
        }
        return true;
    };
    for (int branch = 0; branch < token.tracks.size(); ++branch) {
        const auto mode = token.wifiModes.value(branch, HandOrderWifiMode::Legacy);
        if (mode < HandOrderWifiMode::Legacy || mode > HandOrderWifiMode::Single) return false;
        if (mode != HandOrderWifiMode::Legacy && !token.wifiBranches.isEmpty()
            && !token.wifiBranches.value(branch)) return false;
        if (mode == HandOrderWifiMode::Split) {
            const auto sectors = token.wifiSectors.value(branch);
            if (sectors.size() != 2 || !token.tracks.at(branch).isEmpty()
                || !valid(sectors.at(0)) || !valid(sectors.at(1))) return false;
        } else {
            const auto& track = token.tracks.at(branch);
            if (!valid(track)) return false;
            if (mode == HandOrderWifiMode::Single)
                for (const auto& stop : track)
                    if (stop.hand != track.constFirst().hand) return false;
        }
    }
    return true;
}

QString handOrderSuffix(const HandOrderTokenEdit& token)
{
    if (!handOrderTokenComplete(token)) return {};
    QStringList clauses;
    if (token.hasHead && token.head != HandOrderHand::Unspecified)
        clauses.append(QStringLiteral("head=") + handGlyph(token.head));
    for (int branch = 0; branch < token.tracks.size(); ++branch) {
        const auto encode = [](const QVector<HandOrderTrackStop>& stops) {
            QStringList nodes;
            for (const auto& stop : stops)
                nodes.append(progressText(stop.progress) + QLatin1Char(':') + handGlyph(stop.hand));
            return nodes.join(QLatin1Char('|'));
        };
        const auto mode = token.wifiModes.value(branch, HandOrderWifiMode::Legacy);
        QString value;
        if (mode == HandOrderWifiMode::Split) {
            const auto& sectors = token.wifiSectors.at(branch);
            value = QStringLiteral("split:") + encode(sectors.at(0)) + QLatin1Char('&') + encode(sectors.at(1));
        } else {
            value = (mode == HandOrderWifiMode::Single ? QStringLiteral("single:") : QString())
                + encode(token.tracks.at(branch));
        }
        const QString key = token.tracks.size() == 1 ? QStringLiteral("track")
            : QStringLiteral("branch%1").arg(branch);
        clauses.append(key + QLatin1Char('=') + value);
    }
    return QStringLiteral("~[") + clauses.join(QLatin1Char(';')) + QLatin1Char(']');
}

HandOrderEditPlan setHandOrderInSelection(const QString& text, int begin, int end, HandOrderHand hand,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    if (hand == HandOrderHand::Unspecified) return {};
    return planSelection(text, begin, end, hand, EditMode::Assign, metadata);
}
HandOrderEditPlan splitHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    return planSelection(text, begin, end, HandOrderHand::Unspecified, EditMode::Split, metadata);
}
HandOrderEditPlan bisectHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    return planSelection(text, begin, end, HandOrderHand::Unspecified, EditMode::Bisect, metadata);
}
HandOrderEditPlan swapHandOrderInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    return planSelection(text, begin, end, HandOrderHand::Unspecified, EditMode::Swap, metadata);
}
} // namespace miacode::chart_transform
