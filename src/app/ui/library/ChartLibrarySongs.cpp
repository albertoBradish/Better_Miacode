#include "app/ui/library/ChartLibraryModel.h"
#include "app/services/ManagedChartLibrary.h"
#include "app/services/UiRequestService.h"
#include "app/ui/document/DocumentModel.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/ChartScoringEvents.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/document/SimaiTimingMetadata.h"
#include "core/chart/parser/SimaiParser.h"

#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include "common/FileContentStamp.h"
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrentRun>

namespace miacode::ui {
namespace {
QString fingerprint(const QString& text)
{
    return QString::fromLatin1(library::digest(text.toUtf8()).toHex());
}

QUrl localAsset(const QString& path)
{
    return !path.isEmpty() && QFileInfo(path).isFile() ? QUrl::fromLocalFile(path) : QUrl{};
}

bool plainDirectory(const QString& path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && !info.isJunction()
        && QDir::cleanPath(info.absoluteFilePath()) == info.canonicalFilePath();
}

bool packNameAllowed(const QString& name)
{
    static const QRegularExpression invalid(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    return !name.isEmpty() && !name.startsWith('.') && !name.endsWith('.') && !name.contains(invalid);
}
}

// Replace only the touched metadata lines. Keeping the original chart body and
// unknown fields verbatim also preserves hand annotations and author comments.
bool ChartLibraryModel::patchFields(QString& text, const QVariantMap& changes, QString& error)
{
    static const QRegularExpression allowed(QStringLiteral("^(title|artist|wholebpm|version|miacode_version_category|genre|shortid|(?:lv|des|level_constant)_[1-7])$"));
    static const QRegularExpression field(QStringLiteral("^[ \\t]*&([^=\\r\\n]+)=([^\\r\\n]*)"), QRegularExpression::MultilineOption);
    struct Edit { qsizetype start, size; QString value; };
    QList<Edit> edits;
    QSet<QString> seen;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.value().toString().contains(QLatin1Char('\n')) || it.value().toString().contains(QLatin1Char('\r'))) {
            error = qtTrId("metadata.advanced.single_line"); return false;
        }
        if (!allowed.match(it.key()).hasMatch()) { error = qtTrId("library.invalid_field"); return false; }
        error = metadata::validateField(it.key(), it.value().toString());
        if (!error.isEmpty()) return false;
    }
    qsizetype checkedValueEnd = -1;
    auto checkContinuation = [&](qsizetype nextField) {
        if (checkedValueEnd < 0) return true;
        return QStringView(text).mid(checkedValueEnd, nextField - checkedValueEnd).trimmed().isEmpty();
    };
    auto matches = field.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!checkContinuation(match.capturedStart())) { error = qtTrId("library.multiline_field"); return false; }
        checkedValueEnd = -1;
        const auto key = match.captured(1).trimmed().toLower();
        if (!changes.contains(key)) continue;
        if (seen.contains(key)) { error = qtTrId("library.duplicate_field").arg(key); return false; }
        seen.insert(key);
        checkedValueEnd = match.capturedEnd(2);
        edits.append({match.capturedStart(2), match.capturedLength(2), changes.value(key).toString()});
    }
    if (!checkContinuation(text.size())) { error = qtTrId("library.multiline_field"); return false; }
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) text.replace(it->start, it->size, it->value);
    QString prefix;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        if (!seen.contains(it.key()) && !it.value().toString().isEmpty())
            prefix += "&" + it.key() + "=" + it.value().toString() + "\n";
    text.prepend(prefix);
    return true;
}

LibraryDocument* ChartLibraryModel::bufferFor(const QString& path) const
{
    for (auto* doc : sessions_) if (doc->path() == path) return doc;
    return nullptr;
}

const ChartLibraryModel::Entry* ChartLibraryModel::entryFor(const QString& path) const
{
    for (const auto& entry : entries_) if (entry.path == path) return &entry;
    return nullptr;
}

QString ChartLibraryModel::pathAt(int row) const
{
    return data(index(row), PathRole).toString();
}

int ChartLibraryModel::rowForPath(const QString& path) const
{
    for (int i = 0; i < visible_.size(); ++i) if (entries_[visible_[i]].path == path) return i;
    return -1;
}

QString ChartLibraryModel::songText(const QString& path, QString* error) const
{
    if (const auto* draft = bufferFor(path)) return draft->text();
    QByteArray bytes;
    QString text;
    if (!entryFor(path)) { *error = qtTrId("library.invalid_root"); return {}; }
    if (!library::read(path, &bytes, error) || !library::decode(bytes, &text, error)) return {};
    return text;
}

