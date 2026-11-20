#include "common/RuntimePaths.h"
#include "common/CrashRecovery.h"
#include "common/DebugLog.h"
#include "app/services/PreferenceDocument.h"
#include "app/runtime/settings/EncoderProbeCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::runtime_paths;
    if (app.arguments().contains(QStringLiteral("--probe"))) {
        QString error;
        if (!initializeFromEnvironment(&error) || !configureProcessEnvironment(&error)) return 78;
        QTextStream(stdout) << rootDirectory();
        return 0;
    }
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* message) {
        if (!condition) { QTextStream(stderr) << "FAIL: " << message << '\n'; ok = false; }
    };
    QTemporaryDir temporary(QDir(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT))
        .filePath(QStringLiteral("better-runtime-spec-XXXXXX")));
    expect(temporary.isValid(), "workspace-scoped synthetic runtime owner");
    if (!temporary.isValid()) return 1;
    QString error;
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", QByteArray());
    expect(!initializeFromEnvironment(&error), "empty root rejected");
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", "runtime");
    expect(!initializeFromEnvironment(&error), "relative root rejected");
    const QString root = temporary.filePath(QStringLiteral("runtime"));
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", root.toUtf8());
    expect(!initializeFromEnvironment(&error), "unowned runtime parent rejected");
    expect(QDir().mkpath(root)
        && writeFile(QDir(root).filePath(QStringLiteral(".better-miacode-runtime")), "MiaCodeSticking.runtime.v1\n"), "foreign marker fixture");
    expect(!initializeFromEnvironment(&error), "foreign product marker rejected");
    expect(writeFile(QDir(root).filePath(QStringLiteral(".better-miacode-runtime")), "BetterMiacode.runtime.v1\n"),
        "synthetic Better Miacode runtime marker");
    qputenv("MIACODE_LOG_DIR", "must-not-be-used");
    qputenv("MIACODE_FATAL_LOG_PATH", "must-not-be-used.log");
    expect(initializeFromEnvironment(&error) && configureProcessEnvironment(&error), "Better Miacode root and process environment initialized");
    expect(rootDirectory() == root && workspaceDirectory() == temporary.path(), "internal runtime/workspace API preserved");
    expect(QCoreApplication::applicationName() == QStringLiteral("BetterMiacode"), "independent Qt identity");
    expect(qEnvironmentVariable("MIACODE_LOG_DIR") == logDirectory() && qEnvironmentVariableIsEmpty("MIACODE_FATAL_LOG_PATH"), "official log overrides removed");
    expect(QDir::tempPath() == tempDirectory(), "Qt temporary files isolated");
    expect(exportsDirectory() == QDir(root).filePath(QStringLiteral("exports")), "exports directory is under runtime");
    QSettings settings;
    settings.setValue(QStringLiteral("isolation/fixture"), true);
    settings.sync();
    expect(settings.fileName().startsWith(configDirectory() + '/'), "QSettings stays in Better Miacode runtime config");
    const QString preferences = QDir(configDirectory()).filePath(QStringLiteral("preferences_v2.json"));
    expect(PreferenceDocument::preferencesFilePath() == preferences, "preferences use the internal config API");
    const QString syntheticChart = temporary.filePath(QStringLiteral("synthetic-chart/maidata.txt"));
    const QString project = projectDirectoryForChart(syntheticChart);
    expect(project.startsWith(root + QStringLiteral("/projects/")), "portable projects namespace preserved");
    expect(project == projectDirectoryForChart(temporary.filePath(QStringLiteral("synthetic-chart/other.txt")))
        && project != projectDirectoryForChart(temporary.filePath(QStringLiteral("other-chart/maidata.txt"))), "stable per-directory project identity");
    expect(miacode::crash_recovery::crashRecoveryFilePath(syntheticChart)
        == QDir(project).filePath(QStringLiteral(".autosave/maidata.txt/maidata.txt.crash_recovery")), "crash recovery stays under the internal autosave tree");
    miacode::runtime::SettingsEncoderProbeCache encoder;
    encoder.rememberPreferredHardwareEncoder(QStringLiteral("synthetic_encoder"));
    expect(encoder.preferredHardwareEncoder() == QStringLiteral("synthetic_encoder")
        && QFileInfo::exists(QDir(cacheDirectory()).filePath(QStringLiteral("video_export_runtime.ini"))), "encoder cache isolated");
    QProcess worker;
    worker.start(app.applicationFilePath(), {QStringLiteral("--probe")});
    expect(worker.waitForFinished(15000) && worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0
        && QString::fromUtf8(worker.readAllStandardOutput()) == rootDirectory(), "worker inherits the exact Better Miacode runtime root");
    QProcess refusedWorker;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("BETTER_MIACODE_RUNTIME_ROOT"), QStringLiteral("relative-runtime"));
    refusedWorker.setProcessEnvironment(environment);
    refusedWorker.start(app.applicationFilePath(), {QStringLiteral("--probe")});
    expect(refusedWorker.waitForFinished(15000) && refusedWorker.exitCode() == 78, "worker refuses an invalid inherited root");
    miacode::debug_log::shutdownAsyncLogWriter();
    QTextStream(stdout) << (ok ? "Better Miacode runtime isolation contracts passed\n" : "Better Miacode runtime isolation contracts failed\n");
    return ok ? 0 : 1;
}
