#include "app/services/ChartLibraryFiles.h"
#include "app/services/ChartFileSafety.h"
#include "app/services/AutomaticChartPreparation.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/document/SimaiTimingMetadata.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/ChartNormalization.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <cmath>

namespace miacode::library {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool write(const QString& path, const QByteArray& bytes, QString* error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("Cannot atomically save %1: %2").arg(path, file.errorString()));
    return true;
}
bool journal(const QString& path, const QVector<Change>& changes, int written, const QString& state, QString* error)
{
    QJsonArray files;
    for (const auto& change : changes) files.append(QJsonObject{{"path", change.path},
        {"before", QString::fromLatin1(digest(change.before).toHex())},
        {"after", QString::fromLatin1(digest(change.after).toHex())}, {"backup", change.backup}});
    QDir().mkpath(QFileInfo(path).absolutePath());
    return write(path, QJsonDocument(QJsonObject{{"schema", 1}, {"state", state},
        {"written", written}, {"files", files}}).toJson(), error);
}
}
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
bool withinRoot(const QString& root, const QString& path)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonical.isEmpty() || file.isSymLink() || !file.isFile()) return false;
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return canonical.startsWith(canonicalRoot + QLatin1Char('/'), sensitivity)
        && QDir::cleanPath(file.absoluteFilePath()).compare(canonical, sensitivity) == 0;
}
bool read(const QString& path, QByteArray* bytes, QString* error)
{
    QFile file(path);
    if (file.size() > kMaximumFileBytes || !file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot read chart (maximum 4 MiB): %1").arg(path));
    *bytes = file.read(kMaximumFileBytes + 1);
    if (bytes->size() > kMaximumFileBytes || file.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("Chart changed or could not be read: %1").arg(path));
    return true;
}
bool decode(const QByteArray& bytes, QString* text, QString* error)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    *text = decoder(bytes);
    if (decoder.hasError()) return fail(error, QStringLiteral("Only valid UTF-8 charts are editable; original bytes were preserved."));
    if (text->startsWith(QChar(0xfeff))) text->remove(0, 1);
    text->replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return true;
}
QByteArray encode(const QString& text, const QByteArray& original)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (original.contains("\r\n")) normalized.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return (original.startsWith("\xef\xbb\xbf") ? QByteArray("\xef\xbb\xbf") : QByteArray()) + normalized.toUtf8();
}
bool normalize(const QByteArray& original, QByteArray* output, QString* error,
               const chart_transform::ChartNormalizationOptions& options)
{
    QString text;
    if (!decode(original, &text, error)) return false;
    const auto document = SimaiDocument::fromText(text);
    const auto timing = simai::buildTimingMetadata(document);
    const QRegularExpression fields(QStringLiteral("^&([^=\\r\\n]+)="), QRegularExpression::MultilineOption);
    struct Span { QString key; int begin; int end; };
    QVector<Span> spans;
    auto matches = fields.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!spans.isEmpty()) spans.last().end = match.capturedStart();
        spans.append({match.captured(1), int(match.capturedEnd()), int(text.size())});
    }
    QSet<QString> seen;
    for (const auto& span : spans) {
        if (seen.contains(span.key)) return fail(error, QStringLiteral("Duplicate fields require manual review: %1").arg(span.key));
        seen.insert(span.key);
    }
    for (auto it = spans.crbegin(); it != spans.crend(); ++it) {
        if (!QRegularExpression(QStringLiteral("^inote_[1-7]$")).match(it->key).hasMatch()) continue;
        const QString chart = text.mid(it->begin, it->end - it->begin);
        if (chart.trimmed().isEmpty()) continue;
        const auto report = SimaiParser::buildValidationReport(chart, SimaiValidationLocale::English, timing);
        const auto before = SimaiParser::parseForTimeline(chart, timing);
        if (!before.ok || report.errorCount > 0) return fail(error, QStringLiteral("Invalid chart syntax: %1").arg(it->key));
        const auto result = chart_transform::normalizeChartText(chart, timing, options);
        if (!result.ok) return fail(error, result.errorMessage);
        const auto after = SimaiParser::parseForTimeline(result.text, timing);
        if (!after.ok || !automatic_chart::normalizationPreservesGameplay(before, after))
            return fail(error, QStringLiteral("Normalization changed chart timing; review manually."));
        QString replacement = result.text;
        if (it->end < text.size() && !replacement.endsWith(QLatin1Char('\n'))) replacement += QLatin1Char('\n');
        text.replace(it->begin, it->end - it->begin, replacement);
    }
    *output = encode(text, original);
    return true;
}
bool apply(const QString& root, QVector<Change>* changes, const QString& journalPath, QString* error,
           std::function<bool(int)> canContinue)
{
    QSet<QString> paths;
    for (const auto& change : *changes) {
        if (!withinRoot(root, change.path) || paths.contains(change.path))
            return fail(error, QStringLiteral("Unsafe or duplicate chart path: %1").arg(change.path));
        paths.insert(change.path);
        QByteArray current;
        if (!read(change.path, &current, error)) return false;
        if (current != change.before) return fail(error, QStringLiteral("External change detected; prepare again: %1").arg(change.path));
        const QFileInfo backupDir(QFileInfo(change.path).absoluteDir().filePath(QStringLiteral(".autosave")));
        if (backupDir.isSymLink() || (backupDir.exists() && backupDir.canonicalFilePath() != backupDir.absoluteFilePath()))
            return fail(error, QStringLiteral("Backup directory must not redirect to another location."));
    }
    for (auto& change : *changes)
        if (!withinRoot(root, change.path) || !chart_file_safety::backupBytes(change.path, change.before, &change.backup, error)) return false;
    if (!journal(journalPath, *changes, 0, QStringLiteral("prepared"), error)) return false;
    int written = 0;
    for (const auto& change : *changes) {
        if (canContinue && !canContinue(written)) {
            fail(error, QStringLiteral("Batch canceled before writing file %1.").arg(written + 1));
            break;
        }
        QByteArray current;
        if (!withinRoot(root, change.path) || !read(change.path, &current, error) || current != change.before
            || !write(change.path, change.after, error)) break;
        ++written;
        if (!journal(journalPath, *changes, written, QStringLiteral("applying"), error)) break;
    }
    if (written == changes->size() && journal(journalPath, *changes, written, QStringLiteral("complete"), error)) return true;
    QString rollbackErrors;
    for (int i = written - 1; i >= 0; --i) {
        const auto& change = changes->at(i);
        QByteArray current; QString detail;
        if (!withinRoot(root, change.path) || !read(change.path, &current, &detail) || current != change.after || !write(change.path, change.before, &detail))
            rollbackErrors += change.path + QLatin1Char('\n');
    }
    journal(journalPath, *changes, written, rollbackErrors.isEmpty() ? QStringLiteral("rolled_back") : QStringLiteral("recovery_required"), nullptr);
    return fail(error, QStringLiteral("Batch stopped. %1\n%2").arg(error ? *error : QString(), rollbackErrors));
}
bool undo(const QString& root, const QVector<Change>& changes, const QString& journalPath, QString* error)
{
    QVector<Change> reverse;
    for (const auto& change : changes) reverse.append({change.path, change.after, change.before, {}});
    return apply(root, &reverse, journalPath, error);
}
}
