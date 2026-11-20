#include "app/services/HandOrderFeaturePreferences.h"
#include "app/services/StickingChartCopy.h"
#include "common/RuntimePaths.h"
#include "export/video_export/VideoExportSnapshot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir fixture(QDir(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT)).filePath("hand-order-features-XXXXXX"));
    if (!fixture.isValid()) return 1;
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QString runtime = fixture.filePath("runtime/features");
    if (!QDir().mkpath(fixture.filePath("source")) || !QDir().mkpath(runtime)
        || !write(fixture.filePath("baseline.json"), "{}")
        || !write(fixture.filePath("source/AGENTS.md"), "synthetic fixture")
        || !write(QDir(runtime).filePath(".better-miacode-runtime"), "BetterMiacode.runtime.v1\n")) return 1;
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", runtime.toUtf8());
    QString error;
    if (!miacode::runtime_paths::initializeFromEnvironment(&error)
        || !miacode::runtime_paths::configureProcessEnvironment(&error)) return 1;
    namespace features = miacode::hand_order_features;
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << label << '\n'; ok &= pass;
    };
    check(!features::enabled() && !features::watermark(), "new and old profiles default both options off");
    const QString chart = fixture.filePath("maidata.txt");
    const QByteArray original = "&title=Feature fixture\n&lv_5=12\n&inote_5=(120){4}1~l,5~r,E\n";
    if (!write(chart, original)) return 1;
    check(miacode::sticking::editableChartPath(chart) == chart
        && !QFile::exists(fixture.filePath("maidata_sticking.txt")), "disabled mode opens original without creating a Sticking copy");
    VideoExportTask task;
    task.showHandOrderAnnotations = true;
    task.showHandOrderWatermark = true;
    task.muriRenderOptions.renderMode = RenderMode::SimulatedPlay;
    features::applyExportPolicy(task);
    check(!task.showHandOrderAnnotations && !task.showHandOrderWatermark
        && task.muriRenderOptions.renderMode == RenderMode::Native && !task.muriRenderOptions.playAnalysisEnabled,
        "disabled features suppress annotated output, watermark and simulated play");
    auto preferences = PreferenceDocument::loadPreferencesObject();
    auto values = preferences.value("app").toObject();
    values.insert("hand_order_features_enabled", true);
    values.insert("hand_order_watermark_enabled", false);
    preferences.insert("app", values);
    check(PreferenceDocument::savePreferencesObject(preferences) && !features::enabled(),
        "an enabled legacy profile still requires verification");
    check(features::unlock(QStringLiteral("wrong")) == features::UnlockResult::IncorrectPassword
        && !features::enabled(), "wrong password leaves access disabled");
    check(features::unlock(QStringLiteral("test-hand-order")) == features::UnlockResult::Enabled
        && features::enabled(), "correct password persists a verified grant");
    task.showHandOrderAnnotations = true;
    features::applyExportPolicy(task);
    check(features::enabled() && task.showHandOrderAnnotations && !task.showHandOrderWatermark,
        "enabling features does not enable watermark");
    check(miacode::sticking::editableChartPath(chart) == fixture.filePath("maidata_sticking.txt"),
        "opt-in restores protected Sticking copy workflow");
    values.insert("hand_order_watermark_enabled", true); preferences.insert("app", values);
    check(PreferenceDocument::savePreferencesObject(preferences), "watermark selection persists independently");
    features::applyExportPolicy(task);
    check(task.showHandOrderAnnotations && task.showHandOrderWatermark, "watermark requires explicit selection");
    VideoExportSnapshot snapshot;
    snapshot.chartTextUtf8 = QString::fromUtf8(original); snapshot.difficultyId = 5;
    snapshot.outputPath = fixture.filePath("unused.mp4"); snapshot.showHandOrderWatermark = true;
    VideoExportSnapshot roundtrip;
    check(VideoExportSnapshot::fromJson(snapshot.toJson(), &roundtrip, &error) && roundtrip.showHandOrderWatermark,
        "watermark survives worker snapshot transport");
    auto legacy = snapshot.toJson(); auto render = legacy.value("render").toObject();
    render.remove("show_hand_order_watermark"); legacy.insert("render", render);
    check(VideoExportSnapshot::fromJson(legacy, &roundtrip, &error) && !roundtrip.showHandOrderWatermark,
        "old snapshots never implicitly enable watermark");
    check(features::disable() && !features::enabled()
        && features::unlock(QStringLiteral("wrong")) == features::UnlockResult::IncorrectPassword
        && !features::enabled(), "disabling revokes the grant and reenabling requires the password");
    features::applyExportPolicy(task);
    QFile kept(chart); kept.open(QIODevice::ReadOnly);
    check(!task.showHandOrderAnnotations && !task.showHandOrderWatermark && features::watermark()
        && miacode::sticking::editableChartPath(chart) == chart && kept.readAll() == original
        && QFile::exists(fixture.filePath("maidata_sticking.txt")),
        "disable preserves data and watermark preference while stopping feature effects");
    QFile stored(PreferenceDocument::preferencesFilePath()); stored.open(QIODevice::ReadOnly);
    const auto persisted = QJsonDocument::fromJson(stored.readAll()).object();
    check(!QFile::exists(features::grantPath()) && features::watermark(persisted), "on-disk state agrees with live state");
    return ok ? 0 : 1;
}
