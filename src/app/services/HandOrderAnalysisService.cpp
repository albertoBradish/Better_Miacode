#include "app/services/HandOrderExperimentService.h"
#include "app/services/HandOrderAnalysisService.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/analysis/hand_order/CombinedHandOrder.h"
#include "core/analysis/hand_order/HandPlayReview.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "app/services/HandOrderProgressEstimate.h"
#include <cmath>
#include <QPointer>
#include <QThreadPool>
#include <QThread>
#include <QElapsedTimer>
#include <algorithm>

namespace miacode {
namespace {
QString patternKey(const hand_order::PatternSuggestion& suggestion)
{
    if (!suggestion.instanceKey.isEmpty()) return suggestion.instanceKey;
    int first = std::numeric_limits<int>::max(), last = -1;
    for (const auto& marker : suggestion.variants.first().markers) {
        first = qMin(first, marker.parseOrder); last = qMax(last, marker.parseOrder);
    }
    return suggestion.registryId + ':' + QString::number(first) + ':' + QString::number(last);
}
using hand_order::handOrderPatternFingerprint;
}
HandOrderAnalysisService::HandOrderAnalysisService(ChartWorkspace& workspace, AnalysisService* analysis, QObject* parent)
    : QObject(parent), workspace_(&workspace), analysis_(analysis)
{
    experiments_ = new HandOrderExperimentService(workspace, this);
    parameters_.overwriteExisting = true;
    worker_.setMaxThreadCount(1);
    worker_.setThreadPriority(QThread::LowestPriority);
    idleTimer_.setSingleShot(true); idleTimer_.setInterval(2000);
    connect(&idleTimer_, &QTimer::timeout, this, &HandOrderAnalysisService::startBackground);
    connect(&workspace, &ChartWorkspace::changed, this, [this] {
        refreshPatternSuggestions();
        emit patternSuggestionsChanged();
        invalidate(localizedText("hand_order.auto.stale"));
        scheduleBackground();
    });
}
QObject* HandOrderAnalysisService::experiments() const { return experiments_; }
void HandOrderAnalysisService::retranslate()
{
    if (experiments_) experiments_->retranslate();
    // Presentation refresh never invalidates the current solution or request.
    emit changed();
    emit patternSuggestionsChanged();
}
void HandOrderAnalysisService::setJobProgress(JobProgressService* progress)
{
    if (experiments_) experiments_->setJobProgress(progress);
    if (progress_) disconnect(progress_, nullptr, this, nullptr);
    endProgress(); progress_ = progress;
    if (progress_) connect(progress_, &JobProgressService::cancellationRequested, this, [this](quint64 token) {
        if (token == progressToken_) cancel();
    });
    if (progress_) connect(progress_, &JobProgressService::pauseRequested, this, [this](quint64 token, bool paused) {
        if (token == progressToken_) { if (paused) pause(); else resume(); }
    });
    if (progress_) connect(progress_, &JobProgressService::changed, this, [this] {
        if (progress_ && progress_->active() && progress_->token() != progressToken_) {
            idleTimer_.stop(); stopBackground();
        } else scheduleBackground();
    });
}
void HandOrderAnalysisService::endProgress()
{
    if (progress_ && progressToken_ != 0 && progress_->token() == progressToken_) progress_->end();
    progressToken_ = 0;
}
HandOrderAnalysisService::~HandOrderAnalysisService()
{
    endProgress();
    idleTimer_.stop();
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    if (execution_) execution_->wake();
    worker_.waitForDone();
}
void HandOrderAnalysisService::invalidate(const LocalizedText& message, bool clearCache)
{
    ++sequence_;
    idleTimer_.stop();
    backgroundOnly_ = false; pauseRequested_ = false; cacheHit_ = false;
    endProgress();
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    if (execution_) execution_->wake();
    pending_ = false; queued_.reset(); completed_.reset(); result_ = {};
    if (clearCache) { userStopped_ = false; cache_.clear(); backgroundStatus_.clear(); backgroundDetail_.clear(); backgroundPercent_ = -1; }
    taken_ = false; message_ = message;
    emit changed();
}
void HandOrderAnalysisService::cancel()
{
    userStopped_ = true;
    invalidate(localizedText("hand_order.auto.cancelled"), false);
    backgroundStatus_ = localizedText("hand_order.background.stopped");
    backgroundDetail_ = backgroundStatus_; backgroundPercent_ = -1; emit changed();
}
bool HandOrderAnalysisService::requestCurrentPatterns()
{
    if (!featuresEnabled_) return false;
    const auto* difficulty = workspace_->document().difficulty(workspace_->snapshot().activeDifficultyId);
    return difficulty && request(difficulty->chart, 0, 0, true);
}
void HandOrderAnalysisService::pause()
{
    if (!pending_) return;
    pauseRequested_ = true;
    if (execution_) execution_->setPaused(true);
    if (backgroundOnly_) backgroundStatus_ = localizedText("hand_order.background.paused");
    if (progress_ && progress_->token() == progressToken_) progress_->setPaused(true);
    emit changed();
}
void HandOrderAnalysisService::resume()
{
    pauseRequested_ = false;
    if (execution_) execution_->setPaused(false);
    if (backgroundOnly_) backgroundStatus_ = backgroundPercent_ < 0
        ? localizedText("hand_order.background.running") : localizedText("hand_order.background.progress").arg(backgroundPercent_);
    if (pending_) {
        const auto estimating = localizedText("hand_order.progress.estimating");
        if (backgroundOnly_) backgroundDetail_ = estimating;
        else {
            message_ = estimating;
            if (progress_ && progress_->token() == progressToken_) progress_->reportIndeterminate(estimating);
        }
    }
    if (progress_ && progress_->token() == progressToken_) progress_->setPaused(false);
    emit changed();
}
void HandOrderAnalysisService::setBeamWidth(int width)
{
    width = qBound(8, width, 128);
    if (parameters_.beamWidth == width) return;
    parameters_.beamWidth = width; ++parameterRevision_;
    invalidate(localizedText("hand_order.auto.stale"));
    scheduleBackground();
}
void HandOrderAnalysisService::setParameters(const hand_order::BasicParameters& parameters)
{
    parameters_ = parameters; ++parameterRevision_;
    if (experiments_) experiments_->setPlayerProfile(parameters.player);
    invalidate(localizedText("hand_order.auto.stale"));
    scheduleBackground();
}
void HandOrderAnalysisService::setOverwriteExisting(bool value)
{
    if (parameters_.overwriteExisting == value) return;
    auto parameters = parameters_; parameters.overwriteExisting = value; setParameters(parameters);
}
void HandOrderAnalysisService::setHighConfidenceOnly(bool value)
{
    if (parameters_.highConfidenceOnly == value) return;
    auto parameters = parameters_; parameters.highConfidenceOnly = value; setParameters(parameters);
}
void HandOrderAnalysisService::setInputPreference(int value)
{
    value = qBound(0, value, 2);
    if (parameters_.inputPreference == value) return;
    auto parameters = parameters_; parameters.inputPreference = value; setParameters(parameters);
}
void HandOrderAnalysisService::setReturnSpeed(double speed)
{
    if (!std::isfinite(speed)) return;
    speed = qBound(hand_order::BasicParameters::minReturnSpeed, speed,
        hand_order::BasicParameters::maxReturnSpeed);
    if (qFuzzyCompare(parameters_.returnSpeed, speed)) return;
    auto parameters = parameters_; parameters.returnSpeed = speed; setParameters(parameters);
}

void HandOrderAnalysisService::setSweepThreshold(int threshold)
{
    threshold = qBound(chart::handOrderMinSweepThreshold, threshold,
        chart::handOrderMaxSweepThreshold);
    if (parameters_.sweepThreshold == threshold) return;
    auto parameters = parameters_; parameters.sweepThreshold = threshold; setParameters(parameters);
}

void HandOrderAnalysisService::setColorUnresolved(bool enabled)
{
    if (parameters_.colorUnresolved == enabled) return;
    auto parameters = parameters_; parameters.colorUnresolved = enabled; setParameters(parameters);
}
void HandOrderAnalysisService::setPlayerProfile(const hand_order::PlayerProfile& profile)
{
    if (!hand_order::validPlayerProfile(profile)) return;
    const auto& old = parameters_.player;
    if (old.maxComfortableTapRate == profile.maxComfortableTapRate
        && old.comfortableMoveSpeed == profile.comfortableMoveSpeed
        && old.staminaCapacity == profile.staminaCapacity
        && old.recoveryHalfLife == profile.recoveryHalfLife
        && old.techniquePreference == profile.techniquePreference
        && old.postureWeight == profile.postureWeight) return;
    auto parameters = parameters_; parameters.player = profile; setParameters(parameters);
}
void HandOrderAnalysisService::setMaxWorkUnits(qint64 units)
{
    units = qBound(qint64(1000), units, hand_order::BasicParameters::maxAllowedWorkUnits);
    if (parameters_.maxWorkUnits == units) return;
    auto parameters = parameters_; parameters.maxWorkUnits = units; setParameters(parameters);
}
void HandOrderAnalysisService::setPreservePatternAnnotations(bool preserve)
{
    if (preservePatternAnnotations_ == preserve) return;
    preservePatternAnnotations_ = preserve; ++parameterRevision_;
    invalidate(localizedText("hand_order.auto.stale"));
    scheduleBackground();
}
bool HandOrderAnalysisService::identityCurrent(const Request& request) const
{
    const auto current = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(current.activeDifficultyId);
    const auto currentPlay = analysis_ ? analysis_->playOptions() : MuriRenderOptions{};
    const auto& requestedPlay = request.playOptions;
    const bool playCurrent = request.optimizePatterns || (currentPlay.playProfile == requestedPlay.playProfile
        && currentPlay.wifiNeedC == requestedPlay.wifiNeedC
        && (!request.reviewPlay || (currentPlay.handRadiusPx == requestedPlay.handRadiusPx
            && currentPlay.playTapFlowSpeed == requestedPlay.playTapFlowSpeed
            && currentPlay.playTouchFlowSpeed == requestedPlay.playTouchFlowSpeed)));
    return playCurrent && current.hasDocument && difficulty && request.parameterRevision == parameterRevision_
        && request.parameters.version == parameters_.version
        && request.parameters.judgeProfileRevision == parameters_.judgeProfileRevision
        && current.revision == request.identity.revision
        && current.documentOpenGeneration == request.identity.documentOpenGeneration
        && current.activeDifficultyId == request.identity.activeDifficultyId
        && current.filePath == request.identity.filePath && current.sourceText == request.identity.sourceText
        && difficulty->chart == request.snapshot.chartText
        && request.allowNegativeHs == SimaiParser::allowNegativeHsEnabled()
        && simai::buildTimingMetadata(workspace_->document()) == request.snapshot.timingMetadata;
}
bool HandOrderAnalysisService::isCurrent(const Request& request) const
{
    return request.sequence == sequence_ && identityCurrent(request);
}
bool HandOrderAnalysisService::sameCalculation(const Request& a, const Request& b) const
{
    // parameterRevision covers every solver, budget and protection option.
    return identityCurrent(a) && identityCurrent(b) && a.parameterRevision == b.parameterRevision
        && a.begin == b.begin && a.end == b.end && a.optimizePatterns == b.optimizePatterns
        && a.preservePatternAnnotations == b.preservePatternAnnotations && a.reviewPlay == b.reviewPlay
        && a.ignoredPatterns == b.ignoredPatterns;
}
std::optional<HandOrderAnalysisService::Request> HandOrderAnalysisService::makeRequest(
    const QString& text, int begin, int end, bool optimizePatterns, bool reviewPlay) const
{
    if (!featuresEnabled_) return std::nullopt;
    Request request;
    request.identity = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(request.identity.activeDifficultyId);
    if (!request.identity.hasDocument || !difficulty || difficulty->chart != text) return {};
    request.sequence = sequence_; request.parameterRevision = parameterRevision_;
    request.parameters = parameters_; request.optimizePatterns = optimizePatterns;
    request.reviewPlay = reviewPlay;
    if (!optimizePatterns && analysis_) request.playOptions = analysis_->playOptions();
    request.ignoredPatterns = ignoredPatternKeys_;
    request.allowNegativeHs = SimaiParser::allowNegativeHsEnabled();
    request.preservePatternAnnotations = preservePatternAnnotations_;
    request.snapshot.chartText = text;
    request.snapshot.revision = request.identity.revision;
    request.snapshot.documentOpenGeneration = request.identity.documentOpenGeneration;
    request.snapshot.difficultyId = request.identity.activeDifficultyId;
    request.snapshot.timingMetadata = simai::buildTimingMetadata(workspace_->document());
    request.snapshot.firstSeconds = timeline::offset::parsedFirstSeconds(workspace_->document().first);
    if (analysis_) {
        const auto& shared = analysis_->parsedSnapshot();
        if (shared.available && shared.revision == request.identity.revision
            && shared.documentOpenGeneration == request.identity.documentOpenGeneration
            && shared.difficultyId == request.identity.activeDifficultyId && shared.chartText == text
            && shared.timingMetadata == request.snapshot.timingMetadata) request.snapshot = shared;
    }
    request.begin = qBound(0, qMin(begin, end), int(text.size()));
    request.end = qBound(request.begin, qMax(begin, end), int(text.size()));
    if (request.begin == request.end) { request.begin = 0; request.end = text.size(); }
    return request;
}
bool HandOrderAnalysisService::prepare(const QString& text, int begin, int end, bool optimizePatterns, bool reviewPlay)
{
    if (!featuresEnabled_) return false;
    if (pending()) cancel();
    optimizingPatterns_ = optimizePatterns;
    reviewingPlay_ = reviewPlay;
    cacheHit_ = false; completed_.reset(); result_ = {}; taken_ = false;
    const auto request = makeRequest(text, begin, end, optimizePatterns, reviewPlay);
    if (!request) return false;
    message_ = localizedText("hand_order.workflow.settings");
    for (const auto& cached : cache_) if (sameCalculation(cached.request, *request)) {
        publishResult(*request, cached.result, true); return true;
    }
    emit changed(); return true;
}
void HandOrderAnalysisService::beginForegroundProgress()
{
    if (progress_ && !progress_->active()) {
        progressToken_ = progress_->begin(LocalizedText::translated(reviewingPlay_ ? "hand_order.play_review"
                : optimizingPatterns_ ? "hand_order.optimize" : "hand_order.analyze"),
            LocalizedText::translated("hand_order.auto.running"), true);
        progress_->setPausable(true);
        if (!optimizingPatterns_ && !reviewingPlay_)
            progress_->reportStages(0, QVector<int>(HandOrderWorkflowProgress::stageCount, 0), -1, {});
        else progress_->reportIndeterminate(LocalizedText::translated("hand_order.auto.running"));
    }
}
bool HandOrderAnalysisService::request(const QString& text, int begin, int end, bool optimizePatterns, bool reviewPlay)
{
    if (!featuresEnabled_) return false;
    userStopped_ = false;
    optimizingPatterns_ = optimizePatterns;
    reviewingPlay_ = reviewPlay;
    auto request = makeRequest(text, begin, end, optimizePatterns, reviewPlay);
    if (!request) { message_ = localizedText("hand_order.auto.stale"); emit changed(); return false; }
    if (parameters_.version != hand_order::basicAlgorithmVersion
        || parameters_.judgeProfileRevision != hand_order::basicJudgeProfileRevision) {
        invalidate(localizedText("hand_order.auto.invalid_parameters")); return false;
    }
    if (backgroundOnly_ && pending_ && ((active_ && isCurrent(*active_) && sameCalculation(*active_, *request))
        || (queued_ && sameCalculation(*queued_, *request)))) {
        // Promote the exact in-flight stack, including its current beam/state.
        backgroundOnly_ = false; cacheHit_ = false; pauseRequested_ = false;
        if (queued_) queued_->background = false;
        if (execution_) { execution_->setBackground(false); execution_->setPaused(false); }
        backgroundStatus_ = localizedText("hand_order.background.promoted");
        message_ = localizedText("hand_order.auto.running");
        beginForegroundProgress(); emit changed(); return true;
    }
    if (optimizePatterns || reviewPlay) patternManualReview_ = true;
    if (optimizePatterns && patternText_.isEmpty()) {
        patternText_ = text; patternIdentity_ = request->identity; patternTiming_ = request->snapshot.timingMetadata;
    }
    std::optional<Cached> reusable;
    for (const auto& cached : cache_) if (sameCalculation(cached.request, *request)) { reusable = cached; break; }
    if (!reusable && optimizePatterns && !preservePatternAnnotations_ && patternFullScan_
        && patternText_ == text && request->begin == 0 && request->end == text.size()) {
        hand_order::BasicResult cached;
        cached.patternOptimization = true; cached.patternSuggestions = patternSuggestions_;
        cached.patternSegments = patternSuggestions_.size();
        cached.patternOutcome = patternSuggestions_.isEmpty() ? "no_patterns" : "recognized";
        for (const auto& s : patternSuggestions_) {
            cached.patternCandidates += s.variants.size();
            if (s.preferred < 0) continue;
            bool changed = false;
            for (const auto& p : s.variants.at(s.preferred).patches)
                if (text.mid(p.start, p.end - p.start) != p.text) { cached.patches.append(p); changed = true; }
            cached.patternAccepted += changed;
        }
        std::sort(cached.patches.begin(), cached.patches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        reusable = Cached{*request, std::move(cached)};
    }
    invalidate({}, false);
    request->sequence = sequence_;
    if (reusable) { publishResult(*request, reusable->result, true); return true; }
    pending_ = true;
    beginForegroundProgress();
    message_ = localizedText("hand_order.auto.running");
    queued_ = std::move(*request);
    emit changed(); dispatch(); return true;
}
void HandOrderAnalysisService::setFeaturesEnabled(bool enabled)
{
    if (featuresEnabled_ == enabled) return;
    featuresEnabled_ = enabled;
    experiments_->setFeaturesEnabled(enabled);
    if (!enabled) {
        cancel(); resetPatternState(); patternStates_.clear(); cache_.clear();
        backgroundStatus_.clear(); backgroundDetail_.clear(); backgroundPercent_ = -1; message_ = {};
        backgroundEnabled_ = false;
        emit patternSuggestionsChanged();
    } else userStopped_ = false;
    emit changed();
}

void HandOrderAnalysisService::setBackgroundEnabled(bool enabled)
{
    backgroundEnabled_ = enabled;
    if (!enabled) { idleTimer_.stop(); stopBackground(); }
    else scheduleBackground();
}
void HandOrderAnalysisService::refreshParserPolicy()
{
    if (experiments_) experiments_->refreshParserPolicy();
    const bool policy = SimaiParser::allowNegativeHsEnabled();
    if (parserPolicy_ == policy) return;
    parserPolicy_ = policy; invalidate(localizedText("hand_order.auto.stale")); scheduleBackground();
}
void HandOrderAnalysisService::setPlaybackActive(bool active)
{
    playbackActive_ = active;
    if (active) { idleTimer_.stop(); stopBackground(); }
    else scheduleBackground();
}
void HandOrderAnalysisService::scheduleBackground()
{
    if (patternManualReview_) return;
    if (!featuresEnabled_ || !backgroundEnabled_ || userStopped_ || playbackActive_ || pending_ || running_ || completed_
        || (progress_ && progress_->active()) || idleTimer_.isActive()) return;
    if (workspace_->snapshot().hasDocument) idleTimer_.start();
}
void HandOrderAnalysisService::stopBackground()
{
    if (!backgroundOnly_) return;
    invalidate(message_, false);
    backgroundStatus_ = localizedText("hand_order.background.yielded");
    backgroundDetail_ = backgroundStatus_; backgroundPercent_ = -1;
    emit changed();
}
void HandOrderAnalysisService::startBackground()
{
    if (patternManualReview_) return;
    if (!featuresEnabled_ || !backgroundEnabled_ || userStopped_ || playbackActive_ || pending_ || running_ || completed_
        || (progress_ && progress_->active())) return;
    const auto identity = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(identity.activeDifficultyId);
    if (!identity.hasDocument || !difficulty || difficulty->chart.trimmed().isEmpty()) return;
    auto request = makeRequest(difficulty->chart, 0, 0, false);
    if (!request) return;
    for (const auto& cached : cache_) if (sameCalculation(cached.request, *request)) {
        backgroundStatus_ = localizedText("hand_order.background.cached");
        backgroundDetail_ = backgroundStatus_; emit changed(); return;
    }
    ++sequence_; request->sequence = sequence_; request->background = true;
    backgroundOnly_ = true; pending_ = true;
    backgroundStatus_ = localizedText("hand_order.background.running");
    backgroundDetail_ = backgroundStatus_; backgroundPercent_ = -1;
    queued_ = std::move(*request); emit changed(); dispatch();
}
void HandOrderAnalysisService::remember(const Request& request, const hand_order::BasicResult& result)
{
    if (!identityCurrent(request) || !result.error.isEmpty()) return;
    retainPatternSuggestions(request, result);
    cache_.removeIf([&](const Cached& cached) { return cached.request.optimizePatterns == request.optimizePatterns
        && cached.request.reviewPlay == request.reviewPlay; });
    cache_.append({request, result});
    while (cache_.size() > 3) cache_.removeFirst();
    if (!request.optimizePatterns && !request.reviewPlay) {
        backgroundStatus_ = localizedText("hand_order.background.cached");
        backgroundDetail_ = backgroundStatus_; backgroundPercent_ = -1;
    }
}
void HandOrderAnalysisService::publishResult(Request request, hand_order::BasicResult result, bool fromCache)
{
    pruneIgnoredPatterns(result, request.snapshot.chartText);
    if (fromCache) retainPatternSuggestions(request, result);
    completed_ = std::move(request); result_ = std::move(result); taken_ = false; cacheHit_ = fromCache;
    int preserved = 0, conflicts = 0, undecided = 0, unsupported = 0;
    for (const auto& row : result_.rows) {
        preserved += row.status == "preserved";
        conflicts += row.status == "conflict";
        undecided += row.status == "undecided" || row.status == "not_found"
            || row.status == "heuristic";
        unsupported += row.status == "unsupported" || row.status == "invalid" || row.status == "skipped";
    }
    message_ = localizedText("hand_order.auto.summary").arg(result_.patches.size())
        .arg(preserved).arg(conflicts).arg(undecided).arg(unsupported).arg(result_.contextObjects)
        + (result_.truncated ? localizedText("hand_order.auto.truncated") : LocalizedText{})
        + (result_.evidenceTruncated ? localizedText("hand_order.auto.evidence_truncated") : LocalizedText{});
    message_ = message_ + localizedText("hand_order.auto.search_scope")
        .arg(result_.includedObjects).arg(result_.windowCount).arg(result_.maxWindowTasks)
        .arg(result_.workUsed).arg(parameters_.maxWorkUnits).arg(result_.auditedObjects);
    if (parameters_.highConfidenceOnly) message_ = message_ + localizedText("hand_order.auto.high_confidence_closed");
    if (!result_.error.isEmpty()) message_ = localizedText(completed_->reviewPlay
        ? "hand_order.play_review.invalid" : "hand_order.auto.invalid_parameters");
    if (result_.patternOptimization && result_.error.isEmpty()) {
        const auto& optimized = result_;
        message_ = localizedText(("hand_order.optimize." + optimized.patternOutcome).toUtf8().constData());
        if (optimized.patternOutcome == "recognized")
            message_ = message_ + localizedText("hand_order.pattern.summary").arg(optimized.patternSegments)
                .arg(optimized.patternCandidates).arg(optimized.patternAccepted);
        if (optimized.truncated) message_ = message_ + localizedText("hand_order.optimize.limited");
    }
    if (completed_->reviewPlay && result_.error.isEmpty()) {
        message_ = localizedText(result_.workLimited ? "hand_order.play_review.limited" : "hand_order.play_review.summary")
            .arg(result_.windowCount).arg(result_.patternAccepted).arg(result_.patches.size());
        if (result_.truncated) message_ = message_ + localizedText("hand_order.play_review.truncated");
    }
    if (result_.combinedOptimization && result_.error.isEmpty()) {
        message_ = message_ + QStringLiteral("\n") + localizedText("hand_order.workflow.combined_summary")
            .arg(result_.patternSegments).arg(result_.conflictWindows)
            .arg(result_.confirmedConflictNotes).arg(result_.conflictPatchesWithheld);
        if (!result_.combinedNormalizationUsed)
            message_ = message_ + QStringLiteral("\n") + localizedText("hand_order.workflow.original_grid");
        if (result_.conflictReviewLimited)
            message_ = message_ + QStringLiteral("\n") + localizedText("hand_order.workflow.conflict_limited");
    }
    if (result_.contextGuardOutcome.startsWith(QStringLiteral("retained_")))
        message_ = localizedText("hand_order.context_guard.retained") + QStringLiteral("\n") + message_;
    else if (result_.contextGuardOutcome == QStringLiteral("baseline_unverified"))
        message_ = localizedText("hand_order.context_guard.unverified") + QStringLiteral("\n") + message_;
    if (fromCache) message_ = localizedText("hand_order.background.reused") + QStringLiteral("\n") + message_;
    emit changed();
}
void HandOrderAnalysisService::dispatch()
{
    if (running_ || !queued_) return;
    Request request = std::move(*queued_); queued_.reset(); running_ = true; active_ = request;
    ++computationCount_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    execution_ = std::make_shared<task::ExecutionControl>();
    execution_->setBackground(request.background); execution_->setPaused(pauseRequested_);
    const auto cancellation = cancellation_;
    const auto execution = execution_;
    QPointer<HandOrderAnalysisService> guard(this);
    worker_.start([guard, request = std::move(request), cancellation, execution]() mutable {
        QThread::currentThread()->setPriority(execution->background() ? QThread::LowestPriority : QThread::NormalPriority);
        std::optional<hand_order::BasicResult> result;
        task::CancellationScope scope(cancellation, execution);
        QElapsedTimer elapsed; elapsed.start();
        qint64 lastReportMs = -100;
        HandOrderProgressEstimate estimate;
        HandOrderWorkflowProgress workflow;
        const bool combined = !request.optimizePatterns && !request.reviewPlay;
        auto lastPhase = hand_order::HandOrderProgress::Preparing;
        auto estimateEpoch = execution->estimateEpoch();
        const auto onProgress = [&](const hand_order::HandOrderProgress& update) {
            if (cancellation->load(std::memory_order_relaxed) || !guard) return;
            QThread::currentThread()->setPriority(execution->background() ? QThread::LowestPriority : QThread::NormalPriority);
            const qint64 now = elapsed.elapsed();
            if (estimateEpoch != execution->estimateEpoch()) {
                estimate = {}; estimateEpoch = execution->estimateEpoch();
                workflow.resetClock(now);
            }
            const auto reading = estimate.update(update, now);
            const auto overall = combined ? workflow.update(update, now) : HandOrderWorkflowProgress::Reading{};
            const bool milestone = update.phase != lastPhase && (update.phase >= hand_order::HandOrderProgress::Normalizing
                || update.phase == hand_order::HandOrderProgress::Finished);
            const bool finished = update.phase == hand_order::HandOrderProgress::Finished;
            if (now - lastReportMs < 160 && !milestone && !finished) return;
            lastPhase = update.phase;
            lastReportMs = now;
            QMetaObject::invokeMethod(guard.data(), [guard, cancellation, execution, reportEpoch = estimateEpoch,
                    identity = request, update, reading, overall] {
                if (!guard || cancellation->load(std::memory_order_relaxed)
                    || guard->cancellation_ != cancellation || execution->estimateEpoch() != reportEpoch) return;
                guard->reportProgress(identity, update, overall.stages.isEmpty() ? reading.percent : overall.percent,
                    overall.stages.isEmpty() ? reading.remainingSeconds : overall.remainingSeconds, overall.stages);
            }, Qt::QueuedConnection);
        };
        try {
            task::CancellationScope::check();
            const auto strict = SimaiParser::validateSyntax(request.snapshot.chartText, request.snapshot.timingMetadata);
            if (request.reviewPlay) {
                result = hand_order::reviewHandPlay(request.snapshot.chartText, strict,
                    request.begin, request.end, request.playOptions, onProgress, request.snapshot.firstSeconds);
            } else if (request.optimizePatterns) {
                hand_order::PatternOptimizationParameters optimization;
                optimization.preserveExisting = request.preservePatternAnnotations;
                optimization.recognition = hand_order::PatternRecognitionMode::Prioritized;
                result = hand_order::optimizeHandOrderPatterns(request.snapshot.chartText, strict,
                    request.begin, request.end, request.parameters, optimization, request.snapshot.timingMetadata, onProgress);
            } else {
                result = hand_order::optimizeHandOrder(request.snapshot.chartText, strict,
                    request.begin, request.end, request.parameters, request.snapshot.timingMetadata,
                    request.playOptions, request.ignoredPatterns, onProgress);
            }
            task::CancellationScope::check();
        } catch (const task::Cancelled&) { result.reset(); }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cancellation, request = std::move(request), result = std::move(result)]() mutable {
            if (!guard) return;
            guard->running_ = false; guard->active_.reset();
            if (result && result->algorithmVersion == request.parameters.version
                && result->judgeProfileRevision == request.parameters.judgeProfileRevision
                && !cancellation->load(std::memory_order_relaxed) && guard->isCurrent(request)) {
                guard->remember(request, *result);
                guard->pending_ = false; guard->pauseRequested_ = false;
                if (guard->backgroundOnly_) {
                    guard->backgroundOnly_ = false;
                    guard->backgroundStatus_ = localizedText(result->error.isEmpty()
                        ? "hand_order.background.cached" : "hand_order.background.failed");
                    guard->backgroundDetail_ = guard->backgroundStatus_; guard->backgroundPercent_ = -1;
                    emit guard->changed();
                } else {
                    guard->endProgress();
                    guard->publishResult(std::move(request), std::move(*result));
                }
            } else if (result && !cancellation->load(std::memory_order_relaxed) && guard->isCurrent(request)) {
                guard->invalidate(localizedText("hand_order.auto.invalid_output"));
            } else if (!cancellation->load(std::memory_order_relaxed) && request.sequence == guard->sequence_) {
                guard->invalidate(localizedText("hand_order.auto.stale"));
            }
            guard->dispatch(); guard->scheduleBackground();
        }, Qt::QueuedConnection);
    });
}
void HandOrderAnalysisService::reportProgress(const Request& request, const hand_order::HandOrderProgress& update,
    int percent, qint64 remainingSeconds, const QVector<int>& stages)
{
    if (!pending_ || !isCurrent(request)) return;
    LocalizedText label;
    if (update.candidate == 0) label = localizedText("hand_order.progress.baseline") + QStringLiteral("\n");
    else if (update.candidate > 0) label = localizedText("hand_order.progress.candidate").arg(update.candidate) + QStringLiteral("\n");
    switch (update.phase) {
    case hand_order::HandOrderProgress::Preparing:
        label = label + localizedText("hand_order.progress.preparing"); break;
    case hand_order::HandOrderProgress::Windows:
        label = label + localizedText("hand_order.progress.windows").arg(update.completed).arg(update.total); break;
    case hand_order::HandOrderProgress::Decisions:
        label = label + localizedText("hand_order.progress.decisions").arg(update.window).arg(update.windows)
            .arg(update.attempt).arg(update.completed).arg(update.total); break;
    case hand_order::HandOrderProgress::Replay:
        label = label + localizedText("hand_order.progress.replay"); break;
    case hand_order::HandOrderProgress::Finished:
        label = label + localizedText("hand_order.progress.complete"); break;
    case hand_order::HandOrderProgress::Normalizing:
        label = localizedText("hand_order.workflow.stage_normalize"); break;
    case hand_order::HandOrderProgress::Patterns:
        label = localizedText("hand_order.workflow.stage_patterns"); break;
    case hand_order::HandOrderProgress::ConflictReview:
        label = localizedText("hand_order.workflow.stage_conflicts"); break;
    }
    if ((stages.isEmpty() || backgroundOnly_) && percent >= 0 && percent < 100) {
        label = label + QStringLiteral("\n") + (remainingSeconds >= 0
            ? localizedText("hand_order.progress.eta").arg(remainingSeconds)
            : localizedText("hand_order.progress.estimating"));
    }
    if (backgroundOnly_) {
        backgroundDetail_ = pauseRequested_ ? localizedText("hand_order.compute.paused") + QStringLiteral("\n") + label : label;
        backgroundPercent_ = percent;
        backgroundStatus_ = pauseRequested_ ? localizedText("hand_order.background.paused")
            : percent < 0 ? localizedText("hand_order.background.running")
                : localizedText("hand_order.background.progress").arg(percent);
        emit changed(); return;
    }
    message_ = label;
    if (progress_ && progressToken_ != 0 && progress_->token() == progressToken_) {
        if (!stages.isEmpty()) progress_->reportStages(percent, stages, remainingSeconds, label);
        else if (percent < 0) progress_->reportIndeterminate(label);
        else progress_->report(percent, label);
    }
    emit changed();
}
bool HandOrderAnalysisService::canApply() const
{
    if (!featuresEnabled_) return false;
    return !pending() && !taken_ && completed_ && !result_.patches.isEmpty()
        && result_.algorithmVersion == parameters_.version && result_.judgeProfileRevision == parameters_.judgeProfileRevision
        && isCurrent(*completed_);
}
QVariantMap HandOrderAnalysisService::navigationRange(int row, const QString& currentText) const
{
    if (pending() || !completed_ || row < 0 || row >= result_.rows.size()
        || currentText != completed_->snapshot.chartText || !isCurrent(*completed_)) return {};
    const auto& target = result_.rows.at(row);
    return {{"start", target.start}, {"end", target.end}, {"line", target.line}, {"col", target.col}};
}
QVariantList HandOrderAnalysisService::rows() const
{
    QVariantList rows;
    const QString source = completed_ ? completed_->snapshot.chartText : QString();
    QVector<int> sourceLines{0};
    for (int i = 0; i < source.size(); ++i) if (source.at(i) == '\n') sourceLines.append(i + 1);
    if (reviewingPlay_) {
        for (int i = 0; i < result_.rows.size(); ++i) {
            const auto& row = result_.rows.at(i);
            rows.append(QVariantMap{{"line", row.line}, {"col", row.col},
                {"original", row.originalAnnotation}, {"replacement", result_.patches.at(i).text}});
        }
        return rows;
    }
    for (const auto& row : result_.rows) {
        const auto handName = [](HandOrderHand hand) {
            return hand == HandOrderHand::Left ? qtTrId("hand_order.left")
                : hand == HandOrderHand::Right ? qtTrId("hand_order.right") : qtTrId("hand_order.unspecified");
        };
        QStringList noteAssignments;
        for (int i = 0; i < row.lanes.size(); ++i)
            noteAssignments.append(QString::number(row.lanes.at(i)) + QLatin1Char('=') + handName(row.noteHands.at(i)));
        QStringList intervals, evidence, handoffs, alternatives;
        QString counterfactualComponentText;
        QVariantList intervalData, causeData, handoffData, counterfactualData;
        for (int branch = 0; branch < row.trackIntervals.size(); ++branch) {
            const auto mode = row.wifiModes.value(branch, HandOrderWifiMode::Legacy);
            const auto appendStops = [&](const QVector<HandOrderTrackStop>& stops, int sector) {
                for (int i = 0; i < stops.size(); ++i) {
                    const double last = i + 1 < stops.size() ? stops.at(i + 1).progress : 1.0;
                    QString label = qtTrId("hand_order.auto.interval").arg(branch + 1)
                        .arg(stops.at(i).progress * 100, 0, 'f', 1).arg(last * 100, 0, 'f', 1).arg(handName(stops.at(i).hand));
                    if (sector >= 0) label = qtTrId("hand_order.auto.wifi_sector").arg(sector + 1).arg(label);
                    else if (mode == HandOrderWifiMode::Single) label = qtTrId("hand_order.auto.wifi_single").arg(label);
                    intervals.append(label);
                    intervalData.append(QVariantMap{{"branch", branch}, {"sector", sector}, {"wifiMode", int(mode)},
                        {"start", stops.at(i).progress}, {"end", last}, {"hand", handName(stops.at(i).hand)}});
                }
            };
            if (mode == HandOrderWifiMode::Split) {
                const auto sectors = row.wifiSectors.value(branch);
                for (int sector = 0; sector < sectors.size(); ++sector) appendStops(sectors.at(sector), sector);
            } else appendStops(row.trackIntervals.at(branch), -1);
        }
        for (const auto& cause : row.causes) {
            const QString hands = cause.handMask == 1 ? handName(HandOrderHand::Left)
                : cause.handMask == 2 ? handName(HandOrderHand::Right) : qtTrId("hand_order.auto.both_contributors");
            const int at = qBound(0, cause.tokenStart, int(source.size()));
            const int sourceLine = int(std::upper_bound(sourceLines.begin(), sourceLines.end(), at) - sourceLines.begin());
            const int sourceCol = at - sourceLines.at(sourceLine - 1) + 1;
            const QString kind = cause.kind == "slide" ? qtTrId("hand_order.auto.action.slide")
                : cause.kind == "touch_hold" ? qtTrId("hand_order.auto.action.touch_hold")
                : cause.kind == "button" ? qtTrId("hand_order.auto.action.button")
                : cause.kind == "screen_button" ? qtTrId("hand_order.auto.action.screen_button") : qtTrId("hand_order.auto.action.touch");
            evidence.append(qtTrId("hand_order.auto.cause").arg(QStringLiteral("%1:%2").arg(sourceLine).arg(sourceCol)).arg(cause.branch + 1).arg(kind)
                .arg(cause.pad).arg(cause.second, 0, 'f', 4).arg(cause.progress * 100, 0, 'f', 1).arg(hands));
            causeData.append(QVariantMap{{"sourceStart", cause.tokenStart}, {"branch", cause.branch}, {"kind", cause.kind},
                {"pad", cause.pad}, {"second", cause.second}, {"progress", cause.progress}, {"handMask", cause.handMask},
                {"sourceLine", sourceLine}, {"sourceCol", sourceCol}});
        }
        for (const auto& proof : row.counterfactualEvidence) {
            QString component;
            if (proof.component == "head") component = qtTrId("hand_order.auto.component.head");
            else if (proof.component == "touch") component = qtTrId("hand_order.auto.component.touch");
            else if (proof.component.startsWith("note")) component = qtTrId("hand_order.auto.component.note").arg(proof.component.mid(4));
            else {
                const auto parts = proof.component.split(QLatin1Char(':'));
                component = qtTrId("hand_order.auto.component.track").arg(parts.value(1).toInt() + 1).arg(proof.progress * 100, 0, 'f', 1);
                bool sectorOk = false;
                const int sector = parts.value(3).toInt(&sectorOk);
                if (parts.size() == 4 && parts.value(2) == "sector" && sectorOk && sector >= 0)
                    component = qtTrId("hand_order.auto.wifi_sector").arg(sector + 1).arg(component);
            }
            if (proof.found && proof.component == row.counterfactualComponent) counterfactualComponentText = component;
            const QString state = !proof.searched || proof.limited ? qtTrId("hand_order.auto.evidence_limited")
                : proof.found ? qtTrId("hand_order.auto.evidence_alternative") : qtTrId("hand_order.auto.evidence_no_alternative");
            QString summary = component + QStringLiteral(": ") + state;
            if (proof.found) summary += qtTrId("hand_order.auto.evidence_gap").arg(proof.signedRiskTierGap)
                .arg(proof.signedPeakTierGap).arg(proof.signedCostGap, 0, 'f', 3);
            alternatives.append(summary);
            counterfactualData.append(QVariantMap{{"component", proof.component}, {"progress", proof.progress},
                {"alternativeHand", handName(proof.alternativeHand)}, {"searched", proof.searched}, {"found", proof.found},
                {"limited", proof.limited}, {"signedCostGap", proof.signedCostGap}, {"signedRiskTierGap", proof.signedRiskTierGap},
                {"signedPeakTierGap", proof.signedPeakTierGap}, {"score", proof.score}});
        }
        for (const auto& proof : row.handoffEvidence) {
            const QString reason = proof.reason.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + proof.reason).toUtf8().constData());
            handoffData.append(QVariantMap{{"branch", proof.branch}, {"sector", proof.sector}, {"handoffCount", proof.handoffCount},
                {"baselineFound", proof.baselineFound}, {"baselineCost", proof.baselineCost}, {"baselinePeak", proof.baselinePeak},
                {"reason", proof.reason}, {"reasonText", reason}});
            if (proof.handoffCount <= 0) continue;
            QString summary = qtTrId("hand_order.auto.handoff_branch").arg(proof.branch + 1).arg(proof.handoffCount).arg(reason);
            if (proof.sector >= 0) summary = qtTrId("hand_order.auto.wifi_sector").arg(proof.sector + 1).arg(summary);
            if (proof.baselineFound) summary += qtTrId("hand_order.auto.handoff_baseline")
                .arg(proof.baselinePeak, 0, 'f', 2).arg(proof.baselineCost, 0, 'f', 3);
            handoffs.append(summary);
        }
        rows.append(QVariantMap{{"line", row.line}, {"col", row.col}, {"status", row.status},
            {"context", row.context}, {"originalAnnotation", row.originalAnnotation},
            {"hasCandidate", row.hasCandidate},
            {"hasAssignments", row.hasCandidate && (!row.noteHands.isEmpty() || row.head != HandOrderHand::Unspecified || row.track != HandOrderHand::Unspecified || !row.wifiSectors.isEmpty())},
            {"statusText", qtTrId(("hand_order.auto.status." + row.status).toUtf8().constData())},
            {"reason", qtTrId(("hand_order.auto.reason." + row.reason).toUtf8().constData())},
            {"noteAssignments", noteAssignments.join(QStringLiteral(" / "))}, {"head", handName(row.head)}, {"track", handName(row.track)},
            {"cost", row.cost}, {"peakSpeed", row.peakSpeed}, {"inputChannel", row.inputChannel}, {"pressureLevel", row.pressureLevel},
            {"playerCost", row.playerCost}, {"peakTapRate", row.peakTapRate}, {"peakLoad", row.peakLoad},
            {"pressureText", row.pressureLevel.isEmpty() ? QString() : qtTrId(("hand_order.auto.pressure." + row.pressureLevel).toUtf8().constData())}, {"hasCounterfactual", row.hasCounterfactual},
            {"dependencyUnit", row.dependencyUnit}, {"confidence", row.confidence}, {"confidenceLevel", row.confidenceLevel},
            {"confidenceText", qtTrId("hand_order.auto.confidence_text").arg(qRound(row.confidence * 100))
                .arg(qtTrId(("hand_order.auto.confidence." + row.confidenceLevel).toUtf8().constData()))},
            {"counterfactualData", counterfactualData}, {"counterfactualSummary", alternatives.join(QStringLiteral("; "))},
            {"intervals", intervals.join(QStringLiteral(" / "))}, {"intervalData", intervalData},
            {"evidence", evidence.join(QStringLiteral("; "))}, {"causeData", causeData},
            {"judgementKind", row.judgementKind}, {"judgementQuality", row.judgementQuality},
            {"judgementKindText", row.judgementKind.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + row.judgementKind).toUtf8().constData())},
            {"judgementQualityText", row.judgementQuality.isEmpty() ? QString() : qtTrId(("hand_order.auto.quality." + row.judgementQuality).toUtf8().constData())},
            {"fullReplayVerified", row.fullReplayVerified}, {"judgementSecond", row.judgementSecond}, {"judgementMargin", row.judgementMargin},
            {"conditions", row.conditions.isEmpty() ? QString() : qtTrId("hand_order.auto.conditions_text")
                + (row.conditions.contains(QStringLiteral("adjusted_slide_input_timing"))
                    ? QStringLiteral(" · ") + qtTrId("hand_order.auto.adjusted_slide_input_timing") : QString())},
            {"handoffCount", row.handoffCount},
            {"handoffReason", row.handoffReason.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + row.handoffReason).toUtf8().constData())},
            {"oneHandoffCost", row.oneHandoffCost}, {"oneHandoffPeak", row.oneHandoffPeak},
            {"handoffEvidence", handoffData}, {"handoffSummary", handoffs.join(QStringLiteral("; "))},
            {"counterfactualComponent", counterfactualComponentText},
            {"counterfactualHand", handName(row.counterfactualHand)}, {"signedCostGap", row.signedCostGap}, {"signedPeakTierGap", row.signedPeakTierGap}});
    }
    return rows;
}
bool HandOrderAnalysisService::patternChoicesEnabled() const
{
    if (!featuresEnabled_) return false;
    const auto current = workspace_->snapshot();
    return !pending() && !preservePatternAnnotations_ && current.hasDocument
        && current.documentOpenGeneration == patternIdentity_.documentOpenGeneration
        && current.activeDifficultyId == patternIdentity_.activeDifficultyId
        && current.revision == patternIdentity_.revision
        && !patternSuggestions_.isEmpty();
}
void HandOrderAnalysisService::retainPatternSuggestions(const Request& request, const hand_order::BasicResult& result)
{
    if ((!request.optimizePatterns && (!result.combinedOptimization || backgroundOnly_))
        || !identityCurrent(request) || !result.error.isEmpty()
        || (result.patternOutcome != "recognized" && result.patternOutcome != "no_patterns")) return;
    // Keep independent ranges when the user asks about several selections.
    if (patternText_ != request.snapshot.chartText) { patternSuggestions_.clear(); patternFullScan_ = false; }
    QHash<QString, QString> identities;
    for (const auto& suggestion : patternSuggestions_) identities.insert(handOrderPatternFingerprint(suggestion), patternKey(suggestion));
    patternSuggestions_.removeIf([&](const auto& s) { return s.start < request.end && s.end > request.begin; });
    for (auto suggestion : result.patternSuggestions) {
        const QString fingerprint = handOrderPatternFingerprint(suggestion);
        if (ignoredPatternKeys_.contains(fingerprint)) continue;
        suggestion.instanceKey = identities.value(fingerprint);
        if (suggestion.instanceKey.isEmpty()) suggestion.instanceKey = QStringLiteral("%1:%2:%3")
            .arg(request.identity.documentOpenGeneration).arg(request.identity.activeDifficultyId).arg(++patternInstanceSequence_);
        patternSuggestions_.append(std::move(suggestion));
    }
    std::sort(patternSuggestions_.begin(), patternSuggestions_.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    patternText_ = request.snapshot.chartText; patternIdentity_ = request.identity;
    if (request.optimizePatterns) patternManualReview_ = true;
    patternTiming_ = request.snapshot.timingMetadata;
    patternFullScan_ |= request.begin == 0 && request.end == request.snapshot.chartText.size();
    emit patternSuggestionsChanged();
}
void HandOrderAnalysisService::resetPatternState()
{
    patternSuggestions_.clear(); ignoredPatternSuggestions_.clear(); ignoredPatternKeys_.clear();
    patternText_.clear(); patternFullScan_ = false; patternManualReview_ = false; patternTiming_ = {};
}
void HandOrderAnalysisService::refreshPatternSuggestions()
{
    if (!featuresEnabled_) { resetPatternState(); return; }
    const auto current = workspace_->snapshot();
    if (current.documentOpenGeneration != patternIdentity_.documentOpenGeneration) {
        patternStates_.clear(); resetPatternState(); patternIdentity_ = current; return;
    }
    if (current.activeDifficultyId != patternIdentity_.activeDifficultyId) {
        if (patternManualReview_ || !patternText_.isEmpty()) patternStates_.insert(patternIdentity_.activeDifficultyId,
            {patternSuggestions_, ignoredPatternSuggestions_, ignoredPatternKeys_, patternText_, patternIdentity_, patternTiming_, patternFullScan_, patternManualReview_});
        resetPatternState();
        const auto saved = patternStates_.constFind(current.activeDifficultyId);
        if (saved == patternStates_.cend()) { patternIdentity_ = current; return; }
        patternSuggestions_ = saved->suggestions; ignoredPatternSuggestions_ = saved->ignored;
        ignoredPatternKeys_ = saved->ignoredKeys; patternText_ = saved->text;
        patternIdentity_ = saved->identity; patternTiming_ = saved->timing; patternFullScan_ = saved->fullScan;
        patternManualReview_ = saved->manualReview;
    }
    if (patternText_.isEmpty()) { patternIdentity_ = current; return; }
    const auto* difficulty = workspace_->document().difficulty(current.activeDifficultyId);
    if (!difficulty) { resetPatternState(); patternIdentity_ = current; return; }
    const QString text = difficulty->chart;
    const auto timing = simai::buildTimingMetadata(workspace_->document());
    if (text == patternText_ && timing == patternTiming_) { patternIdentity_ = current; return; }
    const auto old = SimaiParser::validateSyntax(patternText_, patternTiming_);
    const auto next = SimaiParser::validateSyntax(text, timing);
    // An incomplete edit does not erase review decisions or trigger a new scan.
    if (!next.ok || !next.annotationErrors.isEmpty()) { patternFullScan_ = false; return; }
    const auto lineStarts = [](const QString& source) {
        QVector<int> lines{0}; for (int i = 0; i < source.size(); ++i) if (source.at(i) == '\n') lines.append(i + 1);
        return lines;
    };
    const auto oldLines = lineStarts(patternText_), nextLines = lineStarts(text);
    struct Span { int start, end, nativeStart, nativeEnd, firstOrder, count; QString base; };
    const auto spans = [&](const QString& source, const SimaiParseResult& parsed, const QVector<int>& lines) {
        QVector<Span> result; int removed = 0;
        for (const auto& t : parsed.sourceTokens) {
            const int start = lines.at(t.line - 1) + t.col - 1;
            const QString base = hand_order::splitToken(source.mid(start, t.length)).baseToken;
            result.append({start, start + t.length, start - removed, start - removed + int(base.size()), t.firstParseOrder, t.markerCount, base});
            removed += t.length - base.size();
        }
        return result;
    };
    const auto before = spans(patternText_, old, oldLines), after = spans(text, next, nextLines);
    const QString oldNative = hand_order::stripAnnotations(patternText_), newNative = hand_order::stripAnnotations(text);
    int prefix = 0, suffix = 0;
    while (prefix < oldNative.size() && prefix < newNative.size() && oldNative.at(prefix) == newNative.at(prefix)) ++prefix;
    while (suffix < oldNative.size() - prefix && suffix < newNative.size() - prefix
        && oldNative.at(oldNative.size() - suffix - 1) == newNative.at(newNative.size() - suffix - 1)) ++suffix;
    const int oldChangeEnd = oldNative.size() - suffix, delta = newNative.size() - oldNative.size();
    QHash<int, int> afterByNative, beforeByStart, mapped, markerOrders;
    for (int i = 0; i < after.size(); ++i) afterByNative.insert(after.at(i).nativeStart, i);
    for (int i = 0; i < before.size(); ++i) {
        const auto& a = before.at(i); beforeByStart.insert(a.start, i);
        int position = -1;
        if (a.nativeEnd <= prefix) position = a.nativeStart;
        else if (a.nativeStart >= oldChangeEnd) position = a.nativeStart + delta;
        const int index = afterByNative.value(position, -1);
        if (index < 0 || a.base != after.at(index).base || a.count != after.at(index).count) continue;
        mapped.insert(a.start, index);
        for (int n = 0; n < a.count; ++n) markerOrders.insert(a.firstOrder + n, after.at(index).firstOrder + n);
    }
    QHash<int, TimelineNoteMarker> markers;
    for (const auto& m : next.noteMarkers) markers.insert(m.parseOrder, m);
    const auto rebase = [&](QVector<hand_order::PatternSuggestion>& list, bool hidden) {
        QVector<hand_order::PatternSuggestion> retained;
        for (auto group : list) {
            const auto& original = group.variants.first().patches;
            const auto firstSpan = beforeByStart.value(original.first().start, -1);
            const auto lastSpan = beforeByStart.value(original.last().start, -1);
            if (firstSpan < 0 || lastSpan < 0) continue;
            if (oldNative != newNative && before.at(firstSpan).nativeStart < oldChangeEnd
                && before.at(lastSpan).nativeEnd > prefix) continue;
            bool valid = true; int preferred = -1;
            QVector<hand_order::PatternVariant> candidates;
            for (int variantIndex = 0; variantIndex < group.variants.size(); ++variantIndex) {
                auto candidate = group.variants.at(variantIndex);
                for (auto& patch : candidate.patches) {
                    if (!mapped.contains(patch.start)) { valid = false; break; }
                    const auto& span = after.at(mapped.value(patch.start)); patch.start = span.start; patch.end = span.end;
                }
                if (!valid) break;
                for (auto& m : candidate.markers) {
                    if (!markerOrders.contains(m.parseOrder)) { valid = false; break; }
                    auto fresh = markers.value(markerOrders.value(m.parseOrder));
                    fresh.handOrderHead = m.handOrderHead; fresh.handOrderTrackStops = m.handOrderTrackStops;
                    fresh.handOrderWifiMode = m.handOrderWifiMode; fresh.handOrderWifiSectorStops = m.handOrderWifiSectorStops;
                    m = std::move(fresh);
                }
                if (!valid) break;
                if (!hidden && !hand_order::respectsHoldLocks(next.noteMarkers, candidate.markers)) continue;
                if (variantIndex == group.preferred) preferred = candidates.size();
                candidates.append(std::move(candidate));
            }
            if (!valid || candidates.isEmpty()) continue;
            group.variants = std::move(candidates); group.preferred = preferred;
            if (preferred < 0) group.definite = false;
            group.start = group.variants.first().patches.first().start; group.end = group.variants.first().patches.last().end;
            group.line = int(std::upper_bound(nextLines.cbegin(), nextLines.cend(), group.start) - nextLines.cbegin());
            group.col = group.start - nextLines.at(group.line - 1) + 1;
            group.second = std::numeric_limits<double>::infinity(); group.endSecond = 0;
            for (const auto& m : group.variants.first().markers) {
                group.second = qMin(group.second, m.second); group.endSecond = qMax(group.endSecond, qMax(m.second, m.endSecond));
            }
            for (auto& candidate : group.variants) {
                candidate.syntax = text.mid(group.start, group.end - group.start);
                for (int i = candidate.patches.size() - 1; i >= 0; --i) {
                    const auto& p = candidate.patches.at(i); candidate.syntax.replace(p.start - group.start, p.end - p.start, p.text);
                }
            }
            if (hidden) ignoredPatternKeys_.insert(handOrderPatternFingerprint(group));
            retained.append(std::move(group));
        }
        list = std::move(retained);
    };
    rebase(patternSuggestions_, false); rebase(ignoredPatternSuggestions_, true);
    // Retain the review projection, but an explicit calculation must see all
    // edited inputs, including hand labels that can change the default roles.
    patternFullScan_ = false;
    patternText_ = text; patternIdentity_ = current; patternTiming_ = timing;
}
QVariantList HandOrderAnalysisService::patternSuggestions() const
{
    QVariantList groups;
    for (int sourceIndex = 0; sourceIndex < patternSuggestions_.size(); ++sourceIndex) {
        const auto& s = patternSuggestions_.at(sourceIndex);
        QVariantList variants;
        for (const auto& candidate : s.variants) {
            bool applied = true;
            for (const auto& p : candidate.patches)
                applied &= patternText_.mid(p.start, p.end - p.start) == p.text;
            hand_order::PatternPreview preview = std::make_shared<hand_order::PatternPreviewData>(hand_order::PatternPreviewData{candidate.markers});
            variants.append(QVariantMap{{"strategy", qtTrId(("hand_order.pattern.strategy." + candidate.strategy).toUtf8().constData())},
                {"choiceKey", candidate.choiceKey}, {"syntax", candidate.syntax}, {"previewData", QVariant::fromValue(preview)}, {"applied", applied}});
        }
        const QString category = s.registryId.left(1);
        const int categoryOrder = QStringLiteral("TSHUNAG").indexOf(category);
        groups.append(QVariantMap{{"registryId", s.registryId}, {"key", patternKey(s)}, {"sourceIndex", sourceIndex},
            {"similarityKey", s.similarityKey}, {"typeKey", s.name}, {"typeLabel", qtTrId(("hand_order.pattern.group." + s.name).toUtf8().constData())},
            {"categoryOrder", categoryOrder < 0 ? 99 : categoryOrder},
            {"category", qtTrId(("hand_order.pattern.category." + category).toUtf8().constData())},
            {"name", qtTrId(("hand_order.pattern.name." + s.name).toUtf8().constData())},
            {"line", s.line}, {"col", s.col}, {"second", s.second}, {"endSecond", s.endSecond},
            {"definite", s.definite}, {"preferred", s.preferred}, {"variants", variants}});
    }
    return groups;
}
QVariantMap HandOrderAnalysisService::patternNavigationRange(int group, const QString& text) const
{
    if (group < 0 || group >= patternSuggestions_.size() || text != patternText_) return {};
    const auto& suggestion = patternSuggestions_.at(group);
    return {{"start", suggestion.start}, {"end", suggestion.end}};
}
QVariantMap HandOrderAnalysisService::patternNavigationTarget(int group) const
{
    const auto current = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(current.activeDifficultyId);
    if (!difficulty || current.documentOpenGeneration != patternIdentity_.documentOpenGeneration
        || current.activeDifficultyId != patternIdentity_.activeDifficultyId
        || current.revision != patternIdentity_.revision || difficulty->chart != patternText_
        || group < 0 || group >= patternSuggestions_.size()) return {};
    const auto& s = patternSuggestions_.at(group);
    return {{"difficultyId", current.activeDifficultyId}, {"revision", QVariant::fromValue(current.revision)},
        {"generation", QVariant::fromValue(current.documentOpenGeneration)}, {"start", s.start}, {"end", s.end},
        {"line", s.line}, {"column", s.col}, {"second", s.second}};
}
void HandOrderAnalysisService::pruneIgnoredPatterns(hand_order::BasicResult& result, const QString& text) const
{
    if (ignoredPatternKeys_.isEmpty()
        || (result.patternOutcome != "recognized" && result.patternOutcome != "no_patterns")) return;
    result.patternSuggestions.removeIf([&](const auto& s) { return ignoredPatternKeys_.contains(handOrderPatternFingerprint(s)); });
    if (!result.patternOptimization) return;
    result.patches.clear(); result.patternAccepted = result.patternCandidates = 0;
    for (const auto& s : result.patternSuggestions) {
        result.patternCandidates += s.variants.size();
        if (s.preferred < 0) continue;
        bool changed = false;
        for (const auto& p : s.variants.at(s.preferred).patches)
            if (text.mid(p.start, p.end - p.start) != p.text) { result.patches.append(p); changed = true; }
        result.patternAccepted += changed;
    }
    std::sort(result.patches.begin(), result.patches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    result.patternSegments = result.patternSuggestions.size();
    result.patternOutcome = result.patternSegments ? "recognized" : "no_patterns";
}
void HandOrderAnalysisService::ignorePattern(int group)
{
    if (patternNavigationTarget(group).isEmpty()) return;
    ignorePatterns({patternKey(patternSuggestions_.at(group))});
}
void HandOrderAnalysisService::ignorePatterns(const QStringList& keys)
{
    bool removed = false;
    for (int index = patternSuggestions_.size() - 1; index >= 0; --index) {
        const auto& suggestion = patternSuggestions_.at(index);
        if (!keys.contains(patternKey(suggestion)) || patternNavigationTarget(index).isEmpty()) continue;
        ignoredPatternKeys_.insert(handOrderPatternFingerprint(suggestion));
        ignoredPatternSuggestions_.append(suggestion);
        patternSuggestions_.removeAt(index);
        removed = true;
    }
    if (!removed) return;
    pruneIgnoredPatterns(result_, patternText_);
    if (result_.patternOptimization)
        message_ = localizedText(("hand_order.optimize." + result_.patternOutcome).toUtf8().constData());
    emit patternSuggestionsChanged(); emit changed();
}
editor::SimaiTextEditResult HandOrderAnalysisService::takePatternEdit(int group, int variant, const QString& text)
{
    editor::SimaiTextEditResult edit;
    if (!patternChoicesEnabled() || text != patternText_ || group < 0 || group >= patternSuggestions_.size()) return edit;
    const auto& s = patternSuggestions_.at(group);
    if (variant < 0 || variant >= s.variants.size()) return edit;
    return takePatternBatchEdit({patternKey(s)},s.variants.at(variant).choiceKey,text);
}
editor::SimaiTextEditResult HandOrderAnalysisService::takePatternBatchEdit(
    const QStringList& keys, const QString& choice, const QString& text)
{
    if (!featuresEnabled_) return {};
    editor::SimaiTextEditResult edit;
    if (!patternChoicesEnabled() || text != patternText_ || keys.isEmpty() || choice.isEmpty()) return edit;
    QString similarity; QSet<QString> seen; QSet<int> orders;
    QVector<chart_transform::HandOrderEditPlan::Patch> patches;
    for (const auto& key : keys) {
        if (seen.contains(key)) return edit;
        seen.insert(key);
        auto group = std::find_if(patternSuggestions_.cbegin(),patternSuggestions_.cend(),[&](const auto& s) { return patternKey(s) == key; });
        if (group == patternSuggestions_.cend() || group->similarityKey.isEmpty()) return edit;
        if (similarity.isEmpty()) similarity = group->similarityKey;
        if (similarity != group->similarityKey) return edit;
        auto candidate = std::find_if(group->variants.cbegin(),group->variants.cend(),[&](const auto& v) { return v.choiceKey == choice; });
        if (candidate == group->variants.cend()) return edit;
        const bool protect = completed_ && completed_->snapshot.chartText == text && result_.combinedOptimization
            && !completed_->parameters.overwriteExisting;
        if (protect && std::any_of(candidate->patches.cbegin(), candidate->patches.cend(), [&](const auto& patch) {
                const auto original = text.mid(patch.start, patch.end - patch.start);
                return original != patch.text && !hand_order::splitToken(original).suffix.isEmpty();
            })) return edit;
        patches += candidate->patches;
        for (const auto& marker : candidate->markers) orders.insert(marker.parseOrder);
    }
    std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b) { return a.start < b.start; });
    if (patches.isEmpty()) return edit;
    for (int i = 1; i < patches.size(); ++i) if (patches[i].start < patches[i-1].end) return edit;
    const int begin = patches.first().start, end = patches.last().end;
    QString replacement = text.mid(begin,end-begin);
    for (int i = patches.size()-1; i >= 0; --i) {
        const auto& p = patches[i]; replacement.replace(p.start-begin,p.end-p.start,p.text);
    }
    if (replacement == text.mid(begin,end-begin)) return edit;
    const QString modified = text.left(begin)+replacement+text.mid(end);
    const auto parsed = SimaiParser::validateSyntax(modified,patternTiming_);
    QVector<TimelineNoteMarker> changed, changedHolds;
    for (const auto& marker : parsed.noteMarkers) if (orders.contains(marker.parseOrder)) {
        changed.append(marker); if (hand_order::isLockingHold(marker)) changedHolds.append(marker);
    }
    if (!parsed.ok || !parsed.annotationErrors.isEmpty()
        || hand_order::stripAnnotations(modified) != hand_order::stripAnnotations(text)
        || !hand_order::respectsHoldLocks(parsed.noteMarkers,changed)
        || !hand_order::respectsHoldLocks(changedHolds,parsed.noteMarkers)) return edit;
    edit.consumed = true;
    edit.transaction = {begin,begin+int(replacement.size()),true,begin,end,replacement,-1};
    return edit;
}
editor::SimaiTextEditResult HandOrderAnalysisService::takeEdit(const QString& text)
{
    editor::SimaiTextEditResult edit;
    if (!canApply() || text != completed_->snapshot.chartText) {
        invalidate(localizedText("hand_order.auto.stale")); return edit;
    }
    const auto& patches = result_.patches;
    const int begin = patches.first().start, end = patches.last().end;
    QString replacement = text.mid(begin, end - begin);
    int delta = 0;
    for (const auto& patch : patches) {
        if (patch.start < completed_->begin || patch.end > completed_->end) return edit;
        replacement.replace(patch.start - begin + delta, patch.end - patch.start, patch.text);
        delta += patch.text.size() - (patch.end - patch.start);
    }
    const auto original = SimaiParser::validateSyntax(text, completed_->snapshot.timingMetadata);
    const auto verified = SimaiParser::validateSyntax(text.left(begin) + replacement + text.mid(end),
        completed_->snapshot.timingMetadata);
    if (!verified.ok || verified.annotationErrors.size() > original.annotationErrors.size()) {
        message_ = localizedText("hand_order.auto.invalid_output"); emit changed(); return edit;
    }
    if (result_.patternOptimization || result_.combinedOptimization) {
        QSet<int> orders;
        if (result_.combinedOptimization) {
            QSet<int> starts;
            for (const auto& patch : patches) starts.insert(patch.start);
            QSet<QPair<int,int>> locations;
            for (const auto& row : result_.rows)
                if (starts.contains(row.start)) locations.insert({row.line, row.col});
            for (const auto& token : original.sourceTokens)
                if (locations.contains({token.line, token.col}))
                    for (int i = 0; i < token.markerCount; ++i) orders.insert(token.firstParseOrder + i);
        } else for (const auto& group : result_.patternSuggestions) if (group.preferred >= 0)
            for (const auto& marker : group.variants.at(group.preferred).markers) orders.insert(marker.parseOrder);
        QVector<TimelineNoteMarker> proposed, holds;
        for (const auto& marker : verified.noteMarkers) if (orders.contains(marker.parseOrder)) {
            proposed.append(marker); if (hand_order::isLockingHold(marker)) holds.append(marker);
        }
        if (!hand_order::respectsHoldLocks(verified.noteMarkers,proposed)
            || !hand_order::respectsHoldLocks(holds,verified.noteMarkers)) {
            message_ = localizedText("hand_order.auto.invalid_output"); emit changed(); return edit;
        }
    }
    edit.consumed = true;
    edit.transaction = {begin, begin + int(replacement.size()), true, begin, end, replacement, -1};
    taken_ = true; emit changed();
    return edit;
}
} // namespace miacode
