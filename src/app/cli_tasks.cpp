#include "app/MainEntrypoints.h"
#include "app/CliRequest.h"
#include "AppVersion.h"
#include "app/services/CliIntegration.h"
#include "app/services/ChartBannerData.h"
#include "app/services/ExportEngine.h"
#include "app/services/UiRequestService.h"
#include "app/ui/export/CoverExportSession.h"
#include "app/runtime/ContentDurationConfig.h"
#include "app/runtime/settings/PreferenceDocumentProvider.h"
#include "audio/bass/BassPreviewAudioBackend.h"
#include "audio/bass/OfflineAudioDecoder.h"
#include "audio/bass/PreviewBassDefaultDevice.h"
#include "common/DebugOptions.h"
#include "common/RuntimePaths.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/ChartBpmExport.h"
#include "core/chart/ChartScoreLoss.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/video/AssetPaths.h"
#include "export/video_export/VideoExportSnapshot.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include "core/chart/SlideJudgementTiming.h"
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStringDecoder>
#include <QTextStream>
#include <csignal>
#include <memory>
#include <optional>

namespace {
using namespace miacode::app::entry;
volatile std::sig_atomic_t canceled = 0;
void cancelSignal(int) { canceled = 1; }

void jsonLine(const QJsonObject& object)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int result(const CliRequest& request, int code, QJsonObject data = {}, const QString& message = {}, const QString& errorCode = {})
{
    QJsonObject envelope{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("type"), QStringLiteral("result")},
        {QStringLiteral("ok"), code == 0}, {QStringLiteral("exitCode"), code}};
    if (!data.isEmpty()) envelope.insert(QStringLiteral("data"), data);
    if (code) envelope.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), errorCode}, {QStringLiteral("message"), message}});
    if (request.json || request.ndjson) jsonLine(envelope);
    else if (code) QTextStream(stderr) << message << Qt::endl;
    else if (data.contains(QStringLiteral("text"))) QTextStream(stdout) << data.value(QStringLiteral("text")).toString() << Qt::endl;
    else QTextStream(stdout) << QJsonDocument(data).toJson(QJsonDocument::Indented);
    return code;
}

QJsonObject capabilities()
{
    return {{QStringLiteral("name"), miacode::CliIntegration::commandName()}, {QStringLiteral("version"), QStringLiteral(MIACODE_DISPLAY_VERSION_STRING)},
        {QStringLiteral("schemaVersion"), 1}, {QStringLiteral("commands"), QJsonArray{QStringLiteral("help"), QStringLiteral("version"),
            QStringLiteral("capabilities"), QStringLiteral("stats"), QStringLiteral("presets"), QStringLiteral("export video"),
            QStringLiteral("export cover"), QStringLiteral("export bpm")}},
        {QStringLiteral("request"), QStringLiteral("--request <file|-> accepts one schemaVersion 1 JSON object")},
        {QStringLiteral("relativePaths"), QStringLiteral("JSON file paths resolve beside the request file; stdin JSON and command flags resolve from the current directory")},
        {QStringLiteral("progress"), QStringLiteral("Video/cover --json or --ndjson emit NDJSON progress followed by one result")},
        {QStringLiteral("tasksRequireEnabledIntegration"), true},
        {QStringLiteral("overwrite"), QStringLiteral("Explicit --overwrite; completed artifacts atomically replace the requested path")},
        {QStringLiteral("exampleRequest"), QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("command"), QStringLiteral("export")},
            {QStringLiteral("type"), QStringLiteral("video")}, {QStringLiteral("chart"), QStringLiteral("/charts/song/maidata.txt")},
            {QStringLiteral("difficulty"), 4}, {QStringLiteral("output"), QStringLiteral("/exports/song.mp4")}, {QStringLiteral("json"), true}}}};
}

