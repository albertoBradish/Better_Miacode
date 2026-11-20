#include "app/services/ManagedChartLibrary.h"
#include "app/services/ChartLibraryFiles.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/ChartAssetPaths.h"

#include "app/services/LibraryImportIndex.h"
#include <QLockFile>
#include "common/RuntimePaths.h"
#include "core/chart/document/AdvancedMetadata.h"
#include <QDirIterator>
#include <QDateTime>
#include <QUuid>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

namespace miacode::library {
namespace {
    const QString marker = QStringLiteral("miacode-library.json");

    bool writeFile(const QString& path, const QByteArray& bytes)
    {
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }

    QString fingerprint(const QString& text) { return QString::fromLatin1(digest(text.toUtf8()).toHex().left(16)); }

    bool realDirectory(const QString& path)
    {
        const QFileInfo info(path);
        return info.isDir() && !info.isSymLink()
            && QDir::cleanPath(info.absoluteFilePath())
                   .compare(info.canonicalFilePath(),
#ifdef Q_OS_WIN
                       Qt::CaseInsensitive
#else
                       Qt::CaseSensitive
#endif
                       )
            == 0;
    }

    bool insideLibrary(QString path)
    {
        QDir directory(path);
        do {
            if (isManagedLibrary(directory.absolutePath()))
                return true;
        } while (directory.cdUp());
        return false;
    }

