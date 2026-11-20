#include "app/services/HandOrderExperimentService.h"
#include "core/chart/document/SimaiTimingMetadata.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <cmath>

namespace miacode {
namespace {
bool sameProfile(const hand_order::PlayerProfile& a, const hand_order::PlayerProfile& b)
{
    return a.maxComfortableTapRate == b.maxComfortableTapRate && a.comfortableMoveSpeed == b.comfortableMoveSpeed
        && a.staminaCapacity == b.staminaCapacity && a.recoveryHalfLife == b.recoveryHalfLife
        && a.techniquePreference == b.techniquePreference && a.postureWeight == b.postureWeight;
}
QString reasonText(const QStringList& keys)
{
    QStringList labels;
    for (const auto& key : keys) labels.append(qtTrId(qPrintable(QStringLiteral("hand_order.experiment.reason.") + key)));
    return labels.join(QStringLiteral(" · "));
}
}

HandOrderExperimentService::HandOrderExperimentService(ChartWorkspace& workspace, QObject* parent)
    : QObject(parent), workspace_(&workspace)
{
    worker_.setMaxThreadCount(1);
    worker_.setThreadPriority(QThread::LowPriority);
    connect(&workspace, &ChartWorkspace::changed, this, [this] {
        stopWork(); analysis_ = {}; simulation_ = {}; state_ = QStringLiteral("stale"); emit changed();
    });
}
HandOrderExperimentService::~HandOrderExperimentService()
{
    stopWork();
    worker_.waitForDone();
}
void HandOrderExperimentService::setJobProgress(JobProgressService* progress)
{
    if (progress_) disconnect(progress_, nullptr, this, nullptr);
    endProgress(); progress_ = progress;
    if (progress_) connect(progress_, &JobProgressService::cancellationRequested, this, [this](quint64 token) {
        if (token == progressToken_) cancel();
    });
}
void HandOrderExperimentService::beginProgress(const char* label)
{
    if (!progress_ || progress_->active()) return;
    const QPointer<JobProgressService> progress = progress_;
    const QPointer<HandOrderExperimentService> self(this);
    const quint64 expectedToken = progress->token() + 1;
    const quint64 requestSequence = sequence_;
    // begin() emits synchronously: a listener may replace this job before it returns.
    progress->begin(localizedText("hand_order.experiment.title"), localizedText(label), true);
    if (!self || !progress || progress_ != progress || !progress->active() || progress->token() != expectedToken) return;
    if (sequence_ != requestSequence || !pending_) {
        progress->end(); return;
    }
    progressToken_ = expectedToken;
    if (progress->cancelRequested()) { cancel(); return; }
    progress->reportIndeterminate(localizedText(label));
}
void HandOrderExperimentService::endProgress()
{
    const quint64 owner = progressToken_;
    progressToken_ = 0;
    if (progress_ && owner && progress_->token() == owner) progress_->end();
}
void HandOrderExperimentService::stopWork()
{
    endProgress();
    ++sequence_;
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    worker_.clear();
    pending_ = false;
}
void HandOrderExperimentService::cancel()
{
    stopWork(); state_ = QStringLiteral("cancelled"); emit changed();
}
bool HandOrderExperimentService::current(const ChartWorkspaceSnapshot& identity) const
{
    const auto now = workspace_->snapshot();
    return now.hasDocument && now.documentOpenGeneration == identity.documentOpenGeneration
        && now.revision == identity.revision && now.activeDifficultyId == identity.activeDifficultyId;
}
bool HandOrderExperimentService::hasAnalysis() const
{
    if (!featuresEnabled_) return false;
    return analysis_.valid && parserPolicy_ == SimaiParser::allowNegativeHsEnabled() && current(identity_);
}
void HandOrderExperimentService::setPlayerProfile(const hand_order::PlayerProfile& profile)
{
    if (!hand_order::validPlayerProfile(profile) || sameProfile(profile_, profile)) return;
    stopWork(); profile_ = profile;
    parameters_.movementSpeed = profile.comfortableMoveSpeed;
    parameters_.stamina = profile.staminaCapacity;
    analysis_ = {}; simulation_ = {}; state_ = QStringLiteral("stale"); emit changed();
}
void HandOrderExperimentService::refreshParserPolicy()
{
    const bool policy = SimaiParser::allowNegativeHsEnabled();
    if (parserPolicy_ == policy) return;
    stopWork(); parserPolicy_ = policy; analysis_ = {}; simulation_ = {};
    state_ = QStringLiteral("stale"); emit changed();
}
QString HandOrderExperimentService::status() const
{
    if ((state_ == QLatin1String("ready") || state_ == QLatin1String("simulated")) && !hasAnalysis())
        return qtTrId("hand_order.experiment.stale");
    if (state_ == QLatin1String("failed")) {
        QString detail = error_;
        if (error_ == QLatin1String("input_limit")) detail = qtTrId("hand_order.experiment.input_limit");
        else if (error_ == QLatin1String("invalid_chart_or_profile") || error_ == QLatin1String("invalid_analysis_or_parameters"))
            detail = qtTrId("hand_order.experiment.invalid_input");
        else if (error_ == QLatin1String("analysis_failed") || error_ == QLatin1String("simulation_failed"))
            detail = qtTrId("hand_order.experiment.worker_failed");
        return qtTrId("hand_order.experiment.failed").arg(detail);
    }
    if (state_ == QLatin1String("ready")) return qtTrId("hand_order.experiment.ready")
        .arg(analysis_.segments.size()).arg(analysis_.workUsed)
        + (analysis_.limited ? QLatin1Char('\n') + qtTrId("hand_order.experiment.reason.bounded_search") : QString())
        + (analysis_.unlocatedRows ? QLatin1Char('\n') + qtTrId("hand_order.experiment.unlocated").arg(analysis_.unlocatedRows) : QString());
    if (state_ == QLatin1String("simulated")) return qtTrId("hand_order.experiment.simulated")
        .arg(simulation_.samples).arg(simulation_.outsideWindow).arg(parameters_.seed);
    return qtTrId(qPrintable(QStringLiteral("hand_order.experiment.") + state_));
}
bool HandOrderExperimentService::requestAnalysis()
{
    if (!featuresEnabled_) return false;
    refreshParserPolicy();
    if (pending_) return false;
    const auto identity = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(identity.activeDifficultyId);
    if (!identity.hasDocument || !difficulty) return false;
    const QString text = difficulty->chart;
    if (!hand_order::experiment::withinInputLimit(text)
        || text.count(QLatin1Char(',')) + text.count(QLatin1Char('/')) + text.count(QLatin1Char('*')) > 20000) {
        state_ = QStringLiteral("failed"); error_ = QStringLiteral("input_limit"); emit changed(); return false;
    }
    const auto timing = simai::buildTimingMetadata(workspace_->document());
    const auto profile = profile_;
    stopWork(); analysis_ = {}; simulation_ = {};
    identity_ = identity; pending_ = true; state_ = QStringLiteral("analyzing");
    const quint64 sequence = sequence_;
    const auto cancellation = cancellation_ = std::make_shared<std::atomic_bool>(false);
    const bool allowNegativeHs = SimaiParser::allowNegativeHsEnabled();
    QPointer<HandOrderExperimentService> guard(this);
    beginProgress("hand_order.experiment.analyzing");
    if (!guard || sequence != guard->sequence_ || !guard->pending_) return false;
    emit changed();
    if (!guard || sequence != guard->sequence_ || !guard->pending_) return false;
    worker_.start([guard, identity, text, timing, profile, sequence, cancellation, allowNegativeHs] {
        hand_order::experiment::Analysis result;
        task::CancellationScope scope(cancellation);
        try {
            task::CancellationScope::check();
            const auto parsed = SimaiParser::validateSyntax(text, timing);
            result = hand_order::experiment::analyze(text, parsed, timing, profile);
        } catch (const task::Cancelled&) { return; }
        catch (...) { result.error = QStringLiteral("analysis_failed"); }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, identity, sequence, cancellation, allowNegativeHs, result = std::move(result)]() mutable {
            if (!guard || cancellation->load(std::memory_order_relaxed) || sequence != guard->sequence_ || !guard->current(identity)) return;
            if (SimaiParser::allowNegativeHsEnabled() != allowNegativeHs) {
                guard->refreshParserPolicy(); return;
            }
            guard->endProgress();
            if (!guard || sequence != guard->sequence_ || !guard->current(identity)) return;
            if (SimaiParser::allowNegativeHsEnabled() != allowNegativeHs) {
                guard->refreshParserPolicy(); return;
            }
            guard->pending_ = false; guard->analysis_ = std::move(result);
            guard->error_ = guard->analysis_.error;
            guard->state_ = guard->analysis_.valid ? QStringLiteral("ready") : QStringLiteral("failed");
            emit guard->changed();
        }, Qt::QueuedConnection);
    });
    return true;
}
bool HandOrderExperimentService::requestSimulation()
{
    if (!featuresEnabled_) return false;
    refreshParserPolicy();
    if (pending_ || !hasAnalysis()) return false;
    stopWork(); simulation_ = {}; pending_ = true; state_ = QStringLiteral("simulating");
    const auto analysis = analysis_; const auto parameters = parameters_; const auto identity = identity_;
    const quint64 sequence = sequence_;
    const auto cancellation = cancellation_ = std::make_shared<std::atomic_bool>(false);
    const bool parserPolicy = parserPolicy_;
    QPointer<HandOrderExperimentService> guard(this);
    beginProgress("hand_order.experiment.simulating");
    if (!guard || sequence != guard->sequence_ || !guard->pending_) return false;
    emit changed();
    if (!guard || sequence != guard->sequence_ || !guard->pending_) return false;
    worker_.start([guard, analysis, parameters, identity, sequence, cancellation, parserPolicy] {
        hand_order::experiment::Simulation result;
        task::CancellationScope scope(cancellation);
        try { result = hand_order::experiment::simulate(analysis, parameters); }
        catch (const task::Cancelled&) { return; }
        catch (...) { result.error = QStringLiteral("simulation_failed"); }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, identity, sequence, cancellation, parserPolicy, result = std::move(result)]() mutable {
            if (!guard || cancellation->load(std::memory_order_relaxed) || sequence != guard->sequence_ || !guard->current(identity)) return;
            if (parserPolicy != SimaiParser::allowNegativeHsEnabled()) {
                guard->refreshParserPolicy(); return;
            }
            guard->endProgress();
            if (!guard || sequence != guard->sequence_ || !guard->current(identity)) return;
            if (SimaiParser::allowNegativeHsEnabled() != parserPolicy) {
                guard->refreshParserPolicy(); return;
            }
            guard->pending_ = false; guard->simulation_ = std::move(result);
            guard->error_ = guard->simulation_.error;
            guard->state_ = guard->simulation_.valid ? QStringLiteral("simulated") : QStringLiteral("failed");
            emit guard->changed();
        }, Qt::QueuedConnection);
    });
    return true;
}
QVariantMap HandOrderExperimentService::simulationParameters() const
{
    return {{"reactionMs", parameters_.reactionMs}, {"movementSpeed", parameters_.movementSpeed},
        {"stamina", parameters_.stamina}, {"judgementJitterMs", parameters_.judgementJitterMs},
        {"referenceWindowMs", parameters_.referenceWindowMs}, {"trials", parameters_.trials},
        {"seed", parameters_.seed}};
}
bool HandOrderExperimentService::setSimulationParameter(const QString& key, const QVariant& value)
{
    bool ok = false; const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || pending_) return false;
    auto candidate = parameters_;
    if (key == QLatin1String("reactionMs")) candidate.reactionMs = number;
    else if (key == QLatin1String("movementSpeed")) candidate.movementSpeed = number;
    else if (key == QLatin1String("stamina")) candidate.stamina = number;
    else if (key == QLatin1String("judgementJitterMs")) candidate.judgementJitterMs = number;
    else if (key == QLatin1String("referenceWindowMs")) candidate.referenceWindowMs = number;
    else if (key == QLatin1String("trials") && number == std::floor(number) && number >= 1 && number <= 200) candidate.trials = static_cast<int>(number);
    else if (key == QLatin1String("seed") && number == std::floor(number) && number >= 0 && number <= 4294967295.0) candidate.seed = static_cast<quint32>(number);
    else return false;
    if (!hand_order::experiment::validParameters(candidate)) return false;
    if (simulationParameters().value(key) == value) return true;
    parameters_ = candidate; simulation_ = {};
    state_ = hasAnalysis() ? QStringLiteral("ready") : QStringLiteral("idle");
    emit changed(); return true;
}
QVariantList HandOrderExperimentService::rows() const
{
    QVariantList result;
    if (!hasAnalysis()) return result;
    const bool simulationCurrent = simulation_.valid;
    for (int index = 0; index < analysis_.segments.size(); ++index) {
        const auto& s = analysis_.segments[index];
        QString simulation;
        if (simulationCurrent && index < simulation_.segments.size()) {
            const auto& row = simulation_.segments[index];
            simulation = qtTrId("hand_order.experiment.segment_simulation").arg(row.outsideWindow).arg(row.samples)
                .arg(row.meanAbsOffsetMs, 0, 'f', 1).arg(row.p95AbsOffsetMs, 0, 'f', 1);
        }
        result.append(QVariantMap{{"index", index}, {"start", s.startSecond}, {"end", s.endSecond},
            {"line", s.line}, {"column", s.column}, {"events", s.events}, {"unresolved", s.unresolved},
            {"contacts", s.contactCandidates}, {"peakSpeed", s.peakSpeed}, {"peakTapRate", s.peakTapRate},
            {"peakLoad", s.peakLoad}, {"candidateSupport", s.candidateSupport},
            {"reasons", reasonText(s.reasons)}, {"simulation", simulation}});
    }
    return result;
}
QVariantMap HandOrderExperimentService::navigationTarget(int row) const
{
    if (!hasAnalysis() || row < 0 || row >= analysis_.segments.size()) return {};
    const auto& segment = analysis_.segments[row];
    return {{"start", segment.sourceStart}, {"end", segment.sourceEnd}, {"difficulty", identity_.activeDifficultyId},
        {"revision", QVariant::fromValue(identity_.revision)}, {"generation", QVariant::fromValue(identity_.documentOpenGeneration)}};
}
QString HandOrderExperimentService::report() const
{
    if (!hasAnalysis()) return {};
    QJsonObject report{{"analysis", hand_order::experiment::toJson(analysis_)},
        {"document_generation", static_cast<qint64>(identity_.documentOpenGeneration)},
        {"revision", static_cast<qint64>(identity_.revision)}, {"difficulty", identity_.activeDifficultyId},
        {"document_sha256", QString::fromLatin1(QCryptographicHash::hash(identity_.sourceText.toUtf8(), QCryptographicHash::Sha256).toHex())}};
    if (hasSimulation()) report.insert("simulation", hand_order::experiment::toJson(simulation_));
    return QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented));
}
} // namespace miacode
