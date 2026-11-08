#pragma once

#include <QByteArray>
#include <QString>

namespace miacode::chart_file_safety {

// Backups contain the original disk bytes, including encoding and line endings.
// Identical content reuses its backup; existing backups are never overwritten.
bool backupOriginal(const QString& path, QString* backupPath = nullptr, QString* error = nullptr);
bool backupBytes(const QString& chartPath, const QByteArray& bytes,
                 QString* backupPath = nullptr, QString* error = nullptr);
QByteArray fileDigest(const QString& path);

} // namespace miacode::chart_file_safety
