#include "core/analysis/hand_order/HandOrderExperiment.h"
#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <limits>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool value, const char* message) {
        QTextStream(value ? stdout : stderr) << (value ? "PASS " : "FAIL ") << message << Qt::endl;
        ok &= value;
    };
    namespace e = miacode::hand_order::experiment;
    e::Analysis source; source.valid = true;
    e::Segment segment; segment.events = 8; segment.unresolved = 2;
    segment.peakSpeed = 16; segment.peakLoad = 2;
    source.segments.append(segment);
    e::SimulationParameters parameters;
    const auto first = e::simulate(source, parameters);
    const auto second = e::simulate(source, parameters);
    expect(first.valid && first.samples == 6 * parameters.trials,
        "unresolved objects are excluded from scenario samples");
    expect(QJsonDocument(e::toJson(first)).toJson() == QJsonDocument(e::toJson(second)).toJson(),
        "fixed analysis parameters and seed reproduce the complete result");
    auto fast = parameters; fast.movementSpeed = 20; fast.stamina = 2; fast.judgementJitterMs = 0;
    const auto easy = e::simulate(source, fast);
    auto slow = fast; slow.movementSpeed = 2;
    expect(easy.outsideWindow == 0 && e::simulate(source, slow).outsideWindow == easy.samples,
        "movement capacity changes the same synthetic timing scenario in the expected direction");
    auto tired = fast; tired.stamina = 0.5;
    expect(e::simulate(source, tired).outsideWindow > easy.outsideWindow,
        "lower stamina raises the scenario timing burden");
    auto delayed = fast; delayed.reactionMs = 500;
    expect(e::simulate(source, delayed).outsideWindow == easy.samples,
        "reaction delay contributes to reference-window exceedances");
    auto invalid = parameters; invalid.judgementJitterMs = std::numeric_limits<double>::quiet_NaN();
    expect(!e::simulate(source, invalid).valid, "nonfinite parameters are rejected");
    auto malformed = source; malformed.segments[0].events = -1;
    expect(!e::simulate(malformed, parameters).valid, "malformed external analysis cannot allocate unbounded samples");
    auto extremeLoad = source;
    extremeLoad.segments[0].peakLoad = std::numeric_limits<double>::max();
    auto lowStamina = parameters; lowStamina.stamina = 0.5;
    expect(!e::simulate(extremeLoad, lowStamina).valid,
        "finite maximum load is rejected when derived fatigue overflows");
    auto extremeSpeed = source;
    extremeSpeed.segments[0].peakSpeed = std::numeric_limits<double>::max();
    expect(!e::simulate(extremeSpeed, parameters).valid,
        "finite maximum speed cannot produce a valid infinite timing result");
    auto largeSum = source;
    largeSum.segments[0].peakSpeed = std::numeric_limits<double>::max() / 100.0;
    auto sumParameters = parameters;
    sumParameters.movementSpeed = 20; sumParameters.reactionMs = 100;
    expect(!e::simulate(largeSum, sumParameters).valid,
        "finite per-sample offsets cannot overflow the accumulated mean");
    const QString multibyte(e::maximumInputUtf8Bytes / 2, QChar(0x4e00));
    expect(multibyte.size() < e::maximumInputUtf8Bytes && !e::withinInputLimit(multibyte),
        "input bound uses UTF-8 bytes rather than UTF-16 code units");
    expect(e::withinInputLimit(QString(e::maximumInputUtf8Bytes, QLatin1Char('a')))
        && !e::withinInputLimit(QString(e::maximumInputUtf8Bytes + 1, QLatin1Char('a'))),
        "ASCII input respects the exact 256 KiB boundary");
    auto unresolved = source; unresolved.segments[0].unresolved = unresolved.segments[0].events;
    expect(e::simulate(unresolved, parameters).samples == 0,
        "an entirely unresolved segment does not invent input hands");
    auto cancellation = std::make_shared<std::atomic_bool>(true);
    bool cancelled = false;
    try {
        miacode::task::CancellationScope scope(cancellation);
        e::simulate(source, parameters);
    } catch (const miacode::task::Cancelled&) { cancelled = true; }
    expect(cancelled, "simulation cooperates with cancellation");

    const QString chart = QStringLiteral("(120){4}1~l,2~r,3~l,4~r,E");
    const auto parsed = SimaiParser::validateSyntax(chart);
    const auto analyzed = e::analyze(chart, parsed, {});
    expect(analyzed.valid && !analyzed.segments.isEmpty() && !analyzed.inputSha256.isEmpty(),
        "existing inference yields versioned source-located read-only evidence");
    bool protectedAnnotations = false;
    for (const auto& row : analyzed.segments) {
        protectedAnnotations |= row.reasons.contains(QStringLiteral("protected_annotation"));
        expect(row.sourceStart >= 0 && row.sourceEnd <= chart.size() && row.line >= 1
            && row.reasons.contains(QStringLiteral("uncalibrated")),
            "segment locations are bounded and uncertainty is explicit");
    }
    expect(protectedAnnotations && chart == QStringLiteral("(120){4}1~l,2~r,3~l,4~r,E"),
        "analysis protects existing annotations and never edits input text");
    const auto report = e::toJson(analyzed);
    expect(report.value("calibrated_against_human_players") == false
        && e::toJson(first).value("official_score_prediction") == false,
        "reports cannot be mistaken for calibrated player or official score predictions");
    return ok ? 0 : 1;
}
