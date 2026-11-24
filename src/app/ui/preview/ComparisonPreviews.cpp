#include "app/ui/preview/ComparisonPreviews.h"
#include "app/ui/preview/PreviewModel.h"
#include "app/services/PreferenceDocument.h"
#include "common/FileContentStamp.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/ChartVersion.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/document/ChartClockCount.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/analysis/MuriAnalyzer.h"
#include "core/scene/PreviewSfxAssets.h"
#include "core/video/AssetPaths.h"

#include <QtConcurrentRun>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <QUuid>

namespace miacode::ui {
namespace {
constexpr auto syncKey = "synchronize_same_song_previews";
constexpr int previewRole = Qt::UserRole + 1;

QString mediaKey(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char(':'))) return path;
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return canonical.isEmpty() ? file.absoluteFilePath() : canonical;
}
}

ComparisonPreviews::ComparisonPreviews(ChartWorkspace& workspace, PreviewModel& main, QObject* parent)
    : QAbstractListModel(parent), workspace_(workspace), main_(main)
{
    const auto ui = PreferenceDocument::loadPreferencesObject().value("ui").toObject();
    synchronize_ = ui.value(syncKey).toBool(true);
    mixAudio_ = ui.value("mix_preview_audio").toBool(false);
    tick_.setInterval(16);
    tick_.setTimerType(Qt::PreciseTimer);
    connect(&tick_, &QTimer::timeout, this, [this] { updateClocks(); });
    connect(&workspace_, &ChartWorkspace::changed, this, [this] { if (!items_.isEmpty()) refreshDocument(); });
    connect(&main_, &PreviewModel::canvasSettingsChanged, this, [this] { if (!items_.isEmpty()) refreshMediaPreference(); });
    const auto follow = [this] { if (!items_.isEmpty()) updateClocks(); };
    connect(&main_, &PreviewModel::playingChanged, this, follow);
    connect(&main_, &PreviewModel::positionChanged, this, follow);
    connect(&main_, &PreviewModel::transportChanged, this, follow);
}

ComparisonPreviews::~ComparisonPreviews()
{
    tick_.stop();
    for (auto* item : items_) {
        if (item->audio_) item->audio_->setOutputRoute(0, 0);
    }
    if (!items_.isEmpty()) {
        qDeleteAll(items_);
        items_.clear();
        main_.routeAudio(1, 0);
    }
}

QVariantList ComparisonPreviews::panels() const
{
    QVariantList result;
    for (auto* item : items_) result.append(QVariant::fromValue(static_cast<QObject*>(item)));
    return result;
}

QVariant ComparisonPreviews::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count() || role != previewRole) return {};
    return QVariant::fromValue(static_cast<QObject*>(items_.at(index.row())));
}

QHash<int,QByteArray> ComparisonPreviews::roleNames() const { return {{previewRole, "comparison"}}; }

void ComparisonPreviews::setPresentationActive(bool value)
{
    if (value == presentationActive_) return;
    presentationActive_ = value;
    if (value) {
        for (auto* item : items_) item->inheritAppearance();
        refreshMediaRoutes();
        updateClocks();
    } else {
        tick_.stop();
        for (auto* item : items_) {
            item->setLocalClock(item->position_, false);
            item->present(item->position_, item->duration(), false, item->rate_, false);
        }
        refreshMediaRoutes();
    }
    updateAudioRoutes();
    emit presentationActiveChanged();
}

void ComparisonPreviews::setMixAudio(bool value)
{
    if (mixAudio_ == value) return;
    mixAudio_ = value;
    auto prefs = PreferenceDocument::loadPreferencesObject();
    auto ui = prefs.value("ui").toObject();
    ui.insert("mix_preview_audio", value);
    prefs.insert("ui", ui);
    PreferenceDocument::savePreferencesObject(prefs);
    updateAudioRoutes();
    emit mixAudioChanged();
}

void ComparisonPreviews::focusMainAudio()
{
    if (!focused_) return;
    focused_ = nullptr;
    updateAudioRoutes();
}

