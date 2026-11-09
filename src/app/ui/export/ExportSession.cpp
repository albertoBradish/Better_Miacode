#include "common/RuntimePaths.h"
#include "app/services/FileDialogPreferences.h"
#include "common/LocalizedText.h"

#include "app/ui/export/ExportSession.h"
#include "app/ui/export/CoverExportSession.h"
#include "app/ui/document/DifficultyOptions.h"

#include "core/chart/document/SimaiDocument.h"

#include "app/services/JobProgressService.h"
#include "app/ui/preferences/LocaleService.h"
#include "core/scene/PreviewSfxAssets.h"
#include "core/video/PreviewRenderSettings.h"
#include "app/services/VideoExportPreferences.h"
#include "export/cover_export/CoverBatchExportPolicy.h"
#include "export/video_export/VideoExportSettings.h"
#include "export/video_export/VideoExportAudioRenderPlan.h"
#include "export/video_export/FontLibrary.h"
#include "app/services/UserFontLibrary.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/IntroConfig.h"
#include "core/video/AssetPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <utility>


namespace miacode::ui {
namespace {

QString legacyBatchExportDirectory(const char* key)
{
    QSettings settings(miacode::runtime_paths::configFile("export-session.ini"), QSettings::IniFormat);
    return miacode::file_dialog_preferences::validDirectory(
        settings.value(QStringLiteral("batch_video_export_dialog/") + QLatin1String(key)).toString());
}
inline constexpr auto& kResolutionPresets = miacode::video_export::kVideoExportResolutionPresets;
inline constexpr auto& kFpsOptions = miacode::video_export::kVideoExportFpsOptions;
inline constexpr auto& kAudioBitrateOptions =
    miacode::video_export::kVideoExportAudioBitrateOptionsKbps;
constexpr double kMinimumExportRangeSeconds = 5.0;
constexpr std::array kSizePresets{
    VideoExportSizePreset::Standard, VideoExportSizePreset::Target30Mb,
    VideoExportSizePreset::Target100Mb, VideoExportSizePreset::Target200Mb, VideoExportSizePreset::Custom,
};

double minimumExportRangeSecondsForChart(double chartDurationSeconds)
{
    return qMin(kMinimumExportRangeSeconds, qMax(0.0, chartDurationSeconds));
}

}  // namespace

ExportSession::ExportSession(miacode::ShellNotifications& notifications,
                                   miacode::UiRequestService& uiRequests,
                                   miacode::JobProgressService& jobProgress,
                                   miacode::PreviewAppearanceState& appearance,
                                   miacode::ExportEngine*& engineSlot,
                                   miacode::PreviewSurface*& previewSlot,
                                   QObject* parent)
    : QObject(parent)
    , uiRequests_(&uiRequests)
    // From the application assembly, not from the hidden window.
    , jobProgress_(&jobProgress)
    , appearance_(&appearance)
    , engineSlot_(&engineSlot)
    , previewSlot_(&previewSlot)
    , notifications_(&notifications)
{
    connect(&miacode::LocaleService::instance(), &miacode::LocaleService::languageChanged,
            this, [this](const QString&) {
                fontLibraryOptionsCacheValid_ = false;
                emit localeLabelsChanged();
                emit outputChanged();
                emit fontLibraryChanged();
                emit introSoundOptionsChanged();
                emit introChanged();
                emit batchChanged();
            });
    connect(&notifications, &miacode::ShellNotifications::videoExportWorkerRunningChanged, this, [this](bool running) {
        if (batchExportRunning_ || exportRunning_ == running) {
            return;
        }
        exportRunning_ = running;
        emit exportRunningChanged();
    });
    connect(&notifications, &miacode::ShellNotifications::documentRevisionChanged, this,
        [this](quint64) { invalidateSingleCoverPreparation(); });
    connect(&notifications, &miacode::ShellNotifications::videoExportSucceeded, this, [this](const QString& actualOutputPath) {
        if (!singleCoverVideoLaunched_) return;
        singleCoverVideoLaunched_ = false;
        auto prepared = singleCoverJob_.takeForSuccessfulVideo();
        if (!prepared) return;
        QStringList errors;
        for (auto& cover : prepared->images) {
            const QString path = miacode::cover_export::companionCoverOutputPath(
                actualOutputPath, cover.token, prepared->images.size() > 1);
            if (!cover.image.isNull()) {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || !cover.image.save(&file, "PNG") || !file.commit())
                    cover.error = file.errorString();
            }
            if (!cover.error.isEmpty()) errors.append(cover.token + QStringLiteral(": ") + cover.error);
        }
        if (!errors.isEmpty()) {
            uiRequests_->postNotice(miacode::NoticeSeverity::Warning,
                miacode::localizedText("cover.export_cover"),
                miacode::localizedText("dialog.video_export.cover_failed_video_kept"), errors.join(QLatin1Char('\n')));
        }
    });
    connect(&notifications, &miacode::ShellNotifications::videoExportWorkerRunningChanged, this, [this](bool running) {
        if (running || batchExportRunning_ || !singleCoverVideoLaunched_) return;
        const quint64 generation = singleCoverJob_.generation();
        // Success is delivered before/alongside worker teardown. Queue cleanup
        // so either signal order works, and never clear a newer job's image.
        QTimer::singleShot(0, this, [this, generation]() {
            if (singleCoverJob_.generation() != generation) return;
            singleCoverVideoLaunched_ = false;
            singleCoverJob_.discard(generation);
        });
    });
    connect(&notifications, &miacode::ShellNotifications::videoExportCompleted, this, [this]() {
        if (!batchExportRunning_ && exportRunning_) {
            resetIntroSelection();
        }
    });
    connect(&notifications, &miacode::ShellNotifications::previewRenderSettingsChanged, this, [this]() {
        if (pageSessionActive_ && !pushingSharedSettings_) {
            adoptPreviewRenderSettings();
            emit videoChanged();
            emit introChanged();
        }
    });
    // The audition ends inside the playback coordinator (music faded out,
    // another transport action); it announces that through the presentation.
    connect(&notifications, &miacode::ShellNotifications::presentationChanged,
            this, &ExportSession::introPreviewStateChanged);
    connect(this, &ExportSession::rangeChanged, this, &ExportSession::outputChanged);
    connect(this, &ExportSession::introChanged, this, &ExportSession::outputChanged);
    connect(this, &ExportSession::videoChanged, this, &ExportSession::outputChanged);
    connect(this, &ExportSession::rangeChanged, this, [this]() {
        if (rangePlaybackEnabled_ && !hasPendingSelectionRangeExport_) {
            emit playbackRangeRequested(true, exportStartSeconds(), exportEndSeconds());
        }
    });
}

void ExportSession::setChartPreviewContextProvider(ChartPreviewContextProvider provider)
{
    chartPreviewContextProvider_ = std::move(provider);
    chartPreviewContextChanged();
}

void ExportSession::chartPreviewContextChanged()
{
    updateBatchOutputDirectory();
    emit dropChanged();
    refreshIntroWindow();
}

ExportSession::ChartPreviewContext ExportSession::chartPreviewContext() const
{
    return chartPreviewContextProvider_
        ? chartPreviewContextProvider_(selectedDifficultyId_) : ChartPreviewContext{};
}

void ExportSession::updateBatchOutputDirectory()
{
    const QString chartPath = chartPreviewContextProvider_ ? chartPreviewContext().chartPath : task_.chartPath;
    if (batchOutputChartPath_ == chartPath) return;
    batchOutputChartPath_ = chartPath;
    batchOutputDirectory_ = chartPath.isEmpty() ? QString() : QFileInfo(chartPath).absolutePath();
    emit batchChanged();
}

std::optional<miacode::chart_preview::DropSelection> ExportSession::currentDrop() const
{
    const auto context = chartPreviewContext();
    return context.document && context.chartPath == task_.chartPath ? context.drop : std::nullopt;
}

bool ExportSession::dropAvailable() const
{
    const auto drop = currentDrop();
    return drop && drop->range.isValid() && drop->range.endSeconds <= chartDurationSeconds_ + 0.001;
}

double ExportSession::dropStartSeconds() const
{
    const auto drop = currentDrop();
    return drop ? drop->range.startSeconds : 0.0;
}

double ExportSession::dropEndSeconds() const
{
    const auto drop = currentDrop();
    return drop ? drop->range.endSeconds : 0.0;
}

void ExportSession::adoptDropRange()
{
    if (!dropAvailable()) return;
    applyExportRangeSeconds(dropStartSeconds(), dropEndSeconds(), 0.0);
}

double ExportSession::introMediaDuration() const
{
    const auto context = chartPreviewContext();
    return context.document && context.chartPath == task_.chartPath && context.mediaDuration > 0.0
        ? context.mediaDuration : chartDurationSeconds_;
}

void ExportSession::refreshIntroWindow(bool requestPreview)
{
    using namespace miacode::chart_preview;
    const auto context = chartPreviewContext();
    if (introDocumentGeneration_ != context.documentGeneration) {
        introDocumentGeneration_ = context.documentGeneration;
        introExplicitRange_.reset();
    }
    IntroWindow window;
    if (context.document && context.chartPath == task_.chartPath) {
        const double duration = context.mediaDuration > 0.0 ? context.mediaDuration : chartDurationSeconds_;
        window = resolveIntroWindow(*context.document, selectedDifficultyId_, introExplicitRange_,
            task_.intro.previewWindowMode, duration, context.dropFingerprint, context.drop, true);
    } else {
        window = {introExplicitRange_.value_or(MediaRange{0.0, miacode::intro::kPvPreviewHoldSeconds}),
                  introExplicitRange_ ? IntroWindowSource::Explicit : IntroWindowSource::Default};
    }
    if (!window.range.isValid()) return;
    const bool changed = task_.intro.pvPreviewStartSeconds != window.range.startSeconds
        || task_.intro.pvPreviewEndSeconds != window.range.endSeconds || task_.intro.previewWindowSource != window.source;
    freezeIntroPreviewWindow(task_.intro, window);
    if (changed && requestPreview) {
        emit introChanged();
        if (pageSessionActive_ && engine()) engine()->refreshIntroState();
    }
}

