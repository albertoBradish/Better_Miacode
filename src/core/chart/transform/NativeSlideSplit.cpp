#include "core/chart/transform/NativeSlideSplit.h"

#include "common/TaskCancellation.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QLineF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::chart_transform {
namespace {
struct Piece {
    const TimelineNoteMarker* original = nullptr;
    int segment = -1;
    QVector<HandOrderTrackStop> stops;
};
struct Draft {
    int item = -1, firstOrder = 0, markerCount = 0;
    QVector<Piece> pieces;
};

bool near(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b)
        && std::abs(a - b) <= 1e-8 + qMax(std::abs(a), std::abs(b)) * std::numeric_limits<double>::epsilon() * 8;
}
QString secondsText(double value)
{
    return QString::number(value == 0 ? 0 : value, 'g', std::numeric_limits<double>::max_digits10);
}
double segmentEnd(const TimelineNoteMarker& marker, int index)
{
    return index + 1 < marker.slideSegmentShootSeconds.size() ? marker.slideSegmentShootSeconds.at(index + 1) : marker.endSecond;
}
bool sameStops(const QVector<HandOrderTrackStop>& a, const QVector<HandOrderTrackStop>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).progress != b.at(i).progress || a.at(i).hand != b.at(i).hand) return false;
    return true;
}
bool samePoints(const QVector<QPointF>& a, const QVector<QPointF>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        task::CancellationScope::checkpoint();
        if (QLineF(a.at(i), b.at(i)).length() > 1e-8) return false;
    }
    return true;
}
bool validGeometry(const TimelineNoteMarker& marker)
{
    const int count = marker.slideSegmentKeys.size();
    if (count == 0 || marker.slideSegmentShootSeconds.size() != count || marker.slideSegmentPoints.size() != count
        || !std::isfinite(marker.second) || !std::isfinite(marker.slideTraceSecond) || !std::isfinite(marker.endSecond)
        || !near(marker.slideTraceSecond, marker.slideSegmentShootSeconds.first())) return false;
    for (int segment = 0; segment < count; ++segment) {
        task::CancellationScope::checkpoint();
        const auto& key = marker.slideSegmentKeys.at(segment);
        const double first = marker.slideSegmentShootSeconds.at(segment), end = segmentEnd(marker, segment);
        if (key.size() < 3 || key.at(0) < '1' || key.at(0) > '8' || !std::isfinite(first) || !std::isfinite(end)
            || end <= first || first < marker.second || marker.slideSegmentPoints.at(segment).size() < 2) return false;
        for (const auto& point : marker.slideSegmentPoints.at(segment))
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return false;
    }
    return true;
}

bool clipStops(const TimelineNoteMarker& marker, int segment, QVector<HandOrderTrackStop>* clipped)
{
    clipped->clear();
    if (marker.handOrderTrackStops.isEmpty()) return true;
    // Cancel the common absolute origin before arithmetic. On MSVC long double
    // has double precision; adding the origin first can erase a near-boundary node.
    const long double total = marker.endSecond - marker.slideTraceSecond;
    const long double first = marker.slideSegmentShootSeconds.at(segment) - marker.slideTraceSecond;
    const long double end = segmentEnd(marker, segment) - marker.slideTraceSecond;
    if (total <= 0 || end <= first) return false;
    HandOrderHand inherited = marker.handOrderTrackStops.first().hand;
    for (const auto& stop : marker.handOrderTrackStops) {
        task::CancellationScope::checkpoint();
        const long double second = total * stop.progress;
        if (second <= first) inherited = stop.hand;
    }
    clipped->append({0, inherited});
    for (const auto& stop : marker.handOrderTrackStops) {
        const long double second = total * stop.progress;
        if (second <= first || second >= end) continue;
        const double progress = double((second - first) / (end - first));
        // An unrepresentable near-boundary node must block, never be discarded.
        if (!std::isfinite(progress) || progress <= clipped->last().progress || progress >= 1) return false;
        clipped->append({progress, stop.hand});
    }
    return true;
}