    const auto formatName = QStringLiteral("BetterMiacode.ChartLibrary");
    QJsonObject jsonFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024)
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    bool saveBundle(const QString& root, const QJsonArray& songs)
    {
        return writeFile(bundleFile(root),
            QJsonDocument(QJsonObject { { "format", formatName }, { "version", 2 }, { "songs", songs } }).toJson());
    }
    QString relativeSource(const QString& root, const QString& path)
    {
        const auto relative = QDir(root).relativeFilePath(path);
        return relative.startsWith("../") ? path : relative;
    }
    QJsonArray records(const QString& root)
    {
        if (QFileInfo::exists(bundleFile(root)))
            return jsonFile(bundleFile(root)).value("songs").toArray();
        QJsonArray songs;
        const QDir packs(QDir(root).filePath("packs"));
        QDirIterator it(packs.path(), { "maidata*.txt" }, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto path = it.next();
            if (withinRoot(root, path))
                songs.append(QJsonObject { { "sources", QJsonArray { relativeSource(root, path) } },
                    { "pack", packs.relativeFilePath(path).section('/', 0, 0) } });
        }
        return songs;
    }
    QString extractedArchive(
        const QString& raw, const QString& member, const std::atomic_bool& canceled, bool fullMedia)
    {
        const QFileInfo info(raw);
        const auto key = fingerprint(raw + "|" + member + "|" + QString::number(info.size()) + "|"
            + QString::number(info.lastModified().toMSecsSinceEpoch()));
        const QDir cache(QDir(runtime_paths::cacheDirectory()).filePath("library-zip"));
        if (!QDir().mkpath(cache.path()))
            return {};
        QDir target(cache.filePath(key + (fullMedia ? "-play" : "-index")));
        if (!target.exists()) {
            QTemporaryDir staging(cache.filePath(".unpack-XXXXXX"));
            if (!staging.isValid() || !unpackChartZip(raw, staging.path(), canceled, member, fullMedia))
                return {};
            if (QDir().rename(staging.path(), target.path()))
                staging.setAutoRemove(false);
            else if (!target.exists())
                return {};
        }
        return target.path();
    }
    QString resolveSource(
        const QString& root, const QString& reference, const std::atomic_bool& canceled, bool fullMedia)
    {
        if (reference.isEmpty())
            return {};
        const int zip = reference.indexOf("!/");
        const auto raw = QDir::cleanPath(QDir(root).absoluteFilePath(zip < 0 ? reference : reference.left(zip)));
        const QFileInfo info(raw);
        if (!info.isFile() || info.isSymLink() || raw != info.canonicalFilePath())
            return {};
        if (zip < 0)
            return raw;
        const QString member = reference.mid(zip + 2);
        const auto directory = extractedArchive(raw, member, canceled, fullMedia);
        if (directory.isEmpty())
            return {};
        const QDir target(directory);
        const auto path = QDir::cleanPath(target.filePath(member));
        return withinRoot(target.path(), path) && chart_assets::isChartDocumentPath(path) ? path : QString {};
    }
    QString viewKey(const QJsonObject& song) { return fingerprint(song.value("sources").toArray().first().toString()); }
    QString editDir(const QString& root, const QJsonObject& song)
    {
        return QDir(root).filePath(".edits/" + viewKey(song));
    }
    QString workingChart(
        const QString& root, const QJsonObject& song, const std::atomic_bool& canceled, bool fullMedia = false)
    {
        const auto sources = song.value("sources").toArray();
        if (sources.isEmpty())
            return {};
        const auto overridePath = QDir(editDir(root, song)).filePath("maidata.txt");
        const bool edited = withinRoot(root, overridePath);
        QStringList paths;
        for (const auto& value : sources) {
            const auto path = resolveSource(root, value.toString(), canceled, fullMedia);
            if (path.isEmpty())
                return edited ? overridePath : QString {};
            paths << path;
        }
        if (edited) {
            if (fullMedia) {
                QStringList dirs;
                for (const auto& path : paths)
                    dirs << QFileInfo(path).absolutePath();
                const auto contents = QJsonDocument(QJsonArray::fromStringList(dirs)).toJson();
                const auto media = QDir(editDir(root, song)).filePath(".miamedia");
                QFile old(media);
                const bool same = old.open(QIODevice::ReadOnly) && old.readAll() == contents;
                old.close();
                if (!same && !writeFile(media, contents))
                    return {};
            }
            return overridePath;
        }
        if (paths.size() == 1)
            return paths.first();
        QByteArray first;
        QString combined;
        if (!read(paths.first(), &first, nullptr) || !decode(first, &combined, nullptr))
            return {};
        for (int i = 1; i < paths.size(); ++i) {
            QByteArray bytes;
            QString text, next, detail;
            if (!read(paths[i], &bytes, nullptr) || !decode(bytes, &text, nullptr))
                return {};
            const auto result = compareCharts(combined, text, next, detail);
            if (result != ImportMatch::Existing && result != ImportMatch::Merged)
                return {};
            combined = next;
        }
        const QDir cache(QDir(runtime_paths::cacheDirectory())
                             .filePath("library-views/" + fingerprint(root) + "/" + viewKey(song)
                                 + (fullMedia ? "-play" : "-index")));
        const QString chart = cache.filePath("maidata.txt");
        const auto bytes = encode(combined, first);
        QStringList directories;
        for (const auto& path : paths)
            directories << QFileInfo(path).absolutePath();
        directories.removeDuplicates();
        const auto media = QJsonDocument(QJsonArray::fromStringList(directories)).toJson(QJsonDocument::Compact);
        auto update = [](const QString& file, const QByteArray& bytes) {
            QFile old(file);
            if (old.open(QIODevice::ReadOnly) && old.readAll() == bytes)
                return true;
            old.close();
            return writeFile(file, bytes);
        };
        if (!QDir().mkpath(cache.path()) || !update(chart, bytes) || !update(cache.filePath(".miamedia"), media))
            return {};
        return chart;
    }
    QString numericId(const SimaiDocument& doc, const QString& source)
    {
        bool ok = false;
        int id = metadata::fieldValue(doc, "shortid").toInt(&ok);
        if (!ok) {
            static const QRegularExpression gameId(
                "(?:/谱面/|/music)([0-9]{1,6})/", QRegularExpression::CaseInsensitiveOption);
            id = gameId.match(source).captured(1).toInt(&ok);
        }
        if (!ok || id < 0 || id > 999999)
            return {};
        if (id < 20000 && (doc.title.endsWith("[宴]") || metadata::fieldValue(doc, "genre") == "宴会場"))
            id += 100000;
        else if (id < 10000
            && (doc.title.endsWith("[DX]", Qt::CaseInsensitive)
                || metadata::fieldValue(doc, "cabinet").compare("DX", Qt::CaseInsensitive) == 0))
            id += 10000;
        return QString::number(id);
    }
    bool newRecord(const QString& root, const QString& path, const QString& pack, QJsonArray& rows)
    {
        rows.append(QJsonObject { { "pack", pack }, { "sources", QJsonArray { relativeSource(root, path) } } });
        return saveBundle(root, rows);
    }
}

