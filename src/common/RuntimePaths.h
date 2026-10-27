#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QUuid>

// Better Miacode owns a marked, explicit portable data root. No fallback to
// upstream profile directories or a caller's current working directory.
namespace miacode::runtime_paths {

inline bool initialize(QString* error = nullptr);

inline QString& validatedRoot()
{
    static QString root;
    return root;
}

inline bool validateRoot(const QString& candidate, QString* error = nullptr)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate))
        return fail(QStringLiteral("BETTER_MIACODE_RUNTIME_ROOT must be an absolute marked directory."));
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
    const QFileInfo info(clean);
    if (!info.isDir() || info.isSymLink() || QDir(clean).isRoot()
        || info.canonicalFilePath().compare(clean, Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("Runtime root must exist and cannot be a filesystem root or redirected directory."));
    QFile marker(QDir(clean).filePath(QStringLiteral(".better-miacode-runtime")));
    if (QFileInfo(marker).isSymLink() || !marker.open(QIODevice::ReadOnly)
        || marker.readAll().trimmed() != QByteArrayLiteral("BetterMiacode.runtime.v1"))
        return fail(QStringLiteral("Runtime root is missing its Better Miacode identity marker."));
    const QStringList directories{QStringLiteral("config"), QStringLiteral("cache"),
        QStringLiteral("sessions"), QStringLiteral("projects"), QStringLiteral("logs"),
        QStringLiteral("temp")};
    for (const QString& name : directories) {
        const QString path = QDir(clean).filePath(name);
        const QFileInfo child(path);
        if (child.isSymLink() || (child.exists() && !child.isDir()))
            return fail(QStringLiteral("Runtime directory is redirected or invalid: %1").arg(path));
        if (child.exists() && child.canonicalFilePath().compare(path, Qt::CaseInsensitive) != 0)
            return fail(QStringLiteral("Runtime directory escapes its root: %1").arg(path));
    }
    return true;
}

inline QString root()
{
    if (validatedRoot().isEmpty()) {
        QString error;
        if (!initialize(&error)) qFatal("Better Miacode runtime isolation: %s", qPrintable(error));
    }
    return validatedRoot();
}

inline QString directory(const char* name)
{
    return QDir(root()).filePath(QString::fromLatin1(name));
}

inline QString configFile(const char* name)
{
    return QDir(directory("config")).filePath(QString::fromLatin1(name));
}

inline QString temporaryFile(const QString& fileName)
{
    return QDir(directory("temp")).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + '-' + QFileInfo(fileName).fileName());
}

inline QString projectDirectory(const QString& chartPath)
{
    if (chartPath.isEmpty()) return {};
    const QFileInfo info(chartPath);
    const QString absoluteDirectory = QDir::cleanPath(info.absolutePath());
    QString identity = QFileInfo(absoluteDirectory).canonicalFilePath();
    if (identity.isEmpty()) identity = absoluteDirectory;
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    const QByteArray key = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(directory("projects")).filePath(QString::fromLatin1(key));
}

inline bool initialize(QString* error)
{
    const QString candidate = qEnvironmentVariable("BETTER_MIACODE_RUNTIME_ROOT");
    if (!validateRoot(candidate, error)) return false;
    validatedRoot() = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
    for (const char* name : {"config", "cache", "sessions", "projects", "logs", "temp"}) {
        if (!QDir().mkpath(directory(name))) {
            if (error) *error = QStringLiteral("Cannot create runtime directory: %1").arg(directory(name));
            validatedRoot().clear();
            return false;
        }
    }
    QFile probe(QDir(directory("temp")).filePath(QStringLiteral(".write-probe-%1")
        .arg(QCoreApplication::applicationPid())));
    if (!probe.open(QIODevice::WriteOnly) || probe.write("isolated") != 8) {
        if (error) *error = QStringLiteral("Runtime root is not writable.");
        validatedRoot().clear();
        return false;
    }
    probe.close();
    probe.remove();
    QCoreApplication::setOrganizationName(QStringLiteral("BetterMiacode"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("better-miacode.local"));
    QCoreApplication::setApplicationName(QStringLiteral("BetterMiacode"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory("config"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory("config"));
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", root().toUtf8());
    for (const char* name : {"TEMP", "TMP", "TMPDIR"})
        qputenv(name, directory("temp").toUtf8());
    qputenv("MIACODE_LOG_DIR", directory("logs").toUtf8());
    // Never honor inherited upstream diagnostic file overrides.
    for (const char* name : {"MIACODE_RUNTIME_LOG_PATH", "MIACODE_AUDIO_LOG_PATH",
        "MIACODE_EXPORT_LOG_PATH", "MIACODE_STARTUP_LOG_PATH", "MIACODE_FATAL_LOG_PATH",
        "MIACODE_PREVIEW_PROFILE_PATH", "MIACODE_OPERATION_LOG_PATH", "MIACODE_PV_MEMORY_LOG_PATH",
        "MIACODE_OPLOG_SHADOW_PATH"}) qunsetenv(name);
    return true;
}

} // namespace miacode::runtime_paths
