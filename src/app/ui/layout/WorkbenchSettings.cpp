#include "app/ui/layout/WorkbenchSettings.h"
#include "app/services/EditorCompletionPreferences.h"

#include "app/runtime/Shared.h"
#include "AppVersion.h"
#include "app/services/PreferenceDocument.h"
#include "app/ui/theme/ThemeVariantResolver.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QFontDatabase>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QScreen>
#include <QDate>
#include <QStringList>
#include <QSysInfo>
#include <QtGlobal>


namespace miacode::ui {
namespace {
constexpr auto kUiSection = "ui";
constexpr auto kWorkspaceLayout = "workspace_layout";
constexpr auto kSavedWorkspaceLayouts = "saved_workspace_layouts";
const QStringList kBuiltinLayouts = {QStringLiteral("default"), QStringLiteral("editing"), QStringLiteral("preview")};
const QStringList kInitialSlots = kBuiltinLayouts;
const QString kSavedPrefix = QStringLiteral("saved:");
constexpr auto kWorkspaceListOrder = "workspace_list_order";
constexpr auto kWorkspaceSlots = "workspace_shortcuts";
constexpr auto kSavedWorkspaceLayoutOrder = "saved_workspace_layout_order";
constexpr auto kWorkspaceHiddenPanels = "workspace_hidden_panels";
constexpr auto kTimelinePanelVisible = "timeline_panel_visible";
constexpr auto kBlurMaterialsEnabled = "blur_materials_enabled";
constexpr auto kEfficientSidebarEnabled = "efficient_sidebar_enabled";
constexpr auto kSidebarVisible = "sidebar_visible";
constexpr auto kSidebarWidth = "sidebar_width";
constexpr auto kBottomPanelVisible = "bottom_panel_visible";
constexpr auto kBottomPanelHeightRatio = "bottom_panel_height_ratio";
constexpr auto kPreviewWidthRatio = "preview_width_ratio";
constexpr auto kPreviewDetached = "preview_detached";
constexpr auto kPreviewNoteStatisticsVisible = "preview_note_statistics_visible";
constexpr auto kPreviewCanvasFreeAspect = "preview_canvas_free_aspect";
constexpr auto kPreviewHidePv = "preview_hide_pv";
constexpr auto kFontSize = "ui_font_size";
constexpr auto kEditorFontFamily = "editor_text_font_family";
constexpr auto kUiScalePercent = "ui_scale_percent";
constexpr auto kEditorAutoWrap = "editor_auto_wrap";
constexpr auto kEditorScrollPastEnd = "editor_scroll_past_end";
constexpr auto kEditorSelectionBeatDisplay = "editor_selection_beat_display";
constexpr auto kEditorOverviewRulerEnabled = "editor_overview_ruler_enabled";

QString releasePlatformDisplay()
{
    QString architecture = QSysInfo::buildCpuArchitecture();
    if (architecture == QLatin1String("x86_64")) {
        architecture = QStringLiteral("x64");
    }
#if defined(Q_OS_WIN)
    if (architecture == QLatin1String("arm64")) {
        architecture = QStringLiteral("ARM64");
    }
    const QString platform = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
    if (architecture == QLatin1String("arm64")) {
        architecture = QStringLiteral("Apple Silicon");
    }
    const QString platform = QStringLiteral("macOS");
#elif defined(Q_OS_LINUX)
    if (architecture == QLatin1String("arm64")) {
        architecture = QStringLiteral("aarch64");
    }
    const QString distribution = QSysInfo::productType();
    const QString platform = distribution == QLatin1String("unknown")
        ? QStringLiteral("Linux")
        : QStringLiteral("Linux (%1)").arg(distribution);
#else
    const QString platform = QSysInfo::prettyProductName();
#endif
    return QStringLiteral("%1 / %2").arg(platform, architecture);
}

QJsonObject loadUiObject()
{
    return PreferenceDocument::loadPreferencesObject().value(QLatin1String(kUiSection)).toObject();
}

void storeUiValue(const char* key, const QJsonValue& value)
{
    QJsonObject root = PreferenceDocument::loadPreferencesObject();
    QJsonObject ui = root.value(QLatin1String(kUiSection)).toObject();
    ui.insert(QLatin1String(key), value);
    root.insert(QLatin1String(kUiSection), ui);
    PreferenceDocument::savePreferencesObject(root);
}

}

WorkbenchSettings::WorkbenchSettings(QObject* parent)
    : QObject(parent)
{
    uiFontFamily_ = QGuiApplication::font().family();

    // Read this build's JSON document and clamp layout values to supported ranges.
    const QJsonObject ui = loadUiObject();
    advancedMetadataEnabled_ = ui.value(QStringLiteral("advanced_metadata_enabled")).toBool(false);
    lowPerformanceMode_ = ui.value(QStringLiteral("low_performance_mode")).toBool(false);
    disableAltMenuActivation_ = ui.value(QStringLiteral("disable_alt_menu_activation")).toBool(true);
    spaceGuardEnabled_ = ui.value(QStringLiteral("space_guard_enabled")).toBool(true);
    autoNormalizeEnabled_ = ui.value(QStringLiteral("auto_normalize_enabled")).toBool(false);
    warnEmptyDifficulty_ = ui.value(QStringLiteral("warn_empty_difficulty")).toBool(true);
    timelineLayoutMode_ = ui.value(QStringLiteral("timeline_layout_mode")).toString(QStringLiteral("horizontal"));
    if (timelineLayoutMode_ == QStringLiteral("vertical_before_editor") || timelineLayoutMode_ == QStringLiteral("vertical_after_editor"))
        timelineLayoutMode_ = QStringLiteral("vertical");
    else if (timelineLayoutMode_ != QStringLiteral("vertical")) timelineLayoutMode_ = QStringLiteral("horizontal");
    timelineVerticalDirection_ = ui.value(QStringLiteral("timeline_vertical_direction")).toString(QStringLiteral("falling"));
    if (!QStringList{QStringLiteral("falling"), QStringLiteral("rising")}.contains(timelineVerticalDirection_)) timelineVerticalDirection_ = QStringLiteral("falling");
    workspaceLayout_ = ui.value(QLatin1String(kWorkspaceLayout)).toObject().toVariantMap();
    savedWorkspaceLayouts_ = ui.value(QLatin1String(kSavedWorkspaceLayouts)).toObject().toVariantMap();
    QStringList available = kBuiltinLayouts;
    // Preserve the user's previous custom order when upgrading.
    for (const auto& value : ui.value(QLatin1String(kSavedWorkspaceLayoutOrder)).toArray()) {
        const QString name = value.toString();
        if (savedWorkspaceLayouts_.contains(name) && !available.contains(kSavedPrefix + name))
            available.append(kSavedPrefix + name);
    }
    for (const auto& name : savedWorkspaceLayouts_.keys()) {
        if (!available.contains(kSavedPrefix + name)) available.append(kSavedPrefix + name);
    }
    for (const auto& value : ui.value(QLatin1String(kWorkspaceListOrder)).toArray()) {
        const QString key = value.toString();
        if (available.removeOne(key)) workspaceListOrder_.append(key);
    }
    workspaceListOrder_.append(available);
    workspaceSlots_ = kInitialSlots;
    const auto slotsJson = ui.value(QLatin1String(kWorkspaceSlots)).toArray();
    for (int i = 0; i < workspaceSlots_.size() && i < slotsJson.size(); ++i) {
        const QString key = slotsJson.at(i).toString();
        if (key.startsWith(kSavedPrefix) && workspaceListOrder_.contains(key) && !workspaceSlots_.contains(key))
            workspaceSlots_[i] = key;
    }
    workspaceHiddenPanels_ = ui.value(QLatin1String(kWorkspaceHiddenPanels)).toObject().toVariantMap();
    timelinePanelVisible_ = ui.value(QLatin1String(kTimelinePanelVisible)).toBool(true);
    timelineNoteScale_ = qBound(0.5, ui.value(QStringLiteral("timeline_note_scale")).toDouble(1.5), 3.0);
    timelineVerticalWidthRatio_ = qBound(0.2, ui.value(QStringLiteral("timeline_vertical_width_ratio")).toDouble(0.34), 0.6);
    timelineLaneOrder_ = ui.value(QStringLiteral("timeline_lane_order")).toString(QStringLiteral("natural"));
    if (!QStringList{QStringLiteral("natural"), QStringLiteral("rotated"), QStringLiteral("reversed"), QStringLiteral("rotated_reversed")}.contains(timelineLaneOrder_)) timelineLaneOrder_ = QStringLiteral("natural");
    efficientSidebarEnabled_ = ui.value(QLatin1String(kEfficientSidebarEnabled)).toBool(false);
    previewDetached_ = ui.value(QLatin1String(kPreviewDetached)).toBool(false);
    blurMaterialsEnabled_ = ui.value(QLatin1String(kBlurMaterialsEnabled)).toBool(true);
    uiScalePercent_ = qBound(kUiScaleMinimumPercent,
                            ui.value(QLatin1String(kUiScalePercent)).toInt(kUiScaleDefaultPercent),
                            kUiScaleMaximumPercent);
    sidebarVisible_ = ui.value(QLatin1String(kSidebarVisible)).toBool(true);
    sidebarWidth_ = qBound(kSidebarMinimumContentWidth,
                           ui.value(QLatin1String(kSidebarWidth)).toInt(kSidebarDefaultWidth),
                           kSidebarMaximumContentWidth);
    bottomPanelVisible_ = ui.value(QLatin1String(kBottomPanelVisible)).toBool(true);
    bottomPanelHeightRatio_ = qBound(kBottomPanelMinimumHeightRatio,
                                     ui.value(QLatin1String(kBottomPanelHeightRatio)).toDouble(kBottomPanelDefaultHeightRatio),
                                     kBottomPanelMaximumHeightRatio);
    previewWidthRatio_ = qBound(kPreviewMinimumWidthRatio,
                                ui.value(QLatin1String(kPreviewWidthRatio)).toDouble(kPreviewDefaultWidthRatio),
                                kPreviewMaximumWidthRatio);
    previewNoteStatisticsVisible_ = ui.value(QLatin1String(kPreviewNoteStatisticsVisible)).toBool(true);
    previewCanvasFreeAspect_ = ui.value(QLatin1String(kPreviewCanvasFreeAspect)).toBool(false);
    previewHidePv_ = ui.value(QLatin1String(kPreviewHidePv)).toBool(false);
    fontSize_ = qBound(12, ui.value(QLatin1String(kFontSize)).toInt(13), 14);
    if (!ui.contains(QLatin1String(kSidebarVisible))
        || !ui.contains(QLatin1String(kSidebarWidth))
        || !ui.contains(QLatin1String(kBottomPanelVisible))
        || !ui.contains(QLatin1String(kBottomPanelHeightRatio))
        || !ui.contains(QLatin1String(kPreviewWidthRatio))
        || !ui.contains(QLatin1String(kFontSize))) {
        QJsonObject root = PreferenceDocument::loadPreferencesObject();
        QJsonObject nextUi = root.value(QLatin1String(kUiSection)).toObject();
        nextUi.insert(QLatin1String(kSidebarVisible), sidebarVisible_);
        nextUi.insert(QLatin1String(kSidebarWidth), sidebarWidth_);
        nextUi.insert(QLatin1String(kBottomPanelVisible), bottomPanelVisible_);
        nextUi.insert(QLatin1String(kBottomPanelHeightRatio), bottomPanelHeightRatio_);
        nextUi.insert(QLatin1String(kPreviewWidthRatio), previewWidthRatio_);
        nextUi.insert(QLatin1String(kFontSize), fontSize_);
        root.insert(QLatin1String(kUiSection), nextUi);
        PreferenceDocument::savePreferencesObject(root);
    }
    reloadTheme();
    if (QGuiApplication::styleHints() != nullptr) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, &WorkbenchSettings::reloadTheme);
    }
    reloadEditorSettings();
}