QString bundleFile(const QString& root)
{
    QDir dir(root);
    const auto files = dir.entryList({ "*.miabundle" }, QDir::Files | QDir::NoSymLinks);
    return dir.filePath(files.isEmpty() ? dir.dirName() + ".miabundle" : files.first());
}
bool isManagedLibrary(const QString& root)
{
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return false;
    QDir dir(root);
    const auto bundles = dir.entryList({ "*.miabundle" }, QDir::Files | QDir::NoSymLinks);
    if (bundles.size() > 1)
        return false;
    const auto path = bundles.isEmpty() ? dir.filePath(marker) : dir.filePath(bundles.first());
    if (!withinRoot(root, path))
        return false;
    const auto obj = jsonFile(path);
    return obj.value("format") == formatName
        && (bundles.isEmpty() ? obj.value("version").toInt() == 1
                              : obj.value("version").toInt() == 2 && obj.value("songs").isArray());
}
QString createLibrary(const QString& parent, QString* error)
{
    QDir location(parent);
    if (realDirectory(parent) && !insideLibrary(parent)) {
        const auto base = qtTrId("library.folder_name");
        for (int n = 1; n < 10000; ++n) {
            const auto name = n == 1 ? base : base + " " + QString::number(n);
            if (location.exists(name))
                continue;
            if (location.mkdir(name)) {
                if (saveBundle(location.filePath(name), {}))
                    return location.filePath(name);
                location.rmdir(name);
            }
            break;
        }
    }
    *error = qtTrId("library.create_failed").arg(parent);
    return {};
}
QString playbackChartPath(const QString& root, const QJsonObject& record)
{
    return workingChart(root, record, false, true);
}
QList<BundleSong> bundleSongs(const QString& root, int* skipped, bool fullMedia, const std::atomic_bool* cancel,
    const std::function<void(int)>& progress)
{
    QList<BundleSong> result;
    const std::atomic_bool canceled { false };
    const auto rows = records(root);
    int last = -1, done = 0;
    for (const auto& v : rows) {
        if (cancel && cancel->load())
            break;
        const int percent = 100 * done++ / rows.size();
        if (progress && percent != last) {
            progress(percent);
            last = percent;
        }
        auto row = v.toObject();
        const auto path = workingChart(root, row, cancel ? *cancel : canceled, fullMedia);
        if (path.isEmpty()) {
            if (skipped)
                ++*skipped;
            continue;
        }
        auto origin = row.value("sources").toArray().first().toString();
        result.append({ path, row["pack"].toString(), QDir(root).absoluteFilePath(origin), row["id"].toString(), row });
    }
    return result;
}
bool registerSong(const QString& root, const QString& path, const QString& pack)
{
    QLockFile lock(QDir(root).filePath(".import.lock"));
    if (!lock.tryLock() || !isManagedLibrary(root) || !withinRoot(root, path))
        return false;
    auto rows = records(root);
    for (const auto& v : rows)
        if (v.toObject()["sources"].toArray().contains(relativeSource(root, path)))
            return saveBundle(root, rows);
    return newRecord(root, path, pack, rows);
}
bool unlinkSong(const QString& root, const QString& path)
{
    QLockFile lock(QDir(root).filePath(".import.lock"));
    if (!lock.tryLock() || !isManagedLibrary(root))
        return false;
    auto rows = records(root);
    for (int i = 0; i < rows.size(); ++i)
        if (workingChart(root, rows[i].toObject(), false) == path) {
            rows.removeAt(i);
            return saveBundle(root, rows);
        }
    return false;
}
QString editableChartPath(const QString& root, const QString& path, const QJsonObject& record)
{
    // Loose files are edited at their original location. ZIP members and generated
    // merges need a durable working chart only when the user opens them for editing.
    if (!record.isEmpty() && workingChart(root, record, false) == path) {
        const auto sources = record.value("sources").toArray();
        if (withinRoot(root, path))
            return workingChart(root, record, false, true);
        if (sources.size() == 1 && !sources.first().toString().contains("!/"))
            return path;
        const QDir dest(editDir(root, record));
        if (!QDir().mkpath(dest.path()) || !realDirectory(dest.path()))
            return {};
        const auto chart = dest.filePath("maidata.txt");
        QStringList dirs;
        for (const auto& ref : sources)
            dirs << QFileInfo(resolveSource(root, ref.toString(), false, true)).absolutePath();
        if (!writeFile(dest.filePath(".miamedia"), QJsonDocument(QJsonArray::fromStringList(dirs)).toJson()))
            return {};
        return QFile::copy(path, chart) || withinRoot(root, chart) ? chart : QString {};
    }
    return {};
}

