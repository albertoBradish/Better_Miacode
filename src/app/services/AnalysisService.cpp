#include "app/services/AnalysisService.h"

#include "core/analysis/MuriConfig.h"
#include "common/TaskCancellation.h"
#include "core/chart/document/SimaiTimingMetadata.h"
#include "core/chart/model/TimelineMarkerOffset.h"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace miacode {
namespace {

constexpr int kDiagnosticsIdleDelayMs = 180;

ParsedChartSnapshot parseRequest(const ChartWorkspace& workspace)
{
    const ChartWorkspaceSnapshot identity = workspace.snapshot();
    ParsedChartSnapshot request;
    request.revision = identity.revision;
    request.documentOpenGeneration = identity.documentOpenGeneration;
    request.difficultyId = identity.activeDifficultyId;
    const SimaiDocument& document = workspace.document();
    const SimaiDifficultyData* difficulty = document.difficulty(request.difficultyId);
    if (identity.hasDocument && difficulty != nullptr) {
        request.chartText = difficulty->chart;
        request.firstSeconds = miacode::timeline::offset::parsedFirstSeconds(document.first);
        request.timingMetadata = miacode::simai::buildTimingMetadata(document);
    } else {
        request.difficultyId = 0;
    }
    return request;
}

bool sameParseSource(const ParsedChartSnapshot& a, const ParsedChartSnapshot& b)
{
    return a.available && a.documentOpenGeneration == b.documentOpenGeneration
        && a.difficultyId == b.difficultyId && a.chartText == b.chartText
        && a.firstSeconds == b.firstSeconds && a.timingMetadata == b.timingMetadata;
}

}  // namespace

AnalysisService::AnalysisService(
    ChartWorkspace& workspace,
    SimaiValidationLocale locale,
    const MuriRenderOptions& renderOptions,
    double staticTapOnSlideThresholdSeconds,
    QObject* parent)
    : QObject(parent)
    , workspace_(&workspace)
    , locale_(locale)
    , renderOptions_(renderOptions)
    , staticTapOnSlideThresholdSeconds_(staticTapOnSlideThresholdSeconds)
{
    diagnosticsTimer_.setSingleShot(true);
    diagnosticsTimer_.setInterval(kDiagnosticsIdleDelayMs);
    connect(&diagnosticsTimer_, &QTimer::timeout, this, &AnalysisService::dispatchDiagnostics);
    playTimer_.setSingleShot(true);
    playTimer_.setInterval(kDiagnosticsIdleDelayMs);
    connect(&playTimer_, &QTimer::timeout, this, &AnalysisService::dispatchPlayAnalysis);
    connect(workspace_, &ChartWorkspace::changed, this, [this](quint64) {
        requestAnalysis();
    });
    if (workspace_->snapshot().hasDocument) requestAnalysis();
}

AnalysisSnapshot AnalysisService::snapshot() const
{
    return snapshot_;
}

AnalysisService::~AnalysisService()
{
    shutdown();
}

void AnalysisService::shutdown()
{
    shuttingDown_ = true;
    cancelPendingAnalysis();
    playTimer_.stop();
    if (playCancellation_) playCancellation_->store(true, std::memory_order_relaxed);
}

void AnalysisService::cancelPendingAnalysis()
{
    if (parseCancellation_) parseCancellation_->store(true, std::memory_order_relaxed);
    if (diagnosticsCancellation_) diagnosticsCancellation_->store(true, std::memory_order_relaxed);
    ++diagnosticsGeneration_;
    diagnosticsTimer_.stop();
    diagnosticsRequested_ = false;
    pendingParse_.reset();
}

void AnalysisService::setLocale(SimaiValidationLocale locale)
{
    if (locale_ == locale) return;
    locale_ = locale;
    snapshot_.locale = locale;
    SimaiParser::localizeValidationReport(snapshot_.validation, locale);
    emit snapshotChanged(snapshot_.difficultyId, snapshot_.revision);
}

void AnalysisService::setDiagnosticsDeferred(bool deferred)
{
    if (diagnosticsDeferred_ == deferred) return;
    diagnosticsDeferred_ = deferred;
    if (deferred) {
        diagnosticsTimer_.stop();
    } else if (diagnosticsRequested_) {
        diagnosticsTimer_.start();
    }
}

