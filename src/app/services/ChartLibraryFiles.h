#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>
#include "core/chart/transform/ChartNormalization.h"

namespace miacode::library {
inline constexpr qint64 kMaximumFileBytes = 4 * 1024 * 1024;
struct Change {
    QString path;
    QByteArray before;
    QByteArray after;
    QString backup;
};
QByteArray digest(const QByteArray& bytes);
bool withinRoot(const QString& root, const QString& path);
bool read(const QString& path, QByteArray* bytes, QString* error);
bool decode(const QByteArray& bytes, QString* text, QString* error);
QByteArray encode(const QString& text, const QByteArray& original);
bool normalize(const QByteArray& original, QByteArray* output, QString* error,
               const chart_transform::ChartNormalizationOptions& options = {});
// Preflight every file before writing. Each file is atomic; failures roll back
// only files still containing this transaction's bytes, preserving outside edits.
// canContinue is a non-throwing cancellation check, called before each write
// with the zero-based file index. False rolls back already-written files.
bool apply(const QString& root, QVector<Change>* changes, const QString& journalPath, QString* error,
           std::function<bool(int)> canContinue = {});
bool undo(const QString& root, const QVector<Change>& changes, const QString& journalPath, QString* error);
}
