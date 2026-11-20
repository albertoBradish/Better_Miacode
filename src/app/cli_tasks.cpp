#include "app/services/VideoEncodingRequest.h"
#include "app/services/HandOrderFeaturePreferences.h"
#include "app/services/VideoWatermarkPreferences.h"
#include "app/MainEntrypoints.h"
#include "app/services/AutomaticChartPreparation.h"
#include "app/services/ChartFileSafety.h"
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
#include "core/scene/PreviewPreparedSceneCache.h"
#include "core/scene/PreviewOpacityCurves.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/video/AssetPaths.h"
#include "export/video_export/VideoExportSnapshot.h"
#include "core/chart/ChartVersion.h"
#include "export/video_export/VideoExportSettings.h"

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
        {QStringLiteral("ok"), code == 0}, {QStringLiteral("exitCode"), code},
        {QStringLiteral("handOrderExportMode"), miacode::hand_order_features::enabled() ? request.handOrderMode() : QStringLiteral("original")}};
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
        {QStringLiteral("videoOptions"), QJsonArray{QStringLiteral("rate-mode"), QStringLiteral("quality"), QStringLiteral("size-limit-mb"), QStringLiteral("video-bitrate"),
            QStringLiteral("no-pv"), QStringLiteral("debug-render"), QStringLiteral("intro-style"),
            QStringLiteral("intro-window"), QStringLiteral("intro-pv-start"), QStringLiteral("hand-order")}},
        {QStringLiteral("tasksRequireEnabledIntegration"), true},
        {QStringLiteral("handOrderExportModes"), QJsonArray{QStringLiteral("annotated"), QStringLiteral("original")}},
        {QStringLiteral("defaultHandOrderExportMode"), QStringLiteral("annotated")},
        {QStringLiteral("overwrite"), QStringLiteral("Explicit --overwrite; completed artifacts atomically replace the requested path")},
        {QStringLiteral("exampleRequest"), QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("command"), QStringLiteral("export")},
            {QStringLiteral("type"), QStringLiteral("video")}, {QStringLiteral("chart"), QStringLiteral("/charts/song/maidata.txt")},
            {QStringLiteral("difficulty"), 4}, {QStringLiteral("output"), QStringLiteral("/exports/song.mp4")},
            {QStringLiteral("handOrderExportMode"), QStringLiteral("original")}, {QStringLiteral("json"), true}}}};
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
    if (file.error() != QFileDevice::NoError) {
        *error = QStringLiteral("Cannot read chart: %1").arg(file.errorString());
        return false;
    }
    const auto decoded = miacode::ChartWorkspaceFileService::decodeDocumentText(bytes);
    if (!decoded) {
        *error = QStringLiteral("Chart text could not be decoded.");
        return false;
    }
    chart->source = *decoded;
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
    if (miacode::hand_order_features::enabled() && request.handOrderMode() == QLatin1String("annotated") && !chart->parsed.annotationErrors.isEmpty()) {
        const auto& issue = chart->parsed.annotationErrors.first();
        *error = QStringLiteral("Hand-order annotation error at %1:%2: %3").arg(issue.line).arg(issue.col).arg(issue.message);
        return false;
    }
    return true;
}