void ComparisonPreviews::setMainPan(double pan)
{
    if (!std::isfinite(pan) || mainPan_ == pan) return;
    mainPan_ = pan;
    if (mixAudio_ && presentationActive_ && !items_.isEmpty()) main_.routeAudio(0.90 / (count() + 1), pan);
}

void ComparisonPreviews::updateAudioRoutes()
{
    const bool multi = presentationActive_ && !items_.isEmpty();
    const bool mix = multi && mixAudio_;
    const double gain = mix ? 0.90 / (items_.size() + 1) : 1.0;
    // Set all output gains before allowing a newly audible renderer to start.
    for (auto* item : items_) {
        if (item->audio_) item->audio_->setOutputRoute(multi && (mix || focused_ == item) ? gain : 0, mix ? item->pan_ : 0);
    }
    main_.routeAudio(!multi || mix || !focused_ ? gain : 0, mix ? mainPan_ : 0);
    updateClocks();
}

void ComparisonPreview::focusAudio(bool enter)
{
    if (enter && owner_.focused_ != this) { owner_.focused_ = this; owner_.updateAudioRoutes(); }
    else if (!enter && owner_.focused_ == this) owner_.focusMainAudio();
}

void ComparisonPreview::setScreenPan(double pan)
{
    if (!std::isfinite(pan) || pan_ == pan) return;
    pan_ = pan;
    if (audio_ && owner_.mixAudio_ && owner_.presentationActive_) audio_->setOutputRoute(0.90 / (owner_.count() + 1), pan_);
}

void ComparisonPreview::setSyncEnabled(bool value)
{
    if (syncEnabled_ == value) return;
    syncEnabled_ = value;
    owner_.regroup();
    emit transportChanged();
}

void ComparisonPreviews::setSynchronize(bool value)
{
    if (synchronize_ == value) return;
    synchronize_ = value;
    auto prefs = PreferenceDocument::loadPreferencesObject();
    auto ui = prefs.value("ui").toObject();
    ui.insert(syncKey, value);
    prefs.insert("ui", ui);
    PreferenceDocument::savePreferencesObject(prefs);
    regroup();
    emit synchronizeChanged();
}

PreviewSong ComparisonPreviews::identify(const ChartWorkspaceSnapshot& snapshot)
{
    PreviewSong song;
    if (!snapshot.hasDocument) return song;
    song.generation = workspace_.songIdentity();
    if (snapshot.filePath.isEmpty()) return song;
    QFileInfo file(snapshot.filePath);
    song.chartPath = file.canonicalFilePath();
    if (song.chartPath.isEmpty()) song.chartPath = file.absoluteFilePath();
    song.trackPath = QFileInfo(chart_assets::resolveTrackPath(song.chartPath)).canonicalFilePath();
    if (song.trackPath.isEmpty()) return song;
    const QString stamp = fs::fileContentStamp(song.trackPath);
    song.trackKey = song.trackPath + QChar(0) + stamp;
    if (!digests_.contains(song.trackKey)) {
        // Managed libraries can hold byte-identical tracks in different folders.
        // Hash off the UI thread and ignore a file that changed while being read.
        digests_.insert(song.trackKey, {});
        auto* watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, key = song.trackKey] {
            digests_.insert(key, watcher->result());
            watcher->deleteLater();
            regroup();
        });
        watcher->setFuture(QtConcurrent::run([path = song.trackPath, stamp] {
            QFile input(path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!input.open(QIODevice::ReadOnly) || !hash.addData(&input) || fs::fileContentStamp(path) != stamp)
                return QByteArray{};
            return hash.result();
        }));
    }
    return song;
}

bool ComparisonPreviews::sameSong(const PreviewSong& a, const PreviewSong& b) const
{
    if (!a.trackKey.isEmpty() && !b.trackKey.isEmpty()) {
        if (a.trackKey == b.trackKey) return true;
        const auto digest = digests_.value(a.trackKey);
        return !digest.isEmpty() && digest == digests_.value(b.trackKey);
    }
    return (a.generation != 0 && a.generation == b.generation)
        || (!a.chartPath.isEmpty() && a.chartPath == b.chartPath);
}