int difficultyId(const QJsonValue& value)
{
    if (value.isUndefined()) return 4;
    if (value.isDouble()) return value.toDouble() == value.toInt() ? value.toInt() : 0;
    const QString token = value.toString().trimmed().toUpper();
    bool numeric = false;
    const int id = token.toInt(&numeric);
    if (numeric) return id;
    for (int id = 1; id <= 7; ++id)
        if (token == SimaiDocument::difficultyName(id).toUpper() || token == SimaiDocument::difficultyShortName(id).toUpper()) return id;
    if (token == QLatin1String("REMASTER")) return 6;
    return 0;
}

struct ChartInput {
    QString path, source;
    SimaiDocument document;
    SimaiParseResult parsed;
    int difficulty = 0;
    double first = 0;
};

bool readChart(const CliRequest& request, ChartInput* chart, QString* error)
{
    QString path = request.values.value(QStringLiteral("chart")).toString();
    if (QFileInfo(path).isDir()) path = QDir(path).filePath(QStringLiteral("maidata.txt"));
    if (!miacode::chart_assets::isChartDocumentPath(path)) {
        *error = QStringLiteral("Chart input must be an existing .txt/.simai file or a directory containing maidata.txt.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("Cannot read chart: %1").arg(file.errorString()); return false; }
    if (file.size() > 64 * 1024 * 1024) { *error = QStringLiteral("Chart exceeds 64 MiB."); return false; }
    const QByteArray bytes = file.readAll();
    QStringDecoder decoder(QStringDecoder::Utf8);
    chart->source = decoder.decode(bytes);
    if (decoder.hasError()) chart->source = QString::fromLocal8Bit(bytes);
    chart->path = QFileInfo(path).canonicalFilePath();
    chart->document = SimaiDocument::fromText(chart->source);
    chart->difficulty = difficultyId(request.values.value(QStringLiteral("difficulty")));
    if (!SimaiDocument::isDifficultyId(chart->difficulty)) { *error = QStringLiteral("Difficulty must be 1..7 or BAS/ADV/EXP/MAS/REMAS."); return false; }
    const auto* difficulty = chart->document.difficulty(chart->difficulty);
    if (!difficulty) { *error = QStringLiteral("Requested difficulty is absent from the chart."); return false; }
    bool firstOk = false;
    chart->first = miacode::timeline::offset::parsedFirstSeconds(chart->document.first, &firstOk);
    if (!firstOk) { *error = QStringLiteral("Chart has an invalid &first value."); return false; }
    chart->parsed = SimaiParser::validateSyntax(difficulty->chart, miacode::simai::buildTimingMetadata(chart->document));
    if (!chart->parsed.ok) {
        *error = chart->parsed.errors.isEmpty() ? QStringLiteral("Chart syntax is invalid.")
            : QStringLiteral("Chart syntax error at %1:%2: %3").arg(chart->parsed.errors.first().line)
                .arg(chart->parsed.errors.first().col).arg(chart->parsed.errors.first().message);
        return false;
    }
    return true;
}

QJsonObject chartStats(const ChartInput& chart, bool includeSlideTiming, bool fromLaunchBeat, bool earlyPerfectFromHead)
{
    const auto events = miacode::chart::buildScoringEvents(chart.parsed.noteMarkers);
    const auto score = miacode::chart::calculateScoreLoss(events);
    QJsonObject counts;
    const QStringList keys{QStringLiteral("tap"), QStringLiteral("hold"), QStringLiteral("star"), QStringLiteral("slide"),
        QStringLiteral("touch"), QStringLiteral("touchHold"), QStringLiteral("break"), QStringLiteral("ex"), QStringLiteral("firework")};
    for (int i = 0; i < keys.size(); ++i) counts.insert(keys[i], score.counts[i]);
    QJsonArray rows;
    for (const auto& row : score.rows) rows.append(QJsonObject{{QStringLiteral("object"), row.object}, {QStringLiteral("judgment"), row.judgment},
        {QStringLiteral("quantity"), row.quantity}, {QStringLiteral("baseLoss"), row.baseLoss}, {QStringLiteral("extraLoss"), row.extraLoss},
        {QStringLiteral("lossPercent"), row.lossPercent}, {QStringLiteral("maximumSss"), row.maximumSss}, {QStringLiteral("maximumSssPlus"), row.maximumSssPlus}});
    QJsonObject result{{QStringLiteral("chart"), chart.path}, {QStringLiteral("difficulty"), chart.difficulty}, {QStringLiteral("title"), chart.document.title},
        {QStringLiteral("counts"), counts}, {QStringLiteral("total"), score.total}, {QStringLiteral("baseTotal"), double(score.baseTotal)},
        {QStringLiteral("calculable"), score.calculable}, {QStringLiteral("hasUnofficialExtensions"), score.hasUnofficialExtensions},
        {QStringLiteral("scoreLoss"), rows}, {QStringLiteral("durationSeconds"), chart.parsed.durationSeconds},
        {QStringLiteral("firstSeconds"), chart.first}, {QStringLiteral("bpmSequence"), miacode::chart::exportChartBpm(chart.parsed, chart.first, miacode::chart::BpmExportFormat::Sequence)}};
    if (includeSlideTiming) {
        QJsonArray timings;
        std::optional<miacode::slide_judge::LaunchBeatIndex> launchBeats;
        if (fromLaunchBeat) launchBeats.emplace(chart.parsed.noteMarkers);
        for (const auto& note : chart.parsed.noteMarkers) {
            if (note.type != QLatin1String("slide") && note.type != QLatin1String("wifi")) continue;
            const auto timing = miacode::slide_judge::timingForMarker(note,launchBeats ? &*launchBeats : nullptr,earlyPerfectFromHead);
            QJsonObject item{{QStringLiteral("line"), note.sourceLine}, {QStringLiteral("column"), note.sourceCol},
                {QStringLiteral("shape"), note.slideDisplayKey}, {QStringLiteral("valid"), timing.valid}};
            if (timing.valid) {
                item.insert(QStringLiteral("referenceModel"), timing.wifiReference ? QStringLiteral("wifi-symmetric-reference") : QStringLiteral("dx-slide-reference"));
                item.insert(QStringLiteral("headSecond"), timing.headSecond + chart.first);
                item.insert(QStringLiteral("traceSecond"), timing.traceSecond + chart.first);
                item.insert(QStringLiteral("endSecond"), timing.endSecond + chart.first);
                item.insert(QStringLiteral("judgementSecond"), timing.judgementSecond + chart.first);
                item.insert(QStringLiteral("brightnessCompleteSecond"), timing.brightnessCompleteSecond + chart.first);
                item.insert(QStringLiteral("brightnessStartSecond"), timing.brightnessStartSecond + chart.first);
                item.insert(QStringLiteral("usesLaunchBeat"), timing.usesLaunchBeat);
                item.insert(QStringLiteral("fullBrightFromHead"), timing.fullBrightFromHead);
                item.insert(QStringLiteral("headToJudgementSeconds"), timing.judgementSecond - timing.headSecond);
                item.insert(QStringLiteral("lastAreaDurationSeconds"), timing.lastAreaDuration);
                item.insert(QStringLiteral("perfectStartSecond"), timing.perfectStartSecond + chart.first);
                item.insert(QStringLiteral("perfectEndSecond"), timing.perfectEndSecond + chart.first);
            }
            timings.append(item);
        }
        result.insert(QStringLiteral("slideJudgementTimings"), timings);
    }
    return result;
}

bool checkOutput(const CliRequest& request, const ChartInput& chart, QString* output, QString* error)
{
    *output = QFileInfo(request.values.value(QStringLiteral("output")).toString()).absoluteFilePath();
    const QFileInfo info(*output);
    if (info.isDir() || info.isSymLink() || (info.exists() && !request.values.value(QStringLiteral("overwrite")).toBool())) {
        *error = QStringLiteral("Output exists or is invalid; use --overwrite for an existing regular file."); return false;
    }
    if (QDir::cleanPath(*output).compare(QDir::cleanPath(chart.path),
#ifdef Q_OS_WIN
        Qt::CaseInsensitive
#else
        Qt::CaseSensitive
#endif
        ) == 0) { *error = QStringLiteral("Output cannot replace the source chart."); return false; }
    if (!QDir().mkpath(info.absolutePath())) { *error = QStringLiteral("Cannot create output directory."); return false; }
    return true;
}

bool commitOutput(const QString& stagingPath, const QString& output, bool overwrite, QString* error)
{
    if (canceled) { *error = QStringLiteral("Canceled."); return false; }
    if (QFileInfo::exists(output) && !overwrite) { *error = QStringLiteral("Output appeared while the task was running; --overwrite is required."); return false; }
    QFile source(stagingPath);
    QSaveFile target(output);
    target.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Cannot open completed artifact or output: %1").arg(target.errorString()); return false;
    }
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() && source.error() != QFileDevice::NoError) { *error = source.errorString(); return false; }
        if (canceled || target.write(bytes) != bytes.size()) { *error = canceled ? QStringLiteral("Canceled.") : target.errorString(); return false; }
    }
    if (!target.commit()) { *error = target.errorString(); return false; }
    return true;
}