// Estimate frame density from the renderer's own per-object visibility windows.
// Merge a marker's overlapping layers so its head and track do not count twice.
QJsonObject densestCoverFrame(const ChartInput& chart)
{
    using namespace miacode::preview::scene;
    PreviewFrameState state;
    state.sceneContentRevision = 1;
    state.noteMarkers = chart.parsed.noteMarkers;
    PreviewPreparedSceneCache cache;
    cache.sync(state);
    QVector<QVector<QPair<double, double>>> windows(state.noteMarkers.size());
    const auto collect = [&](const auto& layer, int kind) {
        for (const auto& entry : layer.entries) {
            if (entry.markerIndex < 0 || entry.markerIndex >= windows.size()) continue;
            const auto& marker = state.noteMarkers[entry.markerIndex];
            double lead = 0.0;
            if (kind == 0) {
                lead = 0.7 * previewTapTimingForEffectiveFlowSpeed(
                    state.render.tapFlowSpeed * marker.hsMultiplier).spawnDurationSeconds;
            } else if (kind == 1) {
                const auto timing = previewSlideTrackTimingForMarker(marker, state.render.tapFlowSpeed, state.render.starFadeTiming);
                lead = 0.5 * qMax<qreal>(0, timing.appearLeadInSeconds - timing.fullBrightLeadInSeconds);
            } else {
                lead = 0.5 * previewTouchTimingForEffectiveFlowSpeed(
                    state.render.touchFlowSpeed * qAbs(marker.hsMultiplier)).showDurationSeconds;
            }
            windows[entry.markerIndex].append({entry.activeStart + lead + chart.first, entry.activeEnd + chart.first});
        }
    };
    collect(cache.headLayer(), 0);
    collect(cache.slideLikeLayer(), 1);
    collect(cache.touchLayer(), 2);
    collect(cache.touchHoldLayer(), 2);
    QVector<QPair<double, int>> events;
    for (auto& ranges : windows) {
        std::sort(ranges.begin(), ranges.end());
        double start = 0, end = 0;
        bool active = false;
        const auto flushRange = [&] {
            if (active && end > start && end > 0) {
                events.append({qMax(0.0, start), 1});
                events.append({end, -1});
            }
        };
        for (const auto& range : ranges) {
            if (!active) { start = range.first; end = range.second; active = true; }
            else if (range.first <= end) end = qMax(end, range.second);
            else { flushRange(); start = range.first; end = range.second; }
        }
        flushRange();
    }
    std::sort(events.begin(), events.end());
    int count = 0, best = 0;
    double chosen = 0;
    for (qsizetype i = 0; i < events.size();) {
        const double time = events[i].first;
        while (i < events.size() && events[i].first == time) count += events[i++].second;
        if (i < events.size() && events[i].first > time && count > best) {
            best = count;
            chosen = (time + events[i].first) / 2.0;
        }
    }
    return {{QStringLiteral("seconds"), chosen}, {QStringLiteral("visibleObjects"), best},
        {QStringLiteral("method"), QStringLiteral("renderer-visibility-windows")}};
}

