#include "app/ui/editor/EditorController.h"

#include "editor/SimaiCompletionCatalog.h"
#include "editor/BookmarkCommentSyntax.h"
#include "editor/TouchPadAuthoringEdit.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/transform/HandOrderScreenSplit.h"
#include "core/chart/transform/HandOrderAlternation.h"

#include <QtGlobal>
#include <QCryptographicHash>
#include <algorithm>

namespace miacode::ui {
namespace {

miacode::editor::SimaiTextEditResult untouched(const QString& text, int anchor, int position)
{
    miacode::editor::SimaiTextEditResult result;
    result.transaction.anchor = qBound(0, anchor, text.size());
    result.transaction.position = qBound(0, position, text.size());
    return result;
}

bool commandModifier(Qt::KeyboardModifiers modifiers)
{
    return modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
}

} // namespace

EditorController::EditorController(QObject* parent) : QObject(parent) {}
bool EditorController::halfWidthInputEnabled() const { return halfWidthInputEnabled_; }
bool EditorController::overwriteMode() const { return overwriteMode_; }
bool EditorController::autoCompletionEnabled() const { return autoCompletionEnabled_; }
bool EditorController::imeInputDisabled() const { return imeInputDisabled_; }
QString EditorController::wholeBpm() const { return wholeBpm_; }
bool EditorController::completionActive() const { return completion_.active; }
QStringList EditorController::completionCandidates() const { return visibleCandidates_; }
int EditorController::completionIndex() const { return completionIndex_; }

void EditorController::setHalfWidthInputEnabled(bool enabled)
{
    if (halfWidthInputEnabled_ == enabled) return;
    halfWidthInputEnabled_ = enabled;
    emit settingsChanged();
}
void EditorController::setOverwriteMode(bool enabled)
{
    if (overwriteMode_ == enabled) return;
    overwriteMode_ = enabled;
    closeCompletion();
    emit settingsChanged();
}
void EditorController::setAutoCompletionEnabled(bool enabled)
{
    if (autoCompletionEnabled_ == enabled) return;
    autoCompletionEnabled_ = enabled;
    if (!enabled) closeCompletion();
    emit settingsChanged();
}
void EditorController::setImeInputDisabled(bool disabled)
{
    if (imeInputDisabled_ == disabled) return;
    imeInputDisabled_ = disabled;
    emit settingsChanged();
}
QString EditorController::completionGroup() const
{
    if (!completion_.active) return {};
    if (completion_.opening == QLatin1Char('[')) return QStringLiteral("duration");
    if (completion_.opening == QLatin1Char('{')) return QStringLiteral("division");
    return {};
}

void EditorController::setCompletionConfig(const miacode::editor::SimaiCompletionConfig& config)
{
    const auto normalized = miacode::editor::normalizeCompletionConfig(config);
    if (completionConfig_ == normalized) return;
    completionConfig_ = normalized;
    closeCompletion();
}

void EditorController::setWholeBpm(const QString& bpm)
{
    if (wholeBpm_ == bpm) return;
    wholeBpm_ = bpm;
    emit settingsChanged();
}

miacode::editor::SimaiTextEditResult EditorController::processKey(
    const QString& text, int anchor, int position, const QString& input, int key, int modifiers)
{
    const auto keyboardModifiers = static_cast<Qt::KeyboardModifiers>(modifiers);
    if (completion_.active) {
        if (key == Qt::Key_Up) {
            moveCompletionSelection(-1);
            auto result = untouched(text, anchor, position);
            result.consumed = true;
            return result;
        }
        if (key == Qt::Key_Down) {
            moveCompletionSelection(1);
            auto result = untouched(text, anchor, position);
            result.consumed = true;
            return result;
        }
        if (key == Qt::Key_Escape) {
            closeCompletion();
            auto result = untouched(text, anchor, position);
            result.consumed = true;
            return result;
        }
        if ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab)
            && !commandModifier(keyboardModifiers))
            return acceptCompletion(text, anchor, position);
    }
    miacode::editor::SimaiTextEditRequest request;
    request.text = text;
    request.anchor = anchor;
    request.position = position;
    request.input = input;
    request.key = key;
    request.modifiers = keyboardModifiers;
    return process(request);
}

miacode::editor::SimaiTextEditResult EditorController::process(const miacode::editor::SimaiTextEditRequest& input)
{
    auto request = input;
    request.halfWidthInputEnabled = halfWidthInputEnabled_ && input.halfWidthInputEnabled;
    request.overwriteMode = overwriteMode_;
    request.autoCompletionEnabled = autoCompletionEnabled_ && request.autoCompletionEnabled;
    request.wholeBpm = wholeBpm_;
    request.completionConfig = completionConfig_;
    request.completionActive = completion_.active;
    auto result = miacode::editor::applySimaiTextEditPolicy(request);
    if (result.completion.active) setCompletion(result.completion);
    return result;
}

