#include "core/chart/transform/ChartNoteSelection.h"

#include "core/chart/transform/ChartBatchTransform.Internal.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/document/SimaiTimingMetadata.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <optional>
#include <QSet>

namespace miacode::chart_selection {
namespace {
bool intersects(const NoteRange& a, const NoteRange& b)
{
    return a.start < b.end && b.start < a.end;
}

NoteTimingState timingAt(const QString& input, int position, NoteTimingState state)
{
    // An annotation container may contain malformed directive-like text. It is
    // metadata, so only the native chart prefix contributes timing context.
    const QString text = miacode::hand_order::stripAnnotations(input.left(position));
    position = text.size();
    static const QRegularExpression directives(QStringLiteral(R"(\([^()]*\)|\{[^{}]*\}|<HS\*[^>]*>)"));
    for (int lineStart = 0; lineStart < position;) {
        int lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0) lineEnd = text.size();
        const QString line = text.mid(lineStart, qMin(position, lineEnd) - lineStart);
        const int comment = line.indexOf(QStringLiteral("||"));
        auto matches = directives.globalMatch(comment < 0 ? line : line.left(comment));
        while (matches.hasNext()) {
            const QString directive = matches.next().captured();
            if (directive.startsWith(QLatin1Char('('))) state.bpm = directive;
            else if (directive.startsWith(QLatin1Char('{'))) state.subdivision = directive;
            else state.hs = directive;
        }
        if (comment >= 0) {
            int numerator, denominator;
            if (miacode::simai::parseInlineTimeSignatureComment(line, comment, &numerator, &denominator)) {
                state.meterNumerator = numerator; state.meterDenominator = denominator;
            }
        }
        lineStart = lineEnd + 1;
    }
    return state;
}
QString timingText(const NoteTimingState& state)
{
    return QStringLiteral("|| %1/%2\n").arg(state.meterNumerator).arg(state.meterDenominator)
        + state.bpm + state.subdivision + state.hs;
}

// Repair only note separators whose neighbour was removed. Timing commas,
// directives, comments and whitespace retain their original source positions.
struct TokenReplacement {
    NoteRange token;
    QString text;
};

QString withoutNotes(const QString& text, const NoteRanges& removed, const NoteRanges& allNotes,
    const QVector<TokenReplacement>& replacements = {})
{
    QByteArray erase(text.size(), 0);
    for (const auto& range : removed)
        for (int i = range.start; i < range.end; ++i) erase[i] = 1;
    QByteArray content(text.size(), 0);
    for (const auto& span : miacode::simai::chartContentSpans(text, 0, text.size()))
        for (int i = span.start; i < span.end; ++i) content[i] = 1;
    QByteArray noteContent(text.size(), 0);
    for (const auto& note : allNotes)
        for (int i = note.start; i < note.end; ++i) noteContent[i] = 1;

    // A run of adjacent notes/separators is one simultaneous-note group. Keep
    // one original separator between surviving tokens; strip leading/trailing
    // separators. Packed lanes need no added separators.
    int groupStart = 0;
    int nextNote = 0;
    for (int i = 0; i <= text.size(); ++i) {
        if (i != text.size() && (content[i] == 0 || (text[i] != QLatin1Char(',')
            && text[i] != QLatin1Char('\n')))) continue;
        QVector<NoteRange> notes;
        bool changed = false;
        while (nextNote < allNotes.size() && allNotes[nextNote].start < i) {
            const auto& note = allNotes[nextNote++];
            if (note.start < groupStart) continue;
            if (erase[note.start]) changed = true;
            else notes.append(note);
        }
        if (changed) {
            for (int j = groupStart; j < i; ++j) {
                if (!content[j] || noteContent[j]) continue;
                const QChar ch = text[j];
                if (ch == QLatin1Char('/') || ch == QLatin1Char('`')) erase[j] = 1;
            }
            for (int n = 1; n < notes.size(); ++n) {
                int separator = -1;
                for (int j = notes[n - 1].end; j < notes[n].start; ++j) {
                    if (!content[j] || noteContent[j]) continue;
                    if (text[j] == QLatin1Char('`')) { separator = j; break; }
                    if (text[j] == QLatin1Char('/') && separator < 0) separator = j;
                }
                if (separator >= 0) erase[separator] = 0;
            }
        }
        groupStart = i + 1;
    }
    QString result;
    result.reserve(text.size());
    auto replacement = replacements.cbegin();
    for (int i = 0; i < text.size();) {
        if (replacement != replacements.cend() && replacement->token.start == i) {
            result.append(replacement->text);
            i = replacement->token.end;
            ++replacement;
        } else {
            if (!erase[i]) result.append(text[i]);
            ++i;
        }
    }
    return result;
}

struct SlideParts {
    QString prefix;
    QStringList tracks;
    QString annotation;
    bool hasHead = false;
};

std::optional<SlideParts> splitSlide(QStringView token)
{
    using chart_transform::detail::isSlideOperatorChar;
    const auto tokenParts = hand_order::splitToken(token.toString());
    const QStringView native(tokenParts.baseToken);
    if (native.size() < 2 || !chart_transform::detail::isDigitLane(native.front())) return {};
    qsizetype prefixLength = 1;
    while (prefixLength < native.size() && QStringView(u"bxm@?!").contains(native.at(prefixLength))) {
        ++prefixLength;
    }
    if (prefixLength >= native.size()) return {};
    SlideParts result;
    result.prefix = native.first(prefixLength).toString();
    result.hasHead = !result.prefix.contains(QLatin1Char('?')) && !result.prefix.contains(QLatin1Char('!'));
    result.tracks = native.sliced(prefixLength).toString().split(QLatin1Char('*'));
    result.annotation = tokenParts.suffix;
    for (const auto& track : result.tracks) {
        if (track.isEmpty() || !isSlideOperatorChar(track.front())) return {};
    }
    return result;
}

struct ComponentSelection {
    bool head = false;
    QSet<int> tracks;
};

std::optional<ComponentSelection> selectComponents(const SlideParts& slide, const NoteObjects& selected)
{
    ComponentSelection parts;
    for (const auto& object : selected) {
        if (object.component == NoteComponent::SlideHead && slide.hasHead) {
            parts.head = true;
        } else if (object.component == NoteComponent::SlideTrack
            && object.trackIndex >= 0 && object.trackIndex < slide.tracks.size()) {
            parts.tracks.insert(object.trackIndex);
        } else {
            return {};
        }
    }
    return parts;
}

QString isolatedSlideHead(const SlideParts& slide)
{
    QString head = slide.prefix;
    if (!head.contains(QLatin1Char('@'))) {
        head += slide.tracks.size() > 1 ? QStringLiteral("$$") : QStringLiteral("$");
    }
    head.remove(QLatin1Char('@'));
    return head;
}

QChar handGlyph(HandOrderHand hand)
{
    if (hand == HandOrderHand::Left) return QLatin1Char('l');
    if (hand == HandOrderHand::Right) return QLatin1Char('r');
    if (hand == HandOrderHand::Either) return QLatin1Char('m');
    return {};
}

chart_transform::HandOrderTokenEdit projectHandOrder(const SlideParts& slide,
    const hand_order::Annotation& annotation, bool keepHead, const QVector<int>& branches)
{
    chart_transform::HandOrderTokenEdit result;
    result.hasHead = keepHead;
    result.head = annotation.head;
    for (const int branch : branches) {
        result.tracks.append(annotation.branchTracks.value(branch));
        result.wifiModes.append(annotation.wifiModes.value(branch, HandOrderWifiMode::Legacy));
        result.wifiSectors.append(annotation.wifiSectors.value(branch));
        result.wifiBranches.append(slide.tracks.at(branch).contains(QLatin1Char('w'), Qt::CaseInsensitive));
    }
    return result;
}

NoteObjectEditResult rewriteSelectedObjects(const QString& text, const NoteObjects& selected,
    const NoteRanges& allNotes, bool keepSelected)
{
    QHash<int, NoteObjects> byToken;
    for (const auto& object : selected) byToken[object.token.start].append(object);
    NoteRanges removed;
    QVector<TokenReplacement> replacements;
    const auto notes = normalizedNoteRanges(allNotes, text.size());
    for (const auto& token : notes) {
        const auto group = byToken.take(token.start);
        if (group.isEmpty()) {
            if (keepSelected) removed.append(token);
            continue;
        }
        if (std::any_of(group.cbegin(), group.cend(), [&](const auto& object) { return object.token != token; })) {
            return {text, false};
        }
        if (group.size() == 1 && group.first().component == NoteComponent::Whole) {
            if (!keepSelected) removed.append(token);
            continue;
        }
        const auto original = QStringView(text).sliced(token.start, token.end - token.start);
        const auto slide = splitSlide(original);
        if (!slide) return {text, false};
        const auto parts = selectComponents(*slide, group);
        if (!parts) return {text, false};
        const bool keepHead = slide->hasHead && (parts->head == keepSelected);
        QVector<int> keptBranches;
        QStringList keptTracks;
        for (int branch = 0; branch < slide->tracks.size(); ++branch) {
            if (parts->tracks.contains(branch) == keepSelected) {
                keptBranches.append(branch);
                keptTracks.append(slide->tracks.at(branch));
            }
        }
        if (keptTracks.isEmpty() && !keepHead) {
            removed.append(token);
            continue;
        }
        if (keptTracks.size() == slide->tracks.size() && keepHead == slide->hasHead) continue;
        QString rewritten;
        if (keptTracks.isEmpty()) {
            rewritten = isolatedSlideHead(*slide);
        } else {
            // A selected track can be copied without its head, and deleting
            // a head leaves the original branch text intact with a gradual headless prefix.
            const QString prefix = keepHead || !slide->hasHead
                ? slide->prefix : slide->prefix.first(1) + QLatin1Char('?');
            rewritten = prefix + keptTracks.join(QLatin1Char('*'));
        }
        if (!slide->annotation.isEmpty()) {
            const auto annotation = hand_order::parseAnnotation(original.toString(), true);
            if (!annotation.valid) return {text, false};
            if (keptTracks.isEmpty()) {
                const QChar hand = handGlyph(annotation.head);
                if (!hand.isNull()) {
                    rewritten += QLatin1Char('~');
                    rewritten += hand;
                }
            } else {
                const QString suffix = chart_transform::handOrderSuffix(
                    projectHandOrder(*slide, annotation, keepHead, keptBranches));
                if (suffix.isEmpty()) return {text, false};
                rewritten += suffix;
            }
        }
        if (rewritten != original) replacements.append({token, rewritten});
    }
    if (!byToken.isEmpty()) return {text, false};
    return {withoutNotes(text, removed, notes, replacements), true};
}
}