bool WorkbenchSettings::setupCompleted() const
{
    return loadUiObject().value(QStringLiteral("setup_completed")).toBool(false);
}

QString WorkbenchSettings::preferenceMigrationStatus() const
{
    return PreferenceDocument::migrationStatus();
}

bool WorkbenchSettings::completeSetup()
{
    const QJsonObject previous = PreferenceDocument::loadPreferencesObject();
    QJsonObject root = previous;
    QJsonObject ui = root.value(QLatin1String(kUiSection)).toObject();
    ui.insert(QStringLiteral("setup_completed"), true);
    root.insert(QLatin1String(kUiSection), ui);
    if (!PreferenceDocument::savePreferencesObject(root)) {
        PreferenceDocument::savePreferencesObject(previous);
        return false;
    }
    emit setupCompletedChanged();
    return true;
}

void WorkbenchSettings::setBlurMaterialsEnabled(bool enabled)
{
    if (blurMaterialsEnabled_ == enabled) {
        return;
    }
    blurMaterialsEnabled_ = enabled;
    storeUiValue(kBlurMaterialsEnabled, enabled);
    emit blurMaterialsEnabledChanged();
}

QVariantMap WorkbenchSettings::aboutInfo() const
{
    QString version = QString::fromLatin1(MIACODE_VERSION_STRING).trimmed();
    const QStringList compilerDate = QStringLiteral(__DATE__).simplified().split(QLatin1Char(' '));
    const QStringList monthNames = {
        QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"),
        QStringLiteral("Apr"), QStringLiteral("May"), QStringLiteral("Jun"),
        QStringLiteral("Jul"), QStringLiteral("Aug"), QStringLiteral("Sep"),
        QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec")
    };
    if (compilerDate.size() == 3) {
        const int month = monthNames.indexOf(compilerDate.at(0)) + 1;
        const QDate buildDate(compilerDate.at(2).toInt(), month, compilerDate.at(1).toInt());
        if (buildDate.isValid()) {
            version += QStringLiteral(" (") + buildDate.toString(QStringLiteral("yyyyMMdd"))
                + QStringLiteral(")");
        }
    }
    if (version.isEmpty()) {
        version = QCoreApplication::applicationVersion().trimmed();
    }
    if (version.isEmpty()) {
        version = QStringLiteral("0.0.0");
    }
    return QVariantMap{
        {QStringLiteral("version"), version},
        {QStringLiteral("platform"), releasePlatformDisplay()},
        {QStringLiteral("buildType"), QStringLiteral(MIACODE_RELEASE_CHANNEL_DISPLAY)},
        {QStringLiteral("platformLabel"), qtTrId("about.platform")},
        {QStringLiteral("buildTypeLabel"), qtTrId("about.build_type")},
        {QStringLiteral("title"), qtTrId("action.about")},
    };
}