void AnalysisService::setMuriParameters(
    const MuriRenderOptions& renderOptions, double staticTapOnSlideThresholdSeconds)
{
    const auto effectiveThreshold = [](double seconds) {
        return seconds >= 0.0
            ? seconds
            : static_cast<double>(miacode::muri::kStaticTapOnSlideThresholdDefaultMs) / 1000.0;
    };
    const bool analysisChanged = renderOptions.wifiNeedC != renderOptions_.wifiNeedC
        || renderOptions.excludeTouchFromMultiTouch != renderOptions_.excludeTouchFromMultiTouch
        || renderOptions.handRadiusPx != renderOptions_.handRadiusPx
        || !qFuzzyCompare(effectiveThreshold(staticTapOnSlideThresholdSeconds) + 1.0,
                          effectiveThreshold(staticTapOnSlideThresholdSeconds_) + 1.0);
    const bool playChanged = (renderOptions.renderMode == RenderMode::SimulatedPlay)
            != (renderOptions_.renderMode == RenderMode::SimulatedPlay)
        || renderOptions.playAnalysisEnabled != renderOptions_.playAnalysisEnabled
        || renderOptions.handRadiusPx != renderOptions_.handRadiusPx || renderOptions.playProfile != renderOptions_.playProfile
        || renderOptions.playTapFlowSpeed != renderOptions_.playTapFlowSpeed
        || renderOptions.playTouchFlowSpeed != renderOptions_.playTouchFlowSpeed || renderOptions.wifiNeedC != renderOptions_.wifiNeedC;
    renderOptions_ = renderOptions;
    staticTapOnSlideThresholdSeconds_ = staticTapOnSlideThresholdSeconds;
    if (analysisChanged && parsedSnapshot_.available) requestDiagnostics();
    if (playChanged) refreshPlayAnalysis(parseRequest(*workspace_));
}

void AnalysisService::requestAnalysis()
{
    if (shuttingDown_) return;
    ParsedChartSnapshot request = parseRequest(*workspace_);
    const bool reuseParse = sameParseSource(parsedSnapshot_, request);
    const bool reuseDiagnostics = reuseParse && snapshot_.available;
    cancelPendingAnalysis();
    if (reuseParse) parsedSnapshot_.revision = request.revision;
    if (reuseParse && !playSnapshot_.pending && playSnapshot_.enabled) {
        playSnapshot_.revision = request.revision;
        emit playSnapshotChanged();
    } else {
        refreshPlayAnalysis(request);
    }

    if (reuseDiagnostics) {
        parsedSnapshot_.revision = request.revision;
        snapshot_.revision = request.revision;
        emit snapshotChanged(snapshot_.difficultyId, snapshot_.revision);
        emit parseReady(parsedSnapshot_.difficultyId, parsedSnapshot_.revision);
        if (snapshot_.available && snapshot_.revision == request.revision)
            emit analysisReady(snapshot_.difficultyId, snapshot_.revision);
        return;
    }

    snapshot_ = AnalysisSnapshot();
    snapshot_.revision = request.revision;
    snapshot_.difficultyId = request.difficultyId;
    snapshot_.locale = locale_;
    snapshot_.pending = request.difficultyId > 0;
    if (!reuseParse) parsedSnapshot_ = ParsedChartSnapshot();
    emit snapshotChanged(snapshot_.difficultyId, snapshot_.revision);
    if (!snapshot_.pending) return;

    if (reuseParse) {
        parsedSnapshot_.revision = request.revision;
        emit parseReady(parsedSnapshot_.difficultyId, parsedSnapshot_.revision);
        requestDiagnostics();
    } else {
        pendingParse_ = std::move(request);
        dispatchPendingParse();
    }
}

ParsedChartSnapshot AnalysisService::parse(ParsedChartSnapshot request)
{
    request.parseResult = SimaiParser::parseForTimeline(request.chartText, request.timingMetadata);
    miacode::task::CancellationScope::check();
    request.previewState = buildTimelinePreviewRefreshState(request.parseResult, request.firstSeconds);
    request.available = true;
    return request;
}

AnalysisSnapshot AnalysisService::diagnose(
    const ParsedChartSnapshot& parsed,
    SimaiValidationLocale locale,
    const MuriRenderOptions& renderOptions,
    double staticTapOnSlideThresholdSeconds,
    const std::function<void(const SimaiValidationReport&)>& validationReady)
{
    TimelineAnalysisRefreshRequest request;
    request.revision = parsed.revision;
    request.difficultyId = parsed.difficultyId;
    request.chartText = parsed.chartText;
    request.validationLocale = locale;
    request.timingMetadata = parsed.timingMetadata;
    request.noteMarkerSignature = parsed.previewState.noteMarkerSignature;
    request.noteMarkers = parsed.previewState.shiftedNoteMarkers;
    request.renderOptions = renderOptions;
    request.staticTapOnSlideThresholdSeconds = staticTapOnSlideThresholdSeconds >= 0.0
        ? staticTapOnSlideThresholdSeconds
        : static_cast<double>(miacode::muri::kStaticTapOnSlideThresholdDefaultMs) / 1000.0;
    TimelineAnalysisRefreshResult result = buildTimelineAnalysisRefreshResult(request, validationReady);

    AnalysisSnapshot snapshot;
    snapshot.revision = parsed.revision;
    snapshot.difficultyId = parsed.difficultyId;
    snapshot.available = true;
    snapshot.validationReady = true;
    snapshot.locale = locale;
    snapshot.validation = std::move(result.validationReport);
    snapshot.noteMarkers = parsed.previewState.shiftedNoteMarkers;
    snapshot.noteMarkerSignature = parsed.previewState.noteMarkerSignature;
    snapshot.muri = std::move(result.analysisReport);
    snapshot.muriStaticReferences = std::move(result.staticReferences);
    snapshot.muriPanelEntries = miacode::muri::selectVisibleMuriPanelEntries(
        snapshot.muri, snapshot.muriStaticReferences);
    return snapshot;
}

