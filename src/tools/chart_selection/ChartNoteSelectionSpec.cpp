#include "core/chart/transform/ChartNoteSelection.h"
#include "core/chart/parser/SimaiParser.h"
#include "timeline/TimelineQuickModel.h"

#include <QCoreApplication>
#include <QDebug>

using namespace miacode::chart_selection;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool condition, const char* label) {
        if (!condition) { ++failures; qWarning() << "FAIL:" << label; }
    };
    const QString text = QStringLiteral("(150){8}<HS*2>1/2/3,4,,(240){16}5h[4:2]/A1,1-5-3[4:4], || 注释,/7\nE");
    TimelineQuickModel model;
    model.rebuildFromText(text, 0.0);
    const auto all = model.noteSourceRanges();
    const auto range = [&](const QString& token) {
        const int at = text.indexOf(token);
        return NoteRange{at, at + int(token.size())};
    };
    const auto first = range(QStringLiteral("1/2/3"));
    expect(deleteSelectedNotes(text, {{first.start + 2, first.start + 3}}, all)
        == QString(text).replace(first.start, 5, QStringLiteral("1/3")), "remove middle simultaneous operand and repair slash");
    expect(deleteSelectedNotes(text, {{first.start, first.start + 1}}, all)
        == QString(text).replace(first.start, 5, QStringLiteral("2/3")), "remove first operand without a leading slash");
    expect(deleteSelectedNotes(text, {{first.start + 4, first.start + 5}}, all)
        == QString(text).replace(first.start, 5, QStringLiteral("1/2")), "remove last operand without a trailing slash");
    const auto hold = range(QStringLiteral("5h[4:2]"));
    const auto chain = range(QStringLiteral("1-5-3[4:4]"));
    const NoteRanges selected{{first.start + 2, first.start + 3}, hold, chain};
    const auto resolved = model.resolveNoteSelection(selected);
    expect(resolved.locations.size() == 4, "a text selection projects the Slide head and track without intervening notes");
    expect(model.noteSourceRanges(&resolved.locations) == selected, "source locations round trip complete Slide tokens");
    const QString copied = copySelectedNotes(text, selected, all);
    const auto parsedCopy = SimaiParser::validateSyntax(copied + QStringLiteral("E"));
    expect(parsedCopy.ok && parsedCopy.noteMarkers.size() == 3, "clipboard is valid standalone Simai containing only selected notes");
    expect(copied.contains(QStringLiteral(",,,(240){16}")) && copied.startsWith(QStringLiteral("|| 4/4\n(150){8}<HS*2>2")),
        "clipboard retains empty beats, tempo and inherited subdivision/HS");
    const auto original = SimaiParser::parseForTimeline(text);
    expect(parsedCopy.noteMarkers.size() == 3 && original.noteMarkers.size() >= 7
        && qAbs(parsedCopy.noteMarkers[1].second - original.noteMarkers[4].second) < 1e-7
        && qAbs(parsedCopy.noteMarkers[2].second - original.noteMarkers.last().second) < 1e-7,
        "copied selected notes retain relative hit times across unselected notes and tempo changes");
    const auto cut = deleteSelectedNotes(text, selected, all);
    const auto parsedCut = SimaiParser::validateSyntax(cut);
    expect(parsedCut.ok && parsedCut.noteMarkers.size() == original.noteMarkers.size() - 3,
        "cut removes only the selected objects and remains valid");
    expect(cut.count(QLatin1Char(',')) == text.count(QLatin1Char(',')) && cut.contains(QStringLiteral("|| 注释,/7")),
        "cut preserves every timing comma and comment");
    for (const auto& note : parsedCut.noteMarkers) {
        bool same = false;
        for (const auto& before : original.noteMarkers)
            if (before.type == note.type && before.lane == note.lane && before.touchPad == note.touchPad
                && qAbs(before.second - note.second) < 1e-7) same = true;
        expect(same, "cut never moves a surviving note in time");
    }
    const QString split = QStringLiteral("(120){4}1/2`3,\nE");
    TimelineQuickModel splitModel;
    splitModel.rebuildFromText(split, 0.0);
    const int middle = split.indexOf(QLatin1Char('2'), 9);
    expect(deleteSelectedNotes(split, {{middle, middle + 1}}, splitModel.noteSourceRanges())
        == QStringLiteral("(120){4}1`3,\nE"), "separator repair preserves the original each-group boundary");
    const QString packed = QStringLiteral("(120){4}123,\nE");
    TimelineQuickModel packedModel;
    packedModel.rebuildFromText(packed, 0.0);
    expect(deleteSelectedNotes(packed, {{9, 10}}, packedModel.noteSourceRanges())
        == QStringLiteral("(120){4}13,\nE"), "packed lanes remain independent during deletion");
    expect(copySelectedNotes(text, {}, all).isEmpty() && deleteSelectedNotes(text, {}, all) == text,
        "an empty collection has no clipboard or document effect");
    const QString target = QStringLiteral("(120){4}<HS*3>6,|| 3/8\n7,8,\nE");
    TimelineQuickModel targetModel;
    targetModel.rebuildFromText(target, 0);
    const int caret = target.indexOf(QStringLiteral("7,"));
    const auto pasted = pasteNoteFragment(target, caret, caret, copied, {}, targetModel.noteSourceRanges());
    const auto parsedPaste = SimaiParser::validateSyntax(pasted.text);
    expect(pasted.error.isEmpty() && parsedPaste.ok && parsedPaste.noteMarkers.size() == 6,
        "a fragment pastes into a nonempty target without terminal E corruption");
    expect(pasted.text.endsWith(target.mid(caret)) && pasted.text.startsWith(target.left(caret)),
        "normal paste preserves every original character outside its insertion");
    if (parsedPaste.noteMarkers.size() == 6) {
        const auto& seven = parsedPaste.noteMarkers[4];
        const auto& eight = parsedPaste.noteMarkers[5];
        expect(seven.lane == 7 && eight.lane == 8 && seven.hsMultiplier == 3 && eight.hsMultiplier == 3
            && qAbs(eight.second - seven.second - 0.5) < 1e-7,
            "paste restores target BPM subdivision and HS for following notes");
        expect(parsedPaste.timingPoints.last().meterNumerator == 3 && parsedPaste.timingPoints.last().meterDenominator == 8,
            "paste restores the target inline meter");
    }
    const auto replace = pasteNoteFragment(target, caret, caret, copied,
        {{caret, caret + 1}, {caret + 2, caret + 3}}, targetModel.noteSourceRanges());
    const auto parsedReplace = SimaiParser::validateSyntax(replace.text);
    expect(replace.error.isEmpty() && parsedReplace.ok && parsedReplace.noteMarkers.size() == 4,
        "noncontiguous collection replacement inserts the copied fragment once");
    const QString defaultHs = QStringLiteral("(120){4}2,\nE");
    TimelineQuickModel defaultHsModel;
    defaultHsModel.rebuildFromText(defaultHs, 0);
    const auto plain = copySelectedNotes(defaultHs, {{8,9}}, defaultHsModel.noteSourceRanges());
    expect(plain.contains(QStringLiteral("<HS*1>")) && !plain.trimmed().endsWith(QLatin1Char('E')),
        "plain clipboard fragment explicitly resets inherited HS and has no terminal marker");
    const QString grouped = QStringLiteral("(120){4}1/3,4h[4:1],|| words\n8,\nE");
    TimelineQuickModel groupedModel;
    groupedModel.rebuildFromText(grouped, 0);
    for (int at : {grouped.indexOf(QLatin1Char('/')), grouped.indexOf(QStringLiteral("h[")) + 2,
        grouped.indexOf(QStringLiteral("words")) + 2, grouped.indexOf(QStringLiteral(",4h"))}) {
        const auto insertion = pasteNoteFragment(grouped, at, at, plain, {}, groupedModel.noteSourceRanges());
        const auto parsedInsertion = SimaiParser::validateSyntax(insertion.text);
        expect(insertion.error.isEmpty() && parsedInsertion.ok && parsedInsertion.noteMarkers.size() == 5,
            "pasting at separators, within tokens, after notes or in comments preserves original tokens and remains valid");
    }
    const QString slideText = QStringLiteral("(120){4}1bx-5[4:1]*-3[8:1]/3,1?w5[8:1],\nE");
    TimelineQuickModel slideModel;
    slideModel.rebuildFromText(slideText, 0);
    const auto slideObjects = slideModel.noteObjects();
    const auto slideNotes = slideModel.noteSourceRanges();
    expect(slideObjects.size() == 5
        && slideObjects.value(0).component == NoteComponent::SlideHead
        && slideObjects.value(1).component == NoteComponent::SlideTrack && slideObjects.value(1).trackIndex == 0
        && slideObjects.value(2).component == NoteComponent::SlideTrack && slideObjects.value(2).trackIndex == 1
        && slideObjects.value(4).component == NoteComponent::SlideTrack,
        "same-head branches have one head and distinct track identities, while headless WiFi has only a track");
    if (slideObjects.size() == 5) {
        const auto parsed = SimaiParser::parseForTimeline(slideText);
        expect(parsed.noteMarkers.size() == 4
            && sourceLocationId(parsed.noteMarkers[0].sourceLine, parsed.noteMarkers[0].slideTrackSourceCol) == slideObjects[1].location
            && sourceLocationId(parsed.noteMarkers[1].sourceLine, parsed.noteMarkers[1].slideTrackSourceCol) == slideObjects[2].location,
            "preview and timeline publish the same branch source identities");

        const NoteObjects head{slideObjects[0]}, firstTrack{slideObjects[1]}, secondTrack{slideObjects[2]};
        const auto deleteHead = deleteSelectedNoteObjects(slideText, head, slideNotes);
        expect(deleteHead.supported && deleteHead.text.contains(QStringLiteral("1?-5[4:1]*-3[8:1]/3"))
            && SimaiParser::validateSyntax(deleteHead.text).ok,
            "deleting the head preserves both tracks using valid headless syntax");
        const auto deleteBranch = deleteSelectedNoteObjects(slideText, firstTrack, slideNotes);
        expect(deleteBranch.supported && deleteBranch.text.contains(QStringLiteral("1bx-3[8:1]/3"))
            && SimaiParser::validateSyntax(deleteBranch.text).ok,
            "deleting one track preserves the original EX/Break head and the other track");
        const auto deleteTracks = deleteSelectedNoteObjects(slideText, {slideObjects[1], slideObjects[2]}, slideNotes);
        expect(deleteTracks.supported && deleteTracks.text.contains(QStringLiteral("1bx$$/3"))
            && SimaiParser::validateSyntax(deleteTracks.text).ok,
            "deleting all tracks preserves a standalone Break EX star");
        const auto headFragment = copySelectedNoteObjects(slideText, head, slideNotes);
        const auto trackFragment = copySelectedNoteObjects(slideText, secondTrack, slideNotes);
        expect(headFragment.supported && headFragment.text.contains(QStringLiteral("1bx$$,"))
            && !headFragment.text.contains(QStringLiteral("-5"))
            && trackFragment.supported && trackFragment.text.contains(QStringLiteral("1?-3[8:1],"))
            && SimaiParser::validateSyntax(trackFragment.text + QStringLiteral("E")).ok,
            "component clipboard fragments retain only the selected semantic parts");
        const auto pastedTrack = pasteNoteFragment(slideText, 0, 0, trackFragment.text, {}, slideNotes, {}, head);
        expect(!pastedTrack.error.isEmpty() && pastedTrack.text == slideText,
            "contextual paste refuses to shift an unselected track by replacing its head");
        const auto insertedTrack = pasteNoteFragment(slideText, slideText.indexOf(QLatin1Char('E')),
            slideText.indexOf(QLatin1Char('E')), trackFragment.text, {}, slideNotes);
        expect(insertedTrack.error.isEmpty() && SimaiParser::validateSyntax(insertedTrack.text).ok,
            "a copied track can be pasted as a headless fragment at a caret");

        const auto token = QStringView(slideText).sliced(head.first().token.start,
            head.first().token.end - head.first().token.start);
        const auto toggleHeadEx = transformSlideComponents(token, head, u"transform.toggle_ex");
        const auto toggleTrackBreak = transformSlideComponents(token, secondTrack, u"transform.toggle_break");
        const auto partialRotate = transformSlideComponents(token, firstTrack, u"transform.rotate_cw_45");
        expect(toggleHeadEx.supported && toggleHeadEx.text == QLatin1String("1b-5[4:1]*-3[8:1]")
            && toggleTrackBreak.supported && toggleTrackBreak.text == QLatin1String("1bx-5[4:1]*-3b[8:1]"),
            "partial modifier changes leave the other components byte-for-byte intact");
        expect(!partialRotate.supported && partialRotate.text == token,
            "a partial spatial transform refuses to move the shared start lane");
        const auto headRange = slideModel.resolveNoteObjects(head, 6.0, 6.0);
        const auto trackRange = slideModel.resolveNoteObjects(firstTrack, 6.0, 6.0);
        expect(headRange.resolved() && trackRange.resolved() && headRange.endSecond < trackRange.endSecond,
            "auditioning only the head excludes the track's tail");
    }
    return failures == 0 ? 0 : 1;
}