bool WorkbenchSettings::efficientSidebarEnabled() const { return efficientSidebarEnabled_; }
bool WorkbenchSettings::sidebarVisible() const { return sidebarVisible_; }
int WorkbenchSettings::sidebarWidth() const { return sidebarWidth_; }
int WorkbenchSettings::sidebarDefaultWidth() const { return kSidebarDefaultWidth; }
int WorkbenchSettings::sidebarMinimumContentWidth() const { return kSidebarMinimumContentWidth; }
int WorkbenchSettings::sidebarMaximumContentWidth() const { return kSidebarMaximumContentWidth; }
bool WorkbenchSettings::bottomPanelVisible() const { return bottomPanelVisible_; }
double WorkbenchSettings::bottomPanelHeightRatio() const { return bottomPanelHeightRatio_; }
double WorkbenchSettings::bottomPanelDefaultHeightRatio() const { return kBottomPanelDefaultHeightRatio; }
double WorkbenchSettings::bottomPanelMinimumHeightRatio() const { return kBottomPanelMinimumHeightRatio; }
double WorkbenchSettings::bottomPanelMaximumHeightRatio() const { return kBottomPanelMaximumHeightRatio; }
double WorkbenchSettings::previewWidthRatio() const { return previewWidthRatio_; }
double WorkbenchSettings::previewDefaultWidthRatio() const { return kPreviewDefaultWidthRatio; }
bool WorkbenchSettings::previewDetached() const { return previewDetached_; }
double WorkbenchSettings::previewMinimumWidthRatio() const { return kPreviewMinimumWidthRatio; }
double WorkbenchSettings::previewMaximumWidthRatio() const { return kPreviewMaximumWidthRatio; }
bool WorkbenchSettings::previewCanvasFreeAspect() const { return previewCanvasFreeAspect_; }
bool WorkbenchSettings::previewHidePv() const { return previewHidePv_; }
QString WorkbenchSettings::uiFontFamily() const { return uiFontFamily_; }
int WorkbenchSettings::uiScalePercent() const { return uiScalePercent_; }