QVariantMap ChartLibraryModel::songDetails(const QString& path)
{
    const auto* entry = entryFor(path);
    if (!entry) return {};
    QString error;
    const auto text = songText(path, &error);
    if (!error.isEmpty()) { status_ = error; emit changed(); return {}; }
    const auto doc = SimaiDocument::fromText(text);
    QVariantMap fields{{"title", doc.title}, {"artist", doc.artist}};
    for (const auto key : {"shortid", "wholebpm", "genre", "version", "miacode_version_category"})
        fields.insert(key, metadata::fieldValue(doc, key).trimmed());
    QVariantList difficulties;
    for (int slot : doc.difficultyIds()) {
        const auto* d = doc.difficulty(slot);
        const QString suffix = QString::number(slot);
        fields.insert("lv_" + suffix, d->level);
        fields.insert("des_" + suffix, d->designer);
        fields.insert("level_constant_" + suffix, metadata::fieldValue(doc, "level_constant_" + suffix).trimmed());
        difficulties.append(QVariantMap{{"id", slot}, {"name", SimaiDocument::difficultyName(slot)}});
    }
    const auto playbackPath=library::playbackChartPath(root_,entry->record);
    return {{"path", path}, {"stamp", fingerprint(text)}, {"fields", fields}, {"difficulties", difficulties},
            {"songId", entry->songId}, {"kind", entry->kind}, {"pack", entry->pack}, {"designer", doc.designer},
            {"jacket", localAsset(chart_assets::resolveBackgroundMediaPath(path, false))},
            {"track", localAsset(chart_assets::resolveTrackPath(playbackPath))}};
}

QString ChartLibraryModel::stageSongFields(const QString& path, const QString& stamp, const QVariantMap& changes)
{
    if (!entryFor(path)) return {};
    QString error;
    QString text = songText(path, &error);
    if (error.isEmpty() && fingerprint(text) != stamp) error = qtTrId("library.conflict");
    if (!error.isEmpty() || !patchFields(text, changes, error)) {
        status_ = error; emit changed(); return {};
    }
    auto* draft = bufferFor(path);
    if (!draft) {
        openBuffer(path);
        draft = bufferFor(path);
        if (draft) formDrafts_.insert(draft);
    }
    if (!draft) return {};
    if (fingerprint(draft->text()) != stamp) { status_ = qtTrId("library.conflict"); emit changed(); return {}; }
    if (!draft->replaceText(text)) { status_ = draft->error(); emit changed(); return {}; }
    return fingerprint(text);
}

void ChartLibraryModel::discardSongEdits(const QString& path)
{
    if (auto* draft = bufferFor(path)) {
        sessions_.removeAll(draft);
        formDrafts_.remove(draft);
        if (active_ == draft) active_ = sessions_.value(0);
        draft->deleteLater();
        saveRecovery();
        emit changed();
    }
}

bool ChartLibraryModel::saveSong(const QString& path)
{
    if (auto* draft = bufferFor(path)) {
        requestSave(draft);
        if (draft->dirty()) return false;
        if (formDrafts_.remove(draft)) {
            sessions_.removeAll(draft);
            if (active_ == draft) active_ = sessions_.value(0);
            draft->deleteLater();
            saveRecovery();
            emit changed();
        }
        return true;
    }
    return true;
}

void ChartLibraryModel::requestNoteCount(const QString& path, int difficulty)
{
    const quint64 request = ++countRequest_;
    if (!entryFor(path) || !SimaiDocument::isDifficultyId(difficulty)) return;
    QString error;
    const auto doc = SimaiDocument::fromText(songText(path, &error));
    const auto* slot = doc.difficulty(difficulty);
    if (!error.isEmpty() || !slot) return;
    auto* watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher, request, path, difficulty] {
        if (request == countRequest_) emit noteCountReady(path, difficulty, watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([chart = slot->chart, timing = simai::buildTimingMetadata(doc)] {
        const auto parsed = SimaiParser::parseForTimeline(chart, timing);
        if (!parsed.ok) return -1;
        const auto notes = chart::buildScoringEvents(parsed.noteMarkers);
        return int(notes.events.size()) + notes.mineCount;
    }));
}

