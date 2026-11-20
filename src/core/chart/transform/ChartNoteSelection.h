#pragma once

#include <QString>
#include <QStringView>
#include <QVector>

namespace miacode::chart_selection {

// Half-open UTF-16 spans of independent notes. A chained/branched Slide owns
// its complete source token; packed taps own one span per lane.
struct NoteRange {
    int start = 0;
    int end = 0;
    bool operator==(const NoteRange&) const = default;
};
using NoteRanges = QVector<NoteRange>;

enum class NoteComponent { Whole, SlideHead, SlideTrack };

struct NoteObject {
    NoteRange token;
    NoteRange highlight;
    NoteComponent component = NoteComponent::Whole;
    int trackIndex = -1;
    quint64 location = 0;
    bool operator==(const NoteObject&) const = default;
};
using NoteObjects = QVector<NoteObject>;

inline quint64 sourceLocationId(int line, int column)
{
    return (quint64(quint32(qMax(0, line))) << 32) | quint32(qMax(0, column));
}

struct NoteTimingState {
    QString bpm = QStringLiteral("(120)");
    QString subdivision = QStringLiteral("{4}");
    QString hs = QStringLiteral("<HS*1>");
    int meterNumerator = 4;
    int meterDenominator = 4;
};
struct NoteObjectEditResult {
    QString text;
    bool supported = true;
};
struct NotePasteResult {
    QString text;
    int caret = 0;
    QString error;
};

NoteRanges normalizedNoteRanges(NoteRanges ranges, int textLength);
QString deleteSelectedNotes(const QString& text, const NoteRanges& selected,
    const NoteRanges& allNotes);
QString copySelectedNotes(const QString& text, const NoteRanges& selected,
    const NoteRanges& allNotes, const NoteTimingState& initialTiming = {});
NoteObjectEditResult deleteSelectedNoteObjects(const QString& text, const NoteObjects& selected,
    const NoteRanges& allNotes);
NoteObjectEditResult copySelectedNoteObjects(const QString& text, const NoteObjects& selected,
    const NoteRanges& allNotes, const NoteTimingState& initialTiming = {});
bool selectsWholeNoteObject(QStringView token, const NoteObjects& objects);
NoteObjectEditResult transformSlideComponents(QStringView token, const NoteObjects& objects, QStringView operation);
NotePasteResult pasteNoteFragment(const QString& text, int start, int end,
    const QString& fragment, const NoteRanges& selectedNotes, const NoteRanges& allNotes,
    const NoteTimingState& initialTiming = {}, const NoteObjects& selectedObjects = {});

} // namespace miacode::chart_selection