void ExportSession::setRangePlaybackEnabled(bool enabled)
{
    enabled = enabled && rangePreviewAvailable();
    if (rangePlaybackEnabled_ == enabled) {
        return;
    }
    rangePlaybackEnabled_ = enabled;
    emit playbackRangeRequested(enabled, enabled ? exportStartSeconds() : 0.0,
                                enabled ? exportEndSeconds() : 0.0);
    emit rangePlaybackStateChanged();
}

QString ExportSession::activeTab() const
{
    return activeTab_;
}

QVariantList ExportSession::resolutionOptions() const
{
    QVariantList list;
    for (const auto& preset : kResolutionPresets) {
        QVariantMap row;
        row.insert(QStringLiteral("label"), QString::fromLatin1(preset.label));
        row.insert(QStringLiteral("width"), preset.width);
        row.insert(QStringLiteral("height"), preset.height);
        list.append(row);
    }
    return list;
}

QVariantList ExportSession::fpsOptions() const
{
    QVariantList list;
    for (int fps : kFpsOptions) {
        list.append(fps);
    }
    return list;
}

QVariantList ExportSession::audioBitrateOptions() const
{
    QVariantList list;
    for (int kbps : kAudioBitrateOptions) {
        list.append(kbps);
    }
    return list;
}

QVariantList ExportSession::outputModeOptions() const
{
    return {QVariantMap{{"value", 0}, {"label", QStringLiteral("MP4")}},
            QVariantMap{{"value", 1}, {"label", QStringLiteral("WAV")}},
            QVariantMap{{"value", 2}, {"label", QStringLiteral("MP4 + WAV")}}};
}

void ExportSession::setOutputModeIndex(int index)
{
    if (index < 0 || index > 2 || outputModeIndex() == index) {
        return;
    }
    task_.outputMode = static_cast<VideoExportOutputMode>(index);
    if (!task_.outputPath.isEmpty()) {
        task_.outputPath = videoExportPrimaryOutputPath(task_.outputPath, task_.outputMode);
    }
    emit outputChanged();
    savePreferences();
}

QVariantList ExportSession::presetOptions() const
{
    return QVariantList{
        qtTrId("dialog.video_export.preset.fast"),
        qtTrId("dialog.video_export.preset.high_quality"),
    };
}

QVariantList ExportSession::sizePresetOptions() const
{
    QVariantList options{qtTrId("dialog.video_export.size_preset.standard")};
    const double duration = miacode::video_export::videoExportAlignedDurationSeconds(buildRequestedTask());
    constexpr std::array labels{"video_size.target30", "video_size.target100", "video_size.target200"};
    for (size_t i = 0; i < labels.size(); ++i) {
        const auto preset = kSizePresets[i + 1];
        const int audio = miacode::video_export::effectiveVideoExportAudioBitrateKbps(preset, task_.audioBitrateKbps);
        const auto bitrate = miacode::video_export::videoExportTargetBitrateKbps(preset, duration, audio);
        options.append(qtTrId(labels[i]).arg(bitrate).arg(audio));
    }
    options.append(qtTrId("video_size.custom"));
    return options;
}

double ExportSession::estimatedCustomSizeMb() const
{
    const double duration = miacode::video_export::videoExportAlignedDurationSeconds(buildRequestedTask());
    const double audioMbps = task_.audioBitrateKbps / 1000.0;
    // Reserve the same mux allowance as the target-size presets.
    return (task_.targetVideoBitrateMbps + audioMbps + 0.016) * duration / (8.0 * 0.975);
}

int ExportSession::presetIndex() const
{
    return task_.preset == VideoExportPreset::Fast ? 0 : 1;
}

int ExportSession::sizePresetIndex() const
{
    for (size_t i = 0; i < kSizePresets.size(); ++i)
        if (task_.sizePreset == kSizePresets[i]) return static_cast<int>(i);
    return 0;
}

int ExportSession::introBackgroundModeIndex() const
{
    return task_.intro.backgroundMode.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0 ? 1 : 0;
}

int ExportSession::introModeIndex() const
{
    if (isAutoIntroBannerMode(task_.intro.mode)) {
        return 0;
    }
    if (task_.intro.mode.compare(QStringLiteral("Standard"), Qt::CaseInsensitive) == 0) {
        return 2;
    }
    return 1;
}

bool ExportSession::introLevelTextRender() const
{
    return task_.intro.lvRenderMode.compare(QStringLiteral("text"), Qt::CaseInsensitive) == 0;
}

QVariantList ExportSession::introSoundOptions() const
{
    QVariantList list;
    list.append(QVariantMap{
        {QStringLiteral("label"),
         qtTrId("dialog.render_settings.music.default_intro_sound")},
        {QStringLiteral("fileName"), QString()},
    });
    const QString musicDirectory = miacode::preview_sfx::assetMusicDirectory();
    if (musicDirectory.isEmpty()) {
        return list;
    }
    const QFileInfoList entries = QDir(musicDirectory).entryInfoList(
        miacode::preview_sfx::supportedIntroSoundFileExtensions(),
        QDir::Files,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& entry : entries) {
        list.append(QVariantMap{
            {QStringLiteral("label"), entry.fileName()},
            {QStringLiteral("fileName"), entry.fileName()},
        });
    }
    return list;
}

int ExportSession::introSoundIndex() const
{
    const QString selected = miacode::preview_sfx::normalizeIntroSoundFileName(
        task_.introSoundFileName);
    const QVariantList options = introSoundOptions();
    for (int i = 0; i < options.size(); ++i) {
        if (options.at(i).toMap().value(QStringLiteral("fileName")).toString() == selected) {
            return i;
        }
    }
    return 0;
}

QVariantList ExportSession::fontLibraryOptions() const
{
    const QString defaultLabel = qtTrId("card_font.default");
    if (fontLibraryOptionsCacheValid_
        && fontLibraryOptionsCacheDefaultLabel_ == defaultLabel) {
        return fontLibraryOptionsCache_;
    }
    QVariantList list;
    const QVector<miacode::video_export::FontLibraryEntry> entries =
        miacode::video_export::fontLibraryEntries(miacode::app_preferences::fontLibraryDirectory(), true, defaultLabel);
    for (const miacode::video_export::FontLibraryEntry& entry : entries) {
        list.append(QVariantMap{
            {QStringLiteral("label"), entry.label},
            {QStringLiteral("path"), entry.path},
            {QStringLiteral("family"), entry.family},
        });
    }
    fontLibraryOptionsCache_ = list;
    fontLibraryOptionsCacheDefaultLabel_ = defaultLabel;
    fontLibraryOptionsCacheValid_ = true;
    return fontLibraryOptionsCache_;
}

QString ExportSession::introSoundLabel() const
{
    return qtTrId("dialog.render_settings.music.intro_sound");
}

QString ExportSession::introSoundImportLabel() const
{
    return qtTrId("dialog.render_settings.video.skin.import");
}

double ExportSession::exportEndSeconds() const
{
    return task_.exportStartSeconds + qMax(0.0, task_.contentDurationSeconds);
}

double ExportSession::minimumExportRangeSeconds() const
{
    const double duration = exportEndSeconds() - exportStartSeconds();
    const double minimum = minimumExportRangeSecondsForChart(chartDurationSeconds_);
    return duration > 0.0 ? qMin(duration, minimum) : minimum;
}

IntroBannerSpec ExportSession::previewIntroSpec() const
{
    IntroBannerSpec spec = task_.intro;
    spec.enabled = task_.intro.enabled && task_.fullRangeExport;
    // The export rebuilds the card from the chart; the preview resolves "auto"
    // the same way instead of handing it to the card, which reads it as DX.
    spec.mode = resolvedIntroBannerMode(task_.intro.mode, detectedIntroMode_);
    return spec;
}

QVariantList ExportSession::batchDifficultyChecks() const
{
    QVariantList list;
    for (const QVariant& option : difficultyOptions({1, 2, 3, 4, 5, 6, 7})) {
        QVariantMap row = option.toMap();
        const int id = row.value(QStringLiteral("id")).toInt();
        row.insert(QStringLiteral("checked"), batchSelectedDifficultyIds_.contains(id));
        list.append(row);
    }
    return list;
}

void ExportSession::setUnavailableReason(const QString& reason)
{
    if (unavailableReason_ == reason) {
        return;
    }
    unavailableReason_ = reason;
    emit unavailableReasonChanged();
    emit rangePlaybackStateChanged();
}

bool ExportSession::difficultyExists(int difficultyId) const
{
    // documentDifficultyIds() lists exactly the difficulties the document
    // holds, so membership is the same question as difficulty(id) != nullptr.
    return engine() != nullptr
        && SimaiDocument::isDifficultyId(difficultyId)
        && engine()->difficultyIds().contains(difficultyId);
}

int ExportSession::resolveDefaultDifficultyId(int previousActiveDifficultyId) const
{
    if (difficultyExists(previousActiveDifficultyId)) {
        return previousActiveDifficultyId;
    }
    if (difficultyExists(selectedDifficultyId_)) {
        return selectedDifficultyId_;
    }
    if (engine() != nullptr && difficultyExists(engine()->lastOpenedDifficultyId())) {
        return engine()->lastOpenedDifficultyId();
    }
    if (engine() != nullptr) {
        const QVector<int> ids = engine()->difficultyIds();
        if (!ids.isEmpty()) {
            return ids.constFirst();
        }
    }
    return 0;
}