AnalysisSnapshot AnalysisService::analyze(
    const ChartWorkspace& workspace,
    SimaiValidationLocale locale,
    const MuriRenderOptions& renderOptions,
    double staticTapOnSlideThresholdSeconds)
{
    ParsedChartSnapshot request = parseRequest(workspace);
    if (request.difficultyId <= 0) {
        AnalysisSnapshot snapshot;
        snapshot.revision = request.revision;
        snapshot.locale = locale;
        return snapshot;
    }
    return diagnose(parse(std::move(request)), locale, renderOptions, staticTapOnSlideThresholdSeconds);
}

void AnalysisService::dispatchPendingParse()
{
    if (shuttingDown_ || parseWorkerRunning_ || !pendingParse_) return;
    ParsedChartSnapshot request = std::move(*pendingParse_);
    pendingParse_.reset();
    parseWorkerRunning_ = true;
    parseCancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = parseCancellation_;
    QPointer<AnalysisService> guard(this);
    QThreadPool::globalInstance()->start([guard, cancellation, request = std::move(request)]() mutable {
        std::optional<ParsedChartSnapshot> result;
        miacode::task::CancellationScope scope(cancellation);
        try {
            miacode::task::CancellationScope::check();
            result = parse(std::move(request));
            miacode::task::CancellationScope::check();
        } catch (const miacode::task::Cancelled&) {
            result.reset();
        }
        request = ParsedChartSnapshot();
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cancellation, result = std::move(result)]() mutable {
            if (guard.isNull()) return;
            guard->parseWorkerRunning_ = false;
            if (result && !cancellation->load(std::memory_order_relaxed)
                && guard->identityIsCurrent(result->difficultyId, result->revision)) {
                guard->parsedSnapshot_ = std::move(*result);
                emit guard->parseReady(guard->parsedSnapshot_.difficultyId, guard->parsedSnapshot_.revision);
                guard->dispatchPlayAnalysis();
                guard->requestDiagnostics();
            }
            guard->dispatchPendingParse();
        }, Qt::QueuedConnection);
    });
}

void AnalysisService::requestDiagnostics()
{
    if (shuttingDown_ || !parsedSnapshot_.available
        || !identityIsCurrent(parsedSnapshot_.difficultyId, parsedSnapshot_.revision)) return;
    if (diagnosticsCancellation_) diagnosticsCancellation_->store(true, std::memory_order_relaxed);
    ++diagnosticsGeneration_;
    diagnosticsRequested_ = true;
    snapshot_ = AnalysisSnapshot();
    snapshot_.revision = parsedSnapshot_.revision;
    snapshot_.difficultyId = parsedSnapshot_.difficultyId;
    snapshot_.locale = locale_;
    snapshot_.pending = true;
    emit snapshotChanged(snapshot_.difficultyId, snapshot_.revision);
    if (!diagnosticsDeferred_) diagnosticsTimer_.start();
}