void WorkbenchSettings::setUiScalePercent(int percent)
{
    percent = qBound(kUiScaleMinimumPercent, percent, kUiScaleMaximumPercent);
    if (uiScalePercent_ == percent) {
        return;
    }
    uiScalePercent_ = percent;
    storeUiValue(kUiScalePercent, percent);
    emit uiScalePercentChanged();
}

QString WorkbenchSettings::themeModeToken() const
{
    return PreferenceDocument::themePreferenceToken(PreferenceDocument::preferredTheme());
}
QString WorkbenchSettings::lightThemeToken() const
{
    return PreferenceDocument::themePaletteToken(PreferenceDocument::preferredLightTheme());
}
QString WorkbenchSettings::darkThemeToken() const
{
    return PreferenceDocument::themePaletteToken(PreferenceDocument::preferredDarkTheme());
}
QString WorkbenchSettings::activeThemeToken() const
{
    if (PreferenceDocument::preferredTheme() == PreferenceDocument::ThemePreference::Legacy) {
        return QStringLiteral("legacy");
    }
    const bool darkAppearance = miacode::ui::ThemeVariantResolver::resolve(
        PreferenceDocument::preferredTheme()) == miacode::ui::ThemeVariant::Dark;
    return darkAppearance ? darkThemeToken() : lightThemeToken();
}
bool WorkbenchSettings::darkTheme() const { return darkTheme_; }
QFont WorkbenchSettings::codeFont() const { return codeFont_; }
QString WorkbenchSettings::editorFontFamily() const
{
    const QString family = loadUiObject().value(QLatin1String(kEditorFontFamily)).toString();
    return QFontDatabase::families().contains(family, Qt::CaseInsensitive) ? family : QString();
}

void WorkbenchSettings::setEditorFontFamily(const QString& family)
{
    const QString normalized = family.trimmed();
    if (!normalized.isEmpty() && !QFontDatabase::families().contains(normalized, Qt::CaseInsensitive)) return;
    if (normalized == editorFontFamily()) return;
    const QFont previousFont = codeFont_;
    storeUiValue(kEditorFontFamily, normalized);
    reloadEditorSettings();
    // An explicit choice can resolve to the same face as the default option.
    if (codeFont_ == previousFont) emit editorSettingsChanged();
}

int WorkbenchSettings::editorBlockSpacing() const { return editorBlockSpacing_; }
int WorkbenchSettings::fontSize() const { return fontSize_; }
bool WorkbenchSettings::editorHalfWidthInputEnabled() const { return editorHalfWidthInputEnabled_; }
bool WorkbenchSettings::editorOverwriteModeEnabled() const { return editorOverwriteModeEnabled_; }
bool WorkbenchSettings::editorAutoCompletionEnabled() const { return editorAutoCompletionEnabled_; }
const miacode::editor::SimaiCompletionConfig& WorkbenchSettings::editorCompletionConfig() const
{
    return editorCompletionConfig_;
}
bool WorkbenchSettings::editorImeInputDisabled() const { return editorImeInputDisabled_; }
bool WorkbenchSettings::editorAutoWrap() const { return editorAutoWrap_; }
bool WorkbenchSettings::editorScrollPastEnd() const { return editorScrollPastEnd_; }
bool WorkbenchSettings::editorSelectionBeatDisplay() const { return editorSelectionBeatDisplay_; }
bool WorkbenchSettings::editorOverviewRulerEnabled() const { return editorOverviewRulerEnabled_; }