void ExportSession::enter(int previousActiveDifficultyId)
{
    if (!pageSessionActive_) {
        pageSessionActive_ = true;
        emit pageSessionActiveChanged();
        emit rangePlaybackStateChanged();
    }
    const int nextDifficultyId = resolveDefaultDifficultyId(previousActiveDifficultyId);
    if (selectedDifficultyId_ != nextDifficultyId) {
        selectedDifficultyId_ = nextDifficultyId;
        emit selectedDifficultyIdChanged();
    }
    rebuildDifficultyList();
    // PageHost marks the QML page after this returns. Seed waits one tick.
    const quint64 generation = ++pagePrepareGeneration_;
    QTimer::singleShot(0, this, [this, generation]() {
        if (!pageSessionActive_ || generation != pagePrepareGeneration_) {
            return;
        }
        seedFromDifficulty(selectedDifficultyId_);
        syncAudition();
    });
    // Re-scan once per real page entry so imports made by another QML surface
    // are visible, while repeated property reads during this entry share the
    // materialized option list.
    fontLibraryOptionsCacheValid_ = false;
    emit fontLibraryChanged();
}

void ExportSession::leave()
{
    invalidateSingleCoverPreparation();
    // Idle no-op: must not tear down the v1 Widgets export audition/session.
    if (!pageSessionActive_) {
        return;
    }
    setRangePlaybackEnabled(false);
    ++pagePrepareGeneration_;
    if (!hasSeededTask_) {
        clearPendingSelectionRangeExport();
    }
    pageSessionActive_ = false;
    emit pageSessionActiveChanged();
    emit rangePlaybackStateChanged();
    savePreferences();
    introPvSegmentDragging_ = false;
    emit introPreviewStateChanged();
    hasSeededTask_ = false;
    stopAudition();
    setUnavailableReason(QString());
}

void ExportSession::replaceDocument(int preferredDifficultyId)
{
    invalidateSingleCoverPreparation();
    setRangePlaybackEnabled(false);
    ++pagePrepareGeneration_;
    stopAudition();
    clearPendingSelectionRangeExport();

    const bool retainUserSettings = hasSeededTask_;
    const VideoExportTask previousTask = task_;
    task_ = VideoExportTask();
    if (retainUserSettings) {
        miacode::video_export::copyVideoExportUserSettings(previousTask, &task_);
    }
    chartDurationSeconds_ = 0.0;
    task_.exportStartSeconds = 0.0;
    task_.contentDurationSeconds = 0.0;
    task_.fullRangeExport = true;

    batchSelectedDifficultyIds_.clear();
    int nextDifficultyId = difficultyExists(preferredDifficultyId)
        ? preferredDifficultyId : 0;
    if (nextDifficultyId == 0 && engine() != nullptr) {
        const QList<int> ids = engine()->difficultyIds();
        if (!ids.isEmpty()) {
            nextDifficultyId = ids.constFirst();
        }
    }
    if (selectedDifficultyId_ != nextDifficultyId) {
        selectedDifficultyId_ = nextDifficultyId;
        emit selectedDifficultyIdChanged();
    }
    rebuildDifficultyList();

    if (pageSessionActive_) {
        seedFromDifficulty(selectedDifficultyId_);
        syncAudition();
    }
    if (!difficultyExists(selectedDifficultyId_)) {
        emit outputChanged();
        emit videoChanged();
        emit introChanged();
        emit rangeChanged();
        emit batchChanged();
    }
}

void ExportSession::selectDifficulty(int difficultyId)
{
    const int next = difficultyExists(difficultyId) ? difficultyId : 0;
    if (selectedDifficultyId_ == next) {
        return;
    }
    invalidateSingleCoverPreparation();
    selectedDifficultyId_ = next;
    emit selectedDifficultyIdChanged();
    if (!pageSessionActive_) {
        return;
    }
    ++pagePrepareGeneration_;
    seedFromDifficulty(selectedDifficultyId_);
    syncAudition();
}

void ExportSession::setActiveTab(const QString& tabId)
{
    const QString next = tabId == QLatin1String("batch")
        ? QStringLiteral("batch")
        : QStringLiteral("export");
    if (activeTab_ == next) {
        return;
    }
    invalidateSingleCoverPreparation();
    activeTab_ = next;
    if (!rangePreviewAvailable()) {
        setRangePlaybackEnabled(false);
    }
    if (activeTab_ == QLatin1String("batch")) {
        setSettingsTab(QStringLiteral("batch"));
    } else if (settingsTab_ == QLatin1String("batch")) {
        setSettingsTab(QStringLiteral("output"));
    }
    emit activeTabChanged();
    emit batchChanged();
    emit rangePlaybackStateChanged();
    if (pageSessionActive_) {
        syncAudition();
    }
}

void ExportSession::setSettingsTab(const QString& tabId)
{
    const QString next = tabId == QLatin1String("range")
        || (tabId == QLatin1String("batch") && activeTab_ != QLatin1String("batch"))
        ? QStringLiteral("output")
        : tabId;
    if (settingsTab_ == next) {
        return;
    }
    settingsTab_ = next;
    emit settingsTabChanged();
}

void ExportSession::rebuildDifficultyList()
{
    const QVariantList next = engine() != nullptr
        ? difficultyOptions(engine()->difficultyIds()) : QVariantList();
    if (difficulties_ != next) {
        difficulties_ = next;
        emit difficultiesChanged();
    }
}

void ExportSession::refreshFromDocument()
{
    invalidateSingleCoverPreparation();
    if (!difficultyExists(selectedDifficultyId_)) {
        selectedDifficultyId_ = resolveDefaultDifficultyId(0);
        emit selectedDifficultyIdChanged();
    }
    rebuildDifficultyList();
    if (pageSessionActive_) {
        ++pagePrepareGeneration_;
        seedFromDifficulty(selectedDifficultyId_);
        syncAudition();
    }
    emit batchChanged();
}

void ExportSession::applyPreferences()
{
    const QJsonObject settings = miacode::app_preferences::loadDialogPreferences();
    // Keep the established first-run defaults shared with the Widgets dialog.
    task_.clockCountEnabled = false;
    task_.intro.mode = QStringLiteral("auto");
    task_.intro.lvRenderMode = QStringLiteral("atlas");
    miacode::video_export::applyVideoExportPreferences(settings, &task_);
    batchCoverPreset_ = settings.value(QStringLiteral("batch_cover_preset")).toString();
    singleCoverPreset_ = settings.value(QStringLiteral("single_cover_preset")).toString();
    const auto selected = [&settings](const char* key, const QString& legacy) {
        QStringList list;
        if (settings.contains(QLatin1String(key))) {
            for (const auto& value : settings.value(QLatin1String(key)).toArray())
                if (value.isString() && !value.toString().isEmpty()) list.append(value.toString());
        } else if (!legacy.isEmpty()) list.append(legacy);
        list.removeDuplicates();
        return list;
    };
    singleCoverPresets_ = selected("single_cover_presets", singleCoverPreset_);
    batchCoverPresets_ = selected("batch_cover_presets", batchCoverPreset_);
    const int savedWidth = task_.outputWidth;
    const int savedHeight = task_.outputHeight;
    resolutionIndex_ = 3;
    for (int i = 0; i < static_cast<int>(std::size(kResolutionPresets)); ++i) {
        if (kResolutionPresets[i].width == savedWidth && kResolutionPresets[i].height == savedHeight) {
            resolutionIndex_ = i;
            break;
        }
    }
    task_.outputWidth = kResolutionPresets[resolutionIndex_].width;
    task_.outputHeight = kResolutionPresets[resolutionIndex_].height;
}

void ExportSession::savePreferences() const
{
    QJsonObject settings = miacode::app_preferences::loadDialogPreferences();
    miacode::video_export::appendVideoExportPreferences(&settings, task_);
    settings.insert(QStringLiteral("batch_cover_preset"), batchCoverPreset_);
    settings.insert(QStringLiteral("single_cover_preset"), singleCoverPreset_);
    settings.insert(QStringLiteral("single_cover_presets"), QJsonArray::fromStringList(singleCoverPresets_));
    settings.insert(QStringLiteral("batch_cover_presets"), QJsonArray::fromStringList(batchCoverPresets_));
    miacode::app_preferences::saveDialogPreferences(settings);
}

void ExportSession::seedFromDifficulty(int difficultyId)
{
    if (!difficultyExists(difficultyId)) {
        VideoExportTask emptyTask;
        if (hasSeededTask_) {
            miacode::video_export::copyVideoExportUserSettings(task_, &emptyTask);
        }
        task_ = std::move(emptyTask);
        detectedIntroMode_ = QStringLiteral("Standard");
        chartDurationSeconds_ = 0.0;
        if (!hasSeededTask_) {
            applyPreferences();
            hasSeededTask_ = true;
        }
        setUnavailableReason(qtTrId("export_page.no_difficulty_is_available_to"));
        setRangePlaybackEnabled(false);
        clearPendingSelectionRangeExport();
        emit outputChanged();
        emit videoChanged();
        updateBatchOutputDirectory();
        emit introChanged();
        emit rangeChanged();
        emit dropChanged();
        return;
    }
    setUnavailableReason(QString());
    VideoExportTask seededTask = engine()->buildSeedTask(difficultyId);
    detectedIntroMode_ = normalizedIntroBannerMode(seededTask.intro.mode);
    if (hasSeededTask_) {
        miacode::video_export::copyVideoExportUserSettings(task_, &seededTask);
    }
    task_ = std::move(seededTask);
    chartDurationSeconds_ = qMax(0.0, task_.contentDurationSeconds);
    if (!hasSeededTask_) {
        applyPreferences();
        hasSeededTask_ = true;
    }
    task_.exportStartSeconds = 0.0;
    task_.contentDurationSeconds = chartDurationSeconds_;
    task_.fullRangeExport = true;
    if (batchSelectedDifficultyIds_.isEmpty()) {
        batchSelectedDifficultyIds_.append(difficultyId);
    }
    updateBatchOutputDirectory();
    refreshIntroWindow(false);
    emit dropChanged();
    adoptPreviewRenderSettings();
    emit outputChanged();
    emit videoChanged();
    emit introChanged();
    emit rangeChanged();
    emit batchChanged();
}

