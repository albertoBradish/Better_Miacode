#include "app/ui/preview/ChartStructureModel.h"

#include "app/services/ChartFileSafety.h"
#include "app/services/ChartWorkspaceFileService.h"
#include "audio/bass/OfflineAudioDecoder.h"
#include "common/RuntimePaths.h"
#include "common/TaskCancellation.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "timeline/TimelineQuickModel.h"

#include <QDataStream>
#include <QDateTime>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace miacode::ui {
namespace {

using namespace chart_structure;
using namespace chart_preview;

constexpr qsizetype kMaxCacheFrames = 16000;
constexpr qint64 kMaxAudioBytes = 128 * 1024 * 1024;
constexpr double kMaxAudioSeconds = 15.0 * 60.0;

QString hash(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString audioHash(const QString& path, const std::atomic_bool& cancelled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaxAudioBytes) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) return {};
        digest.addData(chunk);
    }
    return QString::fromLatin1(digest.result().toHex());
}

QDataStream& operator<<(QDataStream& stream, const AudioFrame& frame)
{
    return stream << frame.energy << frame.flux << frame.lowBand << frame.highBand;
}

QDataStream& operator>>(QDataStream& stream, AudioFrame& frame)
{
    return stream >> frame.energy >> frame.flux >> frame.lowBand >> frame.highBand;
}

QDataStream& operator<<(QDataStream& stream, const PressureFrame& frame)
{
    return stream << frame.density << frame.simultaneous << frame.held << frame.slides
        << frame.movement << frame.rhythm << frame.hybrid << frame.endurance << frame.strength;
}

QDataStream& operator>>(QDataStream& stream, PressureFrame& frame)
{
    return stream >> frame.density >> frame.simultaneous >> frame.held >> frame.slides
        >> frame.movement >> frame.rhythm >> frame.hybrid >> frame.endurance >> frame.strength;
}

template<typename Curve>
bool readCache(const QString& path, Curve& result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 version = 0;
    qint32 count = 0;
    Curve curve;
    stream >> version >> count >> curve.durationSeconds >> curve.secondsPerFrame;
    if (version != kDropAlgorithmVersion || count <= 0 || count > kMaxCacheFrames
        || !std::isfinite(curve.durationSeconds) || curve.durationSeconds <= 0.0
        || !std::isfinite(curve.secondsPerFrame) || curve.secondsPerFrame <= 0.0
        || std::ceil(curve.durationSeconds / curve.secondsPerFrame) != count) return false;
    curve.frames.resize(count);
    for (auto& frame : curve.frames) stream >> frame;
    if (stream.status() != QDataStream::Ok || !file.atEnd()) return false;
    result = std::move(curve);
    return true;
}

template<typename Curve>
void writeCache(const QString& path, const Curve& curve)
{
    if (curve.frames.isEmpty() || curve.frames.size() > kMaxCacheFrames) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << static_cast<quint32>(kDropAlgorithmVersion) << static_cast<qint32>(curve.frames.size())
        << curve.durationSeconds << curve.secondsPerFrame;
    for (const auto& frame : curve.frames) stream << frame;
    if (stream.status() == QDataStream::Ok) file.commit();
}

QString cachePath(const QString& root, const char* kind, const QString& key)
{
    if (root.isEmpty()) return {};
    return QDir(root).filePath(QStringLiteral("%1-v%2-%3.bin")
        .arg(QString::fromLatin1(kind)).arg(kDropAlgorithmVersion).arg(key));
}

} // namespace