class TaskEngine final : public miacode::ExportEngine {
public:
    VideoExportTask task;
    ChartInput chart;
    VideoExportTask buildSeedTask(int) override { return task; }
    void applySharedTaskSettings(const VideoExportTask&) override {}
    bool startAudition(int, const VideoExportTask&) override { return false; }
    void clearAudition() override {}
    void stopAudition() override {}
    bool launchVideoExport(const VideoExportTask&, int, QString*) override { return false; }
    bool launchBatchExport(const VideoExportTask&, const QStringList&, const QList<int>&, const QString&, BatchResult*, const BatchCallbacks&, QString*) override { return false; }
    void cancelVideoExport() override { canceled = 1; }
    QList<int> difficultyIds() const override { return {chart.difficulty}; }
    QString difficultyChartText(int id) const override { const auto* value = chart.document.difficulty(id); return value ? value->chart : QString(); }
    int lastOpenedDifficultyId() const override { return chart.difficulty; }
    MuriRenderOptions muriRenderOptions() const override { return task.muriRenderOptions; }
    double currentAudioClockSecond() const override { return 0; }
    void refreshIntroState() override {}
    void setIntroAuditionPlaying(bool) override {}
    bool introAuditionPlaying() const override { return false; }
};

bool prepareTask(const CliRequest& request, const ChartInput& chart, const QString& output, VideoExportTask* task, QString* error)
{
    VideoExportSnapshot snapshot;
    snapshot.chartTextUtf8 = chart.source;
    snapshot.difficultyId = chart.difficulty;
    snapshot.originalChartPath = chart.path;
    snapshot.projectDir = QFileInfo(chart.path).absolutePath();
    snapshot.trackPath = request.values.value(QStringLiteral("track")).toString(miacode::chart_assets::resolveTrackPath(chart.path));
    snapshot.backgroundMediaPath = miacode::chart_assets::resolveChartVideoPath(chart.path, chart.document.videoPath);
    const auto validSkin = [](const QString& directory) {
        const QDir skin(directory);
        return !directory.isEmpty() && QFileInfo::exists(skin.filePath(QStringLiteral("tap.png")))
            && QFileInfo::exists(skin.filePath(QStringLiteral("hold.png"))) && QFileInfo::exists(skin.filePath(QStringLiteral("star.png")));
    };
    snapshot.skinDirectory = request.values.value(QStringLiteral("skin")).toString();
    if (snapshot.skinDirectory.isEmpty()) {
        for (const QString& name : {QStringLiteral("skin/skinSD"), QStringLiteral("skin/skinDX")}) {
            const QString candidate = miacode::assets::assetPath(name);
            if (validSkin(candidate)) { snapshot.skinDirectory = candidate; break; }
        }
    }
    if (!validSkin(snapshot.skinDirectory)) { *error = QStringLiteral("Skin directory lacks tap.png/hold.png/star.png; pass --skin or install packaged assets."); return false; }
    snapshot.outputPath = output;
    snapshot.outputWidth = request.values.value(QStringLiteral("width")).toInt(1080);
    snapshot.outputHeight = request.values.value(QStringLiteral("height")).toInt(1080);
    snapshot.fps = request.values.value(QStringLiteral("fps")).toInt(60);
    snapshot.exportStartSeconds = request.values.value(QStringLiteral("start")).toDouble(0);
    const double musicDuration = request.values.value(QStringLiteral("type")).toString() == QLatin1String("video") && !snapshot.trackPath.isEmpty()
        ? miacode::audio_decode::probeFileDurationSeconds(snapshot.trackPath) : 0;
    const double contentEnd = miacode::content_duration::totalContentDurationSeconds(
        chart.parsed.durationSeconds + chart.first, musicDuration, !chart.parsed.noteMarkers.isEmpty());
    snapshot.contentDurationSeconds = request.values.value(QStringLiteral("duration")).toDouble(qMax(0.0, contentEnd - snapshot.exportStartSeconds));
    snapshot.fullRangeExport = snapshot.exportStartSeconds <= 1e-6;
    snapshot.tapFlowSpeed = snapshot.touchFlowSpeed = request.values.value(QStringLiteral("flow-speed")).toDouble(miacode::preview_gameplay::kPreviewTimingDefaultFlowSpeed);
    snapshot.showTimestamp = !request.values.value(QStringLiteral("hide-timestamp")).toBool();
    snapshot.showObjectStatsHud = request.values.value(QStringLiteral("show-object-stats")).toBool();
    snapshot.showChartInfoHud = request.values.value(QStringLiteral("show-chart-info")).toBool();
    snapshot.showBpmHud = request.values.value(QStringLiteral("show-bpm")).toBool();
    snapshot.showCpsHud = request.values.value(QStringLiteral("show-cps")).toBool();
    snapshot.showMeterHud = request.values.value(QStringLiteral("show-meter")).toBool();
    snapshot.showModsHud = request.values.value(QStringLiteral("show-mods")).toBool();
    snapshot.showSlideJudgementTiming = request.values.value(QStringLiteral("slide-judgement-timing")).toBool();
    snapshot.slideTimingFromLaunchBeat = request.values.value(QStringLiteral("slide-timing-from-launch-beat")).toBool();
    snapshot.slideTimingEarlyPerfectFromHead = request.values.value(QStringLiteral("slide-timing-early-perfect-from-head")).toBool();
    snapshot.intro = miacode::chart_banner::forDifficulty(chart.document, chart.difficulty);
    snapshot.intro.jacketPath = miacode::chart_assets::resolveDisplayBackgroundImagePath(chart.path);
    snapshot.intro.enabled = request.values.value(QStringLiteral("intro")).toBool() && snapshot.fullRangeExport;
    return buildVideoExportTaskFromSnapshot(snapshot, task, error);
}
}