NoteRanges normalizedNoteRanges(NoteRanges ranges, int textLength)
{
    for (auto& range : ranges) {
        range.start = qBound(0, range.start, textLength);
        range.end = qBound(range.start, range.end, textLength);
    }
    ranges.removeIf([](const NoteRange& range) { return range.end <= range.start; });
    std::sort(ranges.begin(), ranges.end(), [](const NoteRange& a, const NoteRange& b) {
        return a.start < b.start || (a.start == b.start && a.end < b.end);
    });
    NoteRanges result;
    for (const auto& range : ranges) {
        // Do not merge adjacent packed notes: they remain independent objects.
        if (!result.isEmpty() && range.start < result.last().end)
            result.last().end = qMax(result.last().end, range.end);
        else result.append(range);
    }
    return result;
}

QString deleteSelectedNotes(const QString& text, const NoteRanges& selected, const NoteRanges& allNotes)
{
    const auto selection = normalizedNoteRanges(selected, text.size());
    NoteRanges removed;
    for (const auto& note : normalizedNoteRanges(allNotes, text.size()))
        if (std::any_of(selection.cbegin(), selection.cend(),
            [&note](const NoteRange& range) { return intersects(note, range); })) removed.append(note);
    return withoutNotes(text, removed, normalizedNoteRanges(allNotes, text.size()));
}