ChartStructureModel::ChartStructureModel(ChartWorkspace& workspace, ChartWorkspaceFileService& files, QObject* parent)
    : QObject(parent), workspace_(workspace), files_(files)
{
    pool_.setMaxThreadCount(1);
    pool_.setThreadPriority(QThread::LowPriority);
    debounce_.setSingleShot(true);
    debounce_.setInterval(450);
    connect(&debounce_, &QTimer::timeout, this, &ChartStructureModel::run);
    connect(&workspace_, &ChartWorkspace::changed, this, [this] { if (!applying_) refresh(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { refresh(true); });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { refresh(); });
    refresh();
}

ChartStructureModel::~ChartStructureModel()
{
    stopCurrentTask();
    pool_.waitForDone();
}

bool ChartStructureModel::Input::hasSameContent(const Input& other) const
{
    return generation == other.generation && difficulty == other.difficulty
        && chartPath == other.chartPath && chart == other.chart && timing == other.timing && first == other.first
        && allowNegativeHs == other.allowNegativeHs
        && audioPath == other.audioPath && audioSize == other.audioSize && audioModified == other.audioModified;
}

void ChartStructureModel::refresh(bool audioChanged)
{
    const auto snapshot = workspace_.snapshot();
    const auto& document = workspace_.document();
    Input next;
    next.generation = snapshot.documentOpenGeneration;
    next.revision = snapshot.revision;
    next.difficulty = snapshot.activeDifficultyId;
    if (const auto* difficulty = document.difficulty(next.difficulty)) next.chart = difficulty->chart;
    next.chartPath = snapshot.filePath;
    next.timing = simai::buildTimingMetadata(document);
    next.first = timeline::offset::parsedFirstSeconds(document.first);
    next.allowNegativeHs = SimaiParser::allowNegativeHsEnabled();
    next.audioPath = chart_assets::resolveTrackPath(next.chartPath);
    if (!next.audioPath.isEmpty()) {
        const QFileInfo audio(next.audioPath);
        next.audioSize = audio.size();
        next.audioModified = audio.lastModified().toMSecsSinceEpoch();
    }
    if (input_.generation != next.generation) {
        edits_.clear();
        attemptedAutoFingerprint_.clear();
    }
    if (!input_.hasSameContent(next) || audioChanged) {
        stopCurrentTask();
        result_.reset();
        editorDropTimeline_.reset();
        cancelled_ = false;
        next.chartFingerprint = dropChartFingerprint(document, next.difficulty);
        input_ = std::move(next);
        updateWatches();
        schedule();
    } else {
        input_.revision = next.revision;
    }
    emit changed();
}

void ChartStructureModel::updateWatches()
{
    const QString directory = input_.chartPath.isEmpty() ? QString() : QFileInfo(input_.chartPath).absolutePath();
    const QStringList files = watcher_.files();
    const QStringList directories = watcher_.directories();
    for (const QString& path : files) if (path != input_.audioPath) watcher_.removePath(path);
    for (const QString& path : directories) if (path != directory) watcher_.removePath(path);
    if (!directory.isEmpty() && !directories.contains(directory)) watcher_.addPath(directory);
    if (!input_.audioPath.isEmpty() && !files.contains(input_.audioPath)) watcher_.addPath(input_.audioPath);
}

void ChartStructureModel::schedule()
{
    if (input_.difficulty == 0 || input_.chart.trimmed().isEmpty() || cancelled_ || result_ || playing_) return;
    if (!cancellation_) debounce_.start(lowPerformanceMode_ ? 1200 : 450);
}

void ChartStructureModel::run()
{
    if (input_.difficulty == 0 || cancelled_ || playing_ || result_ || cancellation_) return;
    QString cache = QDir(runtime_paths::directory("cache")).filePath(QStringLiteral("chart-structure"));
    if (QFileInfo(cache).isSymLink() || !QDir().mkpath(cache)
        || QFileInfo(cache).canonicalFilePath().compare(cache, Qt::CaseInsensitive) != 0) cache.clear();
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const quint64 sequence = ++sequence_;
    pool_.start([this, input = input_, sequence, cache, cancellation = cancellation_] {
        task::CancellationScope scope(cancellation);
        Result result;
        try {
            result = calculate(input, cache, *cancellation);
        } catch (const task::Cancelled&) {
            return;
        }
        if (cancellation->load(std::memory_order_relaxed)) return;
        QMetaObject::invokeMethod(this, [this, input, sequence, result = std::move(result)]() mutable {
            acceptResult(input, sequence, std::move(result));
        }, Qt::QueuedConnection);
    });
    emit changed();
}

void ChartStructureModel::stopCurrentTask()
{
    debounce_.stop();
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    cancellation_.reset();
    pool_.clear();
    ++sequence_;
}

ChartStructureModel::Result ChartStructureModel::calculate(
    const Input& input, const QString& cacheRoot, const std::atomic_bool& cancelled)
{
    Result result;
    AudioFeatures audio;
    result.audioHash = audioHash(input.audioPath, cancelled);
    if (!result.audioHash.isEmpty() && !cancelled.load(std::memory_order_relaxed)) {
        const QString cache = cachePath(cacheRoot, "audio", result.audioHash);
        if (!readCache(cache, audio)) {
            const auto decoded = audio_decode::decodeFileToMono(input.audioPath, 8000, kMaxAudioSeconds, &cancelled);
            audio = analyzeAudio(decoded.samples, decoded.sampleRate, &cancelled);
            if (cancelled.load(std::memory_order_relaxed)) return {};
            if (audioHash(input.audioPath, cancelled) != result.audioHash) {
                result.staleAudio = true;
                return result;
            }
            if (!cancelled.load(std::memory_order_relaxed)) writeCache(cache, audio);
        }
    }
    if (cancelled.load(std::memory_order_relaxed)) return {};
    result.audioAvailable = !audio.frames.isEmpty();
    result.audioDuration = audio.durationSeconds;
    const QString pressureKey = hash((input.chartFingerprint + '|' + QString::number(result.audioDuration, 'g', 17)).toUtf8());
    const QString cache = cachePath(cacheRoot, "pressure", pressureKey);
    result.validChart = readCache(cache, result.pressure);
    if (!result.validChart) {
        const auto parsed = SimaiParser::parseForTimeline(input.chart, input.timing);
        result.validChart = parsed.ok;
        result.pressure = estimatePressure(parsed, input.first, result.audioDuration);
        if (result.validChart && !cancelled.load(std::memory_order_relaxed)) writeCache(cache, result.pressure);
    }
    if (result.audioAvailable && result.validChart) {
        result.fingerprint = combineDropFingerprints(input.chartFingerprint, result.audioHash);
        result.candidates = recommendDrops(audio, result.pressure);
    }
    return result;
}

void ChartStructureModel::acceptResult(const Input& input, quint64 sequence, Result result)
{
    if (sequence != sequence_) return;
    cancellation_.reset();
    const auto snapshot = workspace_.snapshot();
    if (!input_.hasSameContent(input) || input_.revision != snapshot.revision
        || input.generation != snapshot.documentOpenGeneration || input.difficulty != snapshot.activeDifficultyId
        || input.allowNegativeHs != SimaiParser::allowNegativeHsEnabled()) {
        refresh();
        schedule();
        return;
    }
    const QFileInfo audio(input.audioPath);
    if (result.staleAudio || (!input.audioPath.isEmpty() && (!audio.exists() || audio.size() != input.audioSize
        || audio.lastModified().toMSecsSinceEpoch() != input.audioModified))) {
        refresh(true);
        return;
    }
    result_ = std::move(result);
    const auto existing = currentSelection();
    auto& edit = edits_[input_.difficulty];
    const bool manual = (existing && existing->source == DropSource::Manual)
        || (!edit.hasOverride && hasManualDropMarker(workspace_.document(), input_.difficulty));
    if (!manual && !edit.suppressAuto && !readDropSelection(workspace_.document(), input_.difficulty,
        inputFingerprint(), mediaDuration()) && !result_->candidates.isEmpty() && result_->candidates.first().confident) {
        const Selection selected = DropSelection{result_->candidates.first().range, DropSource::Auto, inputFingerprint()};
        if (attemptedAutoFingerprint_ != inputFingerprint()) {
            attemptedAutoFingerprint_ = inputFingerprint();
            persist(selected);
        }
    }
    emit changed();
}

bool ChartStructureModel::available() const { return input_.difficulty > 0 && !input_.chart.trimmed().isEmpty(); }
bool ChartStructureModel::pressureAvailable() const { return result_ && !result_->pressure.frames.isEmpty(); }
bool ChartStructureModel::pending() const { return debounce_.isActive() || cancellation_ != nullptr; }
bool ChartStructureModel::dropAvailable() const { return effectiveDrop().has_value(); }
double ChartStructureModel::dropStartSeconds() const { const auto drop = effectiveDrop(); return drop ? drop->range.startSeconds : 0.0; }
double ChartStructureModel::dropEndSeconds() const { const auto drop = effectiveDrop(); return drop ? drop->range.endSeconds : 0.0; }

double ChartStructureModel::mediaDuration() const
{
    if (result_ && result_->audioDuration > 0.0) return result_->audioDuration;
    return result_ ? std::max(playbackDuration_, result_->pressure.durationSeconds) : playbackDuration_;
}

QString ChartStructureModel::inputFingerprint() const { return result_ ? result_->fingerprint : QString(); }

QString ChartStructureModel::audioFingerprint() const { return result_ ? result_->audioHash : QString(); }

QString ChartStructureModel::inputFingerprintForDifficulty(int difficultyId) const
{
    if (difficultyId == input_.difficulty) return inputFingerprint();
    return dropInputFingerprint(workspace_.document(), difficultyId, audioFingerprint());
}

ChartStructureModel::Selection ChartStructureModel::currentSelection() const
{
    const auto state = edits_.constFind(input_.difficulty);
    if (state != edits_.cend() && state->hasOverride) return state->overrideValue;
    return storedDropSelection(workspace_.document(), input_.difficulty);
}

ChartStructureModel::Selection ChartStructureModel::effectiveDrop() const
{
    auto selection = currentSelection();
    if (!selection || (mediaDuration() > 0.0 && selection->range.endSeconds > mediaDuration() + 0.001)) return {};
    if (selection->source == DropSource::Auto && selection->inputFingerprint != inputFingerprint()) return {};
    return selection;
}

ChartStructureModel::Selection ChartStructureModel::effectiveDropForDifficulty(int difficultyId) const
{
    if (difficultyId == input_.difficulty) return effectiveDrop();
    const auto state = edits_.constFind(difficultyId);
    if (state != edits_.cend() && state->hasOverride) {
        const auto selection = state->overrideValue;
        if (!selection || (mediaDuration() > 0.0 && selection->range.endSeconds > mediaDuration() + 0.001)) return {};
        if (selection->source == DropSource::Auto && selection->inputFingerprint != inputFingerprintForDifficulty(difficultyId)) return {};
        return selection;
    }
    return readDropSelection(workspace_.document(), difficultyId, inputFingerprintForDifficulty(difficultyId), mediaDuration());
}

QVariantList ChartStructureModel::candidates() const
{
    QVariantList values;
    if (!result_) return values;
    for (const auto& candidate : result_->candidates) {
        values.append(QVariantMap{{QStringLiteral("start"), candidate.range.startSeconds},
            {QStringLiteral("end"), candidate.range.endSeconds}, {QStringLiteral("confident"), candidate.confident}});
    }
    return values;
}

bool ChartStructureModel::canUndo() const
{
    const auto it = edits_.constFind(input_.difficulty);
    return it != edits_.cend() && !it->undo.isEmpty();
}

bool ChartStructureModel::canRedo() const
{
    const auto it = edits_.constFind(input_.difficulty);
    return it != edits_.cend() && !it->redo.isEmpty();
}

bool ChartStructureModel::unsaved() const
{
    const auto it = edits_.constFind(input_.difficulty);
    return it != edits_.cend() && it->unsaved;
}

QString ChartStructureModel::status() const
{
    const auto selection = currentSelection();
    if (unsaved()) return qtTrId("structure.session_only");
    if (!selection && hasManualDropMarker(workspace_.document(), input_.difficulty)) return qtTrId("structure.manual_outside");
    if (selection && selection->source == DropSource::Manual) {
        if (!effectiveDrop()) return qtTrId("structure.manual_outside");
        if (result_ && !selection->inputFingerprint.isEmpty() && selection->inputFingerprint != result_->audioHash)
            return qtTrId("structure.manual_audio_changed");
        return qtTrId("structure.manual");
    }
    if (effectiveDrop()) return qtTrId("structure.detected");
    if (pending()) return qtTrId("structure.analyzing");
    if (cancelled_) return qtTrId("structure.canceled");
    if (!result_) return qtTrId("structure.wait_for_pause");
    if (!result_->validChart) return qtTrId("structure.syntax_estimate");
    if (!result_->audioAvailable) return qtTrId("structure.no_audio");
    if (!result_->candidates.isEmpty()) return qtTrId("structure.candidates");
    return qtTrId("structure.no_clear_drop");
}

void ChartStructureModel::setPlaying(bool playing)
{
    if (playing_ == playing) return;
    playing_ = playing;
    if (playing_) stopCurrentTask();
    else schedule();
    emit changed();
}

void ChartStructureModel::setLowPerformanceMode(bool enabled)
{
    if (lowPerformanceMode_ == enabled) return;
    lowPerformanceMode_ = enabled;
    if (debounce_.isActive()) schedule();
}

void ChartStructureModel::setPlaybackDuration(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds == playbackDuration_) return;
    playbackDuration_ = seconds;
    emit changed();
}