QString bodyText(const TimelineNoteMarker& marker, int segment)
{
    const double first = marker.slideSegmentShootSeconds.at(segment), end = segmentEnd(marker, segment);
    return marker.slideSegmentKeys.at(segment).mid(1)
        + (marker.trackMine ? QStringLiteral("m") : QString())
        + (marker.trackBreak ? QStringLiteral("b") : QString())
        + '[' + secondsText(first - marker.second) + QStringLiteral("##") + secondsText(end - first) + ']';
}
int headCount(const QVector<const TimelineNoteMarker*>& markers)
{
    QSet<QString> heads;
    for (const auto* marker : markers) {
        if (!marker || !marker->hasHeadStar || (marker->type != "slide" && marker->type != "wifi")) continue;
        heads.insert(QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(secondsText(marker->second)).arg(marker->eachGroupId).arg(marker->lane)
            .arg(marker->headBreak).arg(marker->headEx).arg(marker->headMine).arg(marker->slideHeadUsesTapMaterial));
    }
    return heads.size();
}

bool matches(const Piece& piece, const TimelineNoteMarker& actual)
{
    const auto& old = *piece.original;
    if (!near(old.second, actual.second) || !near(old.hsMultiplier, actual.hsMultiplier)) return false;
    if (piece.segment < 0) {
        if (old.type != actual.type || old.lane != actual.lane || old.endLane != actual.endLane
            || !near(old.endSecond, actual.endSecond) || !near(old.slideTraceSecond, actual.slideTraceSecond)
            || old.touchPad != actual.touchPad || old.hasHeadStar != actual.hasHeadStar
            || old.headlessImmediate != actual.headlessImmediate || old.headBreak != actual.headBreak
            || old.headEx != actual.headEx || old.headMine != actual.headMine
            || old.slideHeadUsesTapMaterial != actual.slideHeadUsesTapMaterial || old.trackBreak != actual.trackBreak
            || old.trackMine != actual.trackMine || old.isMine != actual.isMine || old.isBreak != actual.isBreak
            || old.isEx != actual.isEx || old.handOrderHead != actual.handOrderHead
            || !sameStops(old.handOrderTrackStops, actual.handOrderTrackStops)
            || old.slideSegmentKeys != actual.slideSegmentKeys || old.handOrderWifiMode != actual.handOrderWifiMode
            || old.handOrderWifiSectorStops.size() != actual.handOrderWifiSectorStops.size()) return false;
        for (int i = 0; i < old.handOrderWifiSectorStops.size(); ++i)
            if (!sameStops(old.handOrderWifiSectorStops.at(i), actual.handOrderWifiSectorStops.at(i))) return false;
        if (old.slideSegmentPoints.size() != actual.slideSegmentPoints.size()) return false;
        for (int i = 0; i < old.slideSegmentPoints.size(); ++i)
            if (!samePoints(old.slideSegmentPoints.at(i), actual.slideSegmentPoints.at(i))
                || !near(old.slideSegmentShootSeconds.at(i), actual.slideSegmentShootSeconds.at(i))
                || !near(segmentEnd(old, i), segmentEnd(actual, i))) return false;
        return true;
    }
    const int index = piece.segment;
    const bool first = index == 0;
    if (actual.type != "slide" || actual.slideSegmentKeys != QStringList{old.slideSegmentKeys.at(index)}
        || actual.slideSegmentPoints.size() != 1 || !samePoints(old.slideSegmentPoints.at(index), actual.slideSegmentPoints.first())
        || !near(old.slideSegmentShootSeconds.at(index), actual.slideTraceSecond)
        || !near(segmentEnd(old, index), actual.endSecond)
        || actual.hasHeadStar != (first && old.hasHeadStar) || actual.headlessImmediate != old.headlessImmediate
        || actual.headBreak != (first && old.headBreak) || actual.headEx != (first && old.headEx)
        || actual.headMine != (first && old.headMine) || actual.slideHeadUsesTapMaterial != (first && old.slideHeadUsesTapMaterial)
        || actual.trackBreak != old.trackBreak || actual.trackMine != old.trackMine
        || actual.handOrderHead != (first ? old.handOrderHead : HandOrderHand::Unspecified)
        || !sameStops(piece.stops, actual.handOrderTrackStops)) return false;
    return true;
}
} // namespace