void ComparisonPreviews::addCurrent()
{
    const auto snapshot = workspace_.snapshot();
    if (!snapshot.hasDocument || !workspace_.document().difficulty(snapshot.activeDifficultyId)) return;
    if (!mainScene_) {
        mainScene_ = qobject_cast<PreviewRuntime*>(main_.runtime());
        if (!mainScene_) return;
        mainMedia_ = qobject_cast<PreviewStageMediaHost*>(main_.mediaHost());
        if (mainMedia_) {
            connect(mainMedia_, &PreviewStageMediaHost::chartMediaSourceChanged, this, [this] {
                if (!mainMedia_->chartPath().isEmpty()) mediaReloadPending_ = false;
                queueMediaRouteRefresh();
            });
            connect(mainMedia_, &PreviewStageMediaHost::decoderReleaseRequested, this, [this] {
                mediaReloadPending_ = true;
                refreshMediaRoutes();
            });
            connect(mainMedia_, &PreviewStageMediaHost::hidePvChanged,
                this, &ComparisonPreviews::queueMediaRouteRefresh);
            connect(mainMedia_, &PreviewStageMediaHost::decodeSettingsChanged,
                this, &ComparisonPreviews::queueMediaRouteRefresh);
        }
        connect(mainScene_, &PreviewRuntime::frameStateChanged, this, [this] {
            if (!presentationActive_) return;
            for (auto* item : items_) item->inheritAppearance();
            const auto policy = mainScene_->frameState().render.backgroundPolicy;
            if (policy != backgroundPolicy_) {
                backgroundPolicy_ = policy;
                refreshMediaRoutes();
            }
            if (!items_.isEmpty()) updateClocks();
        });
    }
    if (items_.isEmpty()) refreshMediaPreference();
    current_ = identify(snapshot);
    mainAvailable_ = true;
    QSet<int> usedWorkspaceNumbers;
    for (const auto* item : items_) usedWorkspaceNumbers.insert(item->workspaceNumber_);
    int workspaceNumber = 0;
    while (usedWorkspaceNumbers.contains(workspaceNumber)) ++workspaceNumber;
    auto* item = new ComparisonPreview(*this, snapshot.activeDifficultyId, workspaceNumber);
    item->song_ = current_;
    item->trackDuration_ = main_.durationSeconds();
    item->position_ = main_.positionSeconds();
    item->setLocalClock(item->position_, false);
    item->readChart(workspace_.document());
    item->inheritAppearance();
    item->scene_.setPlayheadSeconds(item->position_);
    beginInsertRows({}, count(), count());
    items_.append(item);
    endInsertRows();
    regroup();
    updateAudioRoutes();
    emit countChanged();
    emit previewAdded();
}

void ComparisonPreviews::refreshMediaPreference()
{
    const auto settings = main_.canvasSettings();
    showMediaWhenPaused_ = settings.value(QStringLiteral("showStageMediaWhenPaused"), true).toBool();
    forceJudgeAreaWhenPaused_ = settings.value(QStringLiteral("forceLabeledJudgeLineWhenPaused")).toBool();
    automaticOutline_ = settings.value(QStringLiteral("outlineAutomatic")).toBool();
    outlineVariant_ = static_cast<PreviewOutlineVariant>(settings.value(QStringLiteral("outlineVariant"),
        static_cast<int>(PreviewOutlineVariant::Line)).toInt());
    for (auto* item : items_) item->inheritAppearance();
    refreshMediaRoutes();
    updateClocks();
}