void ExportSession::syncAudition()
{
    if (engine() == nullptr || !pageSessionActive_) {
        return;
    }
    if (!difficultyExists(selectedDifficultyId_)) {
        engine()->clearAudition();
        return;
    }
    const bool applySelectionRange = hasPendingSelectionRangeExport_;
    if (applySelectionRange) {
        setExportRangeSeconds(pendingRangeStartSeconds_, pendingRangeEndSeconds_);
    }
    engine()->startAudition(selectedDifficultyId_, task_);
    if (applySelectionRange) {
        hasPendingSelectionRangeExport_ = false;
        setSettingsTab(QStringLiteral("output"));
    }
    if (applySelectionRange && !rangePlaybackEnabled_) {
        setRangePlaybackEnabled(true);
    } else if (rangePlaybackEnabled_) {
        // 安装试听场景会重设播放头边界；目标场景就绪后同步区间。
        emit playbackRangeRequested(true, exportStartSeconds(), exportEndSeconds());
    }
}

void ExportSession::applyLivePreviewSettings()
{
    if (engine() == nullptr) {
        return;
    }
    adoptPreviewRenderSettings();
    VideoExportTask liveTask = task_;
    applyOwnerLiveFields(&liveTask);
    {
        const QScopedValueRollback pushing(pushingSharedSettings_, true);
        engine()->applySharedTaskSettings(liveTask);
    }
}

void ExportSession::adoptPreviewRenderSettings()
{
    // The task is seeded from this live state on page entry. Left alone, the page would
    // keep that entry-time copy and write it back over the newer value on its next edit
    // or export.
    if (preview() == nullptr) {
        return;
    }
    const QVariantMap values = preview()->renderSettings();
    const auto fromPercent = [&values](const char* key, double fallback) {
        const QVariant value = values.value(QLatin1String(key));
        return value.isValid() ? value.toDouble() / 100.0 : fallback;
    };
    task_.backgroundBrightnessOuter = fromPercent("brightnessOuter", task_.backgroundBrightnessOuter);
    task_.backgroundBrightnessInner = fromPercent("brightnessInner", task_.backgroundBrightnessInner);
    task_.layoutSquareScale = fromPercent("layoutSquareScale", task_.layoutSquareScale);
    task_.backgroundScaleMode = static_cast<PreviewBackgroundScaleMode>(
        values.value(QStringLiteral("scaleMode"), static_cast<int>(task_.backgroundScaleMode)).toInt());
    task_.fireworkBrightness = miacode::preview_video::normalizedFireworkBrightness(fromPercent("fireworkBrightness", task_.fireworkBrightness));
    task_.fireworkStyle = static_cast<PreviewFireworkStyle>(values.value(QStringLiteral("fireworkStyle"), static_cast<int>(task_.fireworkStyle)).toInt());
    task_.smoothBrightness = values.value(QStringLiteral("smoothBrightness"), task_.smoothBrightness).toBool();
    task_.showTimestamp = values.value(QStringLiteral("showTimestamp"), task_.showTimestamp).toBool();
    task_.showBpmHud = values.value(QStringLiteral("showBpmHud"), task_.showBpmHud).toBool();
    task_.showCpsHud = values.value(QStringLiteral("showCpsHud"), task_.showCpsHud).toBool();
    task_.showMeterHud = values.value(QStringLiteral("showMeterHud"), task_.showMeterHud).toBool();
    task_.showModsHud = values.value(QStringLiteral("showModsHud"), task_.showModsHud).toBool();
    task_.showChartInfoHud = values.value(QStringLiteral("showChartInfoHud"), task_.showChartInfoHud).toBool();
    task_.showMeasureLines = values.value(QStringLiteral("showMeasureLines"), task_.showMeasureLines).toBool();
    task_.showJudgementSkeleton = values.value(QStringLiteral("showJudgementSkeleton"), task_.showJudgementSkeleton).toBool();
    task_.showSlideJudgementTiming = values.value(QStringLiteral("showSlideJudgementTiming"), task_.showSlideJudgementTiming).toBool();
    task_.measureLineMode = values.value(QStringLiteral("measureLineMode"), task_.measureLineMode).toInt();
    task_.measureLineCustomBeats = values.value(QStringLiteral("measureLineCustomBeats"), task_.measureLineCustomBeats).toInt();

    task_.tapFlowSpeed = values.value(QStringLiteral("tapFlowSpeed"), task_.tapFlowSpeed).toDouble();
    task_.touchFlowSpeed = values.value(QStringLiteral("touchFlowSpeed"), task_.touchFlowSpeed).toDouble();
    task_.slideTimingFromLaunchBeat =
        values.value(QStringLiteral("slideTimingFromLaunchBeat"), task_.slideTimingFromLaunchBeat).toBool();
    task_.slideTimingEarlyPerfectFromHead =
        values.value(QStringLiteral("slideTimingEarlyPerfectFromHead"), task_.slideTimingEarlyPerfectFromHead).toBool();
}

void ExportSession::stopAudition()
{
    if (engine() == nullptr) {
        return;
    }
    engine()->stopAudition();
}

void ExportSession::applyOwnerLiveFields(VideoExportTask* task) const
{
    if (task == nullptr || preview() == nullptr) {
        return;
    }
    task->outlineVariant = appearance_->outlineVariant();
    task->slideEarlierSecondAndTextOnTop = appearance_->slideEarlierSecondAndTextOnTop();
    task->tapJudgeTextDistance = appearance_->tapJudgeTextDistance();
    task->judgeEffectStyle = appearance_->judgeEffectStyle();
    task->centerDisplayMode = appearance_->centerDisplayMode();
    task->muriRenderOptions = engine()->muriRenderOptions();
}

VideoExportTask ExportSession::buildRequestedTask() const
{
    VideoExportTask task = task_;
    task.outputWidth = kResolutionPresets[qBound(0, resolutionIndex_, static_cast<int>(std::size(kResolutionPresets)) - 1)].width;
    task.outputHeight = kResolutionPresets[qBound(0, resolutionIndex_, static_cast<int>(std::size(kResolutionPresets)) - 1)].height;
    task.fullRangeExport = miacode::video_export::isFullRangeVideoExport(task.exportStartSeconds);
    task.intro.enabled = task.intro.enabled && task.fullRangeExport;
    applyOwnerLiveFields(&task);
    return task;
}