namespace miacode::app::entry {
int runPublicCli(int argc, char** argv, const QStringList& arguments)
{
    const CliRequest request = parseCliRequest(arguments);
    if (!request.error.isEmpty()) return result(request, 2, {}, request.error, QStringLiteral("invalid_request"));
    try {
    if (!request.metadataOnly()) {
        QString error;
        if (!miacode::runtime_paths::initializePackagedRuntime(&error) || !miacode::runtime_paths::initialize(&error))
            return result(request, 78, {}, error, QStringLiteral("runtime_isolation"));
        if (!miacode::CliIntegration::tasksEnabled())
            return result(request, 77, {}, QStringLiteral("CLI tasks are disabled. Enable command-line integration in Better Miacode Settings → About."), QStringLiteral("cli_disabled"));
    }
    std::unique_ptr<QCoreApplication> application;
    if (request.needsGui()) {
#ifdef Q_OS_WIN
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
        application = std::make_unique<QGuiApplication>(argc, argv);
    } else application = std::make_unique<QCoreApplication>(argc, argv);
    application->setApplicationName(QStringLiteral("BetterMiacode"));
    application->setApplicationVersion(QStringLiteral(MIACODE_DISPLAY_VERSION_STRING));
    const QString command = request.values.value(QStringLiteral("command")).toString();
    if (command == QLatin1String("version")) return result(request, 0, {{QStringLiteral("version"), QStringLiteral(MIACODE_DISPLAY_VERSION_STRING)}, {QStringLiteral("text"), QStringLiteral("Better Miacode ") + QStringLiteral(MIACODE_DISPLAY_VERSION_STRING)}});
    if (command == QLatin1String("capabilities")) return result(request, 0, capabilities());
    if (command == QLatin1String("help")) return result(request, 0, {{QStringLiteral("text"), QStringLiteral(
        "Better Miacode CLI\n"
        "  better-miacode help | version | capabilities [--json]\n"
        "  better-miacode stats --chart PATH [--difficulty 4] [--slide-judgement-timing] [--slide-timing-from-launch-beat] [--slide-timing-early-perfect-from-head] [--json]\n"
        "  better-miacode presets [--json]\n"
        "  better-miacode export video --chart PATH --output FILE.mp4 [--width 1080 --height 1080 --fps 60 --start 0 --duration SECONDS] [--slide-judgement-timing] [--slide-timing-from-launch-beat] [--slide-timing-early-perfect-from-head]\n"
        "  better-miacode export cover --chart PATH --output FILE.png [--preset builtin:card --time SECONDS]\n"
        "  better-miacode export bpm --chart PATH --output FILE.txt [--format sequence|audio-time|maidata]\n"
        "  better-miacode --request FILE|- [--json]\n"
        "JSON file paths are relative to the request file; stdin/command flag paths are relative to the current directory.\n"
        "Tasks require command-line integration enabled in Settings > About and an isolated marked runtime.\n"
        "Existing outputs require --overwrite. Completed files replace the exact output atomically.\n"
        "--json/--ndjson: video/cover emit NDJSON progress and a final result; other commands emit one result.\n"
        "Exit codes: 0 success, 1 task failure, 2 invalid request, 3 output conflict, 77 disabled, 78 runtime, 130 canceled.")}});
    miacode::runtime::installPreferenceDocumentProvider();
    SimaiParser::setAllowNegativeHsEnabled(!miacode::debug_options::previewRejectNegativeHsEnabled());
    if (command == QLatin1String("presets")) {
        QJsonArray presets;
        for (const auto& row : miacode::ui::CoverExportSession::batchPresetOptions())
            if (!row.toMap().value(QStringLiteral("value")).toString().isEmpty()) presets.append(QJsonObject::fromVariantMap(row.toMap()));
        return result(request, 0, {{QStringLiteral("presets"), presets}});
    }
    ChartInput chart;
    QString error;
    if (!readChart(request, &chart, &error)) return result(request, 2, {}, error, QStringLiteral("invalid_chart"));
    if (command == QLatin1String("stats")) return result(request, 0,
        chartStats(chart, request.values.value(QStringLiteral("slide-judgement-timing")).toBool(),
            request.values.value(QStringLiteral("slide-timing-from-launch-beat")).toBool(),
            request.values.value(QStringLiteral("slide-timing-early-perfect-from-head")).toBool()));
    QString output;
    if (!checkOutput(request, chart, &output, &error)) return result(request, 3, {}, error, QStringLiteral("output_conflict"));
    const QString type = request.values.value(QStringLiteral("type")).toString();
    const QString suffix = QFileInfo(output).suffix().toLower();
    if ((type == QLatin1String("video") && suffix != QLatin1String("mp4"))
        || (type == QLatin1String("cover") && suffix != QLatin1String("png") && suffix != QLatin1String("jpg") && suffix != QLatin1String("jpeg")))
        return result(request, 2, {}, QStringLiteral("Video output requires .mp4; cover output requires .png, .jpg or .jpeg."), QStringLiteral("output_format"));
    QTemporaryDir staging(QDir(QFileInfo(output).absolutePath()).filePath(QStringLiteral(".better-cli-XXXXXX")));
    if (!staging.isValid()) return result(request, 1, {}, QStringLiteral("Cannot create task staging directory."), QStringLiteral("output_io"));
    const QString staged = QDir(staging.path()).filePath(QFileInfo(output).fileName());
    canceled = 0;
    const auto previousInt = std::signal(SIGINT, cancelSignal);
    const auto previousTerm = std::signal(SIGTERM, cancelSignal);
    const auto restoreSignals = qScopeGuard([&] { std::signal(SIGINT, previousInt); std::signal(SIGTERM, previousTerm); });
    if (type == QLatin1String("bpm")) {
        const QString format = request.values.value(QStringLiteral("format")).toString(QStringLiteral("sequence"));
        if (format != QLatin1String("sequence") && format != QLatin1String("audio-time") && format != QLatin1String("maidata"))
            return result(request, 2, {}, QStringLiteral("BPM format must be sequence, audio-time or maidata."), QStringLiteral("invalid_format"));
        const auto mode = format == QLatin1String("sequence") ? miacode::chart::BpmExportFormat::Sequence
            : format == QLatin1String("audio-time") ? miacode::chart::BpmExportFormat::AudioTime : miacode::chart::BpmExportFormat::EmptyMaidata;
        QFile file(staged);
        const auto bytes = (miacode::chart::exportChartBpm(chart.parsed, chart.first, mode) + QLatin1Char('\n')).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return result(request, 1, {}, file.errorString(), QStringLiteral("output_io"));
        file.close();
    } else {
#ifdef Q_OS_WIN
        miacode::preview_audio::disableBassDefaultDeviceEntry();
#endif
        miacode::preview_audio::installPreviewAudioBackendProvider(miacode::preview_audio::bassPreviewAudioBackendProvider());
        VideoExportTask task;
        if (!prepareTask(request, chart, staged, &task, &error)) return result(request, 1, {}, error, QStringLiteral("task_preparation"));
        const auto progress = [&](int percent, const QString& text) {
            if (percent >= 0 && (request.json || request.ndjson)) jsonLine({{QStringLiteral("schemaVersion"), 1},
                {QStringLiteral("type"), QStringLiteral("progress")}, {QStringLiteral("percent"), percent}, {QStringLiteral("message"), text}});
            else if (percent >= 0 && !text.isEmpty()) QTextStream(stderr) << percent << "% " << text << Qt::endl;
            return bool(canceled);
        };
        if (type == QLatin1String("video")) {
            const auto exported = VideoExportController::exportPreparedTask(task, progress);
            if (!exported.success) return result(request, canceled ? 130 : 1, {}, exported.message + QLatin1Char('\n') + exported.details,
                canceled ? QStringLiteral("canceled") : QStringLiteral("export_failed"));
        } else {
            progress(0, QStringLiteral("Rendering cover..."));
            TaskEngine engine;
            engine.task = task; engine.chart = chart;
            miacode::UiRequestService ui;
            const QString preset = request.values.value(QStringLiteral("preset")).toString(QStringLiteral("builtin:card"));
            const double time = request.values.value(QStringLiteral("time")).toDouble(0);
            const QImage image = miacode::ui::CoverExportSession::renderPresetCover(engine, ui, task, preset, time, &error);
            const QByteArray format = suffix == QLatin1String("png") ? QByteArrayLiteral("PNG") : QByteArrayLiteral("JPEG");
            if (image.isNull() || !image.save(staged, format.constData()))
                return result(request, 1, {}, error.isEmpty() ? QStringLiteral("Cannot render/write cover.") : error, QStringLiteral("export_failed"));
            progress(100, QStringLiteral("Cover rendered."));
        }
    }
    if (!commitOutput(staged, output, request.values.value(QStringLiteral("overwrite")).toBool(), &error))
        return result(request, canceled ? 130 : 1, {}, error, canceled ? QStringLiteral("canceled") : QStringLiteral("output_io"));
    return result(request, 0, {{QStringLiteral("output"), output}, {QStringLiteral("bytes"), double(QFileInfo(output).size())},
        {QStringLiteral("chart"), chart.path}, {QStringLiteral("difficulty"), chart.difficulty}});
    } catch (...) {
        return result(request, 1, {}, currentExceptionDetail(), QStringLiteral("internal_error"));
    }
}
}