NativeSlideSplitPlan planNativeSlideSplit(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    NativeSlideSplitPlan plan;
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    plan.replacement = text.mid(begin, end - begin);
    if (begin == end) { plan.blockers.append(QStringLiteral("empty_selection")); return plan; }
    task::CancellationScope::check();
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    plan.diagnostics = parsed.errors;
    if (!parsed.errors.isEmpty()) plan.blockers.append(QStringLiteral("invalid_chart"));
    QVector<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') lineStarts.append(i + 1);
    QVector<const TimelineNoteMarker*> byOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byOrder.size()) byOrder[marker.parseOrder] = &marker;
    QVector<Draft> drafts;
    QVector<HandOrderEditPlan::Patch> patches;
    QVector<const TimelineNoteMarker*> selectedHeads;
    const auto block = [&](NativeSlideSplitItem& item, const QString& reason) {
        item.status = QStringLiteral("unsupported"); item.reason = reason; ++plan.unsupported;
        if (!plan.blockers.contains(reason)) plan.blockers.append(reason);
    };
    for (const auto& source : parsed.sourceTokens) {
        task::CancellationScope::checkpoint();
        if (source.line <= 0 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1, finish = start + source.length;
        if (start >= end || finish <= begin) continue;
        if (source.nativeValid && source.markerCount == 0) continue;
        NativeSlideSplitItem item;
        item.start = start; item.end = finish; item.line = source.line; item.col = source.col;
        item.before = text.mid(start, source.length); item.after = item.before;
        if (start < begin || finish > end) {
            item.status = QStringLiteral("partial"); item.reason = QStringLiteral("partial_token"); ++plan.partial;
            plan.items.append(item); continue;
        }
        ++plan.selected;
        if (!parsed.errors.isEmpty() || !source.nativeValid) {
            block(item, QStringLiteral("invalid_chart"));
            plan.items.append(item); continue;
        }
        QVector<const TimelineNoteMarker*> branches;
        for (int i = 0; i < source.markerCount; ++i) branches.append(byOrder.value(source.firstParseOrder + i));
        bool slide = false, wifi = false, missing = false, chain = false;
        for (const auto* marker : branches) {
            missing |= marker == nullptr; if (!marker) continue;
            slide |= marker->type == "slide"; wifi |= marker->type == "wifi"; chain |= marker->slideSegmentKeys.size() > 1;
        }
        if (!slide && !wifi && !missing) {
            item.status = QStringLiteral("ignored"); item.reason = QStringLiteral("non_slide"); ++plan.ignored;
            plan.items.append(item); continue;
        }
        bool invalidAnnotation = false;
        for (const auto& error : parsed.annotationErrors) if (error.line == source.line && error.col >= source.col
            && error.col < source.col + source.length) { plan.diagnostics.append(error); invalidAnnotation = true; }
        if (invalidAnnotation) { block(item, QStringLiteral("invalid_annotation")); plan.items.append(item); continue; }
        item.beforeTracks = item.afterTracks = branches.size(); item.beforeHeads = item.afterHeads = headCount(branches);
        plan.beforeTracks += item.beforeTracks; plan.afterTracks += item.afterTracks;
        selectedHeads += branches; plan.beforeHeads = plan.afterHeads = headCount(selectedHeads);
        if (wifi || missing || !chain) {
            block(item, wifi ? QStringLiteral("wifi_not_supported") : missing ? QStringLiteral("missing_markers") : QStringLiteral("single_segment"));
            plan.items.append(item); continue;
        }
        bool reliable = true;
        for (const auto* marker : branches) reliable &= marker->type == "slide" && validGeometry(*marker);
        if (!reliable) { block(item, QStringLiteral("invalid_geometry")); plan.items.append(item); continue; }
        const auto parts = hand_order::splitToken(item.before);
        int shapeAt = 1;
        while (shapeAt < parts.baseToken.size() && !QStringLiteral("-^v<>VpqszwW").contains(parts.baseToken.at(shapeAt))) ++shapeAt;
        if (shapeAt >= parts.baseToken.size()) { block(item, QStringLiteral("unrepresentable_native")); plan.items.append(item); continue; }
        QStringList firstBodies, laterTokens;
        HandOrderTokenEdit firstAnnotation;
        firstAnnotation.hasHead = branches.first()->handOrderHead != HandOrderHand::Unspecified;
        firstAnnotation.head = branches.first()->handOrderHead;
        Draft draft; draft.item = plan.items.size(); draft.firstOrder = source.firstParseOrder; draft.markerCount = source.markerCount;
        for (const auto* marker : branches) {
            QVector<HandOrderTrackStop> stops;
            reliable &= clipStops(*marker, 0, &stops);
            firstBodies.append(bodyText(*marker, 0)); firstAnnotation.tracks.append(stops);
            draft.pieces.append({marker, 0, stops});
        }
        QString firstToken = parts.baseToken.left(shapeAt) + firstBodies.join('*');
        if (!parts.suffix.isEmpty()) {
            firstAnnotation.baseToken = firstToken;
            const auto suffix = handOrderSuffix(firstAnnotation); reliable &= !suffix.isEmpty(); firstToken += suffix;
        }
        for (const auto* marker : branches) for (int segment = 1; segment < marker->slideSegmentKeys.size(); ++segment) {
            QVector<HandOrderTrackStop> stops; reliable &= clipStops(*marker, segment, &stops);
            QString token = marker->slideSegmentKeys.at(segment).left(1)
                + (marker->headlessImmediate ? QStringLiteral("!") : QStringLiteral("?")) + bodyText(*marker, segment);
            if (!parts.suffix.isEmpty()) {
                HandOrderTokenEdit annotation; annotation.baseToken = token; annotation.tracks = {stops};
                const auto suffix = handOrderSuffix(annotation); reliable &= !suffix.isEmpty(); token += suffix;
            }
            laterTokens.append(token); draft.pieces.append({marker, segment, stops});
        }
        if (!reliable) { block(item, QStringLiteral("hand_order_unrepresentable")); plan.items.append(item); continue; }
        item.after = firstToken + '/' + laterTokens.join('/');
        item.status = QStringLiteral("ready"); item.reason = QStringLiteral("native_segments");
        item.afterTracks = draft.pieces.size(); item.afterHeads = item.beforeHeads;
        item.changes = {QStringLiteral("independent_judgement_boundaries"), QStringLiteral("slide_count_changes"),
            QStringLiteral("availability_may_change"), QStringLiteral("scoring_may_change"), QStringLiteral("each_may_change"),
            QStringLiteral("disappearance_may_change"), QStringLiteral("sfx_may_change"), QStringLiteral("muri_may_change")};
        bool breaks = false; for (const auto* marker : branches) breaks |= marker->trackBreak;
        if (breaks) item.changes.append(QStringLiteral("break_count_scoring_changes"));
        plan.afterTracks += item.afterTracks - item.beforeTracks; ++plan.changed;
        patches.append({start, finish, item.after}); drafts.append(draft); plan.items.append(item);
    }
    if (patches.isEmpty()) {
        if (plan.blockers.isEmpty()) plan.blockers.append(QStringLiteral("no_chain_selected"));
        return plan;
    }
    if (!plan.blockers.isEmpty()) return plan;
    QString candidate = text;
    for (int i = patches.size() - 1; i >= 0; --i) candidate.replace(patches.at(i).start, patches.at(i).end - patches.at(i).start, patches.at(i).text);
    const auto verified = SimaiParser::validateSyntax(candidate, metadata);
    // Existing annotations outside converted tokens are preserved verbatim.
    // Their diagnostics may move by a patch's UTF-16 length, but no new error
    // is allowed and no existing error may silently disappear.
    const auto annotationErrors = [&](const QVector<SimaiMessage>& errors, bool remap) {
        QMap<QString, int> keys;
        for (const auto& error : errors) {
            int delta = 0;
            if (remap && error.line > 0 && error.line <= lineStarts.size()) {
                const int absolute = lineStarts.at(error.line - 1) + error.col - 1;
                for (const auto& patch : patches) if (patch.end <= absolute
                    && text.lastIndexOf('\n', patch.start) == text.lastIndexOf('\n', absolute)) delta += patch.text.size() - (patch.end - patch.start);
            }
            ++keys[QStringLiteral("%1|%2|%3|%4").arg(error.line).arg(error.col + delta).arg(error.endCol + delta).arg(error.message)];
        }
        return keys;
    };
    if (!verified.errors.isEmpty()
        || annotationErrors(parsed.annotationErrors, true) != annotationErrors(verified.annotationErrors, false)) {
        plan.diagnostics += verified.errors; plan.diagnostics += verified.annotationErrors;
        plan.blockers.append(QStringLiteral("round_trip_invalid")); return plan;
    }
    // The parser sorts noteMarkers by time/lane after emission. Source-token
    // ranges and the new text's emitted pieces instead use stable parseOrder.
    QVector<const TimelineNoteMarker*> verifiedByOrder(verified.noteMarkers.size(), nullptr);
    for (const auto& marker : verified.noteMarkers) {
        if (marker.parseOrder < 0 || marker.parseOrder >= verifiedByOrder.size() || verifiedByOrder.at(marker.parseOrder)) {
            plan.blockers.append(QStringLiteral("missing_markers")); return plan;
        }
        verifiedByOrder[marker.parseOrder] = &marker;
    }
    QVector<Piece> expected;
    for (int order = 0; order < byOrder.size();) {
        task::CancellationScope::checkpoint();
        const auto at = std::find_if(drafts.cbegin(), drafts.cend(), [&](const Draft& draft) { return draft.firstOrder == order; });
        if (at != drafts.cend()) { expected += at->pieces; order += at->markerCount; }
        else { if (!byOrder.at(order)) { plan.blockers.append(QStringLiteral("missing_markers")); return plan; } expected.append({byOrder.at(order), -1, {}}); ++order; }
    }
    bool equivalentGeometry = expected.size() == verified.noteMarkers.size();
    for (int i = 0; equivalentGeometry && i < expected.size(); ++i) {
        task::CancellationScope::checkpoint();
        equivalentGeometry &= verifiedByOrder.at(i) && matches(expected.at(i), *verifiedByOrder.at(i));
        if (!equivalentGeometry) {
            const auto& wanted = *expected.at(i).original;
            SimaiMessage diagnostic; diagnostic.line = wanted.sourceLine; diagnostic.col = wanted.sourceCol;
            diagnostic.endCol = wanted.sourceCol;
            const auto* actual = verifiedByOrder.at(i);
            diagnostic.message = QStringLiteral("Native split round-trip mismatch at emitted marker %1: source %2 lane %3, result %4 lane %5")
                .arg(i).arg(wanted.type).arg(wanted.lane).arg(actual ? actual->type : QStringLiteral("missing")).arg(actual ? actual->lane : -1);
            plan.diagnostics.append(diagnostic);
        }
    }
    if (!equivalentGeometry) { plan.blockers.append(QStringLiteral("round_trip_mismatch")); return plan; }
    QVector<const TimelineNoteMarker*> originalHeads, resultingHeads;
    for (const auto& draft : drafts) {
        auto& item = plan.items[draft.item]; QVector<const TimelineNoteMarker*> before, after;
        for (int order = draft.firstOrder; order < draft.firstOrder + draft.markerCount; ++order) before.append(byOrder.at(order));
        for (int i = 0; i < expected.size(); ++i)
            if (expected.at(i).segment >= 0 && before.contains(expected.at(i).original)) after.append(verifiedByOrder.at(i));
        item.beforeHeads = headCount(before); item.afterHeads = headCount(after);
        if (item.beforeHeads != item.afterHeads) { plan.blockers.append(QStringLiteral("head_count_mismatch")); return plan; }
        originalHeads += before; resultingHeads += after;
    }
    plan.beforeHeads = headCount(originalHeads); plan.afterHeads = headCount(resultingHeads);
    if (plan.beforeHeads != plan.afterHeads) { plan.blockers.append(QStringLiteral("head_count_mismatch")); return plan; }
    plan.replacement = text.mid(begin, end - begin);
    for (int i = patches.size() - 1; i >= 0; --i)
        plan.replacement.replace(patches.at(i).start - begin, patches.at(i).end - patches.at(i).start, patches.at(i).text);
    plan.patches = patches; plan.canApply = true;
    return plan;
}
} // namespace miacode::chart_transform