void ComparisonPreviews::refreshMediaRoutes()
{
    if (!mainScene_ || items_.isEmpty()) return;
    const auto& render = mainScene_->frameState().render;
    backgroundPolicy_ = render.backgroundPolicy;
    const bool useMedia = presentationActive_ && !mediaReloadPending_ && usesBackgroundMedia(backgroundPolicy_)
        && std::any_of(items_.cbegin(), items_.cend(), [](const ComparisonPreview* item) { return item->surfaceVisible_; });
    const bool includePv = !mainMedia_ || !mainMedia_->hidePv();
    const QString primaryKey = mainMedia_ ? mediaKey(mainMedia_->resolvedMediaPath()) : QString();
    const QString primaryChart = mainMedia_ ? mediaKey(mainMedia_->chartPath()) : QString();
    QHash<ComparisonPreview*, QHash<QString, PreviewStageMediaHost*>> clockSources;
    QHash<QPair<QString, QString>, QString> chartSources;
    QSet<PreviewStageMediaHost*> activeSources;
    QList<PreviewStageMediaHost*> routes;
    for (auto* item : items_) {
        PreviewStageMediaHost* source = nullptr;
        if (useMedia) {
            const auto chartSource = qMakePair(item->song_.chartPath, item->videoOverridePath_);
            if (!chartSources.contains(chartSource)) {
                chartSources.insert(chartSource, mediaKey(PreviewStageMediaHost::resolveChartMediaPath(
                    chartSource.first, chartSource.second, includePv)));
            }
            const QString key = chartSources.value(chartSource);
            if (!item->leader_ && mainMedia_
                && ((!key.isEmpty() && key == primaryKey)
                    || (key.isEmpty() && !primaryChart.isEmpty() && primaryChart == item->song_.chartPath))) {
                source = mainMedia_;
            } else if (!key.isEmpty()) {
                // The clock identity prevents independent seeks from moving other panes.
                auto& first = clockSources[item->leader_][key];
                if (!first) first = &item->media_;
                source = first;
            }
        }
        routes.append(source);
        if (source && item->surfaceVisible_) activeSources.insert(source);
    }
    for (qsizetype i = 0; i < items_.size(); ++i) {
        auto* source = routes.at(i);
        items_.at(i)->setMediaSource(activeSources.contains(source) ? source : nullptr);
    }
    for (auto* item : items_) {
        if (item->mediaSource_ != &item->media_) item->releaseOwnedMedia();
    }
    for (auto* item : items_) {
        if (item->mediaSource_ != &item->media_) continue;
        if (mainMedia_) {
            item->media_.setVideoDecodePreference(mainMedia_->videoDecodePrefersSoftware());
            item->media_.setPreviewFrameRateLimit(mainMedia_->previewFrameRateLimit());
        }
        item->media_.setHidePv(!includePv);
        item->media_.setBackgroundScaleMode(render.backgroundScaleMode);
        item->media_.setLayoutSquareScale(render.layoutSquareScale);
        item->media_.setChartPath(item->song_.chartPath, item->videoOverridePath_);
    }
}

void ComparisonPreviews::queueMediaRouteRefresh()
{
    if (mediaRouteRefreshQueued_) return;
    mediaRouteRefreshQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        mediaRouteRefreshQueued_ = false;
        refreshMediaRoutes();
        updateClocks();
    }, Qt::QueuedConnection);
}

void ComparisonPreviews::refreshDocument()
{
    const auto snapshot = workspace_.snapshot();
    current_ = identify(snapshot);
    mainAvailable_ = snapshot.hasDocument && workspace_.document().difficulty(snapshot.activeDifficultyId);
    for (auto* item : items_) {
        if (!snapshot.hasDocument || (item->song_.generation != current_.generation
                && (current_.chartPath.isEmpty() || item->song_.chartPath != current_.chartPath))) continue;
        const bool trackChanged = item->song_.trackKey != current_.trackKey;
        item->song_ = current_;
        if (trackChanged) {
            item->audioReady_ = false;
            item->audioRevision_ = 0;
            item->audio_.reset();
            item->sounding_ = false;
            item->trackDuration_ = 0;
        }
        item->readChart(workspace_.document());
    }
    regroup();
}

void ComparisonPreviews::regroup()
{
    for (int i = 0; i < items_.size(); ++i) {
        auto* item = items_[i];
        ComparisonPreview* leader = item;
        if (synchronize_ && item->syncEnabled_ && mainAvailable_ && sameSong(item->song_, current_)) leader = nullptr;
        else if (synchronize_ && item->syncEnabled_) {
            for (int j = 0; j < i; ++j) {
                if (items_[j]->syncEnabled_ && sameSong(item->song_, items_[j]->song_)) { leader = items_[j]->leader_; break; }
            }
        }
        if (leader != item->leader_) {
            // A newly independent pane stays at the last shared frame. It
            // only produces its own audio after a local play request.
            item->setLocalClock(item->position_, false);
            item->leader_ = leader;
        }
    }
    refreshMediaRoutes();
    updateClocks();
}