void ExportSession::startExport()
{
    if (engine() == nullptr || exportRunning_) {
        return;
    }
    const quint64 coverGeneration = singleCoverJob_.begin();
    singleCoverSourceChanged_ = false;
    singleCoverVideoLaunched_ = false;
    ++pagePrepareGeneration_;
    if (!hasSeededTask_) {
        seedFromDifficulty(selectedDifficultyId_);
        syncAudition();
    }
    adoptPreviewRenderSettings();
    if (activeTab_ == QLatin1String("batch")) {
        if (batchOutputDirectory_.isEmpty() || !QDir::isAbsolutePath(batchOutputDirectory_)) {
            browseBatchOutputDirectory();
            return;
        }
        savePreferences();
        batchCancellationRequested_ = false;
        batchExportRunning_ = true;
        exportRunning_ = true;
        emit exportRunningChanged();
        miacode::ExportEngine::BatchResult result;
        miacode::ExportEngine::BatchCallbacks callbacks;
        // Batch runs synchronously on the UI thread, so it reports onto the same
        // shell overlay every other job uses. Before this the callback pumped
        // events and threw the percentage away, leaving batch with no progress
        // at all.
        miacode::JobProgressService* const jobProgress =
            jobProgress_;
        const miacode::LocalizedText batchJobTitle = miacode::localizedText("action.batch_export");
        quint64 batchJobToken = 0;
        if (jobProgress != nullptr) {
            batchJobToken = jobProgress->begin(
                batchJobTitle,
                miacode::localizedText("export.preparing_package"),
                /*cancellable=*/true,
                miacode::JobProgressService::TaskType::ChartExport);
        }
        callbacks.progressChanged = [jobProgress, batchJobToken](int percent, const QString& label) {
            if (jobProgress != nullptr && jobProgress->token() == batchJobToken) {
                jobProgress->report(percent, label);
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        };
        callbacks.cancellationRequested = [this, jobProgress, batchJobToken]() {
            const bool shellCancelled = jobProgress != nullptr
                && jobProgress->token() == batchJobToken
                && jobProgress->cancelRequested();
            return batchCancellationRequested_ || shellCancelled;
        };
        const QStringList coverPresets = batchCoverPresets_;
        if (!coverPresets.isEmpty()) {
            callbacks.exportCover = [this, coverPresets](const VideoExportTask& task,
                const QString& exportPath, QString* error, QStringList* adjustments) {
                QStringList paths, errors;
                for (const auto& token : coverPresets) {
                    const auto cover = CoverExportSession::exportBatchCover(
                        *engine(), *uiRequests_, task, token,
                        miacode::cover_export::companionCoverOutputPath(exportPath, token, coverPresets.size() > 1), adjustments);
                    if (cover.success) paths.append(cover.outputPath);
                    else errors.append(token + QStringLiteral(": ") + cover.errorMessage);
                }
                if (error) *error = errors.join(QLatin1Char('\n'));
                return paths;
            };
        }
        VideoExportTask batchTask = buildRequestedTask();
        // Batch is always full-range, so keep the user's intro preference even
        // if the single-export range currently starts after chart zero.
        batchTask.intro.enabled = task_.intro.enabled;
        QString error;
        bool launched = false;
        {
            const QScopedValueRollback pushing(pushingSharedSettings_, true);
            launched = engine()->launchBatchExport(
                batchTask,
                chartDirectories_,
                batchSelectedDifficultyIds_,
                batchOutputDirectory_,
                &result,
                callbacks,
                &error);
        }
        if (jobProgress != nullptr && jobProgress->token() == batchJobToken) {
            jobProgress->end();
        }
        batchExportRunning_ = false;
        exportRunning_ = false;
        emit exportRunningChanged();
        if (launched && result.successCount > 0) {
            resetIntroSelection();
        }
        const miacode::LocalizedText batchTitle = miacode::localizedText("action.batch_export");
        if (!launched) {
            uiRequests_->postNotice(
                miacode::NoticeSeverity::Error,
                batchTitle,
                error.isEmpty()
                    ? miacode::localizedText("dialog.video_export.error.failed")
                    : miacode::LocalizedText(error));
            return;
        }
        if (result.canceled) {
            uiRequests_->postNotice(
                miacode::NoticeSeverity::Information,
                batchTitle,
                miacode::localizedText("dialog.batch_export.message.canceled"));
            return;
        }
        const auto shortenDetails = [](QString details) {
            return details.size() > 3000 ? details.left(3000) + QStringLiteral("\n...") : details;
        };
        QStringList details = result.exportedFiles;
        details.append(result.failedCharts);
        details.append(result.failedCovers);
        details.append(result.coverAdjustments);
        const miacode::LocalizedText summary = result.failedCharts.isEmpty()
            ? miacode::localizedText("dialog.batch_export.message.success").arg(result.successCount)
            : miacode::localizedText("dialog.batch_export.message.partial_failed")
                .arg(result.successCount).arg(result.failedCharts.size());
        const miacode::LocalizedText coverSummary = coverPresets.isEmpty() ? miacode::LocalizedText()
            : miacode::LocalizedText(QStringLiteral("\n")) + miacode::localizedText("dialog.batch_export.cover_summary")
                .arg(result.coverSuccessCount).arg(result.failedCovers.size());
        uiRequests_->postNotice(
            result.failedCharts.isEmpty() && result.failedCovers.isEmpty()
                ? miacode::NoticeSeverity::Information : miacode::NoticeSeverity::Warning,
            batchTitle, summary + coverSummary,
            shortenDetails(details.join(QLatin1Char('\n'))));
        return;
    }

    if (!difficultyExists(selectedDifficultyId_)) {
        return;
    }
    savePreferences();
    QString error;
    exportRunning_ = true;
    emit exportRunningChanged();
    // Freeze parsed notes, presentation, preset, resources and preview time
    // before renderCoverComposite pumps events or the worker starts.
    const int difficultyId = selectedDifficultyId_;
    const VideoExportTask requestedTask = buildRequestedTask();
    const QStringList coverPresets = singleCoverPresets_;
    const double previewSecond = preview() && preview()->positionSeconds() < 0
        ? preview()->positionSeconds() : engine()->currentAudioClockSecond();
    if (!coverPresets.isEmpty()) {
        VideoExportTask coverTask = requestedTask;
        coverTask.contentDurationSeconds = chartDurationSeconds_;
        coverTask.exportStartSeconds = 0;
        QVector<miacode::cover_export::CoverCompanionJob::NamedImage> images;
        for (const auto& token : coverPresets) {
            QString coverError;
            QImage cover = CoverExportSession::renderPresetCover(
                *engine(), *uiRequests_, coverTask, token, previewSecond, &coverError);
        if (singleCoverJob_.generation() != coverGeneration) {
                exportRunning_ = false;
                emit exportRunningChanged();
                if (singleCoverSourceChanged_) {
                    uiRequests_->postNotice(miacode::NoticeSeverity::Warning,
                        miacode::localizedText("dialog.video_export.title"),
                        miacode::localizedText("dialog.video_export.cover_source_changed"));
                }
                return;
            }
            images.append({token, std::move(cover), std::move(coverError)});
        }
        singleCoverJob_.prepareMany(coverGeneration, std::move(images));
    }
    if (singleCoverJob_.generation() != coverGeneration) {
        exportRunning_ = false;
        emit exportRunningChanged();
        if (singleCoverSourceChanged_) uiRequests_->postNotice(miacode::NoticeSeverity::Warning,
            miacode::localizedText("dialog.video_export.title"),
            miacode::localizedText("dialog.video_export.cover_source_changed"));
        return;
    }
    bool launched = false;
    {
        const QScopedValueRollback pushing(pushingSharedSettings_, true);
        launched = engine()->launchVideoExport(requestedTask, difficultyId, &error);
        singleCoverVideoLaunched_ = launched;
    }
    if (!launched) {
        singleCoverJob_.cancel();
        exportRunning_ = false;
        emit exportRunningChanged();
        uiRequests_->postNotice(
            miacode::NoticeSeverity::Error,
            miacode::localizedText("dialog.video_export.title"),
            error.isEmpty()
                ? miacode::localizedText("dialog.video_export.error.launch_failed")
                : miacode::LocalizedText(error));
        return;
    }
}

void ExportSession::invalidateSingleCoverPreparation()
{
    if (singleCoverJob_.invalidatePreparation()) singleCoverSourceChanged_ = true;
}

void ExportSession::cancelExport()
{
    if (engine() == nullptr) {
        return;
    }
    if (batchExportRunning_) {
        batchCancellationRequested_ = true;
        return;
    }
    singleCoverVideoLaunched_ = false;
    singleCoverJob_.cancel();
    engine()->cancelVideoExport();
}

void ExportSession::browseOutputPath()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("dialog.video_export.title");
    const QString directory = task_.chartPath.isEmpty() ? QDir::currentPath() : QFileInfo(task_.chartPath).absolutePath();
    request.startPath = QDir(directory).absoluteFilePath(task_.outputPath.isEmpty()
        ? (task_.outputMode == VideoExportOutputMode::Wav ? QStringLiteral("out.wav") : QStringLiteral("out.mp4"))
        : task_.outputPath);
    request.nameFilters = {miacode::localizedText(
        task_.outputMode == VideoExportOutputMode::Wav ? "file_filter.wav" : "file_filter.mp4")};
    request.saveMode = true;
    uiRequests_->requestFile(request, [this](const QString& path) {
        if (!path.isEmpty()) {
            setOutputPath(path);
        }
    });
}

void ExportSession::browseIntroBackground()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("dialog.video_export.choose_intro_background");
    request.startPath = task_.intro.customBackgroundPath;
    request.nameFilters = {miacode::localizedText("file_filter.image")};
    uiRequests_->requestFile(request, [this](const QString& path) {
        if (!path.isEmpty()) {
            setIntroCustomBackgroundPath(path);
        }
    });
}

void ExportSession::importIntroSound()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("dialog.render_settings.music.intro_sound");
    request.startPath = miacode::file_dialog_preferences::recentDirectory(miacode::file_dialog_preferences::Purpose::IntroSound);
    request.nameFilters = {miacode::localizedText("file_filter.audio")};
    uiRequests_->requestFile(request, [this](const QString& path) {
        miacode::file_dialog_preferences::rememberFile(miacode::file_dialog_preferences::Purpose::IntroSound, path);
        applyIntroSoundImport(path);
    });
}

void ExportSession::importIntroFont()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("card_font.import");
    request.startPath = miacode::file_dialog_preferences::recentDirectory(miacode::file_dialog_preferences::Purpose::IntroFont);
    request.nameFilters = {miacode::localizedText("file_filter.font")};
    uiRequests_->requestFile(request, [this](const QString& path) {
        miacode::file_dialog_preferences::rememberFile(miacode::file_dialog_preferences::Purpose::IntroFont, path);
        applyFontImport(path);
    });
}

void ExportSession::applyFontImport(const QString& selectedPath)
{
    if (selectedPath.isEmpty()) {
        return;
    }
    const miacode::video_export::FontImportResult result =
        miacode::video_export::importFontFileIntoLibrary(selectedPath, miacode::app_preferences::fontLibraryDirectory());
    if (result.path.isEmpty()) {
        const miacode::LocalizedText text = result.failure == miacode::video_export::FontImportFailure::CopyFailed
            ? miacode::localizedText("card_font.copy_failed")
            : miacode::localizedText("card_font.invalid_font");
        uiRequests_->postNotice(miacode::NoticeSeverity::Warning,
                                miacode::localizedText("card_font.import"), text);
        return;
    }

    fontLibraryOptionsCacheValid_ = false;
    emit fontLibraryChanged();
    // Match the established card-font picker: an imported font becomes the
    // title/display choice while the body selection remains independent.
    setIntroFontDisplayPath(result.path);
}

void ExportSession::applyIntroSoundImport(const QString& selectedPath)
{
    if (selectedPath.isEmpty()) {
        return;
    }

    const QString musicDirectory = miacode::preview_sfx::assetMusicDirectory();
    if (musicDirectory.isEmpty() || !QDir().mkpath(musicDirectory)) {
        return;
    }

    const QFileInfo sourceInfo(selectedPath);
    QString importedName = sourceInfo.fileName();
    QString importedPath = QDir(musicDirectory).filePath(importedName);
    if (QFileInfo(selectedPath).canonicalFilePath() != QFileInfo(importedPath).canonicalFilePath()) {
        int suffix = 2;
        while (QFileInfo::exists(importedPath)) {
            importedName = QStringLiteral("%1_%2.%3")
                .arg(sourceInfo.completeBaseName())
                .arg(suffix++)
                .arg(sourceInfo.suffix());
            importedPath = QDir(musicDirectory).filePath(importedName);
        }
        if (!QFile::copy(selectedPath, importedPath)) {
            return;
        }
    }

    emit introSoundOptionsChanged();
    setIntroSoundFileName(importedName);
}