QString copySelectedNotes(const QString& text, const NoteRanges& selected, const NoteRanges& allNotes,
    const NoteTimingState& initialTiming)
{
    const auto selection = normalizedNoteRanges(selected, text.size());
    const auto notes = normalizedNoteRanges(allNotes, text.size());
    NoteRanges kept, removed;
    for (const auto& note : notes) {
        if (std::any_of(selection.cbegin(), selection.cend(),
            [&note](const NoteRange& range) { return intersects(note, range); })) kept.append(note);
        else removed.append(note);
    }
    if (kept.isEmpty()) return {};
    const int start = kept.first().start, end = kept.last().end;
    // Restore the timing state in force at the first selected note. Directives
    // inside the copied interval, including BPM changes and empty beats, stay.
    const QString prefix = timingText(timingAt(text, start, initialTiming));
    NoteRanges localNotes, localRemoved;
    for (const auto& note : notes) if (note.start >= start && note.end <= end)
        localNotes.append({note.start - start, note.end - start});
    for (const auto& note : removed) if (note.start >= start && note.end <= end)
        localRemoved.append({note.start - start, note.end - start});
    // A clipboard fragment deliberately has no terminal E: ordinary text paste
    // in another editor must not terminate the recipient chart.
    return prefix + withoutNotes(text.mid(start, end - start), localRemoved, localNotes) + QStringLiteral(",\n");
}