QString ChartLibraryModel::createSong(const QString& pack, const QString& title, int difficulty)
{
    if (busy_ || !library::isManagedLibrary(root_)) return {};
    const QString name = pack.trimmed();
    if (!packNameAllowed(name) || title.trimmed().isEmpty() || !SimaiDocument::isDifficultyId(difficulty)
        || !metadata::validateField("title", title).isEmpty()) {
        status_ = qtTrId("library.invalid_song"); emit changed(); return {};
    }
    const QDir packs(QDir(root_).filePath("packs"));
    if (!packs.exists() && !QDir(root_).mkdir("packs")) return {};
    QDir target(packs.filePath(name));
    if (!plainDirectory(packs.path()) || (!target.exists() && !packs.mkdir(name)) || !plainDirectory(target.path())) {
        status_ = qtTrId("library.create_failed").arg(target.path()); emit changed(); return {};
    }
    QSet<int> occupied; for (const auto& entry:entries_) occupied.insert(entry.songId.toInt());
    int id=0; while (occupied.contains(id) || target.exists(library::safeFolderName(title)+" - "+QString::number(id))) ++id;
    const auto folder=library::safeFolderName(title)+" - "+QString::number(id);
    if (!target.mkdir(folder)) { status_ = qtTrId("library.create_failed").arg(target.path()); emit changed(); return {}; }
    const auto path = target.filePath(folder + "/maidata.txt");
    QSaveFile output(path);
    const QByteArray contents = QStringLiteral("&title=%1\n&shortid=%3\n&inote_%2=(120){4}\nE\n").arg(title.trimmed(), QString::number(difficulty),QString::number(id)).toUtf8();
    if (!output.open(QIODevice::WriteOnly) || output.write(contents) != contents.size() || !output.commit()) {
        output.cancelWriting();
        target.rmdir(folder);
        status_ = qtTrId("library.create_failed").arg(path); emit changed(); return {};
    }
    if (!library::registerSong(root_, path, name, true)) { QFile::remove(path); target.rmdir(folder); status_=qtTrId("library.conflict");emit changed();return {}; }
    pack_ = name; genre_.clear(); versionCategory_.clear(); query_.clear(); duplicatesOnly_ = false;
    refresh();
    return path;
}

void ChartLibraryModel::removeSong(const QString& path)
{
    if (busy_ || !entryFor(path)) return;
    if (pathOpen(path)) { status_=qtTrId("library.close_editor_first"); emit changed(); return; }
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestConfirmation(qtTrId("library.remove_song"),qtTrId("library.unlink_prompt").arg(chartCaption(path)),
        qtTrId("library.remove_song"),[guard,root=root_,path](bool yes){
            if (!guard || !yes || guard->busy_ || guard->root_!=root || guard->pathOpen(path)) return;
            if (library::unlinkSongs(root, {path})) guard->refresh();
            else { guard->status_=qtTrId("library.remove_failed");emit guard->changed(); }
        });
}

void ChartLibraryModel::removeSelectedSongs()
{
    if (busy_ || selected_.isEmpty()) return;
    for (const auto& path : selected_) {
        if (pathOpen(path)) {
            status_ = qtTrId("library.close_editor_first");
            emit changed();
            return;
        }
    }
    if (library::unlinkSongs(root_, selected_)) refresh();
    else { status_ = qtTrId("library.remove_failed"); emit changed(); }
}

bool ChartLibraryModel::deleteCollection(const QString& pack, const QString& typedName)
{
    const bool wholeLibrary = pack.isEmpty();
    if (busy_ || root_.isEmpty() || typedName.isEmpty() || typedName != (wholeLibrary ? collectionName() : pack)
        || (!wholeLibrary && !packs_.contains(pack)))
        return false;
    const auto reject = [&](const QString& status) {
        status_ = status;
        emit changed();
        return false;
    };
    if (wholeLibrary && !sessions_.isEmpty())
        return reject(qtTrId("library.delete_library_close_sessions"));
    if (wholeLibrary && !unavailableRecovery_.isEmpty())
        return reject(qtTrId("library.delete_library_recovery_pending"));
    if (!wholeLibrary) {
        for (const auto& entry : entries_)
            if (entry.pack == pack && pathOpen(entry.path))
                return reject(qtTrId("library.close_editor_first"));
    }
    QSet<QString> protectedPaths;
    if (document_)
        for (const auto& open : document_->openSongPaths())
            protectedPaths.insert(QFileInfo(open).canonicalFilePath());
    for (const auto* session : sessions_)
        protectedPaths.insert(QFileInfo(session->path()).canonicalFilePath());
    const auto result = library::removeCollection(root_, pack, protectedPaths);
    if (result == library::CollectionRemoval::InUse)
        return reject(qtTrId("library.close_editor_first"));
    if (result != library::CollectionRemoval::Removed)
        return reject(wholeLibrary ? qtTrId("library.delete_library_failed") : qtTrId("library.delete_pack_failed"));

    if (wholeLibrary) {
        refreshTimer_.stop();
        if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
        if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
        QFile::remove(cachePath());
        QDir().rmdir(root_);
        root_.clear();
        indexedPacks_.clear();
        pendingSavedCharts_.clear();
        clearImportSources();
        selected_.clear();
        prepared_.clear();
        undo_.clear();
        QVector<Entry> empty;
        status_ = qtTrId("library.collection_deleted").arg(typedName);
        rebuildFilter(&empty);
        saveRecovery();
    } else {
        importStatus_ = qtTrId("library.collection_deleted").arg(pack);
        refresh();
    }
    return true;
}