miacode::editor::SimaiTextEditResult EditorController::acceptCompletion(const QString& text, int anchor, int position)
{
    auto result = untouched(text, anchor, position);
    updateCompletion(text, position);
    if (!completion_.active || completionIndex_ < 0 || completionIndex_ >= visibleCandidates_.size()) return result;
    const int start = qBound(0, completion_.startPosition + completionReplacementOffset_, text.size());
    int end = qBound(start, position, text.size());
    const QString candidate = visibleCandidates_.at(completionIndex_);
    // Catalog candidates include their closing glyph. When policy inserted a
    // matching pair, consume that existing right glyph in the same QML edit.
    const QChar closing = miacode::editor::closingBracketFor(completion_.opening);
    if (completion_.closingPresent && !closing.isNull() && end < text.size()
        && text.at(end) == closing) {
        ++end;
    }
    result.transaction.replacementStart = start;
    result.transaction.replacementEnd = end;
    result.transaction.replacementText = candidate;
    result.transaction.anchor = result.transaction.position = start + candidate.size();
    result.transaction.hasEdit = true;
    result.consumed = true;
    closeCompletion();
    return result;
}

void EditorController::triggerCompletion(QChar glyph, const QString& text, int position, bool closingPresent)
{
    if (!autoCompletionEnabled_ || overwriteMode_ || completion_.active) return;
    const miacode::editor::HandOrderCompletion hand =
        handOrderFeaturesEnabled_ ? miacode::editor::handOrderCompletionAt(text, position)
            : miacode::editor::HandOrderCompletion{};
    if (hand.active()) {
        setCompletion({true, QLatin1Char('~'), false, hand.startPosition, hand.candidates});
        return;
    }
    miacode::editor::SimaiCompletionSession completion;
    if (miacode::editor::isBracketOpening(glyph)) {
        completion.opening = glyph;
        completion.candidates = miacode::editor::candidatesForOpening(glyph, wholeBpm_, text, completionConfig_);
    } else if (glyph == QLatin1Char('h')) {
        if (position < text.size() && text.at(position) == QLatin1Char('[')) return;
        completion.opening = QLatin1Char('[');
        completion.candidates = miacode::editor::holdDurationCandidates(completionConfig_);
    } else return;
    completion.startPosition = position;
    completion.closingPresent = closingPresent;
    completion.active = !completion.candidates.isEmpty();
    setCompletion(completion);
}

void EditorController::setDifficulty(int difficultyId)
{
    if (activeDifficultyId_ != difficultyId) closeCompletion();
    activeDifficultyId_ = difficultyId;
}

void EditorController::setCompletion(const miacode::editor::SimaiCompletionSession& completion)
{
    completion_ = completion;
    completionReplacementOffset_ = 0;
    visibleCandidates_ = completion.candidates;
    completionIndex_ = visibleCandidates_.isEmpty() ? -1 : 0;
    emit completionChanged();
}

void EditorController::filterCompletion(const QString& text, int position)
{
    const int start = completion_.startPosition;
    if (start < 0 || position < start || position > text.size()) { closeCompletion(); return; }
    const QString prefix = text.mid(start, position - start);
    const QChar closing = miacode::editor::closingBracketFor(completion_.opening);
    if (prefix.contains(QLatin1Char('\n')) || (!closing.isNull() && prefix.contains(closing))) { closeCompletion(); return; }
    const QString selected = completionIndex_ >= 0 && completionIndex_ < visibleCandidates_.size()
        ? visibleCandidates_.at(completionIndex_) : QString();
    const bool insideSquareBrackets = completion_.opening == QLatin1Char('[')
        && start > 0 && text.at(start - 1) == QLatin1Char('[');
    const auto match = miacode::editor::matchCompletionCandidates(
        completion_.candidates, prefix, completionConfig_, insideSquareBrackets);
    if (match.candidates.isEmpty()) { closeCompletion(); return; }
    completionReplacementOffset_ = match.replacementOffset;
    visibleCandidates_ = match.candidates;
    const int selectedIndex = visibleCandidates_.indexOf(selected);
    completionIndex_ = selectedIndex >= 0 ? selectedIndex : 0;
    emit completionChanged();
}

void EditorController::updateCompletion(const QString& text, int position)
{
    if (autoCompletionEnabled_ && !overwriteMode_) {
        const miacode::editor::HandOrderCompletion hand =
            handOrderFeaturesEnabled_ ? miacode::editor::handOrderCompletionAt(text, position)
            : miacode::editor::HandOrderCompletion{};
        if (hand.active()) {
            const miacode::editor::SimaiCompletionSession next{
                true, QLatin1Char('~'), false, hand.startPosition, hand.candidates};
            if (completion_.opening != next.opening
                || completion_.startPosition != next.startPosition
                || visibleCandidates_ != next.candidates) {
                setCompletion(next);
            }
            return;
        }
    }
    if (completion_.active && completion_.opening == QLatin1Char('~')) closeCompletion();
    else if (completion_.active) filterCompletion(text, position);
}

void EditorController::moveCompletionSelection(int delta)
{
    if (!completion_.active || visibleCandidates_.isEmpty()) return;
    const int next = qBound(0, completionIndex_ + delta, visibleCandidates_.size() - 1);
    if (next == completionIndex_) return;
    completionIndex_ = next;
    emit completionChanged();
}

void EditorController::selectCompletionIndex(int index)
{
    if (!completion_.active || index < 0 || index >= visibleCandidates_.size()
        || completionIndex_ == index) {
        return;
    }
    completionIndex_ = index;
    emit completionChanged();
}