QJsonObject chartStats(const ChartInput& chart, bool includeSlideTiming, bool fromLaunchBeat)
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
    result.insert(QStringLiteral("coverFrame"), densestCoverFrame(chart));
    if (includeSlideTiming) {
        QJsonArray timings;
        const miacode::slide_judge::LaunchBeatIndex launchBeats(chart.parsed.noteMarkers);
        for (const auto& note : chart.parsed.noteMarkers) {
            if (note.type != QLatin1String("slide") && note.type != QLatin1String("wifi")) continue;
            const auto timing = miacode::slide_judge::timingForMarker(note,&launchBeats,fromLaunchBeat);
            QJsonObject item{{QStringLiteral("line"), note.sourceLine}, {QStringLiteral("column"), note.sourceCol},
                {QStringLiteral("shape"), note.slideDisplayKey}, {QStringLiteral("valid"), timing.valid}};
            if (timing.valid) {
                item.insert(QStringLiteral("referenceModel"), timing.wifiReference ? QStringLiteral("wifi-symmetric-reference") : QStringLiteral("dx-slide-reference"));
                item.insert(QStringLiteral("headSecond"), timing.headSecond + chart.first);
                item.insert(QStringLiteral("traceSecond"), timing.traceSecond + chart.first);
                item.insert(QStringLiteral("endSecond"), timing.endSecond + chart.first);
                item.insert(QStringLiteral("judgementSecond"), timing.judgementSecond + chart.first);
                const auto nativeFade = miacode::preview::scene::previewSlideTrackTimingForMarker(
                    note, miacode::preview_gameplay::kPreviewTimingDefaultFlowSpeed, 0.0);
                const double full = timing.fullBrightFromHead ? note.second - nativeFade.fullBrightLeadInSeconds : timing.brightnessCompleteSecond;
                const double start = timing.fullBrightFromHead ? note.second - nativeFade.appearLeadInSeconds : timing.brightnessStartSecond;
                item.insert(QStringLiteral("brightnessCompleteSecond"), full + chart.first);
                item.insert(QStringLiteral("brightnessStartSecond"), start + chart.first);
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
    const auto version = miacode::chart_version::detect(chart.document, chart.parsed.noteMarkers);
    snapshot.completionDisplay = resolvedCompletionDisplay(CompletionDisplay::Automatic, version.deluxe());
    snapshot.outlineImagePath = miacode::assets::customOutlinePathForFileName(version.outlineFileName());
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
    if (!miacode::video_export::readVideoEncodingRequest(request.values, &snapshot.encoding, error)) return false;
    snapshot.includePv = !request.values.value(QStringLiteral("no-pv")).toBool();
    if (request.values.value(QStringLiteral("debug-render")).toBool())
        snapshot.muriRenderOptions.renderMode = RenderMode::Debug;
    snapshot.exportStartSeconds = request.values.value(QStringLiteral("start")).toDouble(0);
    const double musicDuration = request.values.value(QStringLiteral("type")).toString() == QLatin1String("video") && !snapshot.trackPath.isEmpty()
        ? miacode::audio_decode::probeFileDurationSeconds(snapshot.trackPath) : 0;
    const double contentEnd = miacode::content_duration::totalContentDurationSeconds(
        chart.parsed.durationSeconds + chart.first, musicDuration, !chart.parsed.noteMarkers.isEmpty());
    snapshot.contentDurationSeconds = request.values.value(QStringLiteral("duration")).toDouble(qMax(0.0, contentEnd - snapshot.exportStartSeconds));
    snapshot.fullRangeExport = snapshot.exportStartSeconds <= 1e-6;
    snapshot.tapFlowSpeed = snapshot.touchFlowSpeed = request.values.value(QStringLiteral("flow-speed")).toDouble(miacode::preview_gameplay::kPreviewTimingDefaultFlowSpeed);
    snapshot.showTimestamp = !request.values.value(QStringLiteral("hide-timestamp")).toBool();
    snapshot.showHandOrderAnnotations = miacode::hand_order_features::enabled() && request.handOrderMode() == QLatin1String("annotated");
    snapshot.showObjectStatsHud = request.values.value(QStringLiteral("show-object-stats")).toBool();
    snapshot.showChartInfoHud = request.values.value(QStringLiteral("show-chart-info")).toBool();
    snapshot.showBpmHud = request.values.value(QStringLiteral("show-bpm")).toBool();
    snapshot.showCpsHud = request.values.value(QStringLiteral("show-cps")).toBool();
    snapshot.showMeterHud = request.values.value(QStringLiteral("show-meter")).toBool();
    snapshot.showModsHud = request.values.value(QStringLiteral("show-mods")).toBool();
    snapshot.showMeasureLines = request.values.value(QStringLiteral("show-measure-lines")).toBool(snapshot.showHandOrderAnnotations);
    snapshot.fireworkBrightness = miacode::preview_video::normalizedFireworkBrightness(
        request.values.value(QStringLiteral("firework-brightness")).toDouble(snapshot.showHandOrderAnnotations ? 50.0 : 100.0) / 100.0);
    snapshot.showSlideJudgementTiming = request.values.value(QStringLiteral("slide-judgement-timing")).toBool();
    snapshot.slideTimingFromLaunchBeat = request.values.value(QStringLiteral("slide-timing-from-launch-beat")).toBool();
    snapshot.slideTimingEarlyPerfectFromHead = request.values.value(QStringLiteral("slide-timing-early-perfect-from-head")).toBool();
    snapshot.intro = miacode::chart_banner::forDifficulty(chart.document, chart.difficulty);
    snapshot.intro.jacketPath = miacode::chart_assets::resolveDisplayBackgroundImagePath(chart.path);
    const QJsonValue styleOption = request.values.value(QStringLiteral("intro-style"));
    const QString style = styleOption.toString(QStringLiteral("classic"));
    const bool explicitSegment = request.values.contains(QStringLiteral("intro-pv-start"));
    snapshot.intro.chartPreview = style == QLatin1String("chart")
        || request.values.value(QStringLiteral("intro-chart-preview")).toBool();
    snapshot.intro.pvPreview = snapshot.intro.chartPreview || style == QLatin1String("pv")
        || (styleOption.isUndefined() && explicitSegment);
    snapshot.intro.enabled = request.values.value(QStringLiteral("intro"))
        .toBool(snapshot.intro.pvPreview || !styleOption.isUndefined()) && snapshot.fullRangeExport;
    if (snapshot.intro.enabled && snapshot.intro.pvPreview) {
        using namespace miacode::chart_preview;
        const QString window = request.values.value(QStringLiteral("intro-window")).toString();
        if (window == QLatin1String("drop")) snapshot.intro.previewWindowMode = IntroWindowMode::FollowDrop;
        else if (window == QLatin1String("default")) snapshot.intro.previewWindowMode = IntroWindowMode::Default;
        std::optional<MediaRange> requestedRange;
        if (explicitSegment) {
            const double start = request.values.value(QStringLiteral("intro-pv-start")).toDouble();
            requestedRange = MediaRange{start, start + miacode::intro::kPvPreviewHoldSeconds};
        }
        QString fingerprint;
        const auto savedDrop = storedDropSelection(chart.document, chart.difficulty);
        const QFileInfo track(snapshot.trackPath);
        if (!requestedRange && savedDrop && savedDrop->source == DropSource::Auto
            && track.size() > 0 && track.size() <= 128LL * 1024 * 1024) {
            const QByteArray hash = miacode::chart_file_safety::fileDigest(snapshot.trackPath);
            if (!hash.isEmpty())
                fingerprint = dropInputFingerprint(chart.document, chart.difficulty, QString::fromLatin1(hash.toHex()));
        }
        freezeIntroPreviewWindow(snapshot.intro, resolveIntroWindow(chart.document, chart.difficulty,
            requestedRange, snapshot.intro.previewWindowMode, musicDuration > 0.0 ? musicDuration : contentEnd, fingerprint));
    }
    miacode::hand_order_features::applyExportPolicy(snapshot);
    snapshot.videoWatermark = miacode::video_watermark_preferences::load();
    if (!buildVideoExportTaskFromSnapshot(snapshot, task, error)) return false;
    task->showHandOrderAnnotations = snapshot.showHandOrderAnnotations;
    task->handOrderExportMode = snapshot.showHandOrderAnnotations ? HandOrderExportMode::Annotated : HandOrderExportMode::Original;
    return true;
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
        if (!miacode::runtime_paths::initializePackagedRuntime(&error) || !miacode::runtime_paths::initializeFromEnvironment(&error)
            || !miacode::runtime_paths::configureProcessEnvironment(&error))
            return result(request, 78, {}, error, QStringLiteral("runtime_isolation"));
        if (!miacode::CliIntegration::tasksEnabled())
            return result(request, 77, {}, QStringLiteral("CLI tasks are disabled. Enable command-line integration in Better Miacode Settings → About."), QStringLiteral("cli_disabled"));
    }
    std::unique_ptr<QCoreApplication> application;
    if (request.needsGui()) {
#ifdef Q_OS_WIN
        const auto backend = miacode::debug_options::exportRenderBackendRequest();
        QQuickWindow::setGraphicsApi(backend == miacode::debug_options::ExportRenderBackendRequest::OpenGl
            ? QSGRendererInterface::OpenGL : QSGRendererInterface::Direct3D11);
#else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
        application = std::make_unique<QGuiApplication>(argc, argv);
    } else application = std::make_unique<QCoreApplication>(argc, argv);
    application->setOrganizationName(QStringLiteral("BetterMiacode"));
    application->setOrganizationDomain(QStringLiteral("better-miacode.local"));
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
        "Render modes: --hand-order annotated|original (default annotated); JSON key handOrderExportMode.\n"
        "Video and cover/preset rendering use the same explicit mode; disk tasks never inherit preview state.\n"
        "Video: --rate-mode quality|file_size|bitrate, --quality high|standard|small, --size-limit-mb 5..1000, --video-bitrate 0.1..100, --no-pv, --debug-render.\n"
        "Intro: --intro-style classic|pv|chart, --intro-window automatic|drop|default, --intro-pv-start seconds.\n"
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
        return result(request, 0, {{QStringLiteral("presets"), presets}, {QStringLiteral("handOrderExportMode"), miacode::hand_order_features::enabled() ? request.handOrderMode() : QStringLiteral("original")}});
    }
    ChartInput chart;
    QString error;
    if (!readChart(request, &chart, &error)) return result(request, 2, {}, error, QStringLiteral("invalid_chart"));
    if (command == QLatin1String("stats")) return result(request, 0,
        chartStats(chart, request.values.value(QStringLiteral("slide-judgement-timing")).toBool(),
            request.values.value(QStringLiteral("slide-timing-from-launch-beat")).toBool()));
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
            ChartWorkspace workspace;
            ChartWorkspaceFileService files(workspace);
            const auto opened = files.open(chart.path);
            if (!opened.accepted) return result(request, 1, {}, opened.error, QStringLiteral("normalization_failed"));
            if (workspace.document().toText() != chart.document.toText()) {
                return result(request, 1, {}, QStringLiteral("Chart changed while preparing the export."),
                    QStringLiteral("normalization_failed"));
            }
            using miacode::automatic_chart::Trigger;
            const auto normalized = miacode::automatic_chart::prepare(workspace, files,
                task.showHandOrderAnnotations ? Trigger::Render : Trigger::RenderOriginal);
            if (!normalized.accepted)
                qWarning().noquote() << normalized.error;
            if (normalized.changed) {
                if (!readChart(request, &chart, &error))
                    return result(request, 2, {}, error, QStringLiteral("invalid_chart"));
                if (workspace.document().toText() != chart.document.toText()) {
                    return result(request, 1, {}, QStringLiteral("Chart changed after automatic preparation."),
                        QStringLiteral("normalization_failed"));
                }
                if (!prepareTask(request, chart, staged, &task, &error))
                    return result(request, 1, {}, error, QStringLiteral("task_preparation"));
            }
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
        {QStringLiteral("chart"), chart.path}, {QStringLiteral("difficulty"), chart.difficulty},
        {QStringLiteral("handOrderExportMode"), miacode::hand_order_features::enabled() ? request.handOrderMode() : QStringLiteral("original")}});
    } catch (...) {
        return result(request, 1, {}, currentExceptionDetail(), QStringLiteral("internal_error"));
    }
}
}