ImportResult importChartFolders(const QString& root, const QStringList& folders, const std::atomic_bool& canceled,
    const QString& targetPack, const QSet<QString>& protectedPaths, bool consolidate, bool manualSong,
    const std::function<void(int)>& progress)
{
    ImportResult result;
    const auto fail = [&](const QString& path) {
        ++result.failed;
        if (result.firstError.isEmpty())
            result.firstError = path;
    };
    QLockFile lock(QDir(root).filePath(".import.lock"));
    if (!isManagedLibrary(root) || !lock.tryLock()) {
        fail(root);
        return result;
    }
    const auto initial = records(root);
    QJsonArray rows;
    QHash<QString, QList<int>> identities;
    QHash<int, QString> texts;
    QSet<QString> references, ids, assigned;
    int nextId = 0;
    for (const auto& value : initial)
        ids.insert(value.toObject().value("id").toString());
    struct Candidate {
        QString path, origin, pack;
        QJsonObject previous;
        QByteArray zipBytes;
        bool zipHasMedia = false;
    };
    QList<Candidate> candidates;
    for (const auto& value : initial) {
        const auto row = value.toObject();
        const auto sources = row["sources"].toArray();
        if (sources.isEmpty()) {
            fail(root);
            return result;
        }
        const auto path = workingChart(root, row, canceled);
        candidates.append({ path, sources.first().toString(), row["pack"].toString(), row });
    }
    if (consolidate)
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
            return protectedPaths.contains(a.path) && !protectedPaths.contains(b.path);
        });
    if (progress)
        progress(0);
    int folderNumber = 0;
    for (const auto& selected : folders) {
        if (progress)
            progress(30 * folderNumber / qMax(1, int(folders.size())));
        const QFileInfo info(selected);
        const QString base = info.canonicalFilePath();
        if (base.isEmpty() || info.isSymLink() || insideLibrary(info.isDir() ? base : info.absolutePath())) {
            fail(selected);
            continue;
        }
        const QString pack
            = targetPack.isEmpty() ? (info.isDir() ? info.fileName() : info.completeBaseName()) : targetPack;
        QStringList archives;
        auto scan = [&](const QString& directory) {
            QStringList pending { directory };
            while (!pending.isEmpty() && !canceled.load()) {
                const QDir dir(pending.takeLast());
                for (const auto& f :
                    dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
                    if (canceled.load())
                        break;
                    if (f.isDir()) {
                        if (!f.isJunction() && !f.fileName().startsWith('.')
                            && !f.fileName().startsWith(QStringLiteral("转谱核验"))
                            && !isManagedLibrary(f.absoluteFilePath()))
                            pending << f.absoluteFilePath();
                    } else if (chart_assets::isChartDocumentPath(f.absoluteFilePath())) {
                        const auto source = f.absoluteFilePath();
                        candidates.append({ f.absoluteFilePath(), relativeSource(root, source), pack, {} });
                    } else if (f.suffix().compare("zip", Qt::CaseInsensitive) == 0)
                        archives << f.absoluteFilePath();
                }
            }
        };
        if (info.isDir())
            scan(base);
        else if (info.suffix().compare("zip", Qt::CaseInsensitive) == 0)
            archives << base;
        else {
            fail(base);
            continue;
        }
        for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex) {
            const auto& archive = archives[archiveIndex];
            if (canceled.load())
                break;
            QList<ZipChart> charts;
            if (!inspectChartZip(archive, canceled, &charts, [&](int p) {
                    if (progress)
                        progress((30 * folderNumber + (archiveIndex * 100 + p) * 30 / (100 * archives.size()))
                            / qMax(1, int(folders.size())));
                })) {
                fail(archive);
                continue;
            }
            for (auto& chart : charts)
                candidates.append({ {}, relativeSource(root, archive + "!/" + chart.member), pack, {},
                    std::move(chart.bytes), chart.hasMedia });
        }
        ++folderNumber;
    }
    int done = 0, last = -1;
    for (const auto& c : candidates) {
        if (canceled.load())
            return {};
        const int percent = 30 + int(69LL * done++ / candidates.size());
        if (progress && percent != last) {
            progress(percent);
            last = percent;
        }
        const bool prior = !c.previous.isEmpty();
        QByteArray bytes = c.zipBytes;
        QString text;
        const bool zipped = !bytes.isEmpty();
        if (!prior && references.contains(c.origin)) {
            ++result.existing;
            continue;
        }
        if ((!zipped && (c.path.isEmpty() || !read(c.path, &bytes, nullptr))) || !decode(bytes, &text, nullptr)) {
            if (prior)
                rows.append(c.previous);
            if (prior || QFileInfo(c.path).fileName().startsWith("maidata"))
                fail(c.origin);
            continue;
        }
        const auto doc = SimaiDocument::fromText(text);
        bool playable = false;
        for (int slot : doc.difficultyIds())
            if (!doc.difficulty(slot)->chart.trimmed().isEmpty())
                playable = true;
        if (!prior && !playable)
            continue;
        if (!prior && !manualSong && targetPack.isEmpty()
            && (zipped ? !c.zipHasMedia
                       : (chart_assets::resolveTrackPath(c.path).isEmpty()
                             || chart_assets::resolveBackgroundMediaPath(c.path, false).isEmpty()))) {
            ++result.missingMedia;
            continue;
        }
        const auto key = songSignature(text);
        bool matched = false;
        bool blockedMerge = false;
        QString conflict;
        if (!prior || (consolidate && !protectedPaths.contains(c.path))) {
            for (int idx : identities.value(key)) {
                QString combined, detail;
                const auto match = compareCharts(texts.value(idx), text, combined, detail);
                if (match == ImportMatch::NewSong)
                    continue;
                if (match == ImportMatch::Conflict) {
                    conflict = detail;
                    continue;
                }
                auto row = rows[idx].toObject();
                if (match == ImportMatch::Merged) {
                    const auto chart = workingChart(root, row, canceled);
                    if (protectedPaths.contains(chart) || withinRoot(editDir(root, row), chart)
                        || combined.toUtf8().size() > kMaximumFileBytes) {
                        blockedMerge = true;
                        conflict = c.origin;
                        continue;
                    }
                    auto refs = row.value("sources").toArray();
                    auto additions = prior ? c.previous["sources"].toArray() : QJsonArray { c.origin };
                    for (const auto& ref : additions)
                        if (!refs.contains(ref))
                            refs.append(ref);
                    row["sources"] = refs;
                    rows[idx] = row;
                    texts[idx] = combined;
                    ++result.merged;
                } else
                    ++result.existing;
                matched = true;
                break;
            }
        }
        if (matched)
            continue;
        if (!conflict.isEmpty()) {
            ++result.conflicts;
            if (result.firstError.isEmpty())
                result.firstError = c.origin + " (&" + conflict + ")";
            if (!prior || (consolidate && !blockedMerge && !protectedPaths.contains(c.path)))
                continue;
        }
        auto row = prior ? c.previous : QJsonObject { { "pack", c.pack }, { "sources", QJsonArray { c.origin } } };
        auto id = row["id"].toString();
        if (id.isEmpty())
            id = numericId(doc, c.origin);
        if (id.isEmpty() || assigned.contains(id)) {
            const bool dx = doc.title.endsWith("[DX]", Qt::CaseInsensitive)
                || metadata::fieldValue(doc, "cabinet").compare("DX", Qt::CaseInsensitive) == 0;
            const int offset = doc.title.endsWith("[宴]") || metadata::fieldValue(doc, "genre") == "宴会場" ? 100000
                : dx                                                                                        ? 10000
                                                                                                            : 0;
            while (ids.contains(QString::number(offset + nextId)))
                ++nextId;
            id = QString::number(offset + nextId++);
        }
        row["id"] = id;
        ids.insert(id);
        assigned.insert(id);
        for (const auto& ref : row["sources"].toArray())
            references.insert(ref.toString());
        identities[key].append(rows.size());
        texts[rows.size()] = text;
        rows.append(row);
        if (!prior)
            ++result.imported;
    }
    if (canceled.load())
        return {};
    if (rows != initial && !saveBundle(root, rows)) {
        fail(bundleFile(root));
        result.imported = result.merged = 0;
    }
    if (progress)
        progress(100);
    return result;
}