void EditorController::closeCompletion()
{
    if (!completion_.active && visibleCandidates_.isEmpty() && completionIndex_ == -1) return;
    completion_ = {};
    completionReplacementOffset_ = 0;
    visibleCandidates_.clear();
    completionIndex_ = -1;
    emit completionChanged();
}

namespace {
miacode::editor::SimaiTextEditResult bookmarkEdit(const QString& lineText, int offset,
                                                const QString& replacement, bool caretAtEnd)
{
    miacode::editor::SimaiTextEditResult result;
    if (lineText == replacement) return result;
    result.consumed = true;
    auto& tx = result.transaction;
    tx.hasEdit = true;
    tx.replacementStart = offset;
    tx.replacementEnd = offset + lineText.size();
    tx.replacementText = replacement;
    tx.anchor = tx.position = offset + (caretAtEnd ? replacement.size() : 0);
    return result;
}
}
miacode::editor::SimaiTextEditResult EditorController::createBookmark(const QString& lineText, int offset, const QString& title) const
{
    return bookmarkEdit(lineText, offset, miacode::editor::appendBookmarkComment(lineText, title), true);
}
miacode::editor::SimaiTextEditResult EditorController::renameBookmark(const QString& lineText, int offset, const QString& title) const
{
    return bookmarkEdit(lineText, offset, miacode::editor::renameBookmarkComment(lineText, title), true);
}
miacode::editor::SimaiTextEditResult EditorController::deleteBookmark(const QString& lineText, int offset) const
{
    return bookmarkEdit(lineText, offset, miacode::editor::removeBookmarkComment(lineText), false);
}
miacode::editor::SimaiTextEditResult EditorController::touchPadAuthoring(
    const QString& text, int anchor, int position, const QString& pad, QChar separator) const
{
    const auto plan = miacode::editor::planTouchPadAuthoringEdit(text, position, pad, separator);
    auto result = untouched(text, anchor, position);
    if (!plan.valid) return result;
    const int start = qBound(0, plan.insertionPosition, text.size());
    const int end = qBound(start, start + plan.removalLength, text.size());
    result.consumed = true;
    auto& tx = result.transaction;
    tx.hasEdit = true;
    tx.replacementStart = start;
    tx.replacementEnd = end;
    tx.replacementText = plan.insertionText;
    tx.anchor = tx.position = start + plan.insertionText.size();
    tx.touchTokenStart = plan.tokenStart;
    return result;
}

} // namespace miacode::ui