QVariantList ChartStructureModel::heatmapSamples(double start, double end, int columns) const
{
    QVariantList values;
    if (!result_ || result_->pressure.frames.isEmpty() || !std::isfinite(start) || !std::isfinite(end)
        || end <= start || columns <= 0) return values;
    columns = std::min(columns, 4096);
    const auto& curve = result_->pressure;
    const double step = (end - start) / columns;
    for (int i = 0; i < columns; ++i) {
        const double left = start + i * step;
        const double right = left + step;
        const double first = std::clamp(std::floor(left / curve.secondsPerFrame), 0.0, static_cast<double>(curve.frames.size()));
        const double last = std::clamp(std::ceil(right / curve.secondsPerFrame), first, static_cast<double>(curve.frames.size()));
        double peak = 0.0;
        for (auto frame = static_cast<qsizetype>(first); frame < static_cast<qsizetype>(last); ++frame)
            peak = std::max(peak, curve.frames.at(frame).strength);
        values.append(peak);
    }
    return values;
}

QVariantMap ChartStructureModel::pressureAt(double seconds) const
{
    if (!result_ || result_->pressure.frames.isEmpty() || !std::isfinite(seconds)
        || seconds < 0.0 || seconds >= result_->pressure.durationSeconds) return {};
    const auto& curve = result_->pressure;
    const auto index = std::min(curve.frames.size() - 1, static_cast<qsizetype>(seconds / curve.secondsPerFrame));
    const auto& frame = curve.frames.at(index);
    return {{QStringLiteral("density"), frame.density}, {QStringLiteral("simultaneous"), frame.simultaneous},
        {QStringLiteral("held"), frame.held}, {QStringLiteral("slides"), frame.slides},
        {QStringLiteral("movement"), frame.movement}, {QStringLiteral("rhythm"), frame.rhythm},
        {QStringLiteral("hybrid"), frame.hybrid}, {QStringLiteral("endurance"), frame.endurance},
        {QStringLiteral("strength"), frame.strength}};
}

