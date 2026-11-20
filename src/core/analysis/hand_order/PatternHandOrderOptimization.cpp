#include "core/analysis/hand_order/PatternHandOrderOptimization.h"

#include "common/TaskCancellation.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/model/HandOrderRhythm.h"
#include "core/chart/transform/HandOrderInteraction.h"
#include <algorithm>
#include <array>
#include <QHash>
#include <QSet>

namespace miacode::hand_order {
namespace {
using Hand = HandOrderHand;

const PatternVariant* phaseVariant(const PatternSuggestion& group)
{
    for (const auto& variant : group.variants)
        if (variant.choiceKey == "alternate" || variant.choiceKey == "mirror") return &variant;
    return nullptr;
}

void retainCoherentSweepSplits(QVector<PatternSuggestion>& suggestions, const QVector<TimelineNoteMarker>& chart)
{
    QHash<int, Hand> hands;
    for (const auto& note : chart) hands.insert(note.parseOrder, note.handOrderHead);
    for (auto& group : suggestions) {
        if ((group.registryId != "T28" && group.registryId != "T30") || group.preferred < 0) continue;
        const auto& notes = group.variants.at(group.preferred).markers;
        if (notes.size() < 3 || notes.size() > 4) continue;
        if (notes.last().second - notes.first().second < 0.10 * (notes.size() - 1)) continue;
        const bool startsLeft = notes.first().lane >= 5;
        if (std::all_of(notes.cbegin(), notes.cend(), [&](const auto& note) { return (note.lane >= 5) == startsLeft; })) continue;
        const Hand first = hands.value(notes.first().parseOrder);
        Hand previous = first;
        int switches = 0;
        bool assigned = concreteHoldHand(first);
        for (int i = 1; i < notes.size(); ++i) {
            const Hand hand = hands.value(notes.at(i).parseOrder);
            assigned &= concreteHoldHand(hand);
            switches += hand != previous;
            previous = hand;
        }
        if (assigned && switches == 1) group.preferred = -1;
    }
}

void reviewSlideLaunchDefaults(QVector<PatternSuggestion>& suggestions, const QVector<TimelineNoteMarker>& chart)
{
    QHash<int, const TimelineNoteMarker*> byOrder;
    for (const auto& note : chart) byOrder.insert(note.parseOrder, &note);
    for (auto& group : suggestions) {
        if (group.registryId != "S01" || group.preferred < 0) continue;
        // Context search may already have settled the phase of the launch.
        const auto& current = group.variants.at(group.preferred).markers;
        const bool existingTracks = std::any_of(current.cbegin(), current.cend(), [&](const auto& note) {
            return note.type == "slide" && !byOrder.value(note.parseOrder)->handOrderTrackStops.isEmpty();
        });
        if (!existingTracks) continue;
        int best = group.preferred;
        int bestMatches = -1;
        for (int i = 0; i < group.variants.size(); ++i) {
            const auto& variant = group.variants.at(i);
            if (i != group.preferred && variant.choiceKey != "mirror") continue;
            int matches = 0;
            for (const auto& note : variant.markers) {
                const auto& existing = *byOrder.value(note.parseOrder);
                if (concreteHoldHand(existing.handOrderHead) && existing.handOrderHead == note.handOrderHead) ++matches;
                if (!existing.handOrderTrackStops.isEmpty() && !note.handOrderTrackStops.isEmpty()
                    && existing.handOrderTrackStops.first().hand == note.handOrderTrackStops.first().hand) ++matches;
            }
            if (matches > bestMatches || (matches == bestMatches && i == group.preferred)) {
                best = i;
                bestMatches = matches;
            }
        }
        group.preferred = best;
    }
}

void reviewInteractionDefaults(QVector<PatternSuggestion>& suggestions,
    const QVector<TimelineNoteMarker>& chart, double returnSpeed)
{
    auto context = chart;
    QHash<int, int> byOrder;
    for (int i = 0; i < context.size(); ++i) byOrder.insert(context.at(i).parseOrder, i);
    // Other configuration defaults provide the entry and exit contacts.
    for (const auto& group : suggestions) {
        if (group.preferred < 0 || group.interactionPhase != InteractionPhaseMode::None) continue;
        for (const auto& note : group.variants.at(group.preferred).markers)
            context[byOrder.value(note.parseOrder)] = note;
    }
    const auto sustained = sustainedInteractionContext(context);

    struct Entry { int suggestion = 0; std::array<int, 2> variants{-1, -1}; };
    QVector<Entry> entries;
    QVector<InteractionPhases> phases;
    for (int i = 0; i < suggestions.size(); ++i) {
        task::CancellationScope::checkpoint();
        auto& group = suggestions[i];
        if (group.interactionPhase == InteractionPhaseMode::None) continue;
        const auto* model = phaseVariant(group);
        if (!model || group.preferred < 0) continue;
        Entry entry{i};
        InteractionPhases phase;
        const Hand first = model->markers.first().handOrderHead;
        for (const auto& note : model->markers)
            phase.steps.append({&note, note.handOrderHead != first});
        phase.preferred = model->markers.first().lane >= 5 ? Hand::Left : Hand::Right;
        for (int variant = 0; variant < group.variants.size(); ++variant) {
            const auto& choice = group.variants.at(variant);
            if (choice.choiceKey != "alternate" && choice.choiceKey != "mirror") continue;
            const Hand phaseHand = choice.markers.first().handOrderHead;
            if (interactionPhaseFits(phase.steps, phaseHand, sustained))
                entry.variants[phaseHand == Hand::Left ? 0 : 1] = variant;
        }
        phase.allowLeft = entry.variants[0] >= 0;
        phase.allowRight = entry.variants[1] >= 0;
        if (!phase.allowLeft && !phase.allowRight) { group.preferred = -1; continue; }

        // Keep the incoming phase preference only when the entire interaction
        // already agrees with it.
        bool complete = true, alternating = true;
        const Hand current = chart.at(byOrder.value(model->markers.first().parseOrder)).handOrderHead;
        for (const auto& step : phase.steps) {
            const auto hand = chart.at(byOrder.value(step.note->parseOrder)).handOrderHead;
            complete &= concreteHoldHand(hand);
            alternating &= hand == interactionHand(current, step.opposite);
        }
        if (complete && alternating) {
            phase.preferred = current;
            const bool whollyCrossed = std::all_of(phase.steps.cbegin(), phase.steps.cend(), [&](const auto& step) {
                const Hand natural = step.note->lane >= 5 ? Hand::Left : Hand::Right;
                return interactionHand(current, step.opposite) != natural;
            });
            if (group.interactionPhase == InteractionPhaseMode::Alternating
                && !whollyCrossed && entry.variants[current == Hand::Left ? 0 : 1] >= 0) {
                // The contextual search has already chosen a feasible phase
                // for every stroke in this contiguous interaction.
                phase.allowLeft = current == Hand::Left;
                phase.allowRight = current == Hand::Right;
            }
        }
        entries.append(entry);
        phases.append(std::move(phase));
    }
    const auto selected = reviewInteractionPhases(phases, context, returnSpeed);
    for (int i = 0; i < entries.size(); ++i) {
        const auto& entry = entries.at(i);
        const Hand hand = selected.at(i);
        suggestions[entry.suggestion].preferred = concreteHoldHand(hand)
            ? entry.variants[hand == Hand::Left ? 0 : 1] : -1;
    }
    bool removed = false;
    do {
        removed = false;
        for (int i = 0; i < entries.size(); ++i) {
            const auto& group = suggestions.at(entries.at(i).suggestion);
            const Hand first = group.preferred >= 0 ? selected.at(i) : Hand::Unspecified;
            for (const auto& step : phases.at(i).steps) {
                const int index = byOrder.value(step.note->parseOrder);
                context[index].handOrderHead = concreteHoldHand(first)
                    ? interactionHand(first, step.opposite) : chart.at(index).handOrderHead;
            }
        }
        for (int i = 0; i < entries.size(); ++i) {
            auto& group = suggestions[entries.at(i).suggestion];
            if (group.preferred < 0
                || interactionPhaseFitsCombined(phases.at(i).steps, selected.at(i), context, sustained)) continue;
            group.preferred = -1;
            removed = true;
        }
    } while (removed);
}

void retainCoherentBoundaryEdges(QVector<PatternSuggestion>& suggestions, const QVector<TimelineNoteMarker>& chart)
{
    QHash<int, int> groupsAt;
    QHash<int, Hand> output;
    QSet<int> phasedHeads;
    for (int i = 0; i < suggestions.size(); ++i) {
        const auto& group = suggestions.at(i);
        if (group.preferred < 0) continue;
        for (const auto& note : group.variants.at(group.preferred).markers) {
            groupsAt.insert(note.parseOrder, i);
            output.insert(note.parseOrder, note.handOrderHead);
            if (group.interactionPhase != InteractionPhaseMode::None) phasedHeads.insert(note.parseOrder);
        }
    }
    for (int i = 1; i < chart.size(); ++i) {
        task::CancellationScope::checkpoint();
        const auto& previous = chart.at(i - 1);
        const auto& next = chart.at(i);
        if (!concreteHoldHand(previous.handOrderHead) || !concreteHoldHand(next.handOrderHead)) continue;
        const int first = groupsAt.value(previous.parseOrder, -1), second = groupsAt.value(next.parseOrder, -1);
        if (first == second || (!phasedHeads.contains(previous.parseOrder) && !phasedHeads.contains(next.parseOrder))) continue;
        const int freeGroup = phasedHeads.contains(previous.parseOrder) ? second : first;
        if (freeGroup < 0 || suggestions.at(freeGroup).preferred < 0
            || suggestions.at(freeGroup).interactionPhase != InteractionPhaseMode::None) continue;
        if (!chart::handOrderIsButtonStrike(previous) || !chart::handOrderIsButtonStrike(next)
            || !chart::handOrderRegularInputStep(previous, next, true)
            || previous.lane == next.lane || previous.handOrderHead == next.handOrderHead) continue;
        const auto firstHand = output.value(previous.parseOrder, previous.handOrderHead);
        const auto secondHand = output.value(next.parseOrder, next.handOrderHead);
        if (firstHand != secondHand) continue;
        for (const auto& note : suggestions.at(freeGroup).variants.at(suggestions.at(freeGroup).preferred).markers)
            output.remove(note.parseOrder);
        suggestions[freeGroup].preferred = -1;
    }
}

} // namespace

BasicResult optimizeHandOrderPatterns(const QString& text, const SimaiParseResult& parsed,
    int begin, int end, const BasicParameters& parameters,
    const PatternOptimizationParameters& optimization, const miacode::simai::SimaiTimingMetadata& metadata,
    const HandOrderProgressCallback& progress)
{
    Q_UNUSED(metadata);
    BasicResult result; result.patternOptimization = true;
    if (progress) progress({HandOrderProgress::Preparing});
    if (optimization.maximumCandidates < 3 || optimization.maximumMarkers < 1) {
        result.error = "invalid_parameters"; return result;
    }
    if (!parsed.ok || !parsed.annotationErrors.isEmpty()) { result.patternOutcome = "invalid_chart"; return result; }
    if (parsed.noteMarkers.size() > optimization.maximumMarkers) { result.patternOutcome = "size_limit"; return result; }
    if (optimization.preserveExisting) { result.patternOutcome = "locked"; return result; }
    result.patternSuggestions = recognizeHandOrderPatterns(text, parsed, begin, end, optimization.recognition, parameters.sweepThreshold);
    int candidates = 0;
    for (int index = 0; index < result.patternSuggestions.size(); ++index) {
        task::CancellationScope::check();
        const auto& suggestion = result.patternSuggestions.at(index);
        if (candidates + suggestion.variants.size() > optimization.maximumCandidates) {
            result.patternSuggestions.resize(index); result.truncated = true; break;
        }
        candidates += suggestion.variants.size();
        if (progress) progress({HandOrderProgress::Windows, index + 1, int(result.patternSuggestions.size())});
    }
    retainCoherentSweepSplits(result.patternSuggestions, parsed.noteMarkers);
    reviewSlideLaunchDefaults(result.patternSuggestions, parsed.noteMarkers);
    reviewInteractionDefaults(result.patternSuggestions, parsed.noteMarkers, parameters.returnSpeed);
    retainCoherentBoundaryEdges(result.patternSuggestions, parsed.noteMarkers);
    // Independent defaults may assign a previously unlabelled Hold. Validate
    // against the combined assignments before allowing a bulk default edit.
    bool removed = false;
    do {
        removed = false;
        QHash<int,TimelineNoteMarker> combined;
        for (const auto& marker : parsed.noteMarkers) if (isLockingHold(marker)) combined.insert(marker.parseOrder,marker);
        for (const auto& group : result.patternSuggestions) if (group.preferred >= 0)
            for (const auto& marker : group.variants.at(group.preferred).markers)
                if (isLockingHold(marker)) combined.insert(marker.parseOrder,marker);
        QVector<TimelineNoteMarker> holds;
        for (const auto& marker : combined) holds.append(marker);
        for (auto& group : result.patternSuggestions) if (group.preferred >= 0)
            if (!respectsHoldLocks(holds,group.variants.at(group.preferred).markers)) {
                group.preferred = -1; removed = true;
            }
    } while (removed);
    result.patches.clear(); result.patternAccepted = 0;
    for (auto& group : result.patternSuggestions) if (group.preferred >= 0) {
        const auto& choice = group.variants.at(group.preferred);
        bool changed = false;
        for (const auto& patch : choice.patches) if (text.mid(patch.start,patch.end-patch.start) != patch.text) {
            result.patches.append(patch); changed = true;
        }
        result.patternAccepted += changed;
    }
    std::sort(result.patches.begin(), result.patches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    result.patternSegments = result.patternSuggestions.size(); result.patternCandidates = candidates;
    result.includedObjects = parsed.noteMarkers.size();
    // A structural recommendation does not claim complete physical verification.
    result.patternOutcome = result.patternSegments ? "recognized" : "no_patterns";
    if (progress) progress({HandOrderProgress::Finished, 1, 1});
    return result;
}
} // namespace miacode::hand_order