NoteObjectEditResult deleteSelectedNoteObjects(const QString& text, const NoteObjects& selected,
    const NoteRanges& allNotes)
{
    return rewriteSelectedObjects(text, selected, allNotes, false);
}

NoteObjectEditResult copySelectedNoteObjects(const QString& text, const NoteObjects& selected,
    const NoteRanges& allNotes, const NoteTimingState& initialTiming)
{
    if (selected.isEmpty()) return {};
    const auto [first, last] = std::minmax_element(selected.cbegin(), selected.cend(),
        [](const auto& left, const auto& right) { return left.token.start < right.token.start; });
    const int start = first->token.start;
    const int end = last->token.end;
    if (start < 0 || end > text.size() || end <= start) return {{}, false};
    NoteRanges localNotes;
    for (const auto& note : allNotes) {
        if (note.start >= start && note.end <= end) localNotes.append({note.start - start, note.end - start});
    }
    auto localObjects = selected;
    for (auto& object : localObjects) {
        object.token.start -= start;
        object.token.end -= start;
    }
    auto projected = rewriteSelectedObjects(text.mid(start, end - start), localObjects, localNotes, true);
    if (projected.supported) {
        projected.text = timingText(timingAt(text, start, initialTiming)) + projected.text + QStringLiteral(",\n");
    }
    return projected;
}

bool selectsWholeNoteObject(QStringView token, const NoteObjects& objects)
{
    if (objects.size() == 1 && objects.first().component == NoteComponent::Whole) return true;
    const auto slide = splitSlide(token);
    if (!slide) return false;
    const auto parts = selectComponents(*slide, objects);
    return parts && parts->head == slide->hasHead && parts->tracks.size() == slide->tracks.size();
}

