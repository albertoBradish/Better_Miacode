#include "app/services/LibraryImportIndex.h"
#include "app/services/ChartLibraryFiles.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/document/SimaiDocument.h"

#include <QDirIterator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace miacode::library {
namespace {
QString canonical(const QString& value) { return value.normalized(QString::NormalizationForm_C).simplified().toCaseFolded(); }

struct Field { QString key, value; qsizetype begin, end; };
QList<Field> fields(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("^[ \\t]*&([^=\\r\\n]+)="), QRegularExpression::MultilineOption);
    QList<Field> list;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto m = matches.next();
        if (!list.isEmpty()) list.last().end = m.capturedStart();
        list.append({m.captured(1).trimmed().toLower(), {}, m.capturedEnd(), text.size()});
    }
    for (auto& f : list) f.value = text.mid(f.begin, f.end - f.begin).trimmed();
    return list;
}

QString baseTitle(QString title)
{
    title = canonical(title);
    for (const QString& suffix : {QStringLiteral("[sd]"), QStringLiteral("[dx]"), QStringLiteral("[宴]")}) {
        if (title.endsWith(suffix)) { title.chop(suffix.size()); break; }
    }
    return title.trimmed();
}

QString signature(const SimaiDocument& doc, const QString& text)
{
    const auto title = baseTitle(doc.title);
    if (title.isEmpty()) return QChar(0) + QString::fromLatin1(digest(text.toUtf8()).toHex());
    return title + QChar(0) + canonical(doc.artist);
}

QSet<QString> authors(const SimaiDocument& doc)
{
    QSet<QString> result;
    for (int id : doc.difficultyIds()) {
        if (doc.difficulty(id)->chart.trimmed().isEmpty()) continue;
        const auto author = canonical(doc.effectiveDesignerForSlot(id));
        if (!author.isEmpty()) result.insert(author);
    }
    return result;
}

bool sameCredits(const SimaiDocument& left, const SimaiDocument& right)
{
    const auto kind = [](const SimaiDocument& doc) {
        const auto id = metadata::fieldValue(doc, "shortid").trimmed().toInt();
        if (id >= 100000 || doc.title.endsWith(QStringLiteral("[宴]"))) return QStringLiteral("UTAGE");
        if (id >= 10000 && id < 20000) return QStringLiteral("DX");
        const auto cabinet = metadata::fieldValue(doc, "cabinet").trimmed().toUpper();
        if (!cabinet.isEmpty()) return cabinet;
        if (doc.title.endsWith("[DX]", Qt::CaseInsensitive)) return QStringLiteral("DX");
        if (id > 0 || doc.title.endsWith("[SD]", Qt::CaseInsensitive)) return QStringLiteral("SD");
        return QString{};
    };
    const auto a = kind(left), b = kind(right);
    if (!a.isEmpty() && !b.isEmpty() && a != b) return false;
    const auto songId = [](const SimaiDocument& doc, const QString& cabinet) {
        bool ok = false;
        int id = metadata::fieldValue(doc, "shortid").trimmed().toInt(&ok);
        if (!ok || id < 0) return -1;
        if (cabinet == "DX" && id < 10000) id += 10000;
        if (cabinet == "UTAGE" && id < 20000) id += 100000;
        return id;
    };
    const int leftId = songId(left, a), rightId = songId(right, b);
    if (leftId >= 0 && rightId >= 0 && leftId != rightId) return false;
    if (!left.designer.trimmed().isEmpty() && !right.designer.trimmed().isEmpty()
        && canonical(left.designer) != canonical(right.designer)) return false;
    if (authors(left) != authors(right)) return false;
    for (int id : left.difficultyIds()) {
        if (right.difficulty(id) && !left.difficulty(id)->chart.trimmed().isEmpty()
            && !right.difficulty(id)->chart.trimmed().isEmpty()
            && canonical(left.effectiveDesignerForSlot(id)) != canonical(right.effectiveDesignerForSlot(id))) return false;
    }
    return true;
}

// Preserve the existing text verbatim except for previously empty fields.
// Any disagreeing populated field (including unknown extensions) blocks a merge.
bool combine(const QString& original, const QString& incoming, QString& merged, QString& conflict)
{
    const auto base = fields(original);
    QHash<QString, Field> known;
    for (const auto& f : base) {
        if (known.contains(f.key)) { conflict = f.key; return false; }
        known.insert(f.key, f);
    }
    QHash<QString, QString> fill;
    QString additions;
    QSet<QString> seen;
    for (const auto& f : fields(incoming)) {
        if (seen.contains(f.key)) { conflict = f.key; return false; }
        seen.insert(f.key);
        if (f.value.isEmpty()) continue;
        const auto old = known.constFind(f.key);
        if (old == known.cend()) additions += "&" + f.key + "=" + f.value + "\n";
        else if (old->value.isEmpty()) fill.insert(f.key, f.value);
        else if (old->value != f.value) {
            if (f.key == "title" && baseTitle(old->value) == baseTitle(f.value)) continue;
            if ((f.key == "artist" || f.key == "des" || f.key.startsWith("des_"))
                && canonical(old->value) == canonical(f.value)) continue;
            conflict = f.key;
            return false;
        }
    }
    merged = original;
    for (auto it = base.crbegin(); it != base.crend(); ++it)
        if (fill.contains(it->key)) merged.insert(it->begin, fill.value(it->key));
    if (!additions.isEmpty()) {
        if (!merged.endsWith('\n')) merged += '\n';
        merged += additions;
    }
    return true;
}

}
QString songSignature(const QString& text) { return signature(SimaiDocument::fromText(text), text); }

ImportMatch compareCharts(const QString& existing, const QString& incoming, QString& combined, QString& detail)
{
    if (!sameCredits(SimaiDocument::fromText(existing), SimaiDocument::fromText(incoming))) return ImportMatch::NewSong;
    if (!combine(existing, incoming, combined, detail)) return ImportMatch::Conflict;
    return existing == combined ? ImportMatch::Existing : ImportMatch::Merged;
}
}