void ComparisonPreviews::control(ComparisonPreview* item, double second, bool playing)
{
    if (!std::isfinite(second) || !items_.contains(item)) return;
    second = qBound(item->lowerBound_, second, item->duration());
    if (!item->leader_) {
        if (std::abs(second - item->position_) > 0.001) main_.setPositionSeconds(second);
        main_.setPlaying(playing);
    } else {
        item->leader_->setLocalClock(second, playing);
    }
    updateClocks(item->leader_, true);
}

void ComparisonPreviews::updateClocks(ComparisonPreview* seekLeader, bool seek)
{
    if (!presentationActive_ || items_.isEmpty() || !mainScene_) return;
    struct ClockFrame {
        double duration = 0;
        double second = 0;
        double lowerBound = 0;
        int members = 0;
        bool playing = false;
    };
    QHash<ComparisonPreview*, ClockFrame> frames;
    for (auto* item : items_) {
        auto& frame = frames[item->leader_];
        frame.duration = std::max({frame.duration, item->chartDuration_, item->trackDuration_});
        frame.lowerBound = std::min(frame.lowerBound, item->first_);
        ++frame.members;
    }
    bool needTimer = false;
    const double rate = main_.rate() > 0 ? main_.rate() : 1.0;
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        auto* leader = it.key();
        auto& frame = it.value();
        if (!leader) {
            frame.duration = main_.durationSeconds();
            frame.lowerBound = main_.lowerBoundSeconds();
            frame.second = mainScene_->frameState().playheadSeconds;
            frame.playing = main_.playing();
            continue;
        }
        frame.second = leader->localPosition();
        frame.playing = leader->clock_.isValid();
        if (frame.playing && frame.duration > 0 && frame.second >= frame.duration) {
            frame.second = frame.duration;
            frame.playing = false;
            leader->setLocalClock(frame.second, false);
        } else if (leader->rate_ != rate) {
            leader->setLocalClock(frame.second, frame.playing);
        }
        needTimer |= frame.playing;
    }
    for (auto* item : items_) {
        const auto frame = frames.value(item->leader_);
        const bool linked = synchronize_ && (!item->leader_ || frame.members > 1);
        if (item->linked_ != linked || item->lowerBound_ != frame.lowerBound) {
            item->linked_ = linked;
            item->lowerBound_ = frame.lowerBound;
            emit item->transportChanged();
        }
        item->present(frame.second, frame.duration, frame.playing, rate, seek && seekLeader == item->leader_);
    }
    if (needTimer && !tick_.isActive()) tick_.start();
    if (!needTimer) tick_.stop();
}

void ComparisonPreviews::remove(ComparisonPreview* item)
{
    const int row = items_.indexOf(item);
    if (row < 0) return;
    ComparisonPreview* successor = nullptr;
    if (item->leader_ == item && item->playing_) {
        for (auto* peer : items_) {
            if (peer != item && peer->leader_ == item) { successor = peer; break; }
        }
    }
    const double resumeAt = item->localPosition();
    if (focused_ == item) focused_ = nullptr;
    beginRemoveRows({}, row, row);
    items_.removeAt(row);
    endRemoveRows();
    item->setMediaSource(nullptr);
    item->releaseOwnedMedia();
    item->present(item->position_, item->duration(), false, item->rate_, true);
    regroup();
    if (successor) control(successor, resumeAt, true);
    item->deleteLater();
    updateAudioRoutes();
    emit countChanged();
}

ComparisonPreview::ComparisonPreview(ComparisonPreviews& owner, int difficulty, int workspaceNumber)
    : QObject(&owner), owner_(owner), panelId_("comparison-" + QUuid::createUuid().toString(QUuid::Id128)),
      difficulty_(difficulty), workspaceNumber_(workspaceNumber)
{
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, &ComparisonPreview::startBuild);
    scene_.setStageMediaPresentationMode(preview::scene::PreviewStageMediaPresentationMode::ExternalQuickMediaItem);
    connect(&media_, &PreviewStageMediaHost::chartMediaSourceChanged, this, [this] { mediaNeedsSync_ = true; });
}