NoteObjectEditResult transformSlideComponents(QStringView token, const NoteObjects& objects, QStringView operation)
{
    const auto slide = splitSlide(token);
    if (!slide) return {token.toString(), false};
    const auto parts = selectComponents(*slide, objects);
    if (!parts) return {token.toString(), false};
    HandOrderHand assigned = HandOrderHand::Unspecified;
    if (operation == u"hand_order.set_left") assigned = HandOrderHand::Left;
    if (operation == u"hand_order.set_right") assigned = HandOrderHand::Right;
    if (operation == u"hand_order.set_either") assigned = HandOrderHand::Either;
    const bool swapHands = operation == u"hand_order.swap_hands";
    if (assigned != HandOrderHand::Unspecified || swapHands) {
        const auto annotation = hand_order::parseAnnotation(token.toString(), true);
        if (!annotation.valid && !slide->annotation.isEmpty()) return {token.toString(), false};
        if (swapHands && !annotation.valid) return {token.toString(), true};
        QVector<int> branches;
        for (int branch = 0; branch < slide->tracks.size(); ++branch) branches.append(branch);
        auto projected = projectHandOrder(*slide, annotation, slide->hasHead, branches);
        const auto editHand = [assigned, swapHands](HandOrderHand hand) {
            if (!swapHands) return assigned;
            if (hand == HandOrderHand::Left) return HandOrderHand::Right;
            if (hand == HandOrderHand::Right) return HandOrderHand::Left;
            return hand;
        };
        bool changed = false;
        const auto updateHand = [&](HandOrderHand& hand) {
            const HandOrderHand next = editHand(hand);
            changed |= next != hand;
            hand = next;
        };
        if (parts->head) updateHand(projected.head);
        const auto editStops = [&](QVector<HandOrderTrackStop>& stops) {
            if (stops.isEmpty() && !swapHands) {
                stops.append({0.0, assigned});
                changed = true;
            } else {
                for (auto& stop : stops) updateHand(stop.hand);
            }
        };
        for (const int branch : parts->tracks) {
            if (projected.wifiModes.at(branch) == HandOrderWifiMode::Split) {
                for (auto& sector : projected.wifiSectors[branch]) editStops(sector);
            } else {
                editStops(projected.tracks[branch]);
            }
        }
        if (!changed) return {token.toString(), true};
        // A complex suffix requires every track. Do not invent assignments for
        // unselected branches or add a head colour the user did not select.
        const QString suffix = chart_transform::handOrderSuffix(projected);
        if (suffix.isEmpty()) return {token.toString(), false};
        return {slide->prefix + slide->tracks.join(QLatin1Char('*')) + suffix, true};
    }
    auto changed = *slide;
    if (operation == u"transform.toggle_ex") {
        if (parts->head) {
            if (changed.prefix.contains(QLatin1Char('x'))) changed.prefix.remove(QLatin1Char('x'));
            else changed.prefix.append(QLatin1Char('x'));
        }
    } else if (operation == u"transform.toggle_break") {
        bool everySelectedBreak = !parts->head || changed.prefix.contains(QLatin1Char('b'));
        for (int branch : parts->tracks) {
            everySelectedBreak &= changed.tracks.at(branch).contains(QLatin1Char('b'));
        }
        if (parts->head) {
            changed.prefix.remove(QLatin1Char('b'));
            if (!everySelectedBreak) changed.prefix.append(QLatin1Char('b'));
        }
        for (int branch : parts->tracks) {
            auto& track = changed.tracks[branch];
            track.remove(QLatin1Char('b'));
            if (!everySelectedBreak) {
                const auto duration = track.indexOf(QLatin1Char('['));
                if (duration < 0) return {token.toString(), false};
                track.insert(duration, QLatin1Char('b'));
            }
        }
    } else {
        return {token.toString(), false};
    }
    return {changed.prefix + changed.tracks.join(QLatin1Char('*')) + changed.annotation, true};
}