void AnalysisService::dispatchDiagnostics()
{
    if (shuttingDown_ || diagnosticsDeferred_ || diagnosticsWorkerRunning_ || !diagnosticsRequested_
        || diagnosticsTimer_.isActive() || !parsedSnapshot_.available) return;
    diagnosticsRequested_ = false;
    diagnosticsWorkerRunning_ = true;
    diagnosticsCancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = diagnosticsCancellation_;
    const quint64 generation = diagnosticsGeneration_;
    QPointer<AnalysisService> guard(this);
    QThreadPool::globalInstance()->start([
        guard, parsed = parsedSnapshot_, locale = locale_, renderOptions = renderOptions_,
        threshold = staticTapOnSlideThresholdSeconds_, generation, cancellation]() mutable {
        std::optional<AnalysisSnapshot> result;
        miacode::task::CancellationScope scope(cancellation);
        try {
            miacode::task::CancellationScope::check();
            const auto publishValidation = [guard, cancellation, generation,
                                            difficultyId = parsed.difficultyId, revision = parsed.revision](
                                               const SimaiValidationReport& validation) {
                if (guard.isNull()) return;
                QMetaObject::invokeMethod(guard.data(),
                    [guard, cancellation, generation, difficultyId, revision, report = validation]() mutable {
                        if (guard.isNull() || cancellation->load(std::memory_order_relaxed)
                            || generation != guard->diagnosticsGeneration_
                            || !guard->identityIsCurrent(difficultyId, revision)) return;
                        SimaiParser::localizeValidationReport(report, guard->locale_);
                        guard->snapshot_.validation = std::move(report);
                        guard->snapshot_.validationReady = true;
                        emit guard->snapshotChanged(difficultyId, revision);
                    }, Qt::QueuedConnection);
            };
            result = diagnose(parsed, locale, renderOptions, threshold, publishValidation);
            miacode::task::CancellationScope::check();
        } catch (const miacode::task::Cancelled&) {
            result.reset();
        }
        parsed = ParsedChartSnapshot();
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cancellation, result = std::move(result), generation]() mutable {
            if (guard.isNull()) return;
            guard->diagnosticsWorkerRunning_ = false;
            if (result && !cancellation->load(std::memory_order_relaxed) && generation == guard->diagnosticsGeneration_
                && guard->identityIsCurrent(result->difficultyId, result->revision)) {
                result->locale = guard->locale_;
                SimaiParser::localizeValidationReport(result->validation, result->locale);
                guard->snapshot_ = std::move(*result);
                emit guard->snapshotChanged(guard->snapshot_.difficultyId, guard->snapshot_.revision);
                emit guard->analysisReady(guard->snapshot_.difficultyId, guard->snapshot_.revision);
            }
            guard->dispatchDiagnostics();
        }, Qt::QueuedConnection);
    });
}

bool AnalysisService::identityIsCurrent(int difficultyId, quint64 revision) const
{
    const ChartWorkspaceSnapshot current = workspace_->snapshot();
    return current.hasDocument && current.activeDifficultyId == difficultyId
        && current.revision == revision;
}

void AnalysisService::refreshPlayAnalysis(const ParsedChartSnapshot& request)
{
    if (shuttingDown_) return;
    const bool enabled = renderOptions_.renderMode == RenderMode::SimulatedPlay && renderOptions_.playAnalysisEnabled;
    if (!enabled && !playSnapshot_.enabled) return;
    playTimer_.stop();
    if (playCancellation_) playCancellation_->store(true, std::memory_order_relaxed);
    playSnapshot_ = {request.revision, playSnapshot_.generation + 1, request.difficultyId,
                    enabled, enabled && request.difficultyId > 0, {}};
    emit playSnapshotChanged();
    if (playSnapshot_.pending) playTimer_.start();
}

void AnalysisService::dispatchPlayAnalysis()
{
    if (shuttingDown_ || playWorkerRunning_ || playTimer_.isActive() || !playSnapshot_.pending
        || !parsedSnapshot_.available || parsedSnapshot_.revision != playSnapshot_.revision
        || parsedSnapshot_.difficultyId != playSnapshot_.difficultyId) return;
    playWorkerRunning_ = true;
    playCancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = playCancellation_;
    const auto generation = playSnapshot_.generation;
    QPointer<AnalysisService> guard(this);
    QThreadPool::globalInstance()->start([guard, cancellation, generation, options = renderOptions_,
                                         marks = parsedSnapshot_.previewState.shiftedNoteMarkers]() {
        QVector<hand_order::PlayIssue> issues;
        try {
            task::CancellationScope scope(cancellation);
            task::CancellationScope::check();
            issues = hand_order::HandPlaySimulation(marks, options, true).detectIssues(marks);
        } catch (const task::Cancelled&) {
            // Superseded by a chart or settings change.
        }
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cancellation, generation, issues = std::move(issues)]() mutable {
            if (guard.isNull()) return;
            guard->playWorkerRunning_ = false;
            auto& snapshot = guard->playSnapshot_;
            if (!guard->shuttingDown_ && !cancellation->load(std::memory_order_relaxed)
                && generation == snapshot.generation && guard->identityIsCurrent(snapshot.difficultyId, snapshot.revision)) {
                snapshot.issues = std::move(issues);
                snapshot.pending = false;
                emit guard->playSnapshotChanged();
            }
            guard->dispatchPlayAnalysis();
        }, Qt::QueuedConnection);
    });
}

}  // namespace miacode