bool ChartStructureModel::persist(Selection selection, bool explicitSave)
{
    auto& state = edits_[input_.difficulty];
    const auto snapshot = workspace_.snapshot();
    const auto original = workspace_.document();
    auto document = original;
    if (!writeDropSelection(document, input_.difficulty, selection)) {
        state.hasOverride = false;
        state.unsaved = false;
        return true;
    }
    state.overrideValue = selection;
    state.hasOverride = true;
    state.unsaved = true;
    const bool unsavedChart = selection && selection->source == DropSource::Auto
        && snapshot.dirtyDifficultyIds.contains(input_.difficulty);
    const bool preserveOtherTextFormat = !explicitSave
        && QFileInfo(snapshot.filePath).fileName().compare(QStringLiteral("maidata.txt"), Qt::CaseInsensitive) != 0;
    if (snapshot.origin != ChartDocumentOrigin::Local || snapshot.filePath.isEmpty() || workspace_.metadataDirty() || unsavedChart || preserveOtherTextFormat
        || !QFileInfo(snapshot.filePath).isWritable() || !chart_file_safety::backupOriginal(snapshot.filePath)) return false;
    if (selection) seedDemoWindow(document, selection->range, mediaDuration());
    const QScopedValueRollback guard(applying_, true);
    const auto change = workspace_.replaceExtraFields(document.extraFields);
    if (!change.accepted) return false;
    const auto saved = files_.save(ChartWorkspace::MetadataSection);
    if (!saved.accepted) workspace_.replaceExtraFields(original.extraFields);
    input_.revision = workspace_.snapshot().revision;
    state.hasOverride = !saved.accepted;
    state.unsaved = !saved.accepted;
    return saved.accepted;
}