NotePasteResult pasteNoteFragment(const QString& text, int start, int end,
    const QString& input, const NoteRanges& selectedNotes, const NoteRanges& allNotes,
    const NoteTimingState& initialTiming, const NoteObjects& selectedObjects)
{
    NotePasteResult result{text, qBound(0, start, int(text.size())), {}};
    QString fragment = input.trimmed();
    static const QRegularExpression terminal(QStringLiteral(R"((?:^|\n)\s*E\s*$)"));
    fragment.remove(terminal);
    fragment = fragment.trimmed();
    if (fragment.isEmpty()) return result;
    auto selected = normalizedNoteRanges(selectedNotes, text.size());
    int at = qBound(0, qMin(start, end), int(text.size()));
    int limit = qBound(at, qMax(start, end), int(text.size()));
    if (!selectedObjects.isEmpty()) {
        selected.clear();
        QHash<int, NoteObjects> groups;
        for (const auto& object : selectedObjects) groups[object.token.start].append(object);
        for (const auto& group : groups) {
            const auto token = group.first().token;
            if (token.start < 0 || token.end > text.size() || token.end <= token.start
                || !selectsWholeNoteObject(QStringView(text).sliced(token.start, token.end - token.start), group)) {
                // A contextual fragment inserts beats. Replacing only one
                // component this way would shift the unselected sibling in time.
                result.error = QStringLiteral("unsupported_note_component");
                return result;
            }
            selected.append(token);
        }
        selected = normalizedNoteRanges(selected, text.size());
    } else if (selected.isEmpty() && limit > at) {
        for (const auto& note : allNotes)
            if (note.start < limit && at < note.end) selected.append(note);
    }
    QString base = text;
    QString leading;
    if (!selected.isEmpty()) {
        // One paste for the whole collection. All selected objects are removed
        // together; the fragment is inserted once before its earliest beat.
        at = miacode::simai::previousChartComma(text, selected.first().start) + 1;
        base = deleteSelectedNotes(text, selected, allNotes);
    } else {
        const int comment = miacode::simai::commentStartForPosition(text, at);
        if (comment >= 0) { at = limit = comment; }
        for (const auto& note : allNotes) {
            if (note.start < at && at < note.end) { at = limit = note.start; break; }
        }
        // Pasting inside a directive or a simultaneous separator goes before
        // that beat, preserving every original token and separator verbatim.
        const int beatStart = miacode::simai::previousChartComma(text, at) + 1;
        const QString prefix = text.mid(beatStart, at - beatStart).trimmed();
        const QChar before = prefix.isEmpty() ? QChar() : prefix.back();
        const QChar after = at < text.size() ? text[at] : QChar();
        bool insideDirective = false;
        for (int i = beatStart; i < at; ++i) {
            QChar close;
            if (text[i] == QLatin1Char('(')) close = QLatin1Char(')');
            else if (text[i] == QLatin1Char('{')) close = QLatin1Char('}');
            else if (text.mid(i, 4) == QLatin1String("<HS*")) close = QLatin1Char('>');
            if (close.isNull()) continue;
            const int closing = text.indexOf(close, i + 1);
            if (closing >= at) { insideDirective = true; break; }
            if (closing >= 0) i = closing;
        }
        if (insideDirective || before == QLatin1Char('/') || before == QLatin1Char('`')
            || after == QLatin1Char('/') || after == QLatin1Char('`')) at = limit = beatStart;
        const bool afterNote = std::any_of(allNotes.cbegin(), allNotes.cend(), [&](const NoteRange& note) {
            return note.start >= beatStart && note.start < at;
        });
        if (afterNote) leading = QStringLiteral(",");
        base.remove(at, limit - at);
    }
    const auto restore = timingAt(text, selected.isEmpty() ? limit : at, initialTiming);
    const QString inserted = leading + fragment + (fragment.endsWith(QLatin1Char(',')) ? QString() : QStringLiteral(","))
        + QStringLiteral("\n") + timingText(restore);
    base.insert(at, inserted);
    result.text = base;
    result.caret = at + inserted.size();
    return result;
}
} // namespace miacode::chart_selection