ComparisonPreview::~ComparisonPreview()
{
    if (cancellation_) cancellation_->store(true);
    disconnect(mediaStateConnection_);
    disconnect(mediaVisibilityConnection_);
}

QString ComparisonPreview::workspaceLabel() const
{
    QString label;
    int number = workspaceNumber_;
    do {
        label.prepend(QChar(static_cast<char16_t>(u'A' + number % 26)));
        number = number / 26 - 1;
    } while (number >= 0);
    return label;
}

void ComparisonPreview::setSurfaceVisible(bool visible)
{
    if (surfaceVisible_ == visible) return;
    surfaceVisible_ = visible;
    owner_.queueMediaRouteRefresh();
    emit surfaceVisibleChanged();
}

void ComparisonPreview::readChart(const SimaiDocument& document)
{
    const auto* data = document.difficulty(difficulty_);
    if (!data) return;
    const QString difficulty = SimaiDocument::difficultyShortName(difficulty_) + " " + data->level;
    const QString title = (document.title.isEmpty() ? qtTrId("library.untitled") : document.title) + " · " + difficulty;
    if (title_ != title) { title_ = title; emit chartChanged(); }
    scene_.setChartInfo(document.title, document.artist, difficulty, document.effectiveDesignerForSlot(difficulty_));
    videoOverridePath_ = document.videoPath;
    const QVector<SimaiRawField> versionFields{
        {QStringLiteral("version"), metadata::fieldValue(document, QStringLiteral("version"))},
        {QStringLiteral("metadata"), metadata::fieldValue(document, QStringLiteral("metadata"))}
    };
    if (versionMetadata_.extraFields != versionFields) {
        versionMetadata_.extraFields = versionFields;
        automaticOutlinePath_ = assets::customOutlinePathForFileName(
            chart_version::detect(versionMetadata_, scene_.frameState().noteMarkers).outlineFileName());
        inheritAppearance();
    }
    const auto timing = simai::buildTimingMetadata(document);
    const double first = timeline::offset::parsedFirstSeconds(document.first);
    const int count = chart_clock::clockCountFromDocument(document);
    if (revision_ && chart_ == data->chart && timing_ == timing && first_ == first && clockCount_ == count) return;
    chart_ = data->chart;
    timing_ = timing;
    first_ = first;
    clockCount_ = count;
    scheduleBuild();
}

void ComparisonPreview::scheduleBuild()
{
    ++revision_;
    if (cancellation_) cancellation_->store(true);
    debounce_.start(revision_ <= 2 ? 0 : 150);
}

void ComparisonPreview::startBuild()
{
    if (building_) return;
    building_ = true;
    const auto revision = revision_;
    disconnect(&build_, nullptr, this, nullptr);
    connect(&build_, &QFutureWatcher<Build>::finished, this, [this, revision] {
        building_ = false;
        if (revision != revision_) { debounce_.start(0); return; }
        auto result = build_.result();
        result.report.revision = revision;
        builtRevision_ = revision;
        scene_.setNoteMarkers(result.markers);
        automaticOutlinePath_ = assets::customOutlinePathForFileName(
            chart_version::detect(versionMetadata_, result.markers).outlineFileName());
        inheritAppearance();
        scene_.setVisualAidChartTiming(result.parsed, clockCount_, first_);
        auto stats = std::make_shared<preview::scene::PreviewProgressStatsCache>();
        stats->rebuild(result.markers);
        scene_.setProgressStatsCache(stats);
        scene_.setMuriAnalysisReport(result.report);
        chartDuration_ = std::max(0.0, result.parsed.durationSeconds + first_);
        owner_.updateClocks();
    });
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    build_.setFuture(QtConcurrent::run([cancel = cancellation_, chart = chart_, timing = timing_, first = first_, options = options_, threshold = thresholdMs_] {
        Build result;
        task::CancellationScope scope(cancel);
        try {
            result.parsed = SimaiParser::parseForTimeline(chart, timing);
            result.markers = timeline::offset::shiftedNoteMarkers(result.parsed.noteMarkers, first, timeline::offset::NonFiniteHandling::PassThrough);
            result.report = MuriAnalyzer::analyze(result.markers, options, threshold / 1000.0);
        } catch (const task::Cancelled&) { return Build{}; }
        return result;
    }));
}