QStringList ChartLibraryModel::catalogNames(const QString& key) const
{
    const QString path = QDir(root_).filePath("categories.json");
    if (root_.isEmpty() || !library::withinRoot(root_, path)) return {};
    QByteArray bytes;
    if (!library::read(path, &bytes, nullptr)) return {};
    return QJsonDocument::fromJson(bytes).object().value(key).toVariant().toStringList();
}

int ChartLibraryModel::categoryUsage(const QString& key, const QString& name) const
{
    int count = 0;
    for (const auto& entry : entries_) if ((key == "genre" ? entry.genre : entry.versionCategory) == name) ++count;
    return count;
}

void ChartLibraryModel::changeCategory(const QString& key, const QString& from, const QString& value)
{
    const QString to = value.trimmed();
    if (busy_ || !library::isManagedLibrary(root_) || (key != "genre" && key != "miacode_version_category") || from == to) return;
    if (!metadata::validateField(key, to).isEmpty()) { status_ = qtTrId("library.invalid_field"); emit changed(); return; }
    const auto names = key == "genre" ? genres_ : versions_;
    if ((!from.isEmpty() && !names.contains(from)) || (!to.isEmpty() && names.contains(to))) {
        status_ = qtTrId("library.category_exists"); emit changed(); return;
    }
    QHash<QString,QString> affected;
    for (const auto& entry : entries_) {
        if (!from.isEmpty() && (key == "genre" ? entry.genre : entry.versionCategory) == from)
            affected.insert(entry.path, entry.hash);
    }
    if (to.isEmpty() && !affected.isEmpty()) { status_ = qtTrId("library.category_used"); emit changed(); return; }
    const auto catalog = QDir(root_).filePath("categories.json");
    if (!QFileInfo::exists(catalog)) {
        QFile initial(catalog);
        if (!initial.open(QIODevice::WriteOnly | QIODevice::NewOnly) || initial.write("{}") != 2 || !initial.flush()) {
            status_ = qtTrId("library.create_failed").arg(catalog); emit changed(); return;
        }
    }
    const auto root = root_;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = cancel_;
    busy_ = true; status_ = qtTrId("library.category_working"); emit changed();
    progress_.begin(localizedText("library.title"), localizedText("library.category_working"), true);
    progress_.reportIndeterminate(localizedText("library.category_working"));
    worker_ = QThread::create([this, root, catalog, key, from, to, affected, canceled] {
        QVector<library::Change> changes;
        QByteArray before;
        QString error;
        if (!library::withinRoot(root, catalog) || !library::read(catalog, &before, &error))
            error = qtTrId("library.invalid_root");
        const auto json = QJsonDocument::fromJson(before);
        if (!json.isObject()) error = qtTrId("library.conflict");
        auto object = json.object();
        auto labels = object.value(key).toVariant().toStringList();
        labels.removeAll(from);
        if (!to.isEmpty() && !labels.contains(to)) labels.append(to);
        object.insert(key, QJsonArray::fromStringList(labels));
        changes.append({catalog, before, QJsonDocument(object).toJson(), {}});
        for (auto it = affected.cbegin(); it != affected.cend() && !canceled->load() && error.isEmpty(); ++it) {
            QString text;
            if (!library::withinRoot(QFileInfo(it.key()).absolutePath(), it.key()) || !library::read(it.key(), &before, &error)
                || QString::fromLatin1(library::digest(before).toHex()) != it.value()
                || !library::decode(before, &text, &error)) { error = qtTrId("library.conflict"); break; }
            if (!patchFields(text, {{key, to}}, error)) break;
            changes.append({it.key(), before, library::encode(text, before), {}});
        }
        QMetaObject::invokeMethod(this, [this, key, from, to, canceled, changes = std::move(changes), error]() mutable {
            if (canceled->load()) status_ = qtTrId("library.canceled");
            else if (!error.isEmpty()) status_ = error;
            else if (protectedChanges(changes)) status_ = qtTrId("library.close_editor_first");
            else if (writeChartChanges(&changes, &error)) {
                undo_ = std::move(changes);
                if (key == "genre" && !from.isEmpty() && genre_ == from) genre_ = to;
                if (key == "miacode_version_category" && !from.isEmpty() && versionCategory_ == from) versionCategory_ = to;
                finishThread();
                refresh(); return;
            } else status_ = error;
            finishThread();
        }, Qt::QueuedConnection);
    });
    worker_->start();
}

