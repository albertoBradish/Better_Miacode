#include "app/ui/preferences/PreferencesModel.h"
#include "app/services/HandOrderFeaturePreferences.h"
#include "app/services/VideoWatermarkPreferences.h"
#include "app/ui/preferences/AppearanceLabels.h"
#include "app/ui/preferences/HandOrderPlayerPreferences.h"
#include "app/ui/preferences/HandOrderSweepPreferences.h"
#include "app/ui/preferences/HandOrderSearchPreferences.h"

#include "app/ui/layout/WorkbenchSettings.h"
#include "app/services/PreferencesStore.h"
#include "app/runtime/Shared.h"
#include "app/ui/preferences/LocaleService.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/EditorTypography.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/model/HandOrderSweepThreshold.h"

#include <QVariantMap>
#include <QCoreApplication>
#include <QFontDatabase>
#include <algorithm>
#include <cmath>

namespace miacode::ui {

namespace {

QVariantMap option(const QVariant& value, const QString& label)
{
    QVariantMap row;
    row.insert(QStringLiteral("value"), value);
    row.insert(QStringLiteral("label"), label);
    return row;
}

}  // namespace

PreferencesModel::PreferencesModel(miacode::PreferencesStore*& storeSlot,
                                         WorkbenchSettings& settings, QObject* parent)
    : QObject(parent)
    , storeSlot_(&storeSlot)
    , settings_(&settings)
{
    connect(&settings, &WorkbenchSettings::workflowSettingsChanged, this, [this] {
        if (store()) store()->setLowPerformanceMode(settings_->lowPerformanceMode());
        emit performanceChanged();
    });
    completionDurations_.setHalfWidthInputEnabled(editorHalfWidthInput());
    completionDivisions_.setHalfWidthInputEnabled(editorHalfWidthInput());
    reloadCompletionCandidates();
    connect(&settings, &WorkbenchSettings::editorSettingsChanged,
            this, &PreferencesModel::reloadCompletionCandidates);
    connect(&settings, &WorkbenchSettings::editorSettingsChanged,
            this, &PreferencesModel::editorChanged);
    connect(&settings, &WorkbenchSettings::themeChanged,
            this, &PreferencesModel::interfaceChanged);
    connect(&completionDurations_, &CompletionCandidateModel::savedValuesChanged,
            this, &PreferencesModel::persistCompletionCandidates);
    connect(&completionDivisions_, &CompletionCandidateModel::savedValuesChanged,
            this, &PreferencesModel::persistCompletionCandidates);
    connect(&settings, &WorkbenchSettings::blurMaterialsEnabledChanged,
            this, &PreferencesModel::interfaceChanged);
    connect(&settings, &WorkbenchSettings::uiScalePercentChanged,
            this, &PreferencesModel::interfaceChanged);
    connect(&miacode::LocaleService::instance(), &miacode::LocaleService::languageChanged,
            this, [this](const QString&) {
                completionDurations_.retranslateErrors();
                completionDivisions_.retranslateErrors();
                emit interfaceChanged();
                emit editorChanged();
                emit performanceChanged();
            });
}

QVariantList PreferencesModel::languageOptions() const
{
    QVariantList rows;
    // Extensions can add language packs, so this is read fresh rather than
    // cached — the Widgets dialog rebuilt its combo for the same reason.
    for (const PreferenceDocument::LanguageOption& available : PreferenceDocument::availableLanguageOptions()) {
        rows.append(option(available.id, available.label));
    }
    return rows;
}

QString PreferencesModel::languageToken() const
{
    return PreferenceDocument::preferredLanguageToken();
}

void PreferencesModel::setLanguageToken(const QString& token)
{
    const QString normalized = token.trimmed().toLower();
    if (normalized.isEmpty() || normalized == PreferenceDocument::preferredLanguageToken()) {
        return;
    }
    miacode::LocaleService::instance().setLanguageToken(normalized);
    emit interfaceChanged();
}

QVariantList PreferencesModel::themeOptions() const
{
    return themeModeOptions();
}

QVariantList PreferencesModel::themeModeOptions() const
{
    return QVariantList{
        option(QStringLiteral("system"), qtTrId("dialog.preferences.theme.system")),
        option(QStringLiteral("light"), qtTrId("dialog.preferences.theme.light")),
        option(QStringLiteral("dark"), qtTrId("dialog.preferences.theme.dark")),
    };
}

QString PreferencesModel::themeToken() const
{
    return themeModeToken();
}

QString PreferencesModel::themeModeToken() const
{
    return settings_ != nullptr ? settings_->themeModeToken() : QStringLiteral("system");
}

void PreferencesModel::setThemeToken(const QString& token)
{
    setThemeModeToken(token);
}

void PreferencesModel::setThemeModeToken(const QString& token)
{
    if (settings_ == nullptr || token.trimmed().toLower() == themeModeToken()) {
        return;
    }
    settings_->setThemeModeToken(token);
    emit interfaceChanged();
}

QVariantList PreferencesModel::themePaletteOptions() const
{
    return appearance_labels::themeOptions();
}

QVariantList PreferencesModel::sidebarModeOptions() const
{
    return {
        option(false, qtTrId("dialog.preferences.sidebar_mode.standard")),
        option(true, qtTrId("dialog.preferences.sidebar_mode.efficient")),
    };
}

QVariantList PreferencesModel::inputHandlingOptions() const
{
    return {
        option(CorrectFullWidthOnly, qtTrId("preferences.correct_full_width_only")),
        option(BlockInputMethodsAndCorrectFullWidth, qtTrId("preferences.block_input_methods_and_correct_full_width")),
        option(LeaveInputUnchanged, qtTrId("preferences.leave_input_unchanged")),
    };
}

QString PreferencesModel::lightThemeToken() const
{
    return settings_ != nullptr ? settings_->lightThemeToken() : QStringLiteral("light");
}

void PreferencesModel::setLightThemeToken(const QString& token)
{
    if (settings_ == nullptr || token.trimmed().toLower() == lightThemeToken()) {
        return;
    }
    settings_->setLightThemeToken(token);
    emit interfaceChanged();
}

QString PreferencesModel::darkThemeToken() const
{
    return settings_ != nullptr ? settings_->darkThemeToken() : QStringLiteral("dark");
}

void PreferencesModel::setDarkThemeToken(const QString& token)
{
    if (settings_ == nullptr || token.trimmed().toLower() == darkThemeToken()) {
        return;
    }
    settings_->setDarkThemeToken(token);
    emit interfaceChanged();
}

bool PreferencesModel::previewOnLeft() const
{
    return store() != nullptr && store()->workspacePanelsSwapped();
}

void PreferencesModel::setPreviewOnLeft(bool onLeft)
{
    if (store() == nullptr || onLeft == previewOnLeft()) {
        return;
    }
    store()->setWorkspacePanelsSwapped(onLeft, true);
    emit interfaceChanged();
}

bool PreferencesModel::blurMaterialsEnabled() const
{
    return settings_ == nullptr || settings_->blurMaterialsEnabled();
}

void PreferencesModel::setBlurMaterialsEnabled(bool enabled)
{
    if (settings_ != nullptr) {
        settings_->setBlurMaterialsEnabled(enabled);
    }
}

QVariantList PreferencesModel::uiScaleOptions() const
{
    QList<int> percentages{75, 90, 100, 110, 125, 150, 175, 200};
    const int current = uiScalePercent();
    if (!percentages.contains(current)) {
        percentages.append(current);
        std::sort(percentages.begin(), percentages.end());
    }
    QVariantList rows;
    for (int percent : percentages) {
        rows.append(option(percent, QStringLiteral("%1%").arg(percent)));
    }
    return rows;
}

int PreferencesModel::uiScalePercent() const
{
    return settings_ != nullptr ? settings_->uiScalePercent() : 100;
}

void PreferencesModel::setUiScalePercent(int percent)
{
    if (settings_ != nullptr) {
        settings_->setUiScalePercent(percent);
    }
}

QString PreferencesModel::editorFontFamily() const
{
    return settings_ != nullptr ? settings_->editorFontFamily() : QString();
}

void PreferencesModel::setEditorFontFamily(const QString& family)
{
    if (settings_ != nullptr) settings_->setEditorFontFamily(family);
}

QVariantList PreferencesModel::editorFontOptions(const QString& filter) const
{
    QVariantList rows{option(QString(), qtTrId("dialog.preferences.font.default"))};
    QStringList families = QFontDatabase::families();
    std::sort(families.begin(), families.end(), [](const QString& left, const QString& right) {
        return QString::localeAwareCompare(left, right) < 0;
    });
    std::stable_partition(families.begin(), families.end(), [](const QString& family) {
        return QFontDatabase::isFixedPitch(family);
    });
    const QString selected = editorFontFamily();
    for (const QString& family : families) {
        if (!family.contains(filter.trimmed(), Qt::CaseInsensitive) && family != selected) continue;
        const bool fixed = QFontDatabase::isFixedPitch(family);
        QVariantMap row = option(family, appearance_labels::fontLabel(family, fixed));
        row.insert(QStringLiteral("family"), family);
        rows.append(row);
    }
    return rows;
}

int PreferencesModel::editorFontSize() const
{
    return store() != nullptr ? store()->editorTextFontSize() : 0;
}

void PreferencesModel::setEditorFontSize(int pointSize)
{
    if (store() == nullptr) {
        return;
    }
    const int clamped = qBound(kEditorFontSizeMinimum, pointSize, kEditorFontSizeMaximum);
    if (clamped == editorFontSize()) {
        return;
    }
    store()->applyEditorTextFontSize(clamped, true);
    emit editorChanged();
}

QVariantList PreferencesModel::lineSpacingOptions() const
{
    QVariantList rows;
    for (double factor : miacode::runtime::shared::kEditorLineSpacingFactorOptions) {
        rows.append(option(
            factor, miacode::runtime::shared::editorLineSpacingFactorLabel(factor)));
    }
    return rows;
}

double PreferencesModel::editorLineSpacing() const
{
    return store() != nullptr
        ? store()->editorLineSpacingFactor()
        : miacode::runtime::shared::kEditorLineSpacingFactorDefault;
}

void PreferencesModel::setEditorLineSpacing(double factor)
{
    if (store() == nullptr || qFuzzyCompare(factor, editorLineSpacing())) {
        return;
    }
    store()->applyEditorLineSpacingFactor(factor, true);
    emit editorChanged();
}

void PreferencesModel::reloadCompletionCandidates()
{
    const bool halfWidthInputEnabled = editorHalfWidthInput();
    completionDurations_.setHalfWidthInputEnabled(halfWidthInputEnabled);
    completionDivisions_.setHalfWidthInputEnabled(halfWidthInputEnabled);
    const auto& config = settings_->editorCompletionConfig();
    completionDurations_.replaceValues(config.durations);
    completionDivisions_.replaceValues(config.divisions);
}

void PreferencesModel::persistCompletionCandidates()
{
    if (store() == nullptr) return;
    store()->applyEditorCompletionCandidates(
        {completionDurations_.savedValues(), completionDivisions_.savedValues(),
         editor::defaultCompletionConfig().durationSuffixes}, true);
}

void PreferencesModel::finishCompletionEditing()
{
    completionDurations_.finishEditing();
    completionDivisions_.finishEditing();
}

void PreferencesModel::resetCompletionCandidates()
{
    const auto defaults = editor::defaultCompletionConfig();
    completionDurations_.replaceValues(defaults.durations, true);
    completionDivisions_.replaceValues(defaults.divisions, true);
    persistCompletionCandidates();
}

bool PreferencesModel::editorAutoCompletion() const
{
    return store() != nullptr && store()->editorAutoCompletionEnabled();
}

void PreferencesModel::setEditorAutoCompletion(bool enabled)
{
    if (store() == nullptr || enabled == editorAutoCompletion()) {
        return;
    }
    if (!enabled) finishCompletionEditing();
    store()->applyEditorAutoCompletionEnabled(enabled, true);
    emit editorChanged();
}

int PreferencesModel::editorInputHandlingMode() const
{
    if (editorImeDisabled()) {
        return BlockInputMethodsAndCorrectFullWidth;
    }
    return editorHalfWidthInput() ? CorrectFullWidthOnly : LeaveInputUnchanged;
}

void PreferencesModel::setEditorInputHandlingMode(int mode)
{
    if (store() == nullptr || mode < CorrectFullWidthOnly || mode > LeaveInputUnchanged) {
        return;
    }
    const bool correctFullWidth = mode != LeaveInputUnchanged;
    const bool blockInputMethods = mode == BlockInputMethodsAndCorrectFullWidth;
    if (editorHalfWidthInput() == correctFullWidth
        && editorImeDisabled() == blockInputMethods) {
        return;
    }
    store()->applyEditorHalfWidthInputEnabled(correctFullWidth, false);
    store()->applyEditorImeInputDisabled(blockInputMethods, true);
    completionDurations_.setHalfWidthInputEnabled(correctFullWidth);
    completionDivisions_.setHalfWidthInputEnabled(correctFullWidth);
    emit editorChanged();
}

bool PreferencesModel::editorHalfWidthInput() const
{
    return store() != nullptr && store()->editorHalfWidthInputEnabled();
}

void PreferencesModel::setEditorHalfWidthInput(bool enabled)
{
    if (store() == nullptr || enabled == editorHalfWidthInput()) {
        return;
    }
    store()->applyEditorHalfWidthInputEnabled(enabled, true);
    completionDurations_.setHalfWidthInputEnabled(enabled);
    completionDivisions_.setHalfWidthInputEnabled(enabled);
    emit editorChanged();
}

bool PreferencesModel::editorImeDisabled() const
{
    return store() != nullptr && store()->editorImeInputDisabled();
}

void PreferencesModel::setEditorImeDisabled(bool disabled)
{
    if (store() == nullptr || disabled == editorImeDisabled()) {
        return;
    }
    store()->applyEditorImeInputDisabled(disabled, true);
    emit editorChanged();
}

bool PreferencesModel::editorAutoWrap() const
{
    return settings_ != nullptr && settings_->editorAutoWrap();
}
void PreferencesModel::setEditorAutoWrap(bool enabled)
{
    if (settings_ == nullptr || enabled == editorAutoWrap()) return;
    settings_->setEditorAutoWrap(enabled);
    emit editorChanged();
}

bool PreferencesModel::editorScrollPastEnd() const
{
    return settings_ != nullptr && settings_->editorScrollPastEnd();
}

void PreferencesModel::setEditorScrollPastEnd(bool enabled)
{
    if (settings_ == nullptr || enabled == editorScrollPastEnd()) {
        return;
    }
    settings_->setEditorScrollPastEnd(enabled);
    emit editorChanged();
}

bool PreferencesModel::editorSelectionBeatDisplay() const
{
    return settings_ != nullptr && settings_->editorSelectionBeatDisplay();
}

void PreferencesModel::setEditorSelectionBeatDisplay(bool enabled)
{
    if (settings_ == nullptr || enabled == editorSelectionBeatDisplay()) {
        return;
    }
    settings_->setEditorSelectionBeatDisplay(enabled);
    emit editorChanged();
}

bool PreferencesModel::editorOverviewRulerEnabled() const
{
    return settings_ != nullptr && settings_->editorOverviewRulerEnabled();
}

void PreferencesModel::setEditorOverviewRulerEnabled(bool enabled)
{
    if (settings_ == nullptr || enabled == editorOverviewRulerEnabled()) {
        return;
    }
    settings_->setEditorOverviewRulerEnabled(enabled);
    emit editorChanged();
}

double PreferencesModel::handOrderReturnSpeed() const
{
    const auto app = PreferenceDocument::loadPreferencesObject().value(QStringLiteral("app")).toObject();
    const double value = app.value(QStringLiteral("hand_order_return_speed"))
        .toDouble(chart::handOrderDefaultReturnSpeed);
    return std::isfinite(value) ? qBound(chart::handOrderMinReturnSpeed, value,
        chart::handOrderMaxReturnSpeed) : chart::handOrderDefaultReturnSpeed;
}

void PreferencesModel::setHandOrderReturnSpeed(double speed)
{
    if (!std::isfinite(speed)) return;
    speed = qBound(chart::handOrderMinReturnSpeed, speed, chart::handOrderMaxReturnSpeed);
    if (qFuzzyCompare(speed, handOrderReturnSpeed())) return;
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    QJsonObject app = root.value(QStringLiteral("app")).toObject();
    app.insert(QStringLiteral("hand_order_return_speed"), speed);
    root.insert(QStringLiteral("app"), app);
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

int PreferencesModel::handOrderSweepThreshold() const
{
    return hand_order_sweep_preferences::threshold(PreferenceDocument::loadPreferencesObject());
}

void PreferencesModel::setHandOrderSweepThreshold(int threshold)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_sweep_preferences::setThreshold(&root, threshold)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

bool PreferencesModel::handOrderFeaturesEnabled() const { return miacode::hand_order_features::enabled(); }
bool PreferencesModel::handOrderWatermarkEnabled() const { return miacode::hand_order_features::watermark(); }

QString PreferencesModel::unlockHandOrderFeatures(const QString& password)
{
    using namespace miacode::hand_order_features;
    switch (unlock(password)) {
    case UnlockResult::IncorrectPassword: return qtTrId("hand_order.features.wrong_password");
    case UnlockResult::StorageError: return qtTrId("hand_order.features.storage_error");
    case UnlockResult::Enabled: emit handOrderFeaturesChanged(); break;
    }
    return {};
}

bool PreferencesModel::disableHandOrderFeatures()
{
    if (!miacode::hand_order_features::disable()) return false;
    emit handOrderFeaturesChanged();
    return true;
}

void PreferencesModel::setHandOrderWatermarkEnabled(bool enabled)
{
    if (handOrderWatermarkEnabled() == enabled) return;
    auto root = PreferenceDocument::loadPreferencesObject();
    auto app = root.value("app").toObject();
    app.insert("hand_order_watermark_enabled", enabled);
    root.insert("app", app);
    PreferenceDocument::savePreferencesObject(root);
    emit handOrderFeaturesChanged();
}

bool PreferencesModel::videoWatermarkEnabled() const
{
    return video_watermark_preferences::load().enabled;
}

void PreferencesModel::setVideoWatermarkEnabled(bool enabled)
{
    auto settings = video_watermark_preferences::load();
    if (settings.enabled == enabled) return;
    settings.enabled = enabled;
    video_watermark_preferences::save(settings);
    emit videoWatermarkEnabledChanged();
}

QString PreferencesModel::videoWatermarkText() const
{
    return video_watermark_preferences::load().text;
}

void PreferencesModel::setVideoWatermarkText(const QString& text)
{
    auto settings = video_watermark_preferences::load();
    if (settings.text == text) return;
    settings.text = text;
    video_watermark_preferences::save(settings);
    emit videoWatermarkTextChanged();
}

int PreferencesModel::videoWatermarkFontSize() const
{
    return video_watermark_preferences::load().fontSize;
}

void PreferencesModel::setVideoWatermarkFontSize(int size)
{
    auto settings = video_watermark_preferences::load();
    const int bounded = qBound(videoWatermarkMinFontSize(), size, videoWatermarkMaxFontSize());
    if (settings.fontSize == bounded) return;
    settings.fontSize = bounded;
    video_watermark_preferences::save(settings);
    emit videoWatermarkFontSizeChanged();
}

int PreferencesModel::videoWatermarkMinFontSize() const
{
    return video_export::VideoWatermarkSettings::kMinimumFontSize;
}

int PreferencesModel::videoWatermarkMaxFontSize() const
{
    return video_export::VideoWatermarkSettings::kMaximumFontSize;
}

bool PreferencesModel::handOrderColorUnresolved() const
{
    return hand_order_sweep_preferences::colorUnresolved(PreferenceDocument::loadPreferencesObject());
}

void PreferencesModel::setHandOrderColorUnresolved(bool enabled)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_sweep_preferences::setColorUnresolved(&root, enabled)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

QVariantMap PreferencesModel::handOrderPlayerProfile() const
{
    return hand_order_player_preferences::read(PreferenceDocument::loadPreferencesObject());
}
QVariantMap PreferencesModel::handOrderPlayerDisplayProfile() const
{
    return hand_order_player_preferences::readDisplay(PreferenceDocument::loadPreferencesObject());
}
void PreferencesModel::setHandOrderPlayerDisplayParameter(const QString& name, double value)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_player_preferences::setDisplay(&root, name, value)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

QVariantList PreferencesModel::handOrderStyleOptions() const
{
    return {
        option(QStringLiteral("comprehensive"), qtTrId("preferences.hand_order_style.comprehensive")),
        option(QStringLiteral("hard"), qtTrId("preferences.hand_order_style.hard")),
        option(QStringLiteral("pagination"), qtTrId("preferences.hand_order_style.pagination")),
        option(QStringLiteral("decomposition"), qtTrId("preferences.hand_order_style.decomposition")),
        option(QStringLiteral("custom"), qtTrId("preferences.hand_order_style.custom")),
    };
}

QString PreferencesModel::handOrderStyleToken() const
{
    return hand_order_player_preferences::styleToken(PreferenceDocument::loadPreferencesObject());
}

void PreferencesModel::setHandOrderStyleToken(const QString& token)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_player_preferences::setStyle(&root, token)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

QVariantList PreferencesModel::handOrderSearchModeOptions() const
{
    return {
        option(QStringLiteral("low"), qtTrId("preferences.hand_order_budget.low")),
        option(QStringLiteral("standard"), qtTrId("preferences.hand_order_budget.standard")),
        option(QStringLiteral("high"), qtTrId("preferences.hand_order_budget.high")),
        option(QStringLiteral("custom"), qtTrId("preferences.hand_order_budget.custom")),
    };
}

QString PreferencesModel::handOrderSearchMode() const
{
    return hand_order_search_preferences::mode(PreferenceDocument::loadPreferencesObject());
}

void PreferencesModel::setHandOrderSearchMode(const QString& mode)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_search_preferences::setMode(&root, mode)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

int PreferencesModel::handOrderSearchBudgetMillions() const
{
    return hand_order_search_preferences::millions(PreferenceDocument::loadPreferencesObject());
}

void PreferencesModel::setHandOrderSearchBudgetMillions(int millions)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_search_preferences::setCustomMillions(&root, millions)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

void PreferencesModel::setHandOrderPlayerParameter(const QString& name, double value)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_player_preferences::set(&root, name, value)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

void PreferencesModel::resetHandOrderPlayerProfile()
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    if (!hand_order_player_preferences::reset(&root)) return;
    PreferenceDocument::savePreferencesObject(root);
    emit editorChanged();
}

bool PreferencesModel::videoDecodePrefersSoftware() const
{
    return store() != nullptr && store()->videoDecodePrefersSoftware();
}

void PreferencesModel::setVideoDecodePrefersSoftware(bool preferSoftware)
{
    if (store() == nullptr || preferSoftware == videoDecodePrefersSoftware()) {
        return;
    }
    store()->setVideoDecodePrefersSoftware(preferSoftware, true);
    emit performanceChanged();
}

double PreferencesModel::displayRefreshRate() const
{
    return store() != nullptr ? store()->previewCanvasRefreshRate() : 0.0;
}

QVariantList PreferencesModel::frameRateOptions(bool includeDisplayRefresh) const
{
    QVariantList rows;
    // Read once: also gates the Fps120 option below (v1 parity, d5a604b7),
    // not just the DisplayRefresh label.
    const double refresh = displayRefreshRate();
    if (includeDisplayRefresh) {
        rows.append(option(
            static_cast<int>(PreviewCanvasFrameRateMode::DisplayRefresh),
            refresh > 0.0
                ? QStringLiteral("%1 (%2 Hz)")
                      .arg(qtTrId("dialog.render_settings.preview.canvas_frame_rate.display"))
                      .arg(refresh, 0, 'f', refresh >= 100.0 ? 0 : 1)
                : qtTrId("dialog.render_settings.preview.canvas_frame_rate.display")));
    }
    rows.append(option(
        static_cast<int>(PreviewCanvasFrameRateMode::Fps30),
        qtTrId("dialog.render_settings.preview.canvas_frame_rate.30")));
    rows.append(option(
        static_cast<int>(PreviewCanvasFrameRateMode::Fps60),
        qtTrId("dialog.render_settings.preview.canvas_frame_rate.60")));
    // Only expose 120 FPS on a display that can sustain it. The backend
    // clamps Fps120 to display refresh at runtime (see
    // PlaybackCoordinator::previewCanvasTargetFrameIntervalNs), so leaving
    // it in the menu on a sub-120 Hz panel would advertise a setting that
    // silently degrades to display refresh. Epsilon (119.5) tolerates
    // panels that report 119.88 Hz (common OEM round-down of true 120 Hz).
    if (refresh >= 119.5) {
        rows.append(option(
            static_cast<int>(PreviewCanvasFrameRateMode::Fps120),
            qtTrId("dialog.render_settings.preview.canvas_frame_rate.120")));
    }
    return rows;
}

QVariantList PreferencesModel::canvasFrameRateOptions() const
{
    return frameRateOptions(/*includeDisplayRefresh=*/true);
}

QVariantList PreferencesModel::stageMediaFrameRateOptions() const
{
    auto rows = frameRateOptions(true);
    rows.prepend(option(static_cast<int>(PreviewCanvasFrameRateMode::SourceRate),
                        qtTrId("preferences.pv_frame_rate_auto")));
    return rows;
}

QVariantList PreferencesModel::appFrameRateOptions() const
{
    return frameRateOptions(/*includeDisplayRefresh=*/true);
}

int PreferencesModel::canvasFrameRateMode() const
{
    return store() != nullptr ? static_cast<int>(store()->previewCanvasFrameRateMode()) : 0;
}

void PreferencesModel::setCanvasFrameRateMode(int mode)
{
    if (store() == nullptr || mode == canvasFrameRateMode()) {
        return;
    }
    store()->setPreviewCanvasFrameRateMode(
        static_cast<PreviewCanvasFrameRateMode>(mode), true);
    emit performanceChanged();
}

int PreferencesModel::stageMediaFrameRateMode() const
{
    return store() != nullptr
        ? static_cast<int>(store()->previewStageMediaFrameRateMode())
        : 0;
}

void PreferencesModel::setStageMediaFrameRateMode(int mode)
{
    if (store() == nullptr || mode == stageMediaFrameRateMode()) {
        return;
    }
    store()->setPreviewStageMediaFrameRateMode(
        static_cast<PreviewCanvasFrameRateMode>(mode), true);
    emit performanceChanged();
}

int PreferencesModel::timelineFrameRateMode() const
{
    return store() != nullptr ? static_cast<int>(store()->timelineFrameRateMode()) : 0;
}

void PreferencesModel::setTimelineFrameRateMode(int mode)
{
    if (store() == nullptr || mode == timelineFrameRateMode()) {
        return;
    }
    store()->setTimelineFrameRateMode(
        static_cast<PreviewCanvasFrameRateMode>(mode), true);
    emit performanceChanged();
}

}  // namespace miacode::ui