void WorkbenchSettings::reloadEditorSettings()
{
    const QJsonObject editorUi = PreferenceDocument::loadPreferencesObject().value(QStringLiteral("ui")).toObject();
    const int fontPointSize = qBound(
        miacode::runtime::shared::kEditorTextFontSizeMin,
        editorUi.value(QStringLiteral("editor_text_font_size")).toInt(
            miacode::runtime::shared::editorFont().pointSize()),
        miacode::runtime::shared::kEditorTextFontSizeMax);
    const double lineSpacingFactor = miacode::runtime::shared::normalizeEditorLineSpacingFactor(
        editorUi.value(QStringLiteral("editor_line_spacing_factor")).toDouble(
            miacode::runtime::shared::kEditorLineSpacingFactorDefault));
    const QFont codeFont = miacode::runtime::shared::editorFont(fontPointSize);
    const int blockSpacing = miacode::runtime::shared::blockSpacingPixelsForPointSize(
        fontPointSize, lineSpacingFactor);
    const bool halfWidth = editorUi.value(QStringLiteral("editor_half_width_input")).toBool(true);
    const bool overwrite = editorUi.value(QStringLiteral("editor_overwrite_mode")).toBool(false);
    const bool autoCompletion = editorUi.value(QStringLiteral("editor_auto_completion")).toBool(true);
    const auto completionConfig = miacode::readEditorCompletionCandidates(editorUi);
    const bool imeDisabled = editorUi.value(QStringLiteral("editor_ime_input_disabled")).toBool(true);
    const bool autoWrap = editorUi.value(QLatin1String(kEditorAutoWrap)).toBool(true);
    const bool scrollPastEnd = editorUi.value(QLatin1String(kEditorScrollPastEnd)).toBool(true);
    const bool selectionBeatDisplay =
        editorUi.value(QLatin1String(kEditorSelectionBeatDisplay)).toBool(true);
    const bool overviewRulerEnabled =
        editorUi.value(QLatin1String(kEditorOverviewRulerEnabled)).toBool(false);
    if (codeFont_ == codeFont
        && editorBlockSpacing_ == blockSpacing
        && editorHalfWidthInputEnabled_ == halfWidth
        && editorOverwriteModeEnabled_ == overwrite
        && editorAutoCompletionEnabled_ == autoCompletion
        && editorCompletionConfig_ == completionConfig
        && editorImeInputDisabled_ == imeDisabled
        && editorAutoWrap_ == autoWrap
        && editorScrollPastEnd_ == scrollPastEnd
        && editorSelectionBeatDisplay_ == selectionBeatDisplay
        && editorOverviewRulerEnabled_ == overviewRulerEnabled) {
        return;
    }
    codeFont_ = codeFont;
    editorBlockSpacing_ = blockSpacing;
    editorHalfWidthInputEnabled_ = halfWidth;
    editorOverwriteModeEnabled_ = overwrite;
    editorAutoCompletionEnabled_ = autoCompletion;
    editorCompletionConfig_ = completionConfig;
    editorImeInputDisabled_ = imeDisabled;
    editorAutoWrap_ = autoWrap;
    editorScrollPastEnd_ = scrollPastEnd;
    editorSelectionBeatDisplay_ = selectionBeatDisplay;
    editorOverviewRulerEnabled_ = overviewRulerEnabled;
    emit editorSettingsChanged();
}

void WorkbenchSettings::reloadTheme()
{
    const bool darkAppearance = miacode::ui::ThemeVariantResolver::resolve(
        PreferenceDocument::preferredTheme()) == miacode::ui::ThemeVariant::Dark;
    const PreferenceDocument::ThemePalette activePalette =
        PreferenceDocument::preferredTheme() == PreferenceDocument::ThemePreference::Legacy
        ? PreferenceDocument::ThemePalette::Legacy
        : (darkAppearance
               ? PreferenceDocument::preferredDarkTheme()
               : PreferenceDocument::preferredLightTheme());
    const bool next = PreferenceDocument::themePaletteIsDark(activePalette);
    const QString nextModeToken = themeModeToken();
    const QString nextLightThemeToken = lightThemeToken();
    const QString nextDarkThemeToken = darkThemeToken();
    if (darkTheme_ == next
        && publishedThemeModeToken_ == nextModeToken
        && publishedLightThemeToken_ == nextLightThemeToken
        && publishedDarkThemeToken_ == nextDarkThemeToken) {
        return;
    }
    darkTheme_ = next;
    publishedThemeModeToken_ = nextModeToken;
    publishedLightThemeToken_ = nextLightThemeToken;
    publishedDarkThemeToken_ = nextDarkThemeToken;
    emit themeChanged();
}

void WorkbenchSettings::setThemeModeToken(const QString& token)
{
    const PreferenceDocument::ThemePreference next = PreferenceDocument::themePreferenceFromToken(token);
    if (next == PreferenceDocument::preferredTheme()) {
        return;
    }
    PreferenceDocument::setPreferredTheme(next);
    // PreferenceDocument only stores and persists — it notifies nobody. Without this the
    // stored preference changes and the timeline follows it (a QSG item reading
    // UiTheme::colors() on its next repaint), while every QML surface stays on
    // the old palette until the next launch. The OS colour-scheme change
    // already ends here; the user's own choice has to as well.
    reloadTheme();
}

