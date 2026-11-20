#include "core/chart/transform/HandOrderAlternation.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderRhythm.h"
#include "core/chart/transform/HandOrderInteraction.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace miacode::chart_transform {
namespace {
using Hand = HandOrderHand;

Hand naturalHand(const TimelineNoteMarker& note)
{
    const bool left = note.type.startsWith(QStringLiteral("touch")) ? note.touchPoint.x() < 270 : note.lane >= 5;
    return left ? Hand::Left : Hand::Right;
}

struct SelectedToken {
    int start;
    const SimaiSourceToken* source;
    const TimelineNoteMarker* note;
    Hand hand;
};

bool simultaneousHead(const TimelineNoteMarker& first, const TimelineNoteMarker& second)
{
    return std::abs(first.second - second.second) < 1e-7
        && (first.sourceLine != second.sourceLine || first.sourceCol != second.sourceCol
            || !miacode::chart::handOrderHasStarStrike(first) || !miacode::chart::handOrderHasStarStrike(second));
}
} // namespace

HandOrderEditPlan alternateHandOrderInSelection(const QString& text,
    const miacode::chart_selection::NoteRanges& selected,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    HandOrderEditPlan plan;
    const auto ranges = miacode::chart_selection::normalizedNoteRanges(selected, text.size());
    if (ranges.isEmpty()) return plan;
    const int begin = ranges.first().start, end = ranges.last().end;
    plan.replacement = text.mid(begin, end - begin);
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    if (!parsed.errors.isEmpty() || !parsed.annotationErrors.isEmpty()) {
        plan.diagnostics = parsed.errors + parsed.annotationErrors;
        return plan;
    }
    QVector<int> lines{0}, commas;
    for (int i = 0; i < text.size(); ++i)
        if (text[i] == QLatin1Char('\n')) lines.append(i + 1);
    for (const auto& span : miacode::simai::chartContentSpans(text, begin, end))
        for (int i = span.start; i < span.end; ++i)
            if (text[i] == QLatin1Char(',')) commas.append(i);
    QVector<const TimelineNoteMarker*> markers(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < markers.size()) markers[marker.parseOrder] = &marker;
    int firstSlot = -1;
    Hand firstHand = Hand::Unspecified;
    QVector<SelectedToken> selectedTokens;
    QHash<int, int> selectedByOrder;
    for (const auto& token : parsed.sourceTokens) {
        const int start = lines.at(token.line - 1) + token.col - 1, finish = start + token.length;
        const auto range = std::lower_bound(ranges.cbegin(), ranges.cend(), start,
            [](const auto& r, int position) { return r.end <= position; });
        if (range == ranges.cend() || range->start >= finish) continue;
        if (start < range->start || finish > range->end) { ++plan.partial; continue; }
        const auto* marker = markers.value(token.firstParseOrder);
        if (!marker || !token.markerCount || marker->isMine || marker->headMine) { ++plan.unsupported; continue; }
        const int slot = std::lower_bound(commas.cbegin(), commas.cend(), start) - commas.cbegin();
        if (firstSlot < 0) {
            firstSlot = slot;
            firstHand = naturalHand(*marker);
        }
        const Hand hand = hand_order::interactionHand(firstHand, (slot - firstSlot) % 2 != 0);
        selectedByOrder.insert(marker->parseOrder, selectedTokens.size());
        selectedTokens.append({start, &token, marker, hand});
    }

    QVector<const TimelineNoteMarker*> stream;
    QSet<int> simultaneousOrders;
    for (int i = 0; i < parsed.noteMarkers.size(); ++i) {
        const auto& note = parsed.noteMarkers.at(i);
        const int selectedIndex = selectedByOrder.value(note.parseOrder, -1);
        if ((note.type == "slide" || note.type == "wifi") && note.slideBranchIndex > 0) continue;
        const bool simultaneous = (i > 0 && simultaneousHead(note, parsed.noteMarkers.at(i - 1)))
            || (i + 1 < parsed.noteMarkers.size() && simultaneousHead(note, parsed.noteMarkers.at(i + 1)));
        if (simultaneous) simultaneousOrders.insert(note.parseOrder);
        if (selectedIndex < 0 || simultaneous || (selectedTokens.at(selectedIndex).source->markerCount != 1
            && !miacode::chart::handOrderHasStarStrike(note))) stream.append(nullptr);
        else stream.append(&note);
    }
    auto interactions = hand_order::findTriangleInteractions(stream);
    QSet<int> triangleHeads;
    for (const auto& group : interactions) for (const auto& hit : group) triangleHeads.insert(hit.note->parseOrder);
    for (auto& note : stream) if (note && triangleHeads.contains(note->parseOrder)) note = nullptr;
    interactions += hand_order::findTrillInteractions(stream);
    std::sort(interactions.begin(), interactions.end(), [](const auto& a, const auto& b) {
        return a.first().note->second < b.first().note->second;
    });
    if (!interactions.isEmpty()) {
        auto context = parsed.noteMarkers;
        QHash<int, int> contextByOrder;
        for (int i = 0; i < context.size(); ++i) contextByOrder.insert(context.at(i).parseOrder, i);
        for (const auto& token : selectedTokens) {
            for (int order = token.note->parseOrder; order < token.note->parseOrder + token.source->markerCount; ++order) {
                auto& note = context[contextByOrder.value(order)];
                note.handOrderHead = token.hand;
                if ((note.type == "slide" || note.type == "wifi") && note.handOrderTrackStops.isEmpty()
                    && note.handOrderWifiSectorStops.isEmpty() && !note.hasHeadStar) {
                    note.handOrderTrackStops = {{0.0, token.hand}};
                    note.handOrderWifiMode = HandOrderWifiMode::Legacy;
                }
            }
        }
        const auto sustained = hand_order::sustainedInteractionContext(context);
        QVector<hand_order::InteractionPhases> phases;
        for (const auto& group : interactions) {
            phases.append({group,
                hand_order::interactionPhaseFits(group, Hand::Left, sustained),
                hand_order::interactionPhaseFits(group, Hand::Right, sustained),
                naturalHand(*group.first().note)});
        }
        auto firstHands = hand_order::reviewInteractionPhases(phases, context, chart::handOrderDefaultReturnSpeed);
        bool changed = false;
        do {
            changed = false;
            for (int i = 0; i < interactions.size(); ++i) for (const auto& step : interactions.at(i)) {
                auto& note = context[contextByOrder.value(step.note->parseOrder)];
                const auto hand = firstHands.at(i);
                note.handOrderHead = hand_order::concreteHoldHand(hand)
                    ? hand_order::interactionHand(hand, step.opposite) : step.note->handOrderHead;
            }
            for (int i = 0; i < interactions.size(); ++i) {
                if (!hand_order::concreteHoldHand(firstHands.at(i))
                    || hand_order::interactionPhaseFitsCombined(interactions.at(i), firstHands.at(i), context, sustained)) continue;
                firstHands[i] = Hand::Unspecified;
                changed = true;
            }
        } while (changed);
        for (int i = 0; i < interactions.size(); ++i) for (const auto& step : interactions.at(i)) {
            selectedTokens[selectedByOrder.value(step.note->parseOrder)].hand = hand_order::interactionHand(firstHands.at(i), step.opposite);
        }
    }

    const auto fixedContacts = hand_order::sustainedInteractionContext(parsed.noteMarkers);
    for (qsizetype i = 1; i < selectedTokens.size(); ++i) {
        const auto& previous = selectedTokens.at(i - 1);
        auto& target = selectedTokens[i];
        if (!previous.source->nativeValid || !target.source->nativeValid
            || !miacode::chart::handOrderSameKeyRebound(*previous.note, *target.note)
            || !hand_order::concreteHoldHand(previous.hand)) continue;
        // Independent notes between the selected objects still define context.
        const int prior = previous.note->parseOrder + previous.source->markerCount;
        if (target.note->parseOrder != prior || triangleHeads.contains(target.note->parseOrder)
            || simultaneousOrders.contains(previous.note->parseOrder) || simultaneousOrders.contains(target.note->parseOrder)
            || target.note->isEach || target.note->headEach || previous.note->isEach || previous.note->headEach) continue;
        if (hand_order::interactionPhaseFits({{target.note, false}}, previous.hand, fixedContacts)) target.hand = previous.hand;
    }

    int delta = 0;
    for (const auto& selectedToken : selectedTokens) {
        const auto& token = *selectedToken.source;
        const int start = selectedToken.start, finish = start + token.length;
        const Hand hand = selectedToken.hand;
        if (hand == Hand::Unspecified) { ++plan.unsupported; continue; }
        const auto base = miacode::hand_order::splitToken(text.mid(start, token.length)).baseToken;
        const QString suffix = hand == Hand::Left ? QStringLiteral("~l") : QStringLiteral("~r");
        QString replacement = base + suffix;
        const bool cluster = base.size() > 1 && std::all_of(base.cbegin(), base.cend(), [](QChar c) {
            return c >= QLatin1Char('1') && c <= QLatin1Char('8');
        });
        if (miacode::chart::handOrderHasStarStrike(*selectedToken.note)) {
            HandOrderTokenEdit edit;
            edit.hasHead = true;
            edit.head = hand;
            bool hasTracks = true;
            for (int branch = 0; branch < token.markerCount; ++branch) {
                const auto* marker = markers.value(token.firstParseOrder + branch);
                if (!marker) { hasTracks = false; break; }
                const bool wifi = marker->type == "wifi";
                edit.wifiBranches.append(wifi);
                edit.wifiModes.append(marker->handOrderWifiMode);
                edit.wifiSectors.append(marker->handOrderWifiSectorStops);
                edit.tracks.append(marker->handOrderTrackStops);
                hasTracks &= !marker->handOrderTrackStops.isEmpty() || marker->handOrderWifiMode == HandOrderWifiMode::Split;
            }
            if (hasTracks) replacement = base + handOrderSuffix(edit);
        } else if (cluster) {
            QStringList notes;
            for (QChar lane : base) notes.append(QString(lane) + suffix);
            replacement = notes.join(QLatin1Char('/'));
        }
        if (replacement == text.mid(start, token.length)) continue;
        plan.replacement.replace(start - begin + delta, token.length, replacement);
        plan.patches.append({start, finish, replacement});
        delta += replacement.size() - token.length;
        ++plan.changed;
    }
    return plan;
}
}
