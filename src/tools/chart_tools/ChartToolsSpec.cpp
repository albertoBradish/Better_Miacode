#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/ChartBpmExport.h"
#include "core/chart/ChartBpmImport.h"
#include "core/chart/ChartScoreLoss.h"
#include "core/chart/ChartTiming.h"
#include "core/chart/document/SimaiDocument.h"
#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    QTextStream log(stdout);
    const auto expect = [&](bool condition, const char* message) {
        log << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
        if (!condition) ++failures;
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-10; };
    using namespace miacode::chart;
    {
        const auto parsed = SimaiParser::parseForTimeline(QStringLiteral("(120){4},(180)(90){8},|| 7/8\n(90){3},E"));
        expect(parsed.ok, "timing fixture parses");
        expect(parsed.timingPoints.size() == 3, "same-time assignments fold in source order");
        expect(timingAt(parsed.timingPoints, 0.6).bpm == 90.0, "last same-time BPM wins");
        expect(close(timingAt(parsed.timingPoints, 0.6).quarterBeat, 1.15), "quarter phase is continuous across BPM changes");
        expect(close(timingAt(parsed.timingPoints, -0.5).quarterBeat, -1.0), "negative count-in phase extrapolates");
        expect(!timingAt(parsed.timingPoints, 0.5).meterExplicit, "implicit 4/4 stays implicit");
        expect(timingAt(parsed.timingPoints, 1.0).meterExplicit
            && timingAt(parsed.timingPoints, 1.0).meterNumerator == 7, "explicit inline meter is authoritative");
        expect(exportChartBpm(parsed, 0, BpmExportFormat::Sequence) == QStringLiteral("120.00-90.00"), "consecutive repeated tempos merge");
        expect(exportChartBpm(parsed, -2.0, BpmExportFormat::AudioTime).startsWith(QStringLiteral("-00:02:000")), "audio timestamps include negative first");
    }
    {
        const auto metadata = miacode::simai::buildTimingMetadataFromRawText(QStringLiteral("&whole_time_signature=3/4"));
        const auto parsed = SimaiParser::parseForTimeline(QStringLiteral("(123.45678912345678){7},,,(180){16},|| 7/8\n,,(180){3},,\nE"), metadata);
        const auto text = exportChartBpm(parsed, -3.125, BpmExportFormat::EmptyMaidata);
        const auto document = SimaiDocument::fromText(text);
        const auto* difficulty = document.difficulty(1);
        expect(difficulty != nullptr, "empty maidata produces a real difficulty");
        if (difficulty) {
            const auto again = SimaiParser::parseForTimeline(difficulty->chart, miacode::simai::buildTimingMetadata(document));
            expect(again.ok && again.noteMarkers.isEmpty(), "empty maidata is parseable and note-free");
            expect(close(parsed.endSecond, again.endSecond) && close(parsed.endQuarterBeat, again.endQuarterBeat), "empty maidata preserves terminal time and phase");
            expect(again.timingPoints.size() == parsed.timingPoints.size(), "empty maidata retains all timing states");
            bool equal = again.timingPoints.size() == parsed.timingPoints.size();
            for (int index = 0; equal && index < parsed.timingPoints.size(); ++index) {
                const auto a = parsed.timingPoints[index], b = again.timingPoints[index];
                equal = close(a.second, b.second) && a.bpm == b.bpm && a.meterExplicit == b.meterExplicit
                    && a.meterNumerator == b.meterNumerator && a.meterDenominator == b.meterDenominator;
            }
            expect(equal, "round-trip timing preserves BPM precision and explicit meters");
            expect(document.first.toDouble() == -3.125, "empty maidata retains first");
        }
    }
    {
        TimelineNoteMarker hold; hold.type = QStringLiteral("hold"); hold.second = 0; hold.endSecond = 2;
        TimelineNoteMarker slide; slide.type = QStringLiteral("slide"); slide.second = 1; slide.endSecond = 3;
        slide.sourceCol = 3; slide.headEx = true;
        auto mine = hold; mine.isMine = true;
        const auto events = buildScoringEvents({hold, slide, slide, mine});
        expect(events.events.size() == 4 && events.hasUnofficialExtensions, "shared slide heads deduplicate while tracks stay independent; mines are excluded");
        expect(scoringEventsInPastSecond(events.events, 1) == 1, "CPS counts tap head at t");
        expect(scoringEventsInPastSecond(events.events, 2) == 1, "CPS excludes t-1 and counts completed hold");
        expect(scoringEventsInPastSecond(events.events, 3) == 2, "CPS counts both completed slide tracks");
        const auto stats = calculateScoreLoss(events);
        expect(!stats.calculable && stats.total == 4 && stats.counts[7] == 1, "extensions keep counts and disable DX calculation without double-counting EX");
    }
    {
        ScoringEvents events;
        events.events.append({0, ScoringKind::Star, true, false, false});
        for (int i = 0; i < 195; ++i) events.events.append({0, ScoringKind::Tap});
        const auto stats = calculateScoreLoss(events);
        expect(stats.calculable && stats.baseTotal == 100000 && stats.breakTotal == 1, "official DX denominator includes break base score");
        bool boundary = false, breakBoundary = false;
        for (const auto& row : stats.rows) {
            if (row.object == QLatin1String("Tap") && row.judgment == QLatin1String("Great"))
                boundary = close(row.lossPercent, .1) && row.maximumSss == 10 && row.maximumSssPlus == 5;
            if (row.object == QLatin1String("Break") && row.judgment == QLatin1String("Perfect 2500"))
                breakBoundary = close(row.lossPercent, .5) && row.maximumSss == 1 && row.maximumSssPlus == 1;
        }
        expect(boundary, "SSS capacities include exact equality before display rounding");
        expect(breakBoundary, "break bonus loss uses 100 points each and capacity is capped by actual count");
        const auto noBreak = calculateScoreLoss(ScoringEvents{{ScoringEvent{0, ScoringKind::Tap}}, false});
        expect(noBreak.rows.first().maximumSss == 0 && noBreak.rows.first().maximumSssPlus == 0,
            "no-break chart has no loss budget for SSS");
        expect(!calculateScoreLoss({}).calculable, "empty chart cannot calculate achievement");
    }
    {
        const auto parsed = SimaiParser::parseForTimeline(QStringLiteral("(135){7}1h[#12.345],{997},,(128){384},E"));
        const auto text = exportChartBpm(parsed, 0.0, BpmExportFormat::EmptyMaidata);
        const auto document = SimaiDocument::fromText(text);
        const auto* difficulty = document.difficulty(1);
        expect(parsed.ok && difficulty, "arbitrary positive integer subdivisions are accepted");
        if (difficulty) {
            const auto again = SimaiParser::parseForTimeline(difficulty->chart);
            expect(close(parsed.endSecond, again.endSecond), "empty maidata preserves source terminal comma position");
            expect(parsed.durationSeconds > again.durationSeconds,
                "removing a sustained note removes its tail duration without inventing a BPM change");
            expect(text.contains(QStringLiteral("{997}")), "export retains the authoritative integer subdivision");
        }
        expect(!SimaiParser::parseForTimeline(QStringLiteral("(120){#1.5},E")).ok,
            "absolute-second subdivisions are not accepted by this parser");
        expect(!SimaiParser::parseForTimeline(QStringLiteral("(120){2.5},E")).ok,
            "fractional subdivisions are not accepted by this parser");
    }
    {
        TimelineNoteMarker mine; mine.type = QStringLiteral("tap"); mine.isMine = true;
        const auto onlyMine = calculateScoreLoss(buildScoringEvents({mine}));
        expect(onlyMine.mineCount == 1 && onlyMine.hasUnofficialExtensions && !onlyMine.calculable,
            "mine-only chart exposes its extension count and cannot calculate DX");
        TimelineNoteMarker tap; tap.type = QStringLiteral("tap"); tap.hsMultiplier = 2.0;
        expect(calculateScoreLoss(buildScoringEvents({tap})).calculable,
            "visual HS does not disable an otherwise official DX calculation");
        TimelineNoteMarker reverse; reverse.type = QStringLiteral("slide"); reverse.hasHeadStar = false;
        reverse.second = 1; reverse.endSecond = 2; reverse.slideTraceSecond = 3;
        expect(buildScoringEvents({reverse}).events.first().second == 2,
            "slide completion keeps the established endSecond priority");
    }
    {
        ScoringEvents events;
        events.events = {{0, ScoringKind::Tap, false, true}, {0, ScoringKind::Star, true, true},
            {0, ScoringKind::Hold, true, true, false, true}};
        const auto stats = calculateScoreLoss(events);
        bool exTapLocked = true, break2550 = false, break2500 = true;
        for (const auto& row : stats.rows) {
            if (row.object == QLatin1String("Tap") && row.judgment == QLatin1String("Great"))
                exTapLocked = false;
            if (row.object == QLatin1String("Break") && row.judgment == QLatin1String("Perfect 2550"))
                break2550 = row.quantity == 1;
            if (row.object == QLatin1String("Break") && row.judgment == QLatin1String("Perfect 2500"))
                break2500 = false;
        }
        expect(exTapLocked, "EX tap cannot supply Great or Good tolerance");
        expect(break2550 && break2500, "EX hold tail may degrade to 2550 but EX heads cannot supply 2500");
    }
    {
        const auto events = buildScoringEvents(SimaiParser::parseForTimeline(QStringLiteral("(120){4}A1/A2/A3/1,A4/A5,2,E")).noteMarkers);
        expect(events.events.size() == 7, "multi-touch leaves score event and object counts intact");
        expect(scoringEventsInPastSecond(events.events, 0) == 2, "simultaneous touches count once alongside a tap");
        expect(scoringEventsInPastSecond(events.events, 0.5) == 3, "different touch timestamps each contribute one CPS");
        expect(scoringEventsInPastSecond(events.events, 1) == 2, "rolling CPS excludes touches at t-1");
        const auto stats = calculateScoreLoss(events);
        bool allLosing = true;
        for (const auto& row : stats.rows) allLosing &= row.lossPercent > 0 && row.quantity > 0;
        expect(allLosing, "score loss table only contains possible losing judgments");
    }
    {
        ScoringEvents events;
        events.events.append({0, ScoringKind::Tap, true});
        for (int i = 0; i < 39; ++i) events.events.append({0, ScoringKind::Slide});
        const auto stats = calculateScoreLoss(events);
        expect(stats.simpleGreatSss == 6 && stats.simpleGreatSssPlus == 3, "simple Tap Great equivalent budget is not capped by Tap inventory");
    }
    {
        const QStringList originals{
            QStringLiteral("(120){4}1,2h[4:1],3-5[4:1],A1/A2,|| keep (200) {99}\n(180){8}4h[8:1],E"),
            QStringLiteral("(123.456789){7}1h[#1.23],2-4[0.25##4:1],<HS*2>A1/A2,{16}4,|| 7/8\n5,E"),
            QStringLiteral("(120){4}1-5[4:1]*-3[8:1],,,E"),
            QStringLiteral("(180){8}1h[120#4:3],2-6[150#8:1],C1h[4:1],E")};
        for (const auto& original : originals) {
            const auto result = importChartBpm(QStringLiteral("00:00:000/150.00 - 00:00:750/90.00"), original, {}, 0);
            expect(result.ok(), "BPM import preserves taps, holds, slides, branches, touchholds, HS and comments");
            if (!result.ok()) log << result.error << '\n';
        }
        const auto clean = importChartBpm(QStringLiteral("00:00:000/135 - 00:03:250/180 - 00:07:500/120"), originals.first(), {}, 0);
        expect(clean.ok() && SimaiParser::validateSyntax(clean.chart).ok && SimaiParser::validateSyntax(clean.chart).warnings.isEmpty(), "common BPM imports introduce no after-note directive or fine-subdivision warnings");
        expect(!importChartBpm(QStringLiteral("00:00:000/150"), QStringLiteral("(120){4}1\n,2,E"), {}, 0).ok(),
            "strict syntax errors cannot be carried into a BPM transaction");
        const auto nonStandard = importChartBpm(QStringLiteral("00:00:000/144"), QStringLiteral("(120){4}1,2,E"), {}, 0);
        expect(nonStandard.ok() && nonStandard.nonStandardSubdivisions.contains(QStringLiteral("10")),
            "non-384 subdivisions needed for exact audio timing are surfaced before applying");
        const auto negative = importChartBpm(QStringLiteral("-00:02:000/150.00 - -00:01:250/90.00"), originals.first(), {}, -2);
        expect(negative.ok(), "negative audio timestamps include first exactly once");
        const auto timing = SimaiParser::parseForTimeline(QStringLiteral("(135.123456789){7},,,(180){16},|| 7/8\n,,E"));
        const auto imported = importChartBpm(exportChartBpm(timing, -0.25, BpmExportFormat::EmptyMaidata), originals.first(), {}, 0);
        expect(imported.ok(), "empty maidata imports precise tempo, offset and meter states into populated charts");
        const auto empty = importChartBpm(QStringLiteral("00:00:000/120 - 00:02:123/150"), QString(), {}, 0);
        expect(empty.ok() && SimaiParser::parseForTimeline(empty.chart).noteMarkers.isEmpty(), "empty chart receives a valid timing skeleton");
        expect(!importChartBpm(QStringLiteral("120-180"), originals.first(), {}, 0).ok(), "untimed BPM sequences are rejected");
        expect(!importChartBpm(QStringLiteral("00:01:000/120 - 00:00:000/180"), originals.first(), {}, 0).ok(), "unordered timestamps are rejected");
        expect(!importChartBpm(QStringLiteral("&first=0\n&inote_1=(120){4}1,E"), originals.first(), {}, 0).ok(), "maidata with notes cannot masquerade as a timing skeleton");
    }
    {
        ScoringEvents events;
        events.events = {{0, ScoringKind::Tap, false, false}, {0, ScoringKind::Tap, false, true},
                         {0, ScoringKind::Star, true, false}, {0, ScoringKind::Hold, true, true, false, true}};
        const auto score = calculateScoreLoss(events);
        QVector<int> values(score.rows.size(), 0);
        auto rowFor = [&](const QString& object, const QString& judgment) {
            for (int i = 0; i < score.rows.size(); ++i)
                if (score.rows[i].object == object && score.rows[i].judgment == judgment) return i;
            return -1;
        };
        const int tapGreat = rowFor("Tap", "Great"), tapGood = rowFor("Tap", "Good");
        const int breakGreat = rowFor("Break", "Great 2000"), breakGood = rowFor("Break", "Good 1000");
        expect(tapGreat >= 0 && tapGood >= 0 && breakGreat >= 0 && breakGood >= 0, "scenario fixture exposes nested official inventories");
        const auto perfect = calculateScoreScenario(score, values);
        expect(perfect.valid() && close(perfect.achievement, 101.0) && perfect.enteredJudgments == 0,
            "zero entered judgments retain the official maximum achievement");
        if (tapGreat >= 0 && tapGood >= 0 && breakGreat >= 0 && breakGood >= 0) {
            values[tapGreat] = 1; values[tapGood] = 1;
            expect(calculateScoreScenario(score, values).error == ScoreScenarioError::InventoryExceeded,
                "Great and Good cannot both consume the same single non-EX tap");
            values.fill(0); values[breakGreat] = 1; values[breakGood] = 1;
            expect(calculateScoreScenario(score, values).valid(),
                "non-EX break and degrading EX hold can supply distinct mixed judgments");
            values[breakGood] = 2;
            expect(calculateScoreScenario(score, values).error == ScoreScenarioError::InventoryExceeded,
                "mixed break rows cannot exceed their shared physical inventory");
            values.fill(0); values[tapGreat] = -1;
            expect(calculateScoreScenario(score, values).error == ScoreScenarioError::InvalidQuantity,
                "negative judgment quantities are rejected");
        }
        values.fill(0);
        for (int i = 0; i < score.rows.size(); ++i)
            if (score.rows[i].judgment == QLatin1String("Miss")) values[i] = score.rows[i].quantity;
        const auto missed = calculateScoreScenario(score, values);
        expect(missed.valid() && close(missed.achievement, 0.0), "all-Miss scenario reaches zero achievement without double counting attributes");
        expect(!calculateScoreScenario(calculateScoreLoss({}), {}).valid(), "empty chart has no scenario result");
        expect(!calculateScoreScenario(calculateScoreLoss(ScoringEvents{{}, true, 1}), {}).valid(),
            "unsupported extension inventory cannot produce a misleading scenario");
    }
    {
        using namespace miacode::metadata;
        expect(validateField("whole_time_signature", "7/8").isEmpty()
            && !validateField("whole_time_signature", "7/0").isEmpty(), "metadata meter references use parser validation");
        expect(validateField("clock_count", "0").isEmpty() && !validateField("clock_count", "2.5").isEmpty(),
            "count-in reference accepts zero and rejects noninteger values");
        bool registeredDrop = false, openCabinet = false;
        for (const auto& field : advancedFields(5)) {
            if (field.key == QLatin1String("miacode_drop_start_5")) registeredDrop = field.readOnly && field.unit == QLatin1String("s");
            if (field.key == QLatin1String("cabinet")) openCabinet = !field.suggestions.isEmpty() && !field.readOnly;
        }
        expect(registeredDrop && openCabinet && validateField("cabinet", "Custom cabinet").isEmpty(),
            "managed drop fields are read-only and catalog suggestions do not reject custom values");
        const auto doc = SimaiDocument::fromText(QStringLiteral("&title=Field fixture\n&demo_seek=5\n&demo_len=10\n&miacode_drop_start_5=20\n&miacode_drop_end_5=30\n&vendor_unknown=keep-me\n&lv_5=1\n&inote_5=(120){4}1,E\n"));
        const auto again = SimaiDocument::fromText(doc.toText());
        expect(fieldValue(again, "vendor_unknown") == QLatin1String("keep-me")
            && fieldValue(again, "demo_seek") == QLatin1String("5")
            && fieldValue(again, "miacode_drop_start_5") == QLatin1String("20"),
            "field registry preserves unknown values and never aliases drop to the demo window");
    }
    return failures ? 1 : 0;
}
