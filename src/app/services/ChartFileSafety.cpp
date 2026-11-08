#include "app/services/ChartFileSafety.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace miacode::chart_file_safety {
namespace {
bool fail(const QString& message, QString* error)
{
    if (error) *error = message;
    return false;
}
}

QByteArray fileDigest(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}

bool backupBytes(const QString& chartPath, const QByteArray& bytes,
                 QString* backupPath, QString* error)
{
    if (backupPath) backupPath->clear();
    if (error) error->clear();
    const QFileInfo source(chartPath);
    if (chartPath.isEmpty() || !source.isAbsolute())
        return fail(QStringLiteral("An absolute chart path is required for backup."), error);
    const QString directory = source.absoluteDir().filePath(QStringLiteral(".autosave"));
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("Cannot create the chart backup directory: %1").arg(directory), error);
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    const QString target = QDir(directory).filePath(
        source.completeBaseName() + QLatin1Char('.') + QString::fromLatin1(digest.toHex()) + QStringLiteral(".bak"));
    if (QFileInfo::exists(target)) {
        if (fileDigest(target) != digest)
            return fail(QStringLiteral("The existing chart backup failed its checksum: %1").arg(target), error);
        if (backupPath) *backupPath = target;
        return true;
    }
    QSaveFile backup(target);
    backup.setDirectWriteFallback(false);
    if (!backup.open(QIODevice::WriteOnly) || backup.write(bytes) != bytes.size() || !backup.commit())
        return fail(QStringLiteral("Cannot save the original chart backup: %1 (%2)")
                    .arg(target, backup.errorString()), error);
    if (fileDigest(target) != digest)
        return fail(QStringLiteral("Chart backup verification failed: %1").arg(target), error);
    if (backupPath) *backupPath = target;
    return true;
}

bool backupOriginal(const QString& path, QString* backupPath, QString* error)
{
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot read the chart before backup: %1 (%2)")
                    .arg(path, source.errorString()), error);
    const QByteArray bytes = source.readAll();
    if (source.error() != QFileDevice::NoError)
        return fail(QStringLiteral("Cannot finish reading the chart before backup: %1").arg(path), error);
    return backupBytes(QFileInfo(path).absoluteFilePath(), bytes, backupPath, error);
}

} // namespace miacode::chart_file_safety