void ComparisonPreview::inheritAppearance()
{
    if (!owner_.mainScene_) return;
    const preview::scene::PreviewAssetState* ownOutline = nullptr;
    if (owner_.automaticOutline_) {
        const bool usePausedJudgeArea = !playing_ && owner_.forceJudgeAreaWhenPaused_;
        const auto variant = usePausedJudgeArea && owner_.outlineVariant_ != PreviewOutlineVariant::None
            ? PreviewOutlineVariant::JudgeAreaLabeled : owner_.outlineVariant_;
        const auto mode = usePausedJudgeArea && !automaticOutlinePath_.isEmpty()
            ? preview::runtime::PreviewOutlineImageMode::PausedJudgeAreaComposite
            : preview::runtime::PreviewOutlineImageMode::Direct;
        if (!outlineAssets_ || variant != outlineVariant_ || automaticOutlinePath_ != outlinePath_ || mode != outlineImageMode_) {
            outlineVariant_ = variant;
            outlinePath_ = automaticOutlinePath_;
            outlineImageMode_ = mode;
            outlineAssets_ = preview::runtime::PreviewSceneAssetLoader::loadAssetState(variant, outlinePath_, mode);
        }
        ownOutline = &*outlineAssets_;
    }
    scene_.inheritDisplaySettings(*owner_.mainScene_, ownOutline);
    const auto& state = scene_.frameState();
    if (options_ != state.muriRenderOptions || thresholdMs_ != owner_.main_.muriTapOnSlideThresholdMs()) {
        options_ = state.muriRenderOptions;
        thresholdMs_ = owner_.main_.muriTapOnSlideThresholdMs();
        scheduleBuild();
    }
    media_.setBackgroundScaleMode(state.render.backgroundScaleMode);
    media_.setLayoutSquareScale(state.render.layoutSquareScale);
}

void ComparisonPreview::setMediaSource(PreviewStageMediaHost* source)
{
    if (source == mediaSource_) return;
    disconnect(mediaStateConnection_);
    disconnect(mediaVisibilityConnection_);
    mediaSource_ = source;
    const auto syncMedia = [this] {
        scene_.setStageMediaAvailable(mediaSource_ && mediaSource_->mediaVisible() && mediaSource_->hasResolvedMedia());
    };
    if (source) {
        mediaStateConnection_ = connect(source, &PreviewStageMediaHost::mediaStateChanged, this, syncMedia);
        mediaVisibilityConnection_ = connect(source, &PreviewStageMediaHost::mediaVisibilityChanged, this, syncMedia);
    }
    syncMedia();
    emit mediaHostChanged();
}

void ComparisonPreview::releaseOwnedMedia()
{
    const bool hadVideo = media_.hasVideoMedia();
    if (mediaPlaying_) media_.pausePlayback();
    if (!media_.chartPath().isEmpty()) media_.setChartPath({});
    if (hadVideo) media_.releaseDecoderForFileReplace();
    mediaPlaying_ = false;
}

void ComparisonPreview::updateMediaPlayback(double second, bool playing, double rate, bool moved, bool jump)
{
    if (mediaSource_ != &media_) return;
    const bool visible = playing || owner_.showMediaWhenPaused_;
    media_.setMediaVisible(visible);
    if (!media_.hasVideoMedia()) return;
    if (rate != rate_ || mediaNeedsSync_) media_.setPlaybackRate(rate);
    if (playing) {
        if (!mediaPlaying_ || mediaNeedsSync_ || jump) media_.startPlayback(second);
        else if (moved) media_.syncPlayback(second);
    } else {
        if (mediaPlaying_) media_.pausePlayback();
        if (visible && (mediaPlaying_ || mediaNeedsSync_ || moved || jump)) media_.setPlayheadSeconds(second);
    }
    mediaNeedsSync_ = !visible;
    mediaPlaying_ = playing;
}