namespace miacode::ui {
namespace {
QString draftKey(const QString& scope, const QString& text)
{
    return scope + QLatin1Char(':') + QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QPair<int, int> slideTokenAtCaret(const QString& text, int caret,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    QVector<const TimelineNoteMarker*> byParseOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byParseOrder.size()) byParseOrder[marker.parseOrder] = &marker;
    QVector<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n')) lineStarts.append(i + 1);
    for (const auto& source : parsed.sourceTokens) {
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        // Native source spans include the full hand-order suffix. Delimiters
        // and whitespace do not borrow a neighboring Slide; EOF is its end.
        if (caret < start || (caret >= finish && !(caret == finish && caret == text.size()))) continue;
        if (!source.nativeValid || source.markerCount == 0) return {-1, -1};
        for (int branch = 0; branch < source.markerCount; ++branch) {
            const auto* marker = byParseOrder.value(source.firstParseOrder + branch);
            if (!marker || marker->type != QStringLiteral("slide")) return {-1, -1};
        }
        return {start, finish};
    }
    return {-1, -1};
}
miacode::editor::SimaiTextEditResult selectionTransaction(
    const QString& text, int anchor, int position, const QString& replacement)
{
    auto result = untouched(text, anchor, position);
    result.consumed = true;
    const int begin = qBound(0, qMin(anchor, position), int(text.size()));
    const int end = qBound(begin, qMax(anchor, position), int(text.size()));
    if (replacement == text.mid(begin, end - begin)) return result;
    auto& tx = result.transaction;
    tx.hasEdit = true;
    tx.replacementStart = begin;
    tx.replacementEnd = end;
    tx.replacementText = replacement;
    tx.anchor = anchor <= position ? begin : begin + replacement.size();
    tx.position = anchor <= position ? begin + replacement.size() : begin;
    return result;
}
}

void EditorController::retranslate()
{
    emit handOrderChanged();
    emit nativeSlideSplitChanged();
}

void EditorController::activateHandOrderDocument(const QString& scope, const QString& text)
{
    const QString key = draftKey(scope, text);
    if (key == handOrderKey_) return;
    if (handOrderScope_ != scope) handOrderFeedback_.clear();
    handOrderSplitAvailable_ = false;
    handOrderScope_ = scope;
    handOrderText_ = text;
    handOrderKey_ = key;
    emit handOrderChanged();
}

void EditorController::rememberHandOrderDraft(const QString& text,
    const QVector<miacode::chart_transform::HandOrderTokenEdit>& tokens,
    const QVector<miacode::chart_transform::HandOrderEditPlan::Patch>& patches)
{
    const QString key = draftKey(handOrderScope_, text);
    auto merged = handOrderDrafts_.value(key);
    if (key != handOrderKey_ && !handOrderDrafts_.contains(key)) {
        for (auto previous : handOrderDrafts_.value(handOrderKey_)) {
            int delta = 0;
            bool replaced = false;
            for (const auto& patch : patches) {
                if (patch.start < previous.end && patch.end > previous.start) { replaced = true; break; }
                if (patch.end <= previous.start) delta += patch.text.size() - (patch.end - patch.start);
            }
            if (!replaced) {
                previous.start += delta;
                previous.end += delta;
                merged.append(previous);
            }
        }
    }
    QVector<int> visible;
    for (const auto& token : tokens) {
        const auto existing = std::find_if(merged.begin(), merged.end(), [&](const auto& other) {
            return other.start == token.start && other.baseToken == token.baseToken;
        });
        if (existing == merged.end()) merged.append(token);
        else *existing = token;
        visible.append(token.start);
    }
    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    for (auto& token : merged) {
        token.line = text.left(token.start).count(QLatin1Char('\n')) + 1;
        token.col = token.start - (token.start == 0 ? -1 : text.lastIndexOf(QLatin1Char('\n'), token.start - 1));
    }
    handOrderDrafts_.insert(key, merged);
    handOrderVisibleStarts_.insert(key, visible);
    handOrderDraftOrder_.removeAll(key);
    handOrderDraftOrder_.append(key);
    // Session-only drafts follow exact document versions, including undo/redo.
    while (handOrderDraftOrder_.size() > 64) {
        const auto oldest = handOrderDraftOrder_.takeFirst();
        handOrderDrafts_.remove(oldest);
        handOrderVisibleStarts_.remove(oldest);
    }
    activateHandOrderDocument(handOrderScope_, text);
}

void EditorController::setHandOrderFeaturesEnabled(bool enabled)
{
    if (handOrderFeaturesEnabled_ == enabled) return;
    handOrderFeaturesEnabled_ = enabled;
    if (!enabled) {
        handOrderDrafts_.clear(); handOrderVisibleStarts_.clear(); handOrderDraftOrder_.clear();
        handOrderSplitAvailable_ = false;
        handOrderFeedback_ = {}; handOrderKey_.clear();
        cancelNativeSlideSplit();
        if (completion_.opening == QLatin1Char('~')) closeCompletion();
    }
    emit handOrderChanged();
}

miacode::editor::SimaiTextEditResult EditorController::handOrderCommand(
    const QString& text, int anchor, int position, const QString& command,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    if (!handOrderFeaturesEnabled_) return untouched(text, anchor, position);
    activateHandOrderDocument(handOrderScope_, text);
    if (command == QStringLiteral("hand_order.object_selection_requires_contiguous")) {
        handOrderSplitAvailable_ = false;
        handOrderFeedback_ = miacode::localizedText("hand_order.object_selection_requires_contiguous");
        emit handOrderChanged();
        return untouched(text, anchor, position);
    }
    int begin = qBound(0, qMin(anchor, position), int(text.size()));
    int end = qBound(begin, qMax(anchor, position), int(text.size()));
    if (command == QStringLiteral("hand_order.basic_layout")) {
        const auto layout = miacode::chart_transform::basicHandOrderLayoutInSelection(
            text, begin, end, metadata, handOrderReturnSpeed_, handOrderSweepThreshold_,
            handOrderColorUnresolved_);
        const auto& plan = layout.edit;
        const bool wholeDifficulty = begin == end;
        auto result = selectionTransaction(text, wholeDifficulty ? 0 : anchor,
            wholeDifficulty ? int(text.size()) : position, plan.replacement);
        if (wholeDifficulty) {
            int mapped = qBound(0, position, int(text.size()));
            int delta = 0;
            for (const auto& patch : plan.patches) {
                if (position >= patch.end) {
                    delta += patch.text.size() - (patch.end - patch.start);
                    mapped = position + delta;
                } else if (position > patch.start) {
                    mapped = patch.start + delta + qMin(position - patch.start, int(patch.text.size()));
                    break;
                }
            }
            result.transaction.anchor = result.transaction.position = qBound(0, mapped, int(plan.replacement.size()));
        }
        handOrderSplitAvailable_ = false;
        handOrderFeedback_ = miacode::localizedText("hand_order.basic_layout_feedback")
            .arg(plan.changed).arg(layout.preserved).arg(plan.partial).arg(plan.unsupported).arg(plan.diagnostics.size());
        handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.basic_layout_notice");
        handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.basic_layout_next_step");
        if (layout.unresolvedColored > 0)
            handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n")
                + miacode::localizedText("hand_order.basic_layout_unverified_defaults")
                    .arg(layout.unresolvedColored);
        for (const auto& diagnostic : plan.diagnostics)
            handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.diagnostic")
                .arg(diagnostic.line).arg(diagnostic.col).arg(diagnostic.message);
        emit handOrderChanged();
        return result;
    }
    const bool bisect = command == QStringLiteral("hand_order.bisect_annotation");
    const bool bisectAtCaret = bisect && begin == end;
    if (bisectAtCaret) {
        const auto token = slideTokenAtCaret(text, begin, metadata);
        if (token.first < 0) return untouched(text, anchor, position);
        begin = token.first;
        end = token.second;
    }
    if (begin == end) return untouched(text, anchor, position);
    namespace transform = miacode::chart_transform;
    const bool split = command == QStringLiteral("hand_order.split_annotation");
    const bool swap = command == QStringLiteral("hand_order.swap_hands");
    const bool alternate = command == QStringLiteral("hand_order.alternate");
    const bool screenSplit = command == QStringLiteral("hand_order.screen_split");
    HandOrderHand hand = HandOrderHand::Unspecified;
    if (command == QStringLiteral("hand_order.set_left")) hand = HandOrderHand::Left;
    else if (command == QStringLiteral("hand_order.set_right")) hand = HandOrderHand::Right;
    else if (command == QStringLiteral("hand_order.set_either")) hand = HandOrderHand::Either;
    if (!split && !bisect && !swap && !screenSplit && !alternate && hand == HandOrderHand::Unspecified) return untouched(text, anchor, position);
    auto plan = split ? transform::splitHandOrderInSelection(text, begin, end, metadata)
        : bisect ? transform::bisectHandOrderInSelection(text, begin, end, metadata)
        : swap ? transform::swapHandOrderInSelection(text, begin, end, metadata)
        : alternate ? transform::alternateHandOrderInSelection(text, {{begin, end}}, metadata)
        : screenSplit ? transform::screenSplitHandOrderInSelection(text, begin, end, metadata)
        : transform::setHandOrderInSelection(text, begin, end, hand, metadata);
    const auto result = selectionTransaction(text, bisectAtCaret ? begin : anchor,
        bisectAtCaret ? end : position, plan.replacement);
    if (split && !plan.intervals.isEmpty()) {
        // Reopening an unchanged source version restores interval choices.
        const auto old = handOrderDrafts_.value(draftKey(handOrderScope_,
            text.left(begin) + plan.replacement + text.mid(end)));
        for (auto& token : plan.intervals) {
            for (const auto& previous : old) {
                if (token.start == previous.start && token.baseToken == previous.baseToken
                    && token.tracks.size() == previous.tracks.size()) {
                    token = previous;
                    break;
                }
            }
        }
        rememberHandOrderDraft(text.left(begin) + plan.replacement + text.mid(end), plan.intervals, plan.patches);
    }
    handOrderSplitAvailable_ = split && !plan.intervals.isEmpty();
    handOrderFeedback_ = miacode::localizedText("hand_order.feedback").arg(plan.changed).arg(plan.partial)
        .arg(plan.unsupported).arg(plan.diagnostics.size());
    for (const auto& diagnostic : plan.diagnostics)
        handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.diagnostic")
            .arg(diagnostic.line).arg(diagnostic.col).arg(diagnostic.message);
    emit handOrderChanged();
    return result;
}

miacode::editor::SimaiTextEditResult EditorController::alternateHandOrderSelection(
    const QString& text, const miacode::chart_selection::NoteRanges& selected,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    if (!handOrderFeaturesEnabled_) return untouched(text, 0, 0);
    const auto ranges = miacode::chart_selection::normalizedNoteRanges(selected, text.size());
    if (ranges.isEmpty()) return untouched(text, 0, 0);
    const auto plan = miacode::chart_transform::alternateHandOrderInSelection(text, ranges, metadata);
    auto result = selectionTransaction(text, ranges.first().start, ranges.last().end, plan.replacement);
    if (result.transaction.hasEdit)
        result.transaction.anchor = result.transaction.position = ranges.first().start;
    handOrderSplitAvailable_ = false;
    handOrderFeedback_ = miacode::localizedText("hand_order.feedback").arg(plan.changed).arg(plan.partial)
        .arg(plan.unsupported).arg(plan.diagnostics.size());
    for (const auto& diagnostic : plan.diagnostics)
        handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.diagnostic")
            .arg(diagnostic.line).arg(diagnostic.col).arg(diagnostic.message);
    emit handOrderChanged();
    return result;
}

miacode::editor::SimaiTextEditResult EditorController::screenSplitHandOrderSelection(
    const QString& text, const miacode::chart_selection::NoteRanges& selected,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    if (!handOrderFeaturesEnabled_) return untouched(text, 0, 0);
    activateHandOrderDocument(handOrderScope_, text);
    const auto ranges = miacode::chart_selection::normalizedNoteRanges(selected, text.size());
    if (ranges.isEmpty()) return untouched(text, 0, 0);
    const auto plan = miacode::chart_transform::screenSplitHandOrderInSelection(text, ranges, metadata);
    auto result = selectionTransaction(text, ranges.first().start, ranges.last().end, plan.replacement);
    if (result.transaction.hasEdit)
        result.transaction.anchor = result.transaction.position = ranges.first().start;
    handOrderSplitAvailable_ = false;
    handOrderFeedback_ = miacode::localizedText("hand_order.feedback").arg(plan.changed).arg(plan.partial)
        .arg(plan.unsupported).arg(plan.diagnostics.size());
    for (const auto& diagnostic : plan.diagnostics)
        handOrderFeedback_ = handOrderFeedback_ + QStringLiteral("\n") + miacode::localizedText("hand_order.diagnostic")
            .arg(diagnostic.line).arg(diagnostic.col).arg(diagnostic.message);
    emit handOrderChanged();
    return result;
}

QVariantList EditorController::handOrderRows() const
{
    QVariantList rows;
    const auto tokens = handOrderDrafts_.value(handOrderKey_);
    for (int tokenIndex = 0; tokenIndex < tokens.size(); ++tokenIndex) {
        const auto& token = tokens.at(tokenIndex);
        if (!handOrderVisibleStarts_.value(handOrderKey_).contains(token.start)) continue;
        const auto append = [&](int branch, int sector, int stop, double begin, double end, HandOrderHand hand) {
            rows.append(QVariantMap{{QStringLiteral("token"), tokenIndex},
                {QStringLiteral("line"), token.line}, {QStringLiteral("col"), token.col},
                {QStringLiteral("branch"), branch}, {QStringLiteral("stop"), stop},
                {QStringLiteral("sector"), sector},
                {QStringLiteral("wifi"), branch >= 0 && token.wifiBranches.value(branch)},
                {QStringLiteral("wifiMode"), int(token.wifiModes.value(branch, HandOrderWifiMode::Legacy))},
                {QStringLiteral("begin"), begin}, {QStringLiteral("end"), end},
                {QStringLiteral("hand"), int(hand)}});
        };
        if (token.hasHead) append(-1, -1, -1, 0, 0, token.head);
        for (int branch = 0; branch < token.tracks.size(); ++branch) {
            const auto mode = token.wifiModes.value(branch, HandOrderWifiMode::Legacy);
            if (mode == HandOrderWifiMode::Single) {
                append(branch, -1, 0, 0, 1, token.tracks.at(branch).value(0).hand);
                continue;
            }
            const int count = mode == HandOrderWifiMode::Split ? 2 : 1;
            for (int sector = 0; sector < count; ++sector) {
              const auto stops = mode == HandOrderWifiMode::Split
                  ? token.wifiSectors.value(branch).value(sector) : token.tracks.at(branch);
              for (int stop = 0; stop < stops.size(); ++stop)
                append(branch, mode == HandOrderWifiMode::Split ? sector : -1, stop, stops.at(stop).progress,
                    stop + 1 < stops.size() ? stops.at(stop + 1).progress : 1.0, stops.at(stop).hand);
            }
        }
    }
    return rows;
}

void EditorController::setHandOrderInterval(int row, int hand)
{
    const auto rows = handOrderRows();
    if (row < 0 || row >= rows.size() || hand < 0 || hand > 3) return;
    const auto data = rows.at(row).toMap();
    auto& token = handOrderDrafts_[handOrderKey_][data.value(QStringLiteral("token")).toInt()];
    const int branch = data.value(QStringLiteral("branch")).toInt();
    if (branch < 0) token.head = HandOrderHand(hand);
    else if (token.wifiModes.value(branch, HandOrderWifiMode::Legacy) == HandOrderWifiMode::Single) {
        // The single-mode row edits the whole track, never one interval.
        for (auto& stop : token.tracks[branch]) stop.hand = HandOrderHand(hand);
    } else if (data.value(QStringLiteral("sector")).toInt() >= 0) {
        token.wifiSectors[branch][data.value(QStringLiteral("sector")).toInt()]
            [data.value(QStringLiteral("stop")).toInt()].hand = HandOrderHand(hand);
    } else token.tracks[branch][data.value(QStringLiteral("stop")).toInt()].hand = HandOrderHand(hand);
    emit handOrderChanged();
}

void EditorController::setHandOrderWifiMode(int row, int mode)
{
    const auto rows = handOrderRows();
    if (row < 0 || row >= rows.size() || mode < 0 || mode > 2) return;
    const auto data = rows.at(row).toMap();
    const int branch = data.value(QStringLiteral("branch")).toInt();
    auto& token = handOrderDrafts_[handOrderKey_][data.value(QStringLiteral("token")).toInt()];
    if (branch < 0 || !token.wifiBranches.value(branch)) return;
    const auto previous = token.wifiModes.value(branch, HandOrderWifiMode::Legacy);
    if (int(previous) == mode) return;
    token.wifiModes.resize(token.tracks.size());
    token.wifiSectors.resize(token.tracks.size());
    auto stops = previous == HandOrderWifiMode::Split ? token.wifiSectors.at(branch).value(0) : token.tracks.at(branch);
    if (stops.isEmpty()) stops.append({0.0, HandOrderHand::Unspecified});
    if (mode == int(HandOrderWifiMode::Split)) {
        token.wifiSectors[branch] = {stops, stops};
        token.tracks[branch].clear();
    } else {
        if (mode == int(HandOrderWifiMode::Single)) {
            // Explicit mode changes require a fresh whole-track choice when
            // the old intervals or the other sector disagree.
            auto chosen = stops.constFirst().hand;
            for (const auto& stop : stops) if (stop.hand != chosen) chosen = HandOrderHand::Unspecified;
            if (previous == HandOrderWifiMode::Split)
                for (const auto& sector : token.wifiSectors.at(branch))
                    for (const auto& stop : sector) if (stop.hand != chosen) chosen = HandOrderHand::Unspecified;
            for (auto& stop : stops) stop.hand = chosen;
        }
        token.tracks[branch] = stops;
        token.wifiSectors[branch].clear();
    }
    token.wifiModes[branch] = HandOrderWifiMode(mode);
    emit handOrderChanged();
}

bool EditorController::handOrderCanApply() const
{
    if (!handOrderFeaturesEnabled_) return false;
    bool changed = false;
    const auto tokens = handOrderDrafts_.value(handOrderKey_);
    for (const auto& token : tokens) {
        if (!handOrderVisibleStarts_.value(handOrderKey_).contains(token.start)) continue;
        if (!miacode::chart_transform::handOrderTokenComplete(token)) return false;
        changed |= handOrderText_.mid(token.start, token.end - token.start)
            != token.baseToken + miacode::chart_transform::handOrderSuffix(token);
    }
    return changed;
}

miacode::editor::SimaiTextEditResult EditorController::applyHandOrderIntervals(const QString& text)
{
    if (!handOrderFeaturesEnabled_) return untouched(text, 0, 0);
    if (text != handOrderText_ || !handOrderCanApply()) return untouched(text, 0, 0);
    auto tokens = handOrderDrafts_.value(handOrderKey_);
    const auto visible = handOrderVisibleStarts_.value(handOrderKey_);
    tokens.removeIf([&](const auto& token) { return !visible.contains(token.start); });
    const int begin = tokens.constFirst().start;
    const int end = tokens.constLast().end;
    QString replacement = text.mid(begin, end - begin);
    int delta = 0;
    QVector<miacode::chart_transform::HandOrderEditPlan::Patch> patches;
    for (auto& token : tokens) {
        const QString encoded = token.baseToken + miacode::chart_transform::handOrderSuffix(token);
        patches.append({token.start, token.end, encoded});
        replacement.replace(token.start - begin + delta, token.end - token.start, encoded);
        const int oldLength = token.end - token.start;
        token.start += delta;
        token.end = token.start + encoded.size();
        delta += encoded.size() - oldLength;
    }
    auto result = selectionTransaction(text, begin, end, replacement);
    const QString resultingText = text.left(begin) + replacement + text.mid(end);
    const auto originalErrors = SimaiParser::validateSyntax(text).annotationErrors;
    const auto writtenErrors = SimaiParser::validateSyntax(resultingText).annotationErrors;
    if (writtenErrors.size() > originalErrors.size()) {
        handOrderFeedback_ = miacode::localizedText("hand_order.diagnostic").arg(writtenErrors.first().line)
            .arg(writtenErrors.first().col).arg(writtenErrors.first().message);
        emit handOrderChanged();
        return untouched(text, begin, end);
    }
    rememberHandOrderDraft(resultingText, tokens, patches);
    handOrderFeedback_ = miacode::localizedText("hand_order.applied");
    emit handOrderChanged();
    return result;
}
} // namespace miacode::ui