void ExportSession::browseBatchOutputDirectory()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("dialog.batch_export.select_folder");
    request.startPath = miacode::file_dialog_preferences::validDirectory(batchOutputDirectory_);
    if (request.startPath.isEmpty()) {
        request.startPath = QDir::homePath();
    }
    request.selectFolder = true;
    uiRequests_->requestFile(request, [this](const QString& path) {
        if (!path.isEmpty()) {
            setBatchOutputDirectory(path);
        }
    });
}

void ExportSession::addChartDirectories()
{
    miacode::FileRequest request;
    request.title = miacode::localizedText("dialog.batch_export.select_charts");
    request.selectFolder = true;
    const QJsonObject preferences = miacode::app_preferences::loadDialogPreferences();
    request.startPath = preferences.value(QStringLiteral("last_batch_chart_directory")).toString();
    if (request.startPath.isEmpty()) {
        const QString legacy = legacyBatchExportDirectory("last_chart_directory");
        if (!legacy.isEmpty()) request.startPath = QFileInfo(legacy).absolutePath();
    }
    if (!QDir(request.startPath).exists() || request.startPath.isEmpty()) {
        request.startPath = task_.chartPath.isEmpty() ? QDir::homePath()
            : QFileInfo(QFileInfo(task_.chartPath).absolutePath()).absolutePath();
    }
    uiRequests_->requestFiles(request, [self = QPointer<ExportSession>(this)](const QStringList& paths) {
        if (!self || paths.isEmpty()) return;
        for (const QString& path : paths) self->addChartDirectory(path);
        // Open the containing directory next time so sibling charts can be
        // added without repeatedly navigating back out of a chart folder.
        QJsonObject preferences = miacode::app_preferences::loadDialogPreferences();
        preferences.insert(QStringLiteral("last_batch_chart_directory"), QFileInfo(paths.last()).absolutePath());
        miacode::app_preferences::saveDialogPreferences(preferences);
    });
}

void ExportSession::addChartDirectory(const QString& path)
{
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        return;
    }
    const QString normalized = QFileInfo(path).canonicalFilePath();
    if (chartDirectories_.contains(normalized)) return;
    chartDirectories_.append(normalized);
    emit batchChanged();
}

void ExportSession::removeChartDirectory(int index)
{
    if (index < 0 || index >= chartDirectories_.size()) {
        return;
    }
    chartDirectories_.removeAt(index);
    emit batchChanged();
}

void ExportSession::clearChartDirectories()
{
    if (chartDirectories_.isEmpty()) {
        return;
    }
    chartDirectories_.clear();
    emit batchChanged();
}

void ExportSession::setBatchDifficultyChecked(int difficultyId, bool checked)
{
    if (!SimaiDocument::isDifficultyId(difficultyId)) {
        return;
    }
    if (checked) {
        if (!batchSelectedDifficultyIds_.contains(difficultyId)) {
            batchSelectedDifficultyIds_.append(difficultyId);
            emit batchChanged();
        }
    } else {
        if (batchSelectedDifficultyIds_.removeAll(difficultyId) > 0) {
            emit batchChanged();
        }
    }
}

void ExportSession::setExportStartToCurrentPreview()
{
    if (engine() == nullptr) {
        return;
    }
    setExportStartSeconds(engine()->currentAudioClockSecond());
}

void ExportSession::setExportEndToCurrentPreview()
{
    if (engine() == nullptr) {
        return;
    }
    setExportEndSeconds(engine()->currentAudioClockSecond());
}

void ExportSession::setExportRangeSeconds(double start, double end)
{
    applyExportRangeSeconds(start, end, minimumExportRangeSeconds());
}

void ExportSession::applyExportRangeSeconds(double start, double end, double minimumDuration)
{
    if (!qIsFinite(start) || !qIsFinite(end)) {
        return;
    }
    const double boundedStart = qBound(0.0, start, qMax(0.0, chartDurationSeconds_ - minimumDuration));
    const double boundedEnd = qBound(boundedStart + minimumDuration, end, chartDurationSeconds_);
    if (boundedStart == task_.exportStartSeconds && boundedEnd == exportEndSeconds()) {
        return;
    }
    const bool previousFullRangeExport = task_.fullRangeExport;
    task_.exportStartSeconds = boundedStart;
    task_.contentDurationSeconds = qMax(0.0, boundedEnd - boundedStart);
    task_.fullRangeExport = miacode::video_export::isFullRangeVideoExport(task_.exportStartSeconds);
    if (task_.intro.enabled && previousFullRangeExport != task_.fullRangeExport && engine() != nullptr) {
        engine()->refreshIntroState();
    }
    emit rangeChanged();
    emit introChanged();
}

void ExportSession::requestSelectionRangeExport(double startSecond, double endSecond)
{
    if (!qIsFinite(startSecond) || !qIsFinite(endSecond) || endSecond <= startSecond) {
        return;
    }
    hasPendingSelectionRangeExport_ = true;
    pendingRangeStartSeconds_ = startSecond;
    pendingRangeEndSeconds_ = endSecond;
}

void ExportSession::clearPendingSelectionRangeExport()
{
    hasPendingSelectionRangeExport_ = false;
}

QString ExportSession::setExportStartText(const QString& text)
{
    double seconds = 0.0;
    const QString normalized = miacode::video_export::sanitizeVideoExportTimestamp(text);
    bool parsed = false;
    if (normalized.contains(QLatin1Char(':'))) {
        parsed = miacode::video_export::parseVideoExportTimestamp(normalized, &seconds);
    } else {
        seconds = normalized.toDouble(&parsed);
    }
    if (parsed) {
        setExportStartSeconds(seconds);
    }
    return QString::number(task_.exportStartSeconds, 'f', 3);
}

QString ExportSession::setIntroPvStartText(const QString& text)
{
    double seconds = 0.0;
    const QString normalized = miacode::video_export::sanitizeVideoExportTimestamp(text);
    bool parsed = false;
    if (normalized.contains(QLatin1Char(':'))) {
        parsed = miacode::video_export::parseVideoExportTimestamp(normalized, &seconds);
    } else {
        seconds = normalized.toDouble(&parsed);
    }
    if (parsed) {
        setIntroPvStartSeconds(seconds);
    }
    return QString::number(task_.intro.pvPreviewStartSeconds, 'f', 3);
}

QString ExportSession::setExportEndText(const QString& text)
{
    double seconds = 0.0;
    const QString normalized = miacode::video_export::sanitizeVideoExportTimestamp(text);
    bool parsed = false;
    if (normalized.contains(QLatin1Char(':'))) {
        parsed = miacode::video_export::parseVideoExportTimestamp(normalized, &seconds);
    } else {
        seconds = normalized.toDouble(&parsed);
    }
    if (parsed) {
        setExportEndSeconds(seconds);
    }
    return QString::number(exportEndSeconds(), 'f', 3);
}

void ExportSession::setOutputPath(const QString& path)
{
    if (task_.outputPath == path) {
        return;
    }
    task_.outputPath = path.trimmed().isEmpty() ? QString() : videoExportPrimaryOutputPath(path, task_.outputMode);
    emit outputChanged();
}

void ExportSession::setResolutionIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(std::size(kResolutionPresets)) || resolutionIndex_ == index) {
        return;
    }
    resolutionIndex_ = index;
    task_.outputWidth = kResolutionPresets[index].width;
    task_.outputHeight = kResolutionPresets[index].height;
    emit outputChanged();
    savePreferences();
    if (pageSessionActive_) {
        applyLivePreviewSettings();
    }
}

void ExportSession::setFps(int fps)
{
    if (task_.fps == fps) {
        return;
    }
    task_.fps = fps;
    emit outputChanged();
    savePreferences();
}

void ExportSession::setAudioBitrateKbps(int kbps)
{
    if (task_.audioBitrateKbps == kbps) {
        return;
    }
    task_.audioBitrateKbps = kbps;
    emit outputChanged();
    savePreferences();
}

void ExportSession::setPresetIndex(int index)
{
    const VideoExportPreset next = index == 0 ? VideoExportPreset::Fast : VideoExportPreset::HighQuality;
    if (task_.preset == next) {
        return;
    }
    task_.preset = next;
    emit outputChanged();
    savePreferences();
}

void ExportSession::setSizePresetIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(kSizePresets.size()) || sizePresetIndex() == index) return;
    task_.sizePreset = kSizePresets[index];
    if (task_.sizePreset == VideoExportSizePreset::Target30Mb) {
        const double aspect = kResolutionPresets[resolutionIndex_].aspectRatio;
        for (int i = 0; i < static_cast<int>(kResolutionPresets.size()); ++i) {
            if (kResolutionPresets[i].height == 480 && qAbs(kResolutionPresets[i].aspectRatio - aspect) < 1e-6) {
                setResolutionIndex(i);
                break;
            }
        }
    }
    emit outputChanged();
    savePreferences();
}

void ExportSession::setTargetVideoBitrateMbps(double value)
{
    if (!std::isfinite(value)) return;
    const double bitrate = miacode::video_export::normalizedCustomVideoBitrateMbps(value);
    if (qFuzzyCompare(task_.targetVideoBitrateMbps, bitrate)) return;
    task_.targetVideoBitrateMbps = bitrate;
    emit outputChanged();
    savePreferences();
}

void ExportSession::setIncludePv(bool include)
{
    if (task_.includePv == include) return;
    task_.includePv = include;
    emit outputChanged();
    emit introChanged();
    savePreferences();
    applyLivePreviewSettings();
    if (engine()) engine()->refreshIntroState();
}

