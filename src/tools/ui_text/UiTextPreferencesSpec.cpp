#include "app/services/PreferenceDocument.h"
#include "app/ui/preferences/HandOrderPlayerPreferences.h"
#include "app/ui/preferences/HandOrderSweepPreferences.h"
#include "app/ui/preferences/HandOrderSearchPreferences.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <limits>

namespace {

bool expect(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    bool ok = true;

    QJsonObject raw{
        {QStringLiteral("extensionShortcuts"), QJsonObject{{QStringLiteral("demo.run"), QStringLiteral("Ctrl+R")}}},
        {QStringLiteral("vendor.demo.setting"), 42},
        {QStringLiteral("theme"), QStringLiteral("light")},
        {QStringLiteral("master_volume"), 0.5},
    };
    QJsonObject normalized = PreferenceDocument::normalizePreferencesObject(raw);
    ok &= expect(normalized.value(QStringLiteral("vendor.demo.setting")).toInt() == 42,
                 QStringLiteral("unknown extension-owned top-level keys survive normalization"), err);
    ok &= expect(normalized.value(QStringLiteral("extensionShortcuts")).toObject()
                         .value(QStringLiteral("demo.run")).toString() == QLatin1String("Ctrl+R"),
                 QStringLiteral("extension shortcuts survive normalization"), err);
    ok &= expect(PreferenceDocument::themeTokenFromPreferencesObject(normalized) == QLatin1String("light"),
                 QStringLiteral("legacy top-level theme migrates to ui.theme"), err);
    ok &= expect(!normalized.contains(QStringLiteral("theme"))
                     && !normalized.contains(QStringLiteral("master_volume")),
                 QStringLiteral("migrated built-in top-level keys are removed"), err);

    raw.insert(QStringLiteral("ui"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
    normalized = PreferenceDocument::normalizePreferencesObject(raw);
    ok &= expect(PreferenceDocument::themeTokenFromPreferencesObject(normalized) == QLatin1String("dark"),
                 QStringLiteral("canonical ui.theme wins over legacy top-level theme"), err);

    QJsonObject canonicalAudio{
        {QStringLiteral("global_volume"), 0.9},
        {QStringLiteral("tap_volume"), 0.8},
        {QStringLiteral("touch_volume"), 0.15},
    };
    QJsonObject canonicalPreview{
        {QStringLiteral("background_brightness"), 0.7},
        {QStringLiteral("audio"), canonicalAudio},
    };
    QJsonObject mixed{
        {QStringLiteral("app"), QJsonObject{
             {QStringLiteral("last_open_dir"), QStringLiteral("canonical-dir")},
             {QStringLiteral("preview"), canonicalPreview},
         }},
        {QStringLiteral("last_open_dir"), QStringLiteral("legacy-dir")},
        {QStringLiteral("preview_background_brightness"), 0.1},
        {QStringLiteral("preview_audio"), QJsonObject{
             {QStringLiteral("global_volume"), 0.2},
             {QStringLiteral("slide_volume"), 0.3},
         }},
        {QStringLiteral("master_volume"), 0.4},
        {QStringLiteral("judge_volume"), 0.5},
        {QStringLiteral("touchhold_volume"), 0.95},
    };
    const QJsonObject mixedNormalized = PreferenceDocument::normalizePreferencesObject(mixed);
    const QJsonObject mixedApp = mixedNormalized.value(QStringLiteral("app")).toObject();
    const QJsonObject mixedPreview = mixedApp.value(QStringLiteral("preview")).toObject();
    const QJsonObject mixedAudio = mixedPreview.value(QStringLiteral("audio")).toObject();
    ok &= expect(mixedApp.value(QStringLiteral("last_open_dir")).toString() == QLatin1String("canonical-dir")
                     && qFuzzyCompare(mixedPreview.value(QStringLiteral("background_brightness")).toDouble(), 0.7)
                     && qFuzzyCompare(mixedAudio.value(QStringLiteral("global_volume")).toDouble(), 0.9)
                     && qFuzzyCompare(mixedAudio.value(QStringLiteral("tap_volume")).toDouble(), 0.8)
                     && qFuzzyCompare(mixedAudio.value(QStringLiteral("touch_volume")).toDouble(), 0.15),
                 QStringLiteral("canonical app/preview/audio values win over legacy aliases"), err);
    ok &= expect(qFuzzyCompare(mixedAudio.value(QStringLiteral("slide_volume")).toDouble(), 0.3),
                 QStringLiteral("missing canonical audio members are backfilled from legacy container"), err);

    const QJsonObject legacyTouchNormalized = PreferenceDocument::normalizePreferencesObject(QJsonObject{
        {QStringLiteral("master_volume"), 0.6},
        {QStringLiteral("touch_volume"), 0.2},
        {QStringLiteral("touchhold_volume"), 0.8},
    });
    const QJsonObject legacyTouchAudio = legacyTouchNormalized.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("preview")).toObject()
        .value(QStringLiteral("audio")).toObject();
    ok &= expect(qFuzzyCompare(legacyTouchAudio.value(QStringLiteral("touch_volume")).toDouble(), 0.8),
                 QStringLiteral("legacy touch and touchhold volumes retain their maximum-value migration"), err);

    QJsonObject written = normalized;
    PreferenceDocument::setThemeTokenInPreferencesObject(&written, QStringLiteral("system"));
    ok &= expect(PreferenceDocument::themeTokenFromPreferencesObject(written) == QLatin1String("system")
                     && !written.contains(QStringLiteral("theme")),
                 QStringLiteral("canonical theme helper writes ui.theme only"), err);

    ok &= expect(PreferenceDocument::themePreferenceFromToken(QStringLiteral("legacy"))
                     == PreferenceDocument::ThemePreference::Legacy
                 && PreferenceDocument::themePreferenceToken(PreferenceDocument::ThemePreference::Legacy)
                        == QLatin1String("legacy"),
                 QStringLiteral("legacy token round-trips without falling back to system"), err);
    ok &= expect(PreferenceDocument::themePreferenceFromToken(QStringLiteral("unknown"))
                     == PreferenceDocument::ThemePreference::System,
                 QStringLiteral("unknown theme tokens still fall back to system"), err);

    QTemporaryDir scaleDirectory;
    ok &= expect(scaleDirectory.isValid(), QStringLiteral("temporary UI scale preferences directory"), err);
    if (scaleDirectory.isValid()) {
        const QString path = scaleDirectory.filePath(QStringLiteral("preferences.json"));
        PreferenceDocument::Repository repository(path);
        QJsonObject scaleRoot = repository.snapshot();
        QJsonObject scaleUi = scaleRoot.value(QStringLiteral("ui")).toObject();
        scaleUi.insert(QStringLiteral("ui_scale_percent"), 137);
        scaleUi.insert(QStringLiteral("editor_text_font_size"), 17);
        scaleRoot.insert(QStringLiteral("ui"), scaleUi);
        ok &= expect(repository.replace(scaleRoot), QStringLiteral("UI scale preferences persist"), err);

        PreferenceDocument::Repository reopened(path);
        QJsonObject reopenedRoot = reopened.snapshot();
        QJsonObject reopenedUi = reopenedRoot.value(QStringLiteral("ui")).toObject();
        ok &= expect(reopenedUi.value(QStringLiteral("ui_scale_percent")).toInt() == 137
                         && reopenedUi.value(QStringLiteral("editor_text_font_size")).toInt() == 17,
                     QStringLiteral("UI scale and editor font size retain independent values across launches"), err);
        reopenedUi.insert(QStringLiteral("ui_scale_percent"), 100);
        reopenedRoot.insert(QStringLiteral("ui"), reopenedUi);
        ok &= expect(reopened.replace(reopenedRoot), QStringLiteral("reset UI scale preferences persist"), err);

        PreferenceDocument::Repository resetLifetime(path);
        const QJsonObject resetUi = resetLifetime.snapshot().value(QStringLiteral("ui")).toObject();
        ok &= expect(resetUi.value(QStringLiteral("ui_scale_percent")).toInt() == 100
                         && resetUi.value(QStringLiteral("editor_text_font_size")).toInt() == 17,
                     QStringLiteral("resetting UI scale preserves the editor font size"), err);
    }

    // Exercise the same JSON accessors used by PreferencesModel, with fresh
    // repositories proving that changes survive a new preferences lifetime.
    namespace player = miacode::ui::hand_order_player_preferences;
    namespace sweep = miacode::ui::hand_order_sweep_preferences;
    namespace search = miacode::ui::hand_order_search_preferences;
    QTemporaryDir playerDirectory;
    ok &= expect(playerDirectory.isValid(), QStringLiteral("temporary player preferences directory"), err);
    if (playerDirectory.isValid()) {
        const QString path = playerDirectory.filePath(QStringLiteral("preferences.json"));
        PreferenceDocument::Repository repository(path);
        QJsonObject playerRoot = repository.snapshot();
        playerRoot.insert(QStringLiteral("app"), QJsonObject{
            {QStringLiteral("hand_order_return_speed"), 4.25},
            {QStringLiteral("hand_order_player"), QJsonObject{{QStringLiteral("vendor.custom"), 42}}}});
        ok &= expect(player::styleToken(playerRoot) == QLatin1String("comprehensive")
                         && search::mode(playerRoot) == QLatin1String("standard")
                         && search::millions(playerRoot) == 300,
                     QStringLiteral("new Sticking defaults are comprehensive with a 300 million work budget"), err);
        QJsonObject presetRoot = playerRoot;
        ok &= expect(player::applyPreset(&presetRoot, QStringLiteral("hard"))
                         && player::styleToken(presetRoot) == QLatin1String("hard")
                         && player::read(presetRoot).value("maxComfortableTapRate").toDouble() == 8.5
                         && player::applyPreset(&presetRoot, QStringLiteral("pagination"))
                         && player::applyPreset(&presetRoot, QStringLiteral("decomposition"))
                         && player::read(presetRoot).value("techniquePreference").toDouble() == -0.9,
                     QStringLiteral("three alternate style presets write distinct player parameters"), err);
        ok &= expect(player::set(&presetRoot, QStringLiteral("comfortableMoveSpeed"), 9.0)
                         && player::styleToken(presetRoot) == QLatin1String("custom")
                         && player::reset(&presetRoot)
                         && player::styleToken(presetRoot) == QLatin1String("comprehensive"),
                     QStringLiteral("manual player adjustments become custom and reset to comprehensive"), err);
        QJsonObject searchRoot = playerRoot;
        ok &= expect(search::setMode(&searchRoot, QStringLiteral("low"))
                         && search::millions(searchRoot) == 50
                         && search::setMode(&searchRoot, QStringLiteral("high"))
                         && search::millions(searchRoot) == 600
                         && search::setMode(&searchRoot, QStringLiteral("custom"))
                         && search::millions(searchRoot) == 600
                         && search::setCustomMillions(&searchRoot, 750)
                         && search::mode(searchRoot) == QLatin1String("custom")
                         && search::millions(searchRoot) == 750,
                     QStringLiteral("search budget modes and custom work cap change the effective budget"), err);
        ok &= expect(repository.replace(searchRoot)
                         && search::mode(PreferenceDocument::Repository(path).snapshot()) == QLatin1String("custom")
                         && search::millions(PreferenceDocument::Repository(path).snapshot()) == 750,
                     QStringLiteral("custom Sticking work budget survives a fresh preference repository"), err);
        QJsonObject boundedSearch = searchRoot;
        ok &= expect(search::setCustomMillions(&boundedSearch, 20000)
                         && search::millions(boundedSearch) == 10000
                         && search::setCustomMillions(&boundedSearch, -10)
                         && search::millions(boundedSearch) == 10,
                     QStringLiteral("custom Sticking work budget stays within its documented bounds"), err);
        ok &= expect(sweep::threshold(playerRoot) == 160 && sweep::colorUnresolved(playerRoot),
            QStringLiteral("sweep threshold and unresolved-color default are stable"), err);
        ok &= expect(sweep::setThreshold(&playerRoot, 300)
                && sweep::setColorUnresolved(&playerRoot, true),
            QStringLiteral("integer sweep threshold and default-color opt-in write together"), err);
        const QVariantMap defaults = player::read(playerRoot);
        const QVariantMap customized{
            {QStringLiteral("maxComfortableTapRate"), 7.25},
            {QStringLiteral("comfortableMoveSpeed"), 9.5},
            {QStringLiteral("staminaCapacity"), 1.4},
            {QStringLiteral("recoveryHalfLife"), 4.0},
            {QStringLiteral("techniquePreference"), -0.7},
            {QStringLiteral("postureWeight"), 6.25},
        };
        for (auto it = customized.cbegin(); it != customized.cend(); ++it)
            ok &= expect(player::set(&playerRoot, it.key(), it.value().toDouble()),
                         QStringLiteral("custom player field writes: ") + it.key(), err);
        ok &= expect(repository.replace(playerRoot), QStringLiteral("custom player profile persists"), err);
        PreferenceDocument::Repository reopened(path);
        const QJsonObject saved = reopened.snapshot();
        ok &= expect(sweep::threshold(saved) == 300 && sweep::colorUnresolved(saved),
            QStringLiteral("sweep settings survive a fresh preference repository"), err);
        QJsonObject invalidSweep = saved;
        QJsonObject invalidApp = invalidSweep.value(QStringLiteral("app")).toObject();
        invalidApp.insert(QStringLiteral("hand_order_sweep_threshold"), 300.5);
        invalidSweep.insert(QStringLiteral("app"), invalidApp);
        ok &= expect(sweep::threshold(invalidSweep) == 160,
            QStringLiteral("fractional stored sweep thresholds return to the integer default"), err);
        ok &= expect(player::read(saved) == customized,
                     QStringLiteral("all six custom fields reload from a new repository"), err);
        ok &= expect(saved.value("app").toObject().value("hand_order_return_speed").toDouble() == 4.25
                         && saved.value("app").toObject().value("hand_order_player").toObject()
                             .value("vendor.custom").toInt() == 42,
                     QStringLiteral("writing profile preserves adjacent and unknown profile fields"), err);

        const QJsonValue badValues[] = {QJsonValue(), QJsonValue(true), QJsonValue("5.5"),
                                       QJsonValue(QJsonObject{}), QJsonValue(-100.0), QJsonValue(100.0)};
        for (auto selected = customized.cbegin(); selected != customized.cend(); ++selected) {
            for (const auto& bad : badValues) {
                QJsonObject damaged = saved;
                QJsonObject damagedApp = damaged.value("app").toObject();
                QJsonObject damagedProfile = damagedApp.value("hand_order_player").toObject();
                damagedProfile.insert(selected.key(), bad);
                damagedApp.insert(QStringLiteral("hand_order_player"), damagedProfile);
                damaged.insert(QStringLiteral("app"), damagedApp);
                const auto actual = player::read(damaged);
                QVariantMap expected = customized;
                expected.insert(selected.key(), defaults.value(selected.key()));
                ok &= expect(actual == expected,
                             QStringLiteral("invalid field falls back without losing valid siblings: ") + selected.key(), err);
            }
        }

        QJsonObject bounded = saved;
        const double lower[] = {2.0, 2.0, 0.5, 0.5, -1.0, 0.0};
        const double upper[] = {12.0, 20.0, 2.0, 10.0, 1.0, 8.0};
        const QStringList names{QStringLiteral("maxComfortableTapRate"), QStringLiteral("comfortableMoveSpeed"),
                                QStringLiteral("staminaCapacity"), QStringLiteral("recoveryHalfLife"),
                                QStringLiteral("techniquePreference"), QStringLiteral("postureWeight")};
        for (int i = 0; i < names.size(); ++i) {
            player::set(&bounded, names.at(i), -100.0);
            ok &= expect(player::read(bounded).value(names.at(i)).toDouble() == lower[i],
                         QStringLiteral("setter clamps lower bound: ") + names.at(i), err);
            player::set(&bounded, names.at(i), 100.0);
            ok &= expect(player::read(bounded).value(names.at(i)).toDouble() == upper[i],
                         QStringLiteral("setter clamps upper bound: ") + names.at(i), err);
        }
        const QJsonObject beforeRejected = bounded;
        ok &= expect(!player::set(&bounded, QStringLiteral("vendor.custom"), 3.0)
                         && !player::set(&bounded, names.first(), std::numeric_limits<double>::quiet_NaN())
                         && !player::set(&bounded, names.first(), std::numeric_limits<double>::infinity())
                         && !player::set(&bounded, names.first(), -std::numeric_limits<double>::infinity())
                         && bounded == beforeRejected,
                     QStringLiteral("unknown names and nonfinite writes do not mutate preferences"), err);

        QJsonObject reset = saved;
        ok &= expect(player::reset(&reset) && reopened.replace(reset),
                     QStringLiteral("reset profile persists"), err);
        PreferenceDocument::Repository resetLifetime(path);
        const auto resetSaved = resetLifetime.snapshot();
        const QVariantMap expectedDefaults{
            {QStringLiteral("maxComfortableTapRate"), 7.5},
            {QStringLiteral("comfortableMoveSpeed"), 7.5},
            {QStringLiteral("staminaCapacity"), 1.2},
            {QStringLiteral("recoveryHalfLife"), 2.5},
            {QStringLiteral("techniquePreference"), 0.2},
            {QStringLiteral("postureWeight"), 6.0},
        };
        ok &= expect(player::read(resetSaved) == expectedDefaults
                         && resetSaved.value("app").toObject().value("hand_order_return_speed").toDouble() == 4.25
                         && resetSaved.value("app").toObject().value("hand_order_player").toObject()
                             .value("vendor.custom").toInt() == 42,
                     QStringLiteral("reset reloads defaults while preserving unrelated values"), err);
        ok &= expect(!player::reset(&reset), QStringLiteral("repeated reset is a no-op"), err);
        const QJsonObject oldPlayerProfile{{"maxComfortableTapRate", 6.5}, {"postureWeight", 4.0}};
        const QJsonObject oldPlayerSettings{{"hand_order_player_style", "comprehensive"},
            {"hand_order_player", oldPlayerProfile}};
        QJsonObject previousComprehensive{{"app", oldPlayerSettings}};
        ok &= expect(player::read(previousComprehensive) == expectedDefaults,
            "existing Comprehensive preset follows the tuned defaults", err);
        ok &= expect(player::set(&previousComprehensive, "comfortableMoveSpeed", 8.0)
            && player::styleToken(previousComprehensive) == "custom"
            && player::read(previousComprehensive).value("maxComfortableTapRate").toDouble() == 7.5
            && player::read(previousComprehensive).value("postureWeight").toDouble() == 6.0,
            "customizing one field starts with the effective tuned profile", err);
    }

    {
        namespace player = miacode::ui::hand_order_player_preferences;
        QJsonObject displayRoot;
        const auto displayed = player::readDisplay(displayRoot);
        ok &= expect(displayed.value("maxComfortableTapRate").toDouble() == 112.5
            && displayed.value("comfortableMoveSpeed").toDouble() == 3.75,
            "default player display uses 16th-note BPM and diameters/s", err);
        for (double rate = 2; rate <= 12; rate += 0.25) {
            player::setDisplay(&displayRoot, "maxComfortableTapRate", rate * 15);
            ok &= expect(player::read(displayRoot).value("maxComfortableTapRate").toDouble() == rate
                && player::readDisplay(displayRoot).value("maxComfortableTapRate").toDouble() == rate * 15,
                "every displayed BPM step round-trips to the existing hits/s storage", err);
        }
        for (double speed = 2; speed <= 20; speed += 0.25) {
            player::setDisplay(&displayRoot, "comfortableMoveSpeed", speed / 2);
            ok &= expect(player::read(displayRoot).value("comfortableMoveSpeed").toDouble() == speed,
                "every displayed diameter-speed step preserves radii/s storage", err);
        }
        const auto before = displayRoot;
        ok &= expect(!player::setDisplay(&displayRoot, "maxComfortableTapRate", std::numeric_limits<double>::infinity())
            && displayRoot == before, "nonfinite display writes are rejected", err);
        QFile qml(QFileInfo(QString::fromLocal8Bit(__FILE__)).dir().filePath("../../app/ui/preferences/PreferencesDialog.qml"));
        const bool opened = qml.open(QIODevice::ReadOnly);
        const QString source = QString::fromUtf8(qml.readAll());
        ok &= expect(opened && source.contains("value: root.preferencesModel.handOrderPlayerDisplayProfile.maxComfortableTapRate")
            && source.contains("setHandOrderPlayerDisplayParameter(\"maxComfortableTapRate\", v)")
            && source.contains("value: root.preferencesModel.handOrderPlayerDisplayProfile.comfortableMoveSpeed")
            && source.contains("setHandOrderPlayerDisplayParameter(\"comfortableMoveSpeed\", v)")
            && source.contains("stepSize: 3.75") && source.contains("stepSize: 0.125"),
            "real preference sliders use display getters/setters and converted steps", err);
    }
    if (ok) {
        QTextStream(stdout) << "ui_text_preferences_spec ok\n";
        return 0;
    }
    return 1;
}