void WorkbenchSettings::setLightThemeToken(const QString& token)
{
    const PreferenceDocument::ThemePalette next = PreferenceDocument::themePaletteFromToken(token);
    if (PreferenceDocument::themePaletteIsDark(next)) return;
    if (next == PreferenceDocument::preferredLightTheme()) {
        return;
    }
    PreferenceDocument::setPreferredLightTheme(next);
    reloadTheme();
}

void WorkbenchSettings::setDarkThemeToken(const QString& token)
{
    const PreferenceDocument::ThemePalette next = PreferenceDocument::themePaletteFromToken(token);
    if (!PreferenceDocument::themePaletteIsDark(next)) return;
    if (next == PreferenceDocument::preferredDarkTheme()) {
        return;
    }
    PreferenceDocument::setPreferredDarkTheme(next);
    reloadTheme();
}

void WorkbenchSettings::setEfficientSidebarEnabled(bool value)
{
    if (efficientSidebarEnabled_ == value) return;
    efficientSidebarEnabled_ = value;
    storeUiValue(kEfficientSidebarEnabled, value);
    emit efficientSidebarEnabledChanged();
}

void WorkbenchSettings::setSidebarVisible(bool value)
{
    if (sidebarVisible_ == value) return;
    sidebarVisible_ = value;
    storeUiValue(kSidebarVisible, value);
    emit sidebarVisibleChanged();
}

void WorkbenchSettings::setSidebarWidth(int value)
{
    value = qBound(kSidebarMinimumContentWidth, value, kSidebarMaximumContentWidth);
    if (sidebarWidth_ == value) return;
    sidebarWidth_ = value;
    storeUiValue(kSidebarWidth, value);
    emit sidebarWidthChanged();
}

void WorkbenchSettings::setBottomPanelVisible(bool value)
{
    if (bottomPanelVisible_ == value) return;
    bottomPanelVisible_ = value;
    storeUiValue(kBottomPanelVisible, value);
    emit bottomPanelVisibleChanged();
}

void WorkbenchSettings::setBottomPanelHeightRatio(double value)
{
    value = qBound(kBottomPanelMinimumHeightRatio, value, kBottomPanelMaximumHeightRatio);
    if (qFuzzyCompare(bottomPanelHeightRatio_, value)) return;
    bottomPanelHeightRatio_ = value;
    storeUiValue(kBottomPanelHeightRatio, value);
    emit bottomPanelHeightRatioChanged();
}

void WorkbenchSettings::setPreviewWidthRatio(double value)
{
    value = qBound(kPreviewMinimumWidthRatio, value, kPreviewMaximumWidthRatio);
    if (qFuzzyCompare(previewWidthRatio_, value)) return;
    previewWidthRatio_ = value;
    storeUiValue(kPreviewWidthRatio, value);
    emit previewWidthRatioChanged();
}

void WorkbenchSettings::setPreviewDetached(bool value)
{
    if (previewDetached_ == value) return;
    previewDetached_ = value;
    storeUiValue(kPreviewDetached, value);
    emit previewDetachedChanged();
}

void WorkbenchSettings::setPreviewCanvasFreeAspect(bool value)
{
    if (previewCanvasFreeAspect_ == value) return;
    previewCanvasFreeAspect_ = value;
    storeUiValue(kPreviewCanvasFreeAspect, value);
    emit previewCanvasFreeAspectChanged();
}

void WorkbenchSettings::setPreviewNoteStatisticsVisible(bool value)
{
    if (previewNoteStatisticsVisible_ == value) return;
    previewNoteStatisticsVisible_ = value;
    storeUiValue(kPreviewNoteStatisticsVisible, value);
    emit previewNoteStatisticsVisibleChanged();
}

void WorkbenchSettings::setPreviewHidePv(bool value)
{
    if (previewHidePv_ == value) return;
    previewHidePv_ = value;
    storeUiValue(kPreviewHidePv, value);
    emit previewHidePvChanged();
}

void WorkbenchSettings::setEditorAppearance(int pointSize, double lineSpacingFactor)
{
    const QFont font = miacode::runtime::shared::editorFont(pointSize);
    const int blockSpacing =
        miacode::runtime::shared::blockSpacingPixelsForPointSize(pointSize, lineSpacingFactor);
    if (codeFont_ == font && editorBlockSpacing_ == blockSpacing) {
        return;
    }
    codeFont_ = font;
    editorBlockSpacing_ = blockSpacing;
    emit editorSettingsChanged();
}

void WorkbenchSettings::setEditorAutoWrap(bool enabled)
{
    if (editorAutoWrap_ == enabled) return;
    editorAutoWrap_ = enabled;
    storeUiValue(kEditorAutoWrap, enabled);
    emit editorSettingsChanged();
}

void WorkbenchSettings::setEditorScrollPastEnd(bool enabled)
{
    if (editorScrollPastEnd_ == enabled) {
        return;
    }
    editorScrollPastEnd_ = enabled;
    storeUiValue(kEditorScrollPastEnd, enabled);
    emit editorSettingsChanged();
}

