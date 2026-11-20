#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>

#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/BasicHandOrderLayout.h"

namespace {

QJsonArray messages(const QVector<SimaiMessage>& items)
{
    QJsonArray result;
    for (const auto& item : items)
        result.append(QJsonObject{{"line", item.line}, {"column", item.col},
                                  {"message", item.message}});
    return result;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

QString identity(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}

QString sha256(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

void increment(QJsonObject& counts, const QString& key)
{
    counts.insert(key, counts.value(key).toInt() + 1);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const miacode::hand_order::BasicParameters defaults;
    QCommandLineParser cli;
    cli.setApplicationDescription("Audit automatic sticking on a local maidata copy. No player-quality claim is implied by replay verification.");
    cli.addHelpOption();
    cli.addPositionalArgument("maidata", "Source maidata.txt (read only).");
    cli.addOption({"difficulty", "Difficulty slot, 1 through 7.", "id", "5"});
    cli.addOption({"output", "Annotated maidata copy; must differ from source.", "path"});
    cli.addOption({"report", "Structured audit JSON; must differ from source and output.", "path"});
    cli.addOption({"layout-output", "Optional full-difficulty basic-layout maidata before inference.", "path"});
    cli.addOption({"begin", "Inclusive UTF-16 offset within the selected inote value.", "offset", "0"});
    cli.addOption({"end", "Exclusive UTF-16 offset within the selected inote value; defaults to its end.", "offset"});
    cli.addOption({"max-work", "Maximum inference work units.", "units", QString::number(defaults.maxWorkUnits)});
    cli.addOption({"beam-width", "Search beam width.", "count", "64"});
    cli.addOption({"max-tasks", "Maximum tasks per search window.", "count", "512"});
    cli.addOption({"counterfactual-budget", "Counterfactual search budget.", "count", "64"});
    cli.addOption({"return-speed", "Hand return speed in field radii per second.", "speed", "3"});
    cli.addOption({"sweep-threshold", "Integer 16th-note BPM equivalent for key sweeps.", "bpm16",
        QString::number(defaults.sweepThreshold)});
    cli.addOption({"input-preference", "0 exterior, 1 screen, 2 mixed.", "mode", "0"});
    cli.addOption({"overwrite-existing", "Permit changing existing annotations in the output copy."});
    cli.addOption({"color-unresolved", "Color unresolved notes with an unverified layout fallback."});
    cli.addOption({"tap-rate", "Single hand reference strike limit (hits/s).", "rate", QString::number(defaults.player.maxComfortableTapRate)});
    cli.addOption({"move-speed", "Reference movement limit (field radii/s).", "speed", QString::number(defaults.player.comfortableMoveSpeed)});
    cli.addOption({"stamina", "Relative stamina capacity.", "factor", "1"});
    cli.addOption({"recovery", "Load recovery half-life (seconds).", "seconds", "2.5"});
    cli.addOption({"technique", "-1 decomposition, 0 balanced, +1 repeated strikes.", "preference", QString::number(defaults.player.techniquePreference)});
    cli.addOption({"posture-weight", "Natural posture weight (0 through 8).", "weight", QString::number(defaults.player.postureWeight)});
    cli.process(app);
    const auto fail = [](const QString& error) {
        QTextStream(stderr) << error << '\n';
        return 2;
    };
    if (cli.positionalArguments().size() != 1 || !cli.isSet("output") || !cli.isSet("report"))
        return fail("Expected one input and both --output and --report; see --help.");
    const QString inputPath = QFileInfo(cli.positionalArguments().first()).absoluteFilePath();
    const QString outputPath = QFileInfo(cli.value("output")).absoluteFilePath();
    const QString reportPath = QFileInfo(cli.value("report")).absoluteFilePath();
    const QString layoutPath = cli.isSet("layout-output")
        ? QFileInfo(cli.value("layout-output")).absoluteFilePath() : QString{};
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    if (identity(inputPath).compare(identity(outputPath), pathCase) == 0
        || identity(inputPath).compare(identity(reportPath), pathCase) == 0
        || identity(outputPath).compare(identity(reportPath), pathCase) == 0)
        return fail("Input, output and report paths must be distinct.");
    if (!layoutPath.isEmpty()) for (const auto& other : {inputPath, outputPath, reportPath})
        if (identity(layoutPath).compare(identity(other), pathCase) == 0)
            return fail("Layout output must differ from input, output and report.");

    bool valid = true;
    const auto integer = [&](const QString& name, qint64 minimum, qint64 maximum) {
        bool ok = false;
        const qint64 value = cli.value(name).toLongLong(&ok);
        valid &= ok && value >= minimum && value <= maximum;
        return value;
    };
    const int difficultyId = int(integer("difficulty", 1, 7));
    auto parameters = defaults;
    parameters.maxWorkUnits = integer("max-work", 1000, miacode::hand_order::BasicParameters::maxAllowedWorkUnits);
    parameters.beamWidth = int(integer("beam-width", 1, 128));
    parameters.maxTasks = int(integer("max-tasks", 1, 2048));
    parameters.counterfactualBudget = int(integer("counterfactual-budget", 0, 128));
    parameters.inputPreference = int(integer("input-preference", 0, 2));
    parameters.sweepThreshold = int(integer("sweep-threshold",
        miacode::chart::handOrderMinSweepThreshold, miacode::chart::handOrderMaxSweepThreshold));
    parameters.overwriteExisting = cli.isSet("overwrite-existing");
    parameters.colorUnresolved = cli.isSet("color-unresolved");
    const auto number = [&](const QString& name) {
        bool ok = false;
        const double value = cli.value(name).toDouble(&ok);
        valid &= ok && std::isfinite(value);
        return value;
    };
    parameters.player.maxComfortableTapRate = number("tap-rate");
    parameters.player.comfortableMoveSpeed = number("move-speed");
    parameters.player.staminaCapacity = number("stamina");
    parameters.player.recoveryHalfLife = number("recovery");
    parameters.player.techniquePreference = number("technique");
    parameters.player.postureWeight = number("posture-weight");
    valid &= miacode::hand_order::validPlayerProfile(parameters.player);
    bool speedOk = false;
    parameters.returnSpeed = cli.value("return-speed").toDouble(&speedOk);
    valid &= speedOk && std::isfinite(parameters.returnSpeed)
        && parameters.returnSpeed >= parameters.minReturnSpeed
        && parameters.returnSpeed <= parameters.maxReturnSpeed;
    if (!valid) return fail("Invalid numeric option or option outside supported bounds.");

    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) return fail("Cannot read " + inputPath);
    const QByteArray inputBytes = input.readAll();
    const bool bom = inputBytes.startsWith("\xEF\xBB\xBF");
    const QString text = QString::fromUtf8(bom ? inputBytes.mid(3) : inputBytes);
    const SimaiDocument document = SimaiDocument::fromText(text);
    const auto* difficulty = document.difficulty(difficultyId);
    if (!difficulty || difficulty->chart.trimmed().isEmpty()) return fail("Requested difficulty has no chart.");
    const int scopeBegin = int(integer("begin", 0, difficulty->chart.size()));
    const int scopeEnd = cli.isSet("end")
        ? int(integer("end", scopeBegin, difficulty->chart.size())) : int(difficulty->chart.size());
    if (!valid) return fail("Invalid selection: require 0 <= begin <= end <= inote UTF-16 length.");
    if (!layoutPath.isEmpty() && (scopeBegin != 0 || scopeEnd != difficulty->chart.size()))
        return fail("Layout output requires a complete difficulty selection.");

    // Locate the original value so unrelated metadata, comments and line endings stay intact.
    // SimaiDocument trims only trailing whitespace; leading whitespace and CRLF remain in chart.
    // Replace exactly chart.size() units and preserve the field's original trailing whitespace.
    const QRegularExpression header(QStringLiteral(R"((?m)^[^\S\r\n]*&([^\r\n=]+)=)"));
    auto headers = header.globalMatch(text);
    int chartOffset = -1;
    int matchingHeaders = 0;
    while (headers.hasNext()) {
        const auto match = headers.next();
        if (match.captured(1) == QStringLiteral("inote_%1").arg(difficultyId)) {
            chartOffset = int(match.capturedEnd());
            ++matchingHeaders;
        }
    }
    if (matchingHeaders != 1 || text.mid(chartOffset, difficulty->chart.size()) != difficulty->chart)
        return fail("Expected exactly one matching inote field; source document is not rewritten automatically.");

    const auto metadata = miacode::simai::buildTimingMetadata(document);
    QElapsedTimer timer;
    timer.start();
    const auto parsed = SimaiParser::validateSyntax(difficulty->chart, metadata);
    const qint64 parseMs = timer.elapsed();
    QJsonObject report{{"schema_version", 1}, {"source_path", inputPath},
        {"source_sha256", sha256(inputBytes)}, {"title", document.title},
        {"difficulty", difficultyId}, {"level", difficulty->level},
        {"scope", QJsonObject{{"begin", scopeBegin}, {"end", scopeEnd},
            {"coordinate_unit", "UTF-16 code units relative to the selected inote value"},
            {"inote_document_offset", chartOffset}, {"inote_length", int(difficulty->chart.size())}}},
        {"note_marker_count", int(parsed.noteMarkers.size())},
        {"source_token_count", int(parsed.sourceTokens.size())},
        {"duration_seconds", parsed.durationSeconds}, {"parse_ok", parsed.ok},
        {"parse_elapsed_ms", parseMs}, {"parse_errors", messages(parsed.errors)},
        {"annotation_errors", messages(parsed.annotationErrors)}, {"parse_warnings", messages(parsed.warnings)},
        {"interpretation", "Replay verification checks the modeled judge, not player comfort or real-machine correctness. Coverage is by selected source row, not individual notes."},
        {"parameters", QJsonObject{{"max_work_units", parameters.maxWorkUnits},
            {"beam_width", parameters.beamWidth}, {"max_tasks", parameters.maxTasks},
            {"counterfactual_budget", parameters.counterfactualBudget},
            {"return_speed", parameters.returnSpeed}, {"sweep_threshold", parameters.sweepThreshold},
            {"input_preference", parameters.inputPreference},
            {"overwrite_existing", parameters.overwriteExisting},
            {"color_unresolved", parameters.colorUnresolved},
            {"player", QJsonObject{{"tap_rate", parameters.player.maxComfortableTapRate},
                {"move_speed", parameters.player.comfortableMoveSpeed}, {"stamina", parameters.player.staminaCapacity},
                {"recovery", parameters.player.recoveryHalfLife}, {"technique", parameters.player.techniquePreference},
                {"posture_weight", parameters.player.postureWeight}}}}}};
    if (!parsed.ok || !parsed.annotationErrors.isEmpty()) {
        report.insert("outcome", "parse_rejected");
        if (!writeFile(reportPath, QJsonDocument(report).toJson())) return fail("Cannot save parse report.");
        return 3;
    }

    if (!layoutPath.isEmpty()) {
        const QString source = parameters.overwriteExisting
            ? miacode::hand_order::stripAnnotations(difficulty->chart) : difficulty->chart;
        const auto layout = miacode::chart_transform::basicHandOrderLayoutInSelection(
            source, 0, 0, metadata, parameters.returnSpeed, parameters.sweepThreshold, parameters.colorUnresolved);
        const auto layoutParsed = SimaiParser::validateSyntax(layout.edit.replacement, metadata);
        if (!layoutParsed.ok || !layoutParsed.annotationErrors.isEmpty()
            || layoutParsed.noteMarkers.size() != parsed.noteMarkers.size())
            return fail("Basic layout failed native parsing or note count validation.");
        QString laidOut = text;
        laidOut.replace(chartOffset, difficulty->chart.size(), layout.edit.replacement);
        QByteArray layoutBytes = laidOut.toUtf8();
        if (bom) layoutBytes.prepend("\xEF\xBB\xBF");
        if (!writeFile(layoutPath, layoutBytes)) return fail("Cannot save basic-layout maidata.");
        report.insert("layout_path", layoutPath);
        report.insert("layout_sha256", sha256(layoutBytes));
    }

    timer.restart();
    const auto result = miacode::hand_order::inferBasicHandOrder(
        difficulty->chart, parsed, scopeBegin, scopeEnd, parameters, metadata);
    const qint64 elapsedMs = timer.elapsed();
    QJsonArray rows, windows, patches;
    QJsonObject statusCounts, pressureCounts, reasonCounts;
    int selected = 0, verified = 0, candidates = 0, preserved = 0;
    double maximumPeak = 0, maximumTapRate = 0, maximumLoad = 0;
    for (const auto& row : result.rows) {
        QJsonArray causes, hands;
        for (const auto hand : row.noteHands) hands.append(int(hand));
        for (const auto& cause : row.causes)
            causes.append(QJsonObject{{"kind", cause.kind}, {"pad", cause.pad},
                {"second", cause.second}, {"progress", cause.progress},
                {"token_start", cause.tokenStart}, {"branch", cause.branch}, {"hand_mask", cause.handMask}});
        rows.append(QJsonObject{{"start", row.start}, {"end", row.end},
            {"line", row.line}, {"column", row.col}, {"context", row.context},
            {"source", difficulty->chart.mid(row.start, row.end - row.start)},
            {"status", row.status}, {"reason", row.reason}, {"head_hand", int(row.head)},
            {"track_hand", int(row.track)}, {"note_hands", hands},
            {"cost", row.cost}, {"peak_speed", row.peakSpeed},
            {"player_cost", row.playerCost}, {"peak_tap_rate", row.peakTapRate}, {"peak_load", row.peakLoad},
            {"confidence", row.confidence}, {"confidence_level", row.confidenceLevel},
            {"pressure_level", row.pressureLevel}, {"input_channel", row.inputChannel},
            {"has_candidate", row.hasCandidate}, {"full_replay_verified", row.fullReplayVerified},
            {"dependency_unit", row.dependencyUnit}, {"judgement_kind", row.judgementKind},
            {"judgement_quality", row.judgementQuality}, {"conditions", row.conditions},
            {"judgement_second", row.judgementSecond}, {"judgement_margin", row.judgementMargin},
            {"handoff_count", row.handoffCount}, {"handoff_reason", row.handoffReason}, {"causes", causes}});
        if (row.context) continue;
        ++selected;
        verified += row.fullReplayVerified;
        candidates += row.hasCandidate;
        preserved += row.status == QStringLiteral("preserved");
        maximumPeak = std::max(maximumPeak, row.peakSpeed);
        maximumTapRate = std::max(maximumTapRate, row.peakTapRate);
        maximumLoad = std::max(maximumLoad, row.peakLoad);
        increment(statusCounts, row.status);
        increment(pressureCounts, row.pressureLevel);
        increment(reasonCounts, row.reason);
    }
    for (const auto& window : result.searchWindows)
        windows.append(QJsonObject{{"start", window.start}, {"end", window.end},
            {"tasks", window.tasks}, {"work_used", window.workUsed}, {"outcome", window.outcome}});
    QString annotated = difficulty->chart;
    auto orderedPatches = result.patches;
    std::sort(orderedPatches.begin(), orderedPatches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    int previousEnd = 0;
    for (const auto& patch : orderedPatches) {
        if (patch.start < previousEnd || patch.end < patch.start || patch.start < scopeBegin || patch.end > scopeEnd)
            return fail("Inference produced invalid, overlapping or out-of-selection patches.");
        previousEnd = patch.end;
        patches.append(QJsonObject{{"start", patch.start}, {"end", patch.end}, {"text", patch.text}});
    }
    for (auto it = orderedPatches.crbegin(); it != orderedPatches.crend(); ++it)
        annotated.replace(it->start, it->end - it->start, it->text);
    const auto outputParse = SimaiParser::validateSyntax(annotated, metadata);
    const bool outputValid = outputParse.ok && outputParse.annotationErrors.isEmpty()
        && outputParse.noteMarkers.size() == parsed.noteMarkers.size();
    report.insert("outcome", result.error.isEmpty() && outputValid ? "completed" : "inference_or_output_error");
    report.insert("error", result.error);
    report.insert("algorithm_version", result.algorithmVersion);
    report.insert("judge_profile_revision", result.judgeProfileRevision);
    report.insert("inference_elapsed_ms", elapsedMs);
    report.insert("work_used", result.workUsed);
    report.insert("truncated", result.truncated);
    report.insert("evidence_truncated", result.evidenceTruncated);
    report.insert("work_limited", result.workLimited);
    report.insert("task_limited", result.taskLimited);
    report.insert("included_objects", result.includedObjects);
    report.insert("audited_objects", result.auditedObjects);
    report.insert("window_count", result.windowCount);
    report.insert("max_window_tasks", result.maxWindowTasks);
    report.insert("selected_rows", selected);
    report.insert("candidate_rows", candidates);
    report.insert("preserved_rows", preserved);
    report.insert("replay_verified_rows", verified);
    report.insert("patch_count", int(result.patches.size()));
    report.insert("patch_row_coverage", selected ? double(result.patches.size()) / selected : 0);
    report.insert("replay_verified_row_ratio", selected ? double(verified) / selected : 0);
    report.insert("maximum_peak_speed", maximumPeak);
    report.insert("maximum_peak_tap_rate", maximumTapRate);
    report.insert("maximum_peak_load", maximumLoad);
    report.insert("status_counts", statusCounts);
    report.insert("pressure_counts", pressureCounts);
    report.insert("reason_counts", reasonCounts);
    report.insert("rows", rows);
    report.insert("windows", windows);
    report.insert("patches", patches);
    report.insert("output_parse_ok", outputValid);
    report.insert("output_parse_errors", messages(outputParse.errors));
    report.insert("output_annotation_errors", messages(outputParse.annotationErrors));
    if (result.error.isEmpty() && outputValid) {
        QString outputText = text;
        outputText.replace(chartOffset, difficulty->chart.size(), annotated);
        QByteArray outputBytes = outputText.toUtf8();
        if (bom) outputBytes.prepend("\xEF\xBB\xBF");
        if (!writeFile(outputPath, outputBytes)) return fail("Cannot save annotated maidata.");
        report.insert("output_path", outputPath);
        report.insert("output_sha256", sha256(outputBytes));
    }
    if (!writeFile(reportPath, QJsonDocument(report).toJson())) return fail("Cannot save audit report.");
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"report", reportPath},
        {"outcome", report.value("outcome")}, {"selected_rows", selected},
        {"patch_count", int(result.patches.size())}, {"inference_elapsed_ms", elapsedMs},
        {"work_limited", result.workLimited}}).toJson(QJsonDocument::Compact) << '\n';
    return result.error.isEmpty() && outputValid ? 0 : 4;
}