void ExportSession::setShowObjectStatsHud(bool value)
{
    task_.showObjectStatsHud = value;
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowBpmHud(bool value)
{
    if (task_.showBpmHud == value) return;
    task_.showBpmHud = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showBpmHud"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowCpsHud(bool value)
{
    if (task_.showCpsHud == value) return;
    task_.showCpsHud = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showCpsHud"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowMeterHud(bool value)
{
    if (task_.showMeterHud == value) return;
    task_.showMeterHud = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showMeterHud"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowModsHud(bool value)
{
    if (task_.showModsHud == value) return;
    task_.showModsHud = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showModsHud"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowChartInfoHud(bool value)
{
    task_.showChartInfoHud = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showChartInfoHud"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}


void ExportSession::setShowMeasureLines(bool value)
{
    if (task_.showMeasureLines == value) return;
    task_.showMeasureLines = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showMeasureLines"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setSlideTimingFromLaunchBeat(bool value)
{
    if (task_.slideTimingFromLaunchBeat == value) return;
    task_.slideTimingFromLaunchBeat = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("slideTimingFromLaunchBeat"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setSlideTimingEarlyPerfectFromHead(bool value)
{
    if (task_.slideTimingEarlyPerfectFromHead == value) return;
    task_.slideTimingEarlyPerfectFromHead = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("slideTimingEarlyPerfectFromHead"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}


void ExportSession::setShowSlideJudgementTiming(bool value)
{
    if (task_.showSlideJudgementTiming == value) return;
    task_.showSlideJudgementTiming = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showSlideJudgementTiming"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setShowJudgementSkeleton(bool value)
{
    if (task_.showJudgementSkeleton == value) return;
    task_.showJudgementSkeleton = value;
    if (preview() != nullptr) preview()->setRenderSetting(QStringLiteral("showJudgementSkeleton"), value);
    emit videoChanged();
    applyLivePreviewSettings();
}

void ExportSession::setClockCountEnabled(bool value)
{
    task_.clockCountEnabled = value;
    emit videoChanged();
    savePreferences();
}

void ExportSession::setIntroEnabled(bool value)
{
    task_.intro.enabled = value;
    emit introChanged();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroBackgroundModeIndex(int index)
{
    task_.intro.backgroundMode = index == 1 ? QStringLiteral("custom") : QStringLiteral("jacket");
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroCustomBackgroundPath(const QString& path)
{
    task_.intro.customBackgroundPath = path;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroBlurBackground(bool value)
{
    task_.intro.blurBackground = value;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroModeIndex(int index)
{
    if (index == 0) {
        task_.intro.mode = QStringLiteral("auto");
    } else if (index == 2) {
        task_.intro.mode = QStringLiteral("Standard");
    } else {
        task_.intro.mode = QStringLiteral("DX");
    }
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroCardShadow(bool value)
{
    task_.intro.cardShadow = value;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroLevelTextRender(bool value)
{
    task_.intro.lvRenderMode = value ? QStringLiteral("text") : QStringLiteral("atlas");
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroFontDisplayPath(const QString& path)
{
    if (task_.intro.fontDisplayPath == path) {
        return;
    }
    task_.intro.fontDisplayPath = path;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroFontBodyPath(const QString& path)
{
    if (task_.intro.fontBodyPath == path) {
        return;
    }
    task_.intro.fontBodyPath = path;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

double ExportSession::introPvSegmentSeconds() const
{
    const double duration = task_.intro.pvPreviewEndSeconds - task_.intro.pvPreviewStartSeconds;
    return duration > 0.0 ? duration : miacode::intro::kPvPreviewHoldSeconds;
}

double ExportSession::introPvMusicSeconds() const
{
    return introPreviewMusicSeconds(task_.intro);
}

bool ExportSession::introPvVideoAvailable() const
{
    return task_.includePv && (task_.intro.chartPreview || engine() == nullptr
        || engine()->muriRenderOptions().renderMode != RenderMode::Debug)
        && miacode::chart_assets::isVideoBackgroundPath(task_.backgroundMediaPath)
        && QFileInfo::exists(task_.backgroundMediaPath);
}

QString ExportSession::introSourceLabel() const
{
    using Source = miacode::chart_preview::IntroWindowSource;
    switch (task_.intro.previewWindowSource) {
    case Source::Explicit: return qtTrId("video_export.intro_source_manual");
    case Source::Demo: return qtTrId("video_export.intro_source_demo");
    case Source::Drop: return qtTrId("video_export.intro_source_drop");
    case Source::Default: return qtTrId("video_export.intro_source_default");
    }
    return {};
}

int ExportSession::introWindowModeIndex() const
{
    return task_.intro.previewWindowMode == miacode::chart_preview::IntroWindowMode::FollowDrop ? 1 : 0;
}

void ExportSession::setIntroWindowModeIndex(int index)
{
    if (index < 0 || index > 1) return;
    task_.intro.previewWindowMode = index == 1 ? miacode::chart_preview::IntroWindowMode::FollowDrop
                                              : miacode::chart_preview::IntroWindowMode::Automatic;
    clearIntroWindowOverride();
}

void ExportSession::clearIntroWindowOverride()
{
    introExplicitRange_.reset();
    introAuditionFromHead_ = true;
    refreshIntroWindow(false);
    emit introChanged();
    if (engine()) engine()->refreshIntroState();
}

void ExportSession::setIntroStyleIndex(int index)
{
    if (index < 0 || index > 2 || introStyleIndex() == index) return;
    task_.intro.pvPreview = index != 0;
    task_.intro.chartPreview = index == 2;
    refreshIntroWindow(false);
    emit introChanged();
    if (engine()) engine()->refreshIntroState();
}

void ExportSession::setIntroPvPreview(bool value)
{
    setIntroStyleIndex(value ? 1 : 0);
}

void ExportSession::setIntroPvStartSeconds(double seconds)
{
    if (!std::isfinite(seconds)) return;
    const double duration = introMediaDuration();
    const double latestStart = qMax(0.0, duration - introPvSegmentSeconds());
    const double start = qBound(0.0, seconds, latestStart);
    if (introExplicitRange_ && qAbs(introExplicitRange_->startSeconds - start) < 1e-6) return;
    introExplicitRange_ = miacode::chart_preview::MediaRange{start, start + introPvSegmentSeconds()};
    introAuditionFromHead_ = true;
    refreshIntroWindow(false);
    emit introChanged();
    if (engine()) engine()->refreshIntroState();
}

bool ExportSession::introAuditionPlaying() const
{
    return engine() != nullptr && engine()->introAuditionPlaying();
}

void ExportSession::toggleIntroAudition()
{
    if (engine() == nullptr) {
        return;
    }
    engine()->setIntroAuditionPlaying(!engine()->introAuditionPlaying());
    emit introPreviewStateChanged();
}

void ExportSession::noteIntroPvSeek()
{
    introAuditionFromHead_ = false;
}

void ExportSession::setIntroPvBrightness(double value)
{
    if (!std::isfinite(value)) {
        return;
    }
    value = qBound(0.0, value, 1.0);
    if (qAbs(value - task_.intro.pvPreviewBrightness) < 1e-6) {
        return;
    }
    task_.intro.pvPreviewBrightness = value;
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroPvSegmentDragging(bool dragging)
{
    if (introPvSegmentDragging_ == dragging) {
        return;
    }
    introPvSegmentDragging_ = dragging;
    emit introPreviewStateChanged();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroAuditionLoop(bool loop)
{
    if (introAuditionLoop_ == loop) {
        return;
    }
    introAuditionLoop_ = loop;
    emit introPreviewStateChanged();
}

void ExportSession::resetIntroSelection()
{
    task_.intro.enabled = true;
    task_.intro.pvPreview = false;
    task_.intro.chartPreview = false;
    introExplicitRange_.reset();
    refreshIntroWindow(false);
    introPvSegmentDragging_ = false;
    introAuditionFromHead_ = true;
    emit introChanged();
    emit introPreviewStateChanged();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::resetIntroFonts()
{
    const bool changed = !task_.intro.fontDisplayPath.isEmpty() || !task_.intro.fontBodyPath.isEmpty();
    task_.intro.fontDisplayPath.clear();
    task_.intro.fontBodyPath.clear();
    if (!changed) {
        return;
    }
    emit introChanged();
    savePreferences();
    if (engine() != nullptr) {
        engine()->refreshIntroState();
    }
}

void ExportSession::setIntroSoundIndex(int index)
{
    const QVariantList options = introSoundOptions();
    if (index < 0 || index >= options.size()) {
        return;
    }
    setIntroSoundFileName(
        options.at(index).toMap().value(QStringLiteral("fileName")).toString());
}

void ExportSession::setIntroSoundFileName(const QString& fileName)
{
    const QString normalized = miacode::preview_sfx::normalizeIntroSoundFileName(fileName);
    if (task_.introSoundFileName == normalized) {
        return;
    }
    task_.introSoundFileName = normalized;
    miacode::preview_sfx::setSelectedIntroSoundFileName(normalized);
    // The window reloads the SFX bank and persists in reaction to this.
    appearance_->setIntroSoundFileName(normalized);
    emit introChanged();
}

void ExportSession::setExportStartSeconds(double value)
{
    if (!qIsFinite(value)) {
        return;
    }
    const double end = exportEndSeconds();
    const double maximumStart = qMax(0.0, end - minimumExportRangeSeconds());
    setExportRangeSeconds(qBound(0.0, value, maximumStart), end);
}

void ExportSession::setExportEndSeconds(double value)
{
    if (!qIsFinite(value)) {
        return;
    }
    const double start = task_.exportStartSeconds;
    const double minimumEnd = start + minimumExportRangeSeconds();
    setExportRangeSeconds(start, qBound(minimumEnd, value, chartDurationSeconds_));
}

QVariantList ExportSession::singleCoverPresetOptions() const
{
    return CoverExportSession::batchPresetOptions();
}

void ExportSession::setSingleCoverPreset(const QString& preset)
{
    if (singleCoverPreset_ == preset) return;
    singleCoverPreset_ = preset;
    singleCoverPresets_ = preset.isEmpty() ? QStringList{} : QStringList{preset};
    savePreferences();
    emit outputChanged();
}

QVariantList ExportSession::batchCoverPresetOptions() const
{
    return CoverExportSession::batchPresetOptions();
}

void ExportSession::setBatchCoverPreset(const QString& preset)
{
    if (batchCoverPreset_ == preset) return;
    batchCoverPreset_ = preset;
    batchCoverPresets_ = preset.isEmpty() ? QStringList{} : QStringList{preset};
    savePreferences();
    emit batchChanged();
}

void ExportSession::setBatchOutputDirectory(const QString& path)
{
    if (batchOutputDirectory_ == path) {
        return;
    }
    batchOutputDirectory_ = path.trimmed();
    emit batchChanged();
}

void ExportSession::setCoverPresetChecked(const QString& token, bool checked, bool batch)
{
    if (exportRunning_) return;
    auto& list = batch ? batchCoverPresets_ : singleCoverPresets_;
    if (token.isEmpty()) list.clear();
    else if (checked && !list.contains(token)) list.append(token);
    else if (!checked) list.removeAll(token);
    if (batch) batchCoverPreset_ = list.value(0); else singleCoverPreset_ = list.value(0);
    savePreferences();
    if (batch) emit batchChanged(); else emit outputChanged();
}

QVariantList ExportSession::videoTemplates() const
{
    QVariantList result;
    const auto templates = miacode::app_preferences::loadDialogPreferences().value(QStringLiteral("templates")).toObject();
    for (auto it = templates.begin(); it != templates.end(); ++it)
        result.append(QVariantMap{{QStringLiteral("name"), it.key()}});
    return result;
}

QJsonObject ExportSession::currentTemplateSettings() const
{
    VideoExportTask current = task_;
    applyOwnerLiveFields(&current);
    auto settings = miacode::video_export::videoExportTemplateSettings(current);
    settings.insert(QStringLiteral("skinDirectoryName"), appearance_->skinDirectoryName());
    settings.insert(QStringLiteral("single_cover_presets"), QJsonArray::fromStringList(singleCoverPresets_));
    settings.insert(QStringLiteral("batch_cover_presets"), QJsonArray::fromStringList(batchCoverPresets_));
    return settings;
}

bool ExportSession::applyTemplateSettings(const QJsonObject& settings)
{
    if (exportRunning_) return false;
    VideoExportTask candidate = task_;
    if (!miacode::video_export::applyVideoExportTemplateSettings(settings, &candidate)) return false;
    const QString skin = settings.value(QStringLiteral("skinDirectoryName")).toString();
    if (!skin.isEmpty() && (skin.contains(QLatin1Char('/')) || skin.contains(QLatin1Char('\\'))
        || !QFileInfo::exists(miacode::assets::assetPath(QStringLiteral("skin/") + skin + QStringLiteral("/tap.png"))))) return false;
    task_ = std::move(candidate);
    introExplicitRange_.reset();
    refreshIntroWindow(false);
    if (!skin.isEmpty()) appearance_->setSkinDirectory(skin);
    const auto covers = [&settings](const char* key) {
        QStringList result;
        for (const auto& value : settings.value(QLatin1String(key)).toArray())
            if (value.isString() && !value.toString().isEmpty()) result.append(value.toString());
        result.removeDuplicates();
        return result;
    };
    singleCoverPresets_ = covers("single_cover_presets");
    batchCoverPresets_ = covers("batch_cover_presets");
    singleCoverPreset_ = singleCoverPresets_.value(0);
    batchCoverPreset_ = batchCoverPresets_.value(0);
    for (int i = 0; i < static_cast<int>(std::size(kResolutionPresets)); ++i)
        if (kResolutionPresets[i].width == task_.outputWidth && kResolutionPresets[i].height == task_.outputHeight) resolutionIndex_ = i;
    appearance_->setOutlineVariant(task_.outlineVariant);
    appearance_->setJudgeEffectStyle(task_.judgeEffectStyle);
    appearance_->setTapJudgeTextDistance(task_.tapJudgeTextDistance);
    appearance_->setSlideEarlierSecondAndTextOnTop(task_.slideEarlierSecondAndTextOnTop);
    appearance_->setCenterDisplayMode(task_.centerDisplayMode);
    {
        const QScopedValueRollback pushing(pushingSharedSettings_, true);
        if (engine()) engine()->applySharedTaskSettings(task_);
    }
    savePreferences();
    emit outputChanged(); emit videoChanged(); emit introChanged(); emit batchChanged();
    if (pageSessionActive_) syncAudition();
    return true;
}

void ExportSession::saveVideoTemplate(const QString& name)
{
    const QString key = name.trimmed();
    if (key.isEmpty() || key.size() > 120 || exportRunning_) return;
    auto preferences = miacode::app_preferences::loadDialogPreferences();
    auto templates = preferences.value(QStringLiteral("templates")).toObject();
    templates.insert(key, currentTemplateSettings());
    preferences.insert(QStringLiteral("templates"), templates);
    miacode::app_preferences::saveDialogPreferences(preferences);
    emit templatesChanged();
}

void ExportSession::applyVideoTemplate(const QString& name)
{
    const auto settings = miacode::app_preferences::loadDialogPreferences().value(QStringLiteral("templates")).toObject().value(name).toObject();
    if (!applyTemplateSettings(settings))
        uiRequests_->postNotice(miacode::NoticeSeverity::Error, miacode::localizedText("video_template.title"), miacode::localizedText("video_template.invalid"));
}

void ExportSession::renameVideoTemplate(const QString& oldName, const QString& newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty() || name.size() > 120 || exportRunning_) return;
    auto preferences = miacode::app_preferences::loadDialogPreferences();
    auto templates = preferences.value(QStringLiteral("templates")).toObject();
    if (!templates.contains(oldName) || templates.contains(name)) return;
    templates.insert(name, templates.take(oldName));
    preferences.insert(QStringLiteral("templates"), templates);
    miacode::app_preferences::saveDialogPreferences(preferences);
    emit templatesChanged();
}

void ExportSession::removeVideoTemplate(const QString& name)
{
    if (exportRunning_) return;
    auto preferences = miacode::app_preferences::loadDialogPreferences();
    auto templates = preferences.value(QStringLiteral("templates")).toObject();
    templates.remove(name);
    preferences.insert(QStringLiteral("templates"), templates);
    miacode::app_preferences::saveDialogPreferences(preferences);
    emit templatesChanged();
}

void ExportSession::exportVideoTemplate()
{
    FileRequest request;
    request.title = miacode::localizedText("video_template.export");
    request.saveMode = true;
    request.startPath = QStringLiteral("video-template.miatemplate");
    request.defaultSuffix = QStringLiteral("miatemplate");
    request.nameFilters = {miacode::localizedText("video_template.file_filter")};
    const QJsonObject object{{QStringLiteral("kind"), QStringLiteral("better-miacode-video-template")},
        {QStringLiteral("version"), 1}, {QStringLiteral("settings"), currentTemplateSettings()}};
    QPointer<ExportSession> guard(this);
    uiRequests_->requestFile(request, [guard, object](const QString& path) {
        if (!guard || path.isEmpty()) return;
        const QByteArray bytes = QJsonDocument(object).toJson();
        const QString suffix = QStringLiteral(".miatemplate");
        QString targetPath = path;
        // Older dialogs and user-entered names can carry either suffix order.
        // Keep the JSON payload, but write exactly one application file suffix.
        while (targetPath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
               || targetPath.endsWith(suffix, Qt::CaseInsensitive)) {
            targetPath.chop(targetPath.endsWith(suffix, Qt::CaseInsensitive) ? suffix.size() : 5);
        }
        targetPath += suffix;
        if (targetPath != path && QFileInfo::exists(targetPath)) {
            guard->uiRequests_->postNotice(miacode::NoticeSeverity::Error,
                miacode::localizedText("video_template.title"), miacode::localizedText("video_template.path_exists"), targetPath);
            return;
        }
        QSaveFile file(targetPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            guard->uiRequests_->postNotice(miacode::NoticeSeverity::Error, miacode::localizedText("video_template.title"), miacode::LocalizedText(file.errorString()));
    });
}

void ExportSession::importVideoTemplate()
{
    FileRequest request;
    request.title = miacode::localizedText("video_template.import");
    request.nameFilters = {miacode::localizedText("video_template.compatible_file_filter")};
    QPointer<ExportSession> guard(this);
    uiRequests_->requestFile(request, [guard](const QString& path) {
        if (!guard || path.isEmpty()) return;
        QFile file(path);
        const bool readable = file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024;
        const auto object = readable ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
        if (object.value(QStringLiteral("kind")).toString() != QLatin1String("better-miacode-video-template")
            || object.value(QStringLiteral("version")).toInt() != 1
            || !guard->applyTemplateSettings(object.value(QStringLiteral("settings")).toObject())) {
            guard->uiRequests_->postNotice(miacode::NoticeSeverity::Error, miacode::localizedText("video_template.title"), miacode::localizedText("video_template.invalid"));
            return;
        }
        QString name = QFileInfo(path).completeBaseName();
        auto existing = miacode::app_preferences::loadDialogPreferences().value(QStringLiteral("templates")).toObject();
        const QString base = name;
        for (int i = 2; existing.contains(name); ++i) name = base + QStringLiteral(" (%1)").arg(i);
        guard->saveVideoTemplate(name);
    });
}

} // namespace miacode::ui