void WorkbenchSettings::setEditorSelectionBeatDisplay(bool enabled)
{
    if (editorSelectionBeatDisplay_ == enabled) {
        return;
    }
    editorSelectionBeatDisplay_ = enabled;
    storeUiValue(kEditorSelectionBeatDisplay, enabled);
    emit editorSettingsChanged();
}

void WorkbenchSettings::setEditorOverviewRulerEnabled(bool enabled)
{
    if (editorOverviewRulerEnabled_ == enabled) {
        return;
    }
    editorOverviewRulerEnabled_ = enabled;
    storeUiValue(kEditorOverviewRulerEnabled, enabled);
    emit editorSettingsChanged();
}

void WorkbenchSettings::setFontSize(int value)
{
    value = qBound(12, value, 14);
    if (fontSize_ == value) return;
    fontSize_ = value;
    storeUiValue(kFontSize, value);
    emit fontSizeChanged();
}


void WorkbenchSettings::setAdvancedMetadataEnabled(bool value)
{
    if (advancedMetadataEnabled_ == value) return;
    advancedMetadataEnabled_ = value;
    storeUiValue("advanced_metadata_enabled", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setLowPerformanceMode(bool value)
{
    if (lowPerformanceMode_ == value) return;
    lowPerformanceMode_ = value;
    emit blurMaterialsEnabledChanged();
    storeUiValue("low_performance_mode", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setDisableAltMenuActivation(bool value)
{
    if (disableAltMenuActivation_ == value) return;
    disableAltMenuActivation_ = value;
    storeUiValue("disable_alt_menu_activation", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setSpaceGuardEnabled(bool value)
{
    if (spaceGuardEnabled_ == value) return;
    spaceGuardEnabled_ = value;
    storeUiValue("space_guard_enabled", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setAutoNormalizeEnabled(bool value)
{
    if (autoNormalizeEnabled_ == value) return;
    autoNormalizeEnabled_ = value;
    storeUiValue("auto_normalize_enabled", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setWarnEmptyDifficulty(bool value)
{
    if (warnEmptyDifficulty_ == value) return;
    warnEmptyDifficulty_ = value;
    storeUiValue("warn_empty_difficulty", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setTimelineLayoutMode(const QString& value)
{
    const QString mode = value.startsWith(QStringLiteral("vertical_")) ? QStringLiteral("vertical") : value;
    if (mode != QStringLiteral("vertical") && mode != QStringLiteral("horizontal")) return;
    if (timelineLayoutMode_ == mode) return;
    timelineLayoutMode_ = mode;
    storeUiValue("timeline_layout_mode", mode);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setTimelineVerticalDirection(const QString& value)
{
    if (!QStringList{QStringLiteral("falling"), QStringLiteral("rising")}.contains(value)) return;
    if (timelineVerticalDirection_ == value) return;
    timelineVerticalDirection_ = value;
    storeUiValue("timeline_vertical_direction", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setWorkspaceLayout(const QVariantMap& layout)
{
    if (workspaceLayout_ == layout) return;
    workspaceLayout_ = layout;
    storeUiValue(kWorkspaceLayout, QJsonObject::fromVariantMap(layout));
    emit workspaceLayoutChanged();
}

QStringList WorkbenchSettings::savedWorkspaceLayoutOrder() const
{
    QStringList names;
    for (const auto& key : workspaceListOrder_)
        if (key.startsWith(kSavedPrefix)) names.append(key.mid(kSavedPrefix.size()));
    return names;
}

void WorkbenchSettings::saveWorkspaceLayout(const QString& name, const QVariantMap& layout, int shortcut)
{
    const QString key = name.trimmed();
    if (key.isEmpty() || key.size() > 48 || shortcut < -2 || shortcut >= workspaceSlots_.size()) return;
    const QString entry = kSavedPrefix + key;
    savedWorkspaceLayouts_.insert(key, layout);
    if (!workspaceListOrder_.contains(entry)) workspaceListOrder_.append(entry);
    // -2 keeps an existing binding, -1 explicitly unbinds the layout.
    if (shortcut >= -1) {
        const int old = workspaceSlots_.indexOf(entry);
        if (old >= 0) workspaceSlots_[old] = kInitialSlots.at(old);
        if (shortcut >= 0) workspaceSlots_[shortcut] = entry;
    }
    persistSavedWorkspaceLayouts();
}

void WorkbenchSettings::persistSavedWorkspaceLayouts()
{
    auto root = PreferenceDocument::loadPreferencesObject();
    auto ui = root.value(QLatin1String(kUiSection)).toObject();
    ui.insert(QLatin1String(kSavedWorkspaceLayouts), QJsonObject::fromVariantMap(savedWorkspaceLayouts_));
    ui.insert(QLatin1String(kSavedWorkspaceLayoutOrder), QJsonArray::fromStringList(savedWorkspaceLayoutOrder()));
    ui.insert(QLatin1String(kWorkspaceListOrder), QJsonArray::fromStringList(workspaceListOrder_));
    ui.insert(QLatin1String(kWorkspaceSlots), QJsonArray::fromStringList(workspaceSlots_));
    root.insert(QLatin1String(kUiSection), ui);
    PreferenceDocument::savePreferencesObject(root);
    emit savedWorkspaceLayoutsChanged();
}

bool WorkbenchSettings::renameWorkspaceLayout(const QString& oldName, const QString& newName)
{
    const QString key = newName.trimmed();
    if (!savedWorkspaceLayouts_.contains(oldName) || key.isEmpty() || key.size() > 48
        || (key != oldName && savedWorkspaceLayouts_.contains(key))) return false;
    if (oldName == key) return true;
    const QVariant layout = savedWorkspaceLayouts_.take(oldName);
    savedWorkspaceLayouts_.insert(key, layout);
    const int row = workspaceListOrder_.indexOf(kSavedPrefix + oldName);
    if (row >= 0) workspaceListOrder_[row] = kSavedPrefix + key;
    const int shortcut = workspaceSlots_.indexOf(kSavedPrefix + oldName);
    if (shortcut >= 0) workspaceSlots_[shortcut] = kSavedPrefix + key;
    persistSavedWorkspaceLayouts();
    return true;
}

bool WorkbenchSettings::copyWorkspaceLayout(const QString& name, const QString& newName)
{
    const QString key = newName.trimmed();
    if (!savedWorkspaceLayouts_.contains(name) || key.isEmpty() || key.size() > 48
        || savedWorkspaceLayouts_.contains(key)) return false;
    savedWorkspaceLayouts_.insert(key, savedWorkspaceLayouts_.value(name));
    const int index = workspaceListOrder_.indexOf(kSavedPrefix + name);
    workspaceListOrder_.insert(index + 1, kSavedPrefix + key);
    persistSavedWorkspaceLayouts();
    return true;
}

void WorkbenchSettings::moveWorkspaceEntry(const QString& key, int destination)
{
    const int index = workspaceListOrder_.indexOf(key);
    if (index < 0 || destination < 0 || destination >= workspaceListOrder_.size() || destination == index) return;
    workspaceListOrder_.move(index, destination);
    persistSavedWorkspaceLayouts();
}

void WorkbenchSettings::setWorkspaceHiddenPanels(const QVariantMap& panels)
{
    if (workspaceHiddenPanels_ == panels) return;
    workspaceHiddenPanels_ = panels;
    storeUiValue(kWorkspaceHiddenPanels, QJsonObject::fromVariantMap(panels));
    emit workspaceHiddenPanelsChanged();
}

QRect WorkbenchSettings::boundedWorkspaceWindowGeometry(const QRect& rect) const
{
    QScreen* screen = QGuiApplication::screenAt(rect.topLeft() + QPoint(60, 15));
    const bool reachable = screen != nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return rect;
    const QRect available = screen->availableGeometry();
    const QSize size = rect.size().boundedTo(available.size());
    if (!reachable) return QRect(available.center() - QPoint(size.width() / 2, size.height() / 2), size);
    return QRect(QPoint(qBound(available.x(), rect.x(), available.x() + available.width() - size.width()),
                        qBound(available.y(), rect.y(), available.y() + available.height() - size.height())), size);
}

void WorkbenchSettings::removeWorkspaceLayout(const QString& name)
{
    if (!savedWorkspaceLayouts_.remove(name)) return;
    const QString entry = kSavedPrefix + name;
    workspaceListOrder_.removeAll(entry);
    const int shortcut = workspaceSlots_.indexOf(entry);
    if (shortcut >= 0) workspaceSlots_[shortcut] = kInitialSlots.at(shortcut);
    persistSavedWorkspaceLayouts();
}

void WorkbenchSettings::setTimelinePanelVisible(bool value)
{
    if (timelinePanelVisible_ == value) return;
    timelinePanelVisible_ = value;
    storeUiValue(kTimelinePanelVisible, value);
    emit timelinePanelVisibleChanged();
}

void WorkbenchSettings::setTimelineNoteScale(double value)
{
    if (!qIsFinite(value)) return;
    value = qBound(0.5, value, 3.0);
    if (qFuzzyCompare(timelineNoteScale_, value)) return;
    timelineNoteScale_ = value;
    storeUiValue("timeline_note_scale", value);
    emit timelineNoteScaleChanged();
}

void WorkbenchSettings::setTimelineLaneOrder(const QString& value)
{
    if (!QStringList{QStringLiteral("natural"), QStringLiteral("rotated"), QStringLiteral("reversed"), QStringLiteral("rotated_reversed")}.contains(value)) return;
    if (timelineLaneOrder_ == value) return;
    timelineLaneOrder_ = value;
    storeUiValue("timeline_lane_order", value);
    emit workflowSettingsChanged();
}

void WorkbenchSettings::setTimelineVerticalWidthRatio(double value)
{
    if (!qIsFinite(value)) return;
    value = qBound(0.2, value, 0.6);
    if (qFuzzyCompare(timelineVerticalWidthRatio_, value)) return;
    timelineVerticalWidthRatio_ = value;
    storeUiValue("timeline_vertical_width_ratio", value);
    emit workflowSettingsChanged();
}

} // namespace miacode::ui