namespace miacode::ui {
namespace {
bool sameNativeSplitContext(const NativeSlideSplitContext& a, const NativeSlideSplitContext& b)
{
    return a.scope == b.scope && a.chartText == b.chartText && a.timing == b.timing
        && a.workspace.hasDocument == b.workspace.hasDocument
        && a.workspace.revision == b.workspace.revision
        && a.workspace.documentOpenGeneration == b.workspace.documentOpenGeneration
        && a.workspace.activeDifficultyId == b.workspace.activeDifficultyId
        && a.workspace.filePath == b.workspace.filePath
        && a.workspace.sourceText == b.workspace.sourceText
        && a.workspace.origin == b.workspace.origin;
}
QString nativeSplitLabel(const char* prefix, const QString& code)
{
    if (code.isEmpty()) return {};
    const QByteArray key = QByteArray(prefix) + code.toUtf8();
    const QString translated = qtTrId(key.constData());
    return translated == QString::fromUtf8(key) ? code : translated;
}
}
void EditorController::updateNativeSlideSplitContext(const NativeSlideSplitContext& context)
{
    if (sameNativeSplitContext(nativeSlideSplitContext_, context)) return;
    nativeSlideSplitContext_ = context;
    if (nativeSlideSplitAvailable_) {
        nativeSlideSplitStale_ = true;
        emit nativeSlideSplitChanged();
    }
}
bool EditorController::prepareNativeSlideSplit(const NativeSlideSplitContext& context, int anchor, int position)
{
    if (!handOrderFeaturesEnabled_) return false;
    updateNativeSlideSplitContext(context);
    cancelNativeSlideSplit();
    if (!context.workspace.hasDocument || context.workspace.activeDifficultyId <= 0 || context.scope.isEmpty()) return false;
    const auto document = SimaiDocument::fromText(context.workspace.sourceText);
    const auto* difficulty = document.difficulty(context.workspace.activeDifficultyId);
    if (!difficulty || difficulty->chart != context.chartText || miacode::simai::buildTimingMetadata(document) != context.timing) return false;
    nativeSlideSplitBegin_ = qBound(0, qMin(anchor, position), int(context.chartText.size()));
    nativeSlideSplitEnd_ = qBound(nativeSlideSplitBegin_, qMax(anchor, position), int(context.chartText.size()));
    if (nativeSlideSplitBegin_ == nativeSlideSplitEnd_) return false;
    preparedNativeSlideSplitContext_ = context;
    nativeSlideSplitAllowNegativeHs_ = SimaiParser::allowNegativeHsEnabled();
    nativeSlideSplitPlan_ = miacode::chart_transform::planNativeSlideSplit(context.chartText,
        nativeSlideSplitBegin_, nativeSlideSplitEnd_, context.timing);
    nativeSlideSplitAvailable_ = true; nativeSlideSplitStale_ = false;
    emit nativeSlideSplitChanged();
    return true;
}
bool EditorController::nativeSlideSplitCanApply() const
{
    if (!handOrderFeaturesEnabled_) return false;
    return nativeSlideSplitAvailable_ && !nativeSlideSplitStale_ && nativeSlideSplitPlan_.canApply
        && nativeSlideSplitAllowNegativeHs_ == SimaiParser::allowNegativeHsEnabled()
        && sameNativeSplitContext(nativeSlideSplitContext_, preparedNativeSlideSplitContext_);
}
QString EditorController::nativeSlideSplitMessage() const
{
    if (!nativeSlideSplitAvailable_) return {};
    if (nativeSlideSplitStale_) return qtTrId("hand_order.native.stale");
    const auto& plan = nativeSlideSplitPlan_;
    QString message = qtTrId("hand_order.native.summary").arg(plan.changed).arg(plan.ignored).arg(plan.partial).arg(plan.unsupported);
    if (!plan.canApply) message += QLatin1Char(10) + qtTrId("hand_order.native.blocked");
    for (const auto& code : plan.blockers) message += QLatin1Char(10) + nativeSplitLabel("hand_order.native.reason.", code);
    for (const auto& diagnostic : plan.diagnostics) message += QLatin1Char(10) + qtTrId("hand_order.diagnostic")
        .arg(diagnostic.line).arg(diagnostic.col).arg(diagnostic.message);
    return message;
}
QVariantList EditorController::nativeSlideSplitRows() const
{
    QVariantList rows;
    for (const auto& item : nativeSlideSplitPlan_.items) {
        QStringList changes;
        for (const auto& change : item.changes) changes.append(nativeSplitLabel("hand_order.native.change.", change));
        rows.append(QVariantMap{{"line", item.line}, {"col", item.col}, {"before", item.before}, {"after", item.after},
            {"status", item.status}, {"statusText", nativeSplitLabel("hand_order.native.status.", item.status)},
            {"reason", nativeSplitLabel("hand_order.native.reason.", item.reason)}, {"changes", changes.join(QLatin1Char(10))},
            {"beforeTracks", item.beforeTracks}, {"afterTracks", item.afterTracks},
            {"beforeHeads", item.beforeHeads}, {"afterHeads", item.afterHeads},
            {"counts", qtTrId("hand_order.native.counts").arg(item.beforeTracks).arg(item.afterTracks)
                .arg(item.beforeHeads).arg(item.afterHeads).arg(qMax(0, item.afterTracks - item.beforeTracks))}});
    }
    return rows;
}
miacode::editor::SimaiTextEditResult EditorController::applyNativeSlideSplit(const NativeSlideSplitContext& context)
{
    if (!handOrderFeaturesEnabled_) return untouched(context.chartText, 0, 0);
    updateNativeSlideSplitContext(context);
    if (nativeSlideSplitAllowNegativeHs_ != SimaiParser::allowNegativeHsEnabled()) {
        nativeSlideSplitStale_ = true; emit nativeSlideSplitChanged();
    }
    if (!nativeSlideSplitCanApply()) return untouched(context.chartText, nativeSlideSplitBegin_, nativeSlideSplitEnd_);
    const auto fresh = miacode::chart_transform::planNativeSlideSplit(context.chartText,
        nativeSlideSplitBegin_, nativeSlideSplitEnd_, context.timing);
    bool samePlan = fresh.canApply && fresh.replacement == nativeSlideSplitPlan_.replacement
        && fresh.patches.size() == nativeSlideSplitPlan_.patches.size();
    for (int i = 0; samePlan && i < fresh.patches.size(); ++i) {
        const auto& a = fresh.patches.at(i); const auto& b = nativeSlideSplitPlan_.patches.at(i);
        samePlan = a.start == b.start && a.end == b.end && a.text == b.text;
    }
    if (!samePlan) {
        nativeSlideSplitStale_ = true; emit nativeSlideSplitChanged();
        return untouched(context.chartText, nativeSlideSplitBegin_, nativeSlideSplitEnd_);
    }
    const auto result = selectionTransaction(context.chartText, nativeSlideSplitBegin_, nativeSlideSplitEnd_, nativeSlideSplitPlan_.replacement);
    nativeSlideSplitPlan_.canApply = false; nativeSlideSplitAvailable_ = false;
    emit nativeSlideSplitChanged();
    return result;
}
void EditorController::cancelNativeSlideSplit()
{
    nativeSlideSplitAvailable_ = false; nativeSlideSplitStale_ = false; nativeSlideSplitPlan_ = {};
    emit nativeSlideSplitChanged();
}
} // namespace miacode::ui
