#include "common/RuntimePaths.h"
#include "common/CrashRecovery.h"
#include "common/DebugLog.h"
#include "app/services/PreferenceDocument.h"
#include "app/runtime/settings/EncoderProbeCache.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
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
    if (app.arguments().contains(QStringLiteral("--probe"))) {
        QString error;
        if (!miacode::runtime_paths::initialize(&error)) return 78;
        QTextStream(stdout) << miacode::runtime_paths::root();
        return 0;
    }
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* message) {
        if (!condition) { QTextStream(stderr) << "FAIL: " << message << '\n'; ok = false; }
    };
    QTemporaryDir temporary(QDir(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT))
        .filePath(QStringLiteral("better-runtime-spec-XXXXXX")));
    expect(temporary.isValid(), "workspace-scoped synthetic runtime");
    if (!temporary.isValid()) return 1;
    using namespace miacode::runtime_paths;
    QString error;
    expect(!validateRoot(QString(), &error), "empty root rejected");
    expect(!validateRoot(QStringLiteral("runtime"), &error), "relative root rejected");
    expect(!validateRoot(temporary.path(), &error), "unmarked root rejected");
    const QString marker = temporary.filePath(QStringLiteral(".better-miacode-runtime"));
    expect(writeFile(marker, "MiaCodeHandOrder.runtime.v1\n"), "foreign marker fixture");
    expect(!validateRoot(temporary.path(), &error), "foreign product marker rejected");
    expect(writeFile(marker, "BetterMiacode.runtime.v1\n"), "runtime marker fixture");
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", temporary.path().toUtf8());
    qputenv("MIACODE_LOG_DIR", "must-not-be-used");
    qputenv("MIACODE_FATAL_LOG_PATH", "must-not-be-used.log");
    expect(initialize(&error), "marked root initialized");
    expect(root() == temporary.path(), "absolute root retained");
    expect(QCoreApplication::applicationName() == QStringLiteral("BetterMiacode"), "independent Qt identity");
    expect(qEnvironmentVariable("MIACODE_LOG_DIR") == directory("logs"), "inherited official log override replaced");
    expect(qEnvironmentVariableIsEmpty("MIACODE_FATAL_LOG_PATH"), "inherited log file override removed");
    expect(QDir::tempPath() == directory("temp"), "Qt temporary files isolated");
    QSettings settings;
    settings.setValue(QStringLiteral("isolation/fixture"), true);
    settings.sync();
    expect(settings.fileName().startsWith(directory("config") + '/'), "QSettings stays in runtime config");
    expect(writeFile(configFile("preferences.json"), "{\"ui_theme\":\"light\",\"recent_files\":[\"foreign-chart\"]}"),
        "synthetic legacy preference sentinel");
    expect(PreferenceDocument::preferencesFilePath() == configFile("preferences_v2.json"), "preferences isolated");
    expect(PreferenceDocument::preferredTheme() == PreferenceDocument::ThemePreference::Dark,
        "fresh profile never imports legacy preferences");
    const QString syntheticChart = temporary.filePath(QStringLiteral("synthetic-chart/maidata.txt"));
    const QString project = projectDirectory(syntheticChart);
    expect(project.startsWith(directory("projects") + '/'), "project cache namespace isolated");
    expect(project == projectDirectory(temporary.filePath(QStringLiteral("synthetic-chart/other.txt"))),
        "charts sharing a directory share project metadata");
    expect(project != projectDirectory(temporary.filePath(QStringLiteral("other-chart/maidata.txt"))),
        "different chart directories do not collide");
    expect(miacode::crash_recovery::crashRecoveryFilePath(syntheticChart)
        == QDir(project).filePath(QStringLiteral(".autosave/maidata.txt/maidata.txt.crash_recovery")),
        "crash recovery uses the isolated autosave tree");
    miacode::runtime::SettingsEncoderProbeCache encoder;
    encoder.rememberPreferredHardwareEncoder(QStringLiteral("synthetic_encoder"));
    expect(encoder.preferredHardwareEncoder() == QStringLiteral("synthetic_encoder")
        && QFileInfo::exists(QDir(directory("cache")).filePath(QStringLiteral("encoder-probe.ini"))),
        "encoder machine cache isolated");
    QProcess worker;
    worker.start(app.applicationFilePath(), {QStringLiteral("--probe")});
    expect(worker.waitForFinished(15000) && worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0
        && QString::fromUtf8(worker.readAllStandardOutput()) == root(), "worker inherits the exact runtime root");
    QProcess refusedWorker;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("BETTER_MIACODE_RUNTIME_ROOT"), QStringLiteral("relative-runtime"));
    refusedWorker.setProcessEnvironment(environment);
    refusedWorker.start(app.applicationFilePath(), {QStringLiteral("--probe")});
    expect(refusedWorker.waitForFinished(15000) && refusedWorker.exitCode() == 78,
        "worker refuses an invalid inherited root");
    miacode::debug_log::shutdownAsyncLogWriter();
    QTextStream(stdout) << (ok ? "Runtime isolation contracts passed\n" : "Runtime isolation contracts failed\n");
    return ok ? 0 : 1;
}