QString safeFolderName(QString name)
{
    static const QRegularExpression invalid(R"([<>:"/\\|?*\x00-\x1f])");
    name = name.trimmed().replace(invalid, "_").left(140);
    while (name.endsWith('.') || name.endsWith(' '))
        name.chop(1);
    static const QRegularExpression reserved(
        R"(^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))", QRegularExpression::CaseInsensitiveOption);
    if (name.isEmpty() || reserved.match(name).hasMatch())
        name.prepend('_');
    return name;
}

QString exportLibrary(const QString& root, const QString& parent, const std::atomic_bool& canceled,
    const std::function<void(int)>& progress, QString* error)
{
    const auto manifest = bundleFile(root);
    const QString name
        = safeFolderName(QFileInfo::exists(manifest) ? QFileInfo(manifest).completeBaseName() : QDir(root).dirName());
    QDir dest(parent);
    auto fail = [&](const QString& path) {
        *error = qtTrId("library.export_failed").arg(path);
        return QString {};
    };
    if (!realDirectory(parent) || insideLibrary(parent) || dest.exists(name))
        return fail(dest.filePath(name));
    QTemporaryDir stage(dest.filePath(".miacode-export-XXXXXX"));
    if (!stage.isValid())
        return fail(parent);
    const QDir output(stage.path());
    const auto byLevel = QStringLiteral("按难度查找"), byAuthor = QStringLiteral("按谱师查找");
    if (!output.mkdir(byLevel) || !output.mkdir(byAuthor))
        return fail(parent);
    int skipped = 0;
    const auto songs = bundleSongs(root, &skipped, true, &canceled);
    if (skipped)
        return fail(bundleFile(root));
    QJsonArray exported;
    QSet<QString> used;
    int done = 0;
    for (const auto& song : songs) {
        if (canceled.load())
            return fail(parent);
        QByteArray bytes;
        QString text;
        if (!read(song.path, &bytes, nullptr) || !decode(bytes, &text, nullptr))
            return fail(song.origin);
        const auto doc = SimaiDocument::fromText(text);
        auto id = song.id;
        static const QRegularExpression digits("^[0-9]{1,6}$");
        if (!digits.match(id).hasMatch())
            id = numericId(doc, song.origin);
        if (id.isEmpty())
            id = QString::number(done + 1);
        const auto folder = safeFolderName(doc.title) + " - " + id;
        if (used.contains(folder.toCaseFolded()) || !output.mkdir(folder))
            return fail(folder);
        used.insert(folder.toCaseFolded());
        const QDir track(output.filePath(folder));
        const auto chart = track.filePath("maidata.txt");
        QSet<QString> copied;
        auto copy = [&](const QString& path, QString relative) {
            if (path.isEmpty() || copied.contains(relative.toCaseFolded()))
                return true;
            relative = QDir::cleanPath(QDir::fromNativeSeparators(relative));
            if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")
                || relative.contains(':'))
                return false;
            copied.insert(relative.toCaseFolded());
            return track.mkpath(QFileInfo(relative).path()) && QFile::copy(path, track.filePath(relative));
        };
        for (const auto& dir : chart_assets::mediaDirectories(QFileInfo(song.path).absolutePath())) {
            const QDir src(dir);
            for (const auto& f : src.entryInfoList(
                     chart_assets::trackCandidateFileNames() + chart_assets::backgroundMediaCandidateFileNames(),
                     QDir::Files | QDir::NoSymLinks))
                if (!copy(f.absoluteFilePath(), f.fileName()))
                    return fail(f.absoluteFilePath());
        }
        if (!doc.videoPath.isEmpty()) {
            const auto video = chart_assets::resolveChartVideoPath(song.path, doc.videoPath);
            auto relative = QDir::cleanPath(QDir::fromNativeSeparators(doc.videoPath));
            if (!video.isEmpty()) {
                if (QDir::isAbsolutePath(relative) || relative.startsWith("../") || relative.contains(':')) {
                    relative = QStringLiteral("media/") + safeFolderName(QFileInfo(video).fileName());
                    static const QRegularExpression field(QStringLiteral("^([ \\t]*&video[ \\t]*=)[^\\r\\n]*"),
                        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
                    const auto match = field.match(text);
                    if (!match.hasMatch())
                        return fail(doc.videoPath);
                    text.replace(match.capturedEnd(1), match.capturedEnd() - match.capturedEnd(1), relative);
                    bytes = encode(text, bytes);
                }
                if (!copy(video, relative))
                    return fail(video);
            }
        }
        if (!writeFile(chart, bytes))
            return fail(chart);
        auto link = [&](const QString& group, const QString& value) {
            const auto category = group + "/" + safeFolderName(value);
            if (!output.mkpath(category))
                return false;
            const QString target = output.filePath(category + "/" + folder);
#ifdef Q_OS_WIN
            // Qt creates a native Shell link. Use the destination after publication.
            return QFile::link(QDir(dest.filePath(name)).filePath(folder), target + ".lnk");
#else
            return QFile::link("../../" + folder, target);
#endif
        };
        QSet<QString> authors, difficulties;
        for (int slot : doc.difficultyIds()) {
            const auto* d = doc.difficulty(slot);
            if (!d || d->chart.trimmed().isEmpty())
                continue;
            difficulties.insert(SimaiDocument::difficultyName(slot));
            const auto author = doc.effectiveDesignerForSlot(slot).trimmed();
            authors.insert(author.isEmpty() ? QStringLiteral("未署名") : author);
        }
        for (const auto& v : authors)
            if (!link(byAuthor, v))
                return fail(v);
        for (const auto& v : difficulties)
            if (!link(byLevel, v))
                return fail(v);
        exported.append(
            QJsonObject { { "pack", song.pack }, { "id", id }, { "sources", QJsonArray { folder + "/maidata.txt" } } });
        ++done;
        if (progress)
            progress(100 * done / qMax(1, int(songs.size())));
    }
    if (!writeFile(output.filePath(name + ".miabundle"),
            QJsonDocument(QJsonObject { { "format", formatName }, { "version", 2 }, { "songs", exported } }).toJson())
        || canceled.load() || !dest.rename(stage.path(), dest.filePath(name)))
        return fail(dest.filePath(name));
    stage.setAutoRemove(false);
    return dest.filePath(name);
}
}
