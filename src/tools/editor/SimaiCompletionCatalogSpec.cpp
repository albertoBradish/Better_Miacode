#include "editor/SimaiCompletionCatalog.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {

bool expect(bool condition, const QString& message, QTextStream& out, int* failed)
{
    if (condition) {
        out << "[PASS] " << message << '\n';
        return true;
    }
    out << "[FAIL] " << message << '\n';
    if (failed != nullptr) {
        ++(*failed);
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);

    QTextStream out(stdout);
    int failed = 0;

    using namespace miacode::editor;

    // Static lists keep their product-decided order.
    expect(
        squareDurationCandidates()
            == QStringList({QStringLiteral("8:1]"), QStringLiteral("4:1]"),
                            QStringLiteral("16:3]"), QStringLiteral("384:1]")}),
        QStringLiteral("'[' duration candidates are 8:1 4:1 16:3 384:1 in order"),
        out, &failed);
    expect(
        braceDivisionCandidates()
            == QStringList({QStringLiteral("16}"), QStringLiteral("24}"),
                            QStringLiteral("32}")}),
        QStringLiteral("'{' subdivision candidates are 16 24 32 in order"),
        out, &failed);
    // Hold candidates mirror the '[' durations in full-bracket form (leading '['
    // included), and stay in lock-step with squareDurationCandidates().
    expect(
        holdDurationCandidates()
            == QStringList({QStringLiteral("[8:1]"), QStringLiteral("[4:1]"),
                            QStringLiteral("[16:3]"), QStringLiteral("[384:1]")}),
        QStringLiteral("'h' hold candidates are [8:1] [4:1] [16:3] [384:1] in order"),
        out, &failed);

    // Opening / closing helpers.
    expect(isBracketOpening(QLatin1Char('[')) && isBracketOpening(QLatin1Char('('))
               && isBracketOpening(QLatin1Char('{')),
           QStringLiteral("isBracketOpening true for ( [ {"), out, &failed);
    expect(!isBracketOpening(QLatin1Char('h')) && !isBracketOpening(QLatin1Char(']')),
           QStringLiteral("isBracketOpening false for non-openers"), out, &failed);
    expect(isBracketClosing(QLatin1Char(']')) && isBracketClosing(QLatin1Char(')'))
               && isBracketClosing(QLatin1Char('}')),
           QStringLiteral("isBracketClosing true for ) ] }"), out, &failed);
    expect(!isBracketClosing(QLatin1Char('h')) && !isBracketClosing(QLatin1Char('[')),
           QStringLiteral("isBracketClosing false for non-closers"), out, &failed);
    expect(closingBracketFor(QLatin1Char('[')) == QLatin1Char(']')
               && closingBracketFor(QLatin1Char('(')) == QLatin1Char(')')
               && closingBracketFor(QLatin1Char('{')) == QLatin1Char('}'),
           QStringLiteral("closingBracketFor maps each opener"), out, &failed);
    expect(closingBracketFor(QLatin1Char('x')).isNull(),
           QStringLiteral("closingBracketFor is null for non-openers"), out, &failed);

    // BPM token normalization.
    expect(normalizeBpmToken(QStringLiteral("120.000")) == QLatin1String("120"),
           QStringLiteral("normalizeBpmToken trims 120.000 -> 120"), out, &failed);
    expect(normalizeBpmToken(QStringLiteral(" 184.50 ")) == QLatin1String("184.5"),
           QStringLiteral("normalizeBpmToken trims trailing zero + whitespace"), out, &failed);
    expect(normalizeBpmToken(QStringLiteral("abc")) == QLatin1String("abc"),
           QStringLiteral("normalizeBpmToken passes through non-numeric"), out, &failed);

    // Appeared-BPM scan: first-seen order, deduplicated, normalized.
    const QString chart = QStringLiteral("(160){8}1,2,(120)3,4,(160.0)5,");
    expect(appearedBpmValues(chart)
               == QStringList({QStringLiteral("160"), QStringLiteral("120")}),
           QStringLiteral("appearedBpmValues finds distinct BPMs in first-seen order"),
           out, &failed);

    // '(' candidates: wholebpm first, then appeared, deduped, each closing.
    expect(parenBpmCandidates(QStringLiteral("120"), chart)
               == QStringList({QStringLiteral("120)"), QStringLiteral("160)")}),
           QStringLiteral("parenBpmCandidates puts wholebpm first then appeared"),
           out, &failed);
    expect(parenBpmCandidates(QString(), QString()).isEmpty(),
           QStringLiteral("parenBpmCandidates empty when nothing is known"), out, &failed);

    // Dispatch.
    expect(candidatesForOpening(QLatin1Char('['), QString(), QString())
               == squareDurationCandidates(),
           QStringLiteral("candidatesForOpening dispatches '[' to durations"), out, &failed);
    expect(candidatesForOpening(QLatin1Char('h'), QString(), QString()).isEmpty(),
           QStringLiteral("candidatesForOpening empty for non-openers"), out, &failed);

    const QString simple = QStringLiteral("1~");
    const auto simpleHand = handOrderCompletionAt(simple, simple.size());
    expect(simpleHand.startPosition == 2
               && simpleHand.candidates.mid(0, 3)
                   == QStringList({QStringLiteral("l"), QStringLiteral("r"), QStringLiteral("m")}),
           QStringLiteral("hand suffix completes after the tilde"), out, &failed);
    expect(!handOrderCompletionAt(QStringLiteral("12~"), 3).active()
               && !handOrderCompletionAt(QStringLiteral("1, || ~"), 7).active(),
           QStringLiteral("ambiguous digit clusters and comments offer no hand completion"), out, &failed);

    const QString singleTrack = QStringLiteral("1-5[4:1]~[");
    const auto singleKeys = handOrderCompletionAt(singleTrack, singleTrack.size());
    expect(singleKeys.startPosition == singleTrack.size()
               && singleKeys.candidates.contains(QStringLiteral("head="))
               && singleKeys.candidates.contains(QStringLiteral("track="))
               && !singleKeys.candidates.contains(QStringLiteral("8:1]")),
           QStringLiteral("annotation bracket offers keys instead of durations"), out, &failed);

    const QString multiTrack = QStringLiteral("1-5[4:1]*-7[4:1]~[branch0=0:l;");
    const auto branchKeys = handOrderCompletionAt(multiTrack, multiTrack.size());
    expect(branchKeys.candidates.contains(QStringLiteral("branch1="))
               && !branchKeys.candidates.contains(QStringLiteral("track="))
               && !branchKeys.candidates.contains(QStringLiteral("branch0=")),
           QStringLiteral("multi-branch container suggests only missing branch keys"), out, &failed);

    const QString handValue = QStringLiteral("1-5[4:1]~[head=l;track=0:l|0.5:");
    const auto trackHand = handOrderCompletionAt(handValue, handValue.size());
    expect(trackHand.startPosition == handValue.size()
               && trackHand.candidates == QStringList({QStringLiteral("l"), QStringLiteral("r"), QStringLiteral("m")}),
           QStringLiteral("track node hand completion replaces only the hand value"), out, &failed);
    const QString closedTrack = QStringLiteral("1-5[4:1]~[track=0:m]");
    expect(!handOrderCompletionAt(closedTrack, closedTrack.size()).active(),
           QStringLiteral("closed container does not reopen completion"), out, &failed);
    const QString wifiMode = QStringLiteral("1w5[4:1]~[track=");
    const auto wifiModes = handOrderCompletionAt(wifiMode, wifiMode.size());
    expect(wifiModes.candidates.contains(QStringLiteral("split:0:l&0:r"))
        && wifiModes.candidates.contains(QStringLiteral("single:0:m")),
        QStringLiteral("Wifi completion offers explicit split and whole-track single modes"), out, &failed);
    const QString wifiSecond = QStringLiteral("1w5[4:1]~[track=split:0:l&");
    expect(handOrderCompletionAt(wifiSecond, wifiSecond.size()).startPosition == wifiSecond.size()
        && handOrderCompletionAt(wifiSecond, wifiSecond.size()).candidates.contains(QStringLiteral("0:r")),
        QStringLiteral("second Wifi sector starts a new independent progress sequence"), out, &failed);
    const QString wifiSingle = QStringLiteral("1w5[4:1]~[track=single:0:l|0.5:");
    expect(handOrderCompletionAt(wifiSingle, wifiSingle.size()).candidates == QStringList{QStringLiteral("l")},
        QStringLiteral("explicit single-mode completion cannot suggest a different hand"), out, &failed);

    if (failed != 0) {
        out << "SimaiCompletionCatalog spec failed: " << failed << '\n';
        return 1;
    }

    out << "SimaiCompletionCatalog spec passed.\n";
    return 0;
}