double ComparisonPreview::localPosition() const
{
    return clockStart_ + (clock_.isValid() ? clock_.nsecsElapsed() * 1e-9 * rate_ : 0);
}

void ComparisonPreview::setLocalClock(double second, bool running)
{
    clockStart_ = second;
    if (running) clock_.start(); else clock_.invalidate();
}

void ComparisonPreview::present(double second, double duration, bool playing, double rate, bool jump)
{
    const bool moved = second != position_;
    if (moved || playing != playing_) {
        if (playing && playing_ && lastFrame_.isValid()) {
            const double expected = lastFrame_.nsecsElapsed() * 1e-9 * rate_;
            jump |= second < position_ || std::abs(second - position_ - expected) > 0.15;
        }
        lastFrame_.start();
    }
    const bool changed = duration != displayedDuration_ || playing != playing_;
    updateMediaPlayback(second, playing, rate, moved, jump);
    const bool audioWanted = playing && (owner_.mixAudio_ || owner_.focused_ == this);
    if (rate != rate_) {
        if (audio_) audio_->applyPlaybackRateAtChartSecond(rate, second);
    }
    if (moved) scene_.setPlayheadSeconds(second);
    position_ = second;
    displayedDuration_ = duration;
    const bool playbackChanged = playing_ != playing;
    playing_ = playing;
    rate_ = rate;
    if (playbackChanged) inheritAppearance();
    if (audioWanted) {
        auto settings = owner_.main_.audioSettings();
        // Synchronized copies of one backing track otherwise interfere at device latency offsets.
        if (owner_.mixAudio_ && linked_ && leader_ != this) settings.trackVolume = 0;
        if (!audio_) {
            audio_ = std::make_unique<QtPreviewSfxRuntime>();
            audio_->setOutputRoute(owner_.mixAudio_ ? 0.90 / (owner_.count() + 1) : 1.0, owner_.mixAudio_ ? pan_ : 0.0);
            connect(audio_.get(), &QtPreviewSfxRuntime::commandCompleted, this, [this](const QtPreviewSfxRuntime::Completion& done) {
                if (done.kind != preview_audio::CommandKind::ReloadAssets) return;
                audioReady_ = done.success;
                if (audioReady_) owner_.updateClocks();
            });
            appliedSettings_ = settings;
            audio_->reloadAssetsForChartWithWarmupPaths(song_.chartPath, song_.trackPath, preview_sfx::resolveSfxDirectory(), settings);
        }
        if (!(appliedSettings_ == settings)) { appliedSettings_ = settings; audio_->applyLevels(settings); }
        if (audioReady_) {
            const auto timing = owner_.main_.audioTiming();
            if (audioRevision_ != builtRevision_ || !sounding_ || !previewTimingSettingsEqual(appliedTiming_, timing)) {
                appliedTiming_ = timing;
                audio_->configureTimeline(scene_.frameState().noteMarkers, rate, appliedTiming_);
                audioRevision_ = builtRevision_;
                jump = true;
            }
            if (!sounding_ || jump) {
                audio_->setOutputRoute(owner_.mixAudio_ ? 0.90 / (owner_.count() + 1) : 1.0, owner_.mixAudio_ ? pan_ : 0.0);
                audio_->startPreviewPlaybackTransaction(second, false, rate);
            }
            if (moved) audio_->drainEvents(second);
        }
    } else if (sounding_ && audio_) {
        audio_->setOutputRoute(0, 0);
        audio_->pausePreviewPlaybackTransaction();
    }
    sounding_ = audioWanted && audioReady_;
    if (changed) emit transportChanged();
    if (moved) emit positionChanged();
}

void ComparisonPreview::togglePlayback() { owner_.control(this, !playing_ && position_ >= duration() ? 0.0 : position_, !playing_); }
void ComparisonPreview::seek(double seconds) { owner_.control(this, seconds, playing_); }
void ComparisonPreview::close() { owner_.remove(this); }
}