void ChartLibraryModel::chooseSongAudio(const QString& path)
{
    if (busy_ || !entryFor(path)) { emit songAudioReady(path); return; }
    const auto old = chart_assets::resolveTrackPath(path);
    FileRequest request;
    request.title = qtTrId("library.replace_track");
    request.startPath = QFileInfo(path).absolutePath();
    if (old.isEmpty()) request.nameFilters = {localizedText("file_filter.audio")};
    else request.nameFilters = {QStringLiteral("%1 (*.%2)").arg(QFileInfo(old).suffix().toUpper(), QFileInfo(old).suffix())};
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request, [guard, path, root = root_](const QString& source) {
        if (!guard) return;
        if (source.isEmpty() || root != guard->root_) { emit guard->songAudioReady(path); return; }
        guard->replaceSongAudio(path, source);
    });
}

void ChartLibraryModel::replaceSongAudio(const QString& path, const QString& source)
{
    auto reject = [&](const QString& message) { status_ = message; emit changed(); emit songAudioReady(path); };
    const auto* entry=entryFor(path);
    if (busy_ || !entry || !chart_assets::isSupportedTrackFilePath(source)) return;
    const QString editable=library::editableChartPath(root_,path,entry->record);
    if (editable.isEmpty()) { reject(qtTrId("library.audio_scope"));return; }
    const QDir directory=QFileInfo(editable).dir();
    const QString current=chart_assets::resolveTrackPath(editable);
    const auto suffix=QFileInfo(source).suffix().toLower();
    const QString old=library::withinRoot(directory.path(),current)?current:QString{};
    const QString destination=old.isEmpty()?directory.filePath("track."+suffix):old;
    if ((!old.isEmpty() && QFileInfo(old).suffix().toLower()!=suffix) || QFileInfo(destination).isSymLink()
        || QFileInfo(source).canonicalFilePath()==QFileInfo(destination).canonicalFilePath()) {
        reject(qtTrId("library.audio_scope"));return;
    }
    if (directory.entryList({"*.txt","*.simai"},QDir::Files).size()>1 ||
        (document_ && !document_->currentFilePath().isEmpty() && QFileInfo(document_->currentFilePath()).dir()==directory)) {
        reject(qtTrId("library.audio_shared"));return;
    }
    const auto stamp = fs::fileContentStamp(destination);
    const auto root = root_;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = cancel_;
    busy_ = true; status_ = qtTrId("library.audio_copy"); emit changed();
    progress_.begin(localizedText("library.title"), localizedText("library.audio_copy"), true);
    progress_.reportIndeterminate(localizedText("library.audio_copy"));
    worker_ = QThread::create([this, root, path, source, old, stamp, destination, canceled] {
        QFile input(source);
        QSaveFile output(destination);
        QString error;
        QString backup;
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) error = qtTrId("library.audio_failed");
        while (error.isEmpty() && !input.atEnd() && !canceled->load()) {
            const auto chunk = input.read(1024 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size()) error = qtTrId("library.audio_failed");
        }
        if (canceled->load()) error = qtTrId("library.canceled");
        if (error.isEmpty() && (!library::withinRoot(QFileInfo(path).absolutePath(), path) || QFileInfo(destination).isSymLink()
            || fs::fileContentStamp(destination) != stamp)) error = qtTrId("library.conflict");
        if (error.isEmpty() && !old.isEmpty()) {
            const QDir backups(QDir(root).filePath("media-backups"));
            backup = backups.filePath(QUuid::createUuid().toString(QUuid::Id128) + "-" + QFileInfo(old).fileName());
            if ((!backups.exists() && !QDir(root).mkdir("media-backups")) || !plainDirectory(backups.path())
                || !QFile::copy(old, backup)) error = qtTrId("library.audio_failed");
        }
        if (error.isEmpty() && !output.commit()) error = qtTrId("library.audio_failed");
        QMetaObject::invokeMethod(this, [this, path, error, backup] {
            status_ = error.isEmpty() ? (backup.isEmpty() ? qtTrId("library.audio_added") : qtTrId("library.audio_replaced").arg(backup)) : error;
            finishThread();
            emit songAudioReady(path);
            if (error.isEmpty()) refresh();
        }, Qt::QueuedConnection);
    });
    worker_->start();
}

}