bool ChartStructureModel::applyEdit(Selection selection)
{
    if (!available()) return false;
    auto& state = edits_[input_.difficulty];
    const auto before = currentSelection();
    if (before != selection) {
        state.undo.append({before, selection});
        if (state.undo.size() > 32) state.undo.removeFirst();
        state.redo.clear();
    }
    restoreEdit(selection);
    return true;
}

void ChartStructureModel::restoreEdit(Selection selection)
{
    edits_[input_.difficulty].suppressAuto = !selection || selection->source == DropSource::Manual;
    persist(selection, true);
    emit changed();
}

bool ChartStructureModel::setManualDrop(double start, double end)
{
    const MediaRange range{start, end};
    if (!range.isValid() || (mediaDuration() > 0.0 && end > mediaDuration() + 0.001)) return false;
    return applyEdit(DropSelection{range, DropSource::Manual, result_ ? result_->audioHash : QString()});
}

std::optional<chart_preview::MediaRange> ChartStructureModel::dropRangeForEditorSelection(
    int difficulty, quint64 revision, quint64 generation, const QString& text, int start, int end)
{
    if (!available() || start == end || difficulty != input_.difficulty || revision != input_.revision
        || generation != input_.generation || text != input_.chart) return std::nullopt;
    bool firstValid = false;
    timeline::offset::parsedFirstSeconds(workspace_.document().first, &firstValid);
    if (!firstValid) return std::nullopt;

    if (!editorDropTimeline_) {
        editorDropTimeline_ = std::make_unique<TimelineQuickModel>();
        editorDropTimeline_->rebuildFromText(input_.chart, input_.first, input_.timing);
    }
    return editorDropTimeline_->resolveMediaRangeForSelection(start, end, mediaDuration());
}

