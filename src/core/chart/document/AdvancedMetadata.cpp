#include "core/chart/document/AdvancedMetadata.h"

#include <QCoreApplication>
#include <QDate>
#include <QHash>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>

namespace miacode::metadata {
QVector<Field> advancedFields(int difficultyId)
{
    QVector<Field> result{
        {QStringLiteral("artistid"), "metadata.advanced.artist_id", "metadata.advanced.song"},
        {QStringLiteral("shortid"), "metadata.advanced.song_id", "metadata.advanced.song"},
        {QStringLiteral("wholebpm"), "metadata.advanced.whole_bpm", "metadata.advanced.timing"},
        {QStringLiteral("demo_seek"), "metadata.advanced.demo_start", "metadata.advanced.timing"},
        {QStringLiteral("demo_len"), "metadata.advanced.demo_length", "metadata.advanced.timing"},
        {QStringLiteral("seek"), "metadata.advanced.legacy_seek", "metadata.advanced.compatibility"},
        {QStringLiteral("wait"), "metadata.advanced.legacy_wait", "metadata.advanced.compatibility"},
        {QStringLiteral("genre"), "metadata.advanced.genre", "metadata.advanced.catalog"},
        {QStringLiteral("genreid"), "metadata.advanced.genre_id", "metadata.advanced.catalog"},
        {QStringLiteral("cabinet"), "metadata.advanced.cabinet", "metadata.advanced.catalog"},
        {QStringLiteral("version"), "metadata.advanced.version", "metadata.advanced.catalog"},
        {QStringLiteral("videourl"), "metadata.advanced.video_url", "metadata.advanced.author"},
        {QStringLiteral("create_date"), "metadata.advanced.created", "metadata.advanced.author"},
        {QStringLiteral("creatorscomment"), "metadata.advanced.comment", "metadata.advanced.author", true},
    };
    if (SimaiDocument::isDifficultyId(difficultyId)) {
        const QString suffix = QString::number(difficultyId);
        result.append({QStringLiteral("desid_") + suffix, "metadata.advanced.creator_id", "metadata.advanced.difficulty"});
        result.append({QStringLiteral("level_constant_") + suffix, "metadata.advanced.constant", "metadata.advanced.difficulty"});
        result.append({QStringLiteral("regulation_") + suffix, "metadata.advanced.regulation", "metadata.advanced.difficulty"});
    }
    return result;
}

QString fieldValue(const SimaiDocument& document, const QString& key)
{
    for (const auto& field : document.extraFields)
        if (field.key.compare(key, Qt::CaseInsensitive) == 0) return field.value;
    return {};
}

QString validateField(const QString& key, const QString& value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) return {};
    if (key != QLatin1String("creatorscomment") && (value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))))
        return qtTrId("metadata.advanced.single_line");
    if (QRegularExpression(QStringLiteral("(?:^|[\\r\\n])\\s*&[A-Za-z0-9_]+\\s*=")).match(value).hasMatch())
        return qtTrId("metadata.advanced.embedded_field");
    if (key == QLatin1String("create_date")) {
        if (!QRegularExpression(QStringLiteral("^[0-9]{8}$")).match(text).hasMatch()
            || !QDate::fromString(text, QStringLiteral("yyyyMMdd")).isValid())
            return qtTrId("metadata.advanced.invalid_date");
    }
    if (key == QLatin1String("videourl")) {
        const QUrl url(text, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
            return qtTrId("metadata.advanced.invalid_url");
    }
    if (key == QLatin1String("artistid") || key == QLatin1String("shortid") || key == QLatin1String("genreid")
        || key.startsWith(QLatin1String("desid_"))) {
        const QString pattern = key.startsWith(QLatin1String("desid_"))
            ? QStringLiteral("^[0-9]+(?:\\s*,\\s*[0-9]+)*$") : QStringLiteral("^[0-9]+$");
        if (!QRegularExpression(pattern).match(text).hasMatch()) return qtTrId("metadata.advanced.invalid_id");
    }
    const bool numeric = key == QLatin1String("wholebpm") || key == QLatin1String("demo_seek")
        || key == QLatin1String("demo_len") || key == QLatin1String("seek") || key == QLatin1String("wait")
        || key.startsWith(QLatin1String("level_constant_"));
    if (numeric) {
        bool valid = false;
        const double number = text.toDouble(&valid);
        const bool positive = key == QLatin1String("wholebpm") || key == QLatin1String("demo_len")
            || key.startsWith(QLatin1String("level_constant_"));
        if (!valid || !std::isfinite(number) || number < 0.0 || (positive && number == 0.0))
            return qtTrId(positive ? "metadata.advanced.positive_number" : "metadata.advanced.nonnegative_number");
    }
    return {};
}

QStringList conflictingKeys(const QString& source)
{
    QHash<QString, QString> values;
    QStringList conflicts;
    for (const auto& field : SimaiDocument::parseRawFields(source)) {
        const QString key = field.key.toLower();
        if (values.contains(key) && values.value(key) != field.value && !conflicts.contains(key)) conflicts.append(key);
        values.insert(key, field.value);
    }
    return conflicts;
}
} // namespace miacode::metadata