bool ChartStructureModel::canSetDropFromEditorSelection(
    int difficulty, qulonglong revision, qulonglong generation, const QString& text, int start, int end)
{
    return dropRangeForEditorSelection(difficulty, revision, generation, text, start, end).has_value();
}

bool ChartStructureModel::setDropFromEditorSelection(
    int difficulty, qulonglong revision, qulonglong generation, const QString& text, int start, int end)
{
    const auto range = dropRangeForEditorSelection(difficulty, revision, generation, text, start, end);
    return range && setManualDrop(range->startSeconds, range->endSeconds);
}

bool ChartStructureModel::clearDrop() { return applyEdit({}); }

bool ChartStructureModel::saveSessionDrop()
{
    if (!unsaved()) return false;
    const auto selection = currentSelection();
    if (selection && !effectiveDrop()) return false;
    const bool saved = persist(selection, true);
    emit changed();
    return saved;
}

void ChartStructureModel::undoDrop()
{
    if (!canUndo()) return;
    auto& state = edits_[input_.difficulty];
    const auto edit = state.undo.takeLast();
    state.redo.append(edit);
    restoreEdit(edit.before);
}

void ChartStructureModel::redoDrop()
{
    if (!canRedo()) return;
    auto& state = edits_[input_.difficulty];
    const auto edit = state.redo.takeLast();
    state.undo.append(edit);
    restoreEdit(edit.after);
}

void ChartStructureModel::startAnalysis()
{
    cancelled_ = false;
    result_.reset();
    auto& state = edits_[input_.difficulty];
    state.suppressAuto = false;
    attemptedAutoFingerprint_.clear();
    stopCurrentTask();
    schedule();
    emit changed();
}

void ChartStructureModel::cancelAnalysis()
{
    stopCurrentTask();
    cancelled_ = true;
    emit changed();
}

} // namespace miacode::ui
