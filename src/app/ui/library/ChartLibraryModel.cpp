#include "app/ui/library/ChartLibraryModel.h"
#include "app/ui/document/DocumentModel.h"
#include "app/services/UiRequestService.h"
#include "common/RuntimePaths.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "core/chart/document/SimaiDocument.h"
#include "app/services/ManagedChartLibrary.h"
#include "core/chart/document/AdvancedMetadata.h"
#include <QRegularExpression>
#include "core/chart/ChartAssetPaths.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextCursor>
#include <QUuid>
#include <algorithm>

namespace miacode::ui {
namespace {
bool saveJson(const QString& path,const QJsonObject& value,QString* error=nullptr)
{
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if(error)*error=QStringLiteral("Cannot create recovery directory: %1").arg(path);
        return false;
    }
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit()) return true;
    if(error)*error=file.errorString();
    return false;
}
QJsonObject loadJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size()>128*1024*1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QString storageDirectory(const char* name)
{
    QDir directory(QFileInfo(miacode::runtime_paths::configFile("preferences_v2.json")).absolutePath());
    directory.cdUp();
    return directory.filePath(QString::fromLatin1(name));
}
QString recoveryPath() { return QDir(storageDirectory("sessions")).filePath("chart-library.json"); }
}
LibraryDocument::LibraryDocument(QString path,QByteArray bytes,QObject* parent)
    : QObject(parent),original(std::move(bytes)),expectedHash(library::digest(original)),path_(std::move(path))
{
    QString value; library::decode(original,&value,nullptr);
    document_.setPlainText(value); savedText_=value; document_.setUndoRedoEnabled(true);
    connect(&document_,&QTextDocument::contentsChanged,this,[this]{++revision_; emit changed();});
}
void LibraryDocument::setText(const QString& value) { replaceText(value); }
bool LibraryDocument::replaceText(const QString& value)
{
    if (value.toUtf8().size()>library::kMaximumFileBytes) {
        error_=qtTrId("library.file_limit");emit changed();return false;
    }
    if (!error_.isEmpty()) {error_.clear();emit changed();}
    if (value==text()) return true;
    historyBytes_+=document_.characterCount()*2;
    if (historyBytes_>8*1024*1024) { document_.clearUndoRedoStacks(); historyBytes_=0; }
    QTextCursor cursor(&document_); cursor.beginEditBlock(); cursor.select(QTextCursor::Document);
    cursor.insertText(value); cursor.endEditBlock();
    return true;
}
void LibraryDocument::markSaved(QByteArray bytes)
{ original=std::move(bytes); expectedHash=library::digest(original); savedText_=text(); emit changed(); }

ChartLibraryModel::ChartLibraryModel(miacode::UiRequestService& requests,DocumentModel& document,QObject* parent)
    : QAbstractListModel(parent),requests_(&requests),document_(&document)
{
    refreshTimer_.setSingleShot(true); refreshTimer_.setInterval(800);
    recoveryTimer_.setSingleShot(true); recoveryTimer_.setInterval(500);
    connect(&refreshTimer_,&QTimer::timeout,this,[this]{if(!busy_)refresh();});
    connect(&recoveryTimer_,&QTimer::timeout,this,&ChartLibraryModel::saveRecovery);
    connect(&watcher_,&QFileSystemWatcher::directoryChanged,this,[this]{refreshTimer_.start();});
    connect(&watcher_,&QFileSystemWatcher::fileChanged,this,[this]{refreshTimer_.start();});
    connect(&progress_, &JobProgressService::cancellationRequested, this, [this](quint64 token) { if(token==progress_.token())cancel(); });
    restoreRecovery();
}
ChartLibraryModel::~ChartLibraryModel()
{ cancel(); if(worker_) { worker_->wait(); delete worker_; } saveRecovery(); }
int ChartLibraryModel::rowCount(const QModelIndex& parent) const { return parent.isValid()?0:visible_.size(); }
QVariant ChartLibraryModel::data(const QModelIndex& index,int role) const
{
    if(!index.isValid() || index.row()<0 || index.row()>=visible_.size())return {};
    const int mapped=visible_[index.row()];
    if(mapped<0||mapped>=entries_.size())return {};
    const auto& e=entries_[mapped];
    switch(role) {
    case JacketRole: {
        const auto file = chart_assets::resolveBackgroundMediaPath(e.path, false);
        return !file.isEmpty() ? QUrl::fromLocalFile(file) : QUrl{};
    }
    case BadgesRole: {
        QVariantList badges;
        const auto levels = e.levels.split(" / ", Qt::SkipEmptyParts);
        for (const auto& level : levels) {
            const auto code = level.section(' ', 0, 0);
            for (int slot = 1; slot <= 7; ++slot) {
                if (SimaiDocument::difficultyShortName(slot) == code) {
                    badges.append(QVariantMap{{"slot", slot}, {"name", code}, {"label", level.section(' ', 1)}});
                    break;
                }
            }
        }
        return badges;
    }
    case PackRole:return e.pack; case GenreRole:return e.genre; case SongIdRole:return e.songId; case KindRole:return e.kind;
    case PathRole:return e.path; case TitleRole:return e.title; case ArtistRole:return e.artist;
    case LevelsRole:return e.levels; case VersionRole:return e.version; case DigestRole:return e.hash;
    case DuplicateRole:return e.copies; case SelectedRole:return selected_.contains(e.path); default:return {};
    }
}
QHash<int,QByteArray> ChartLibraryModel::roleNames() const
{ return {{PathRole,"chartPath"},{TitleRole,"chartTitle"},{ArtistRole,"artist"},{LevelsRole,"levels"},
    {JacketRole,"jacketUrl"},{BadgesRole,"difficultyBadges"},{PackRole,"chartPack"},{GenreRole,"chartGenre"},{SongIdRole,"songId"},{KindRole,"chartKind"},
    {VersionRole,"chartVersion"},{DigestRole,"sha256"},{DuplicateRole,"copies"},{SelectedRole,"selected"}}; }
void ChartLibraryModel::Entry::updateSearch()
{
    search = QStringList{path, title, artist, levels, version, pack, genre, songId, kind, versionCategory}.join(' ').toCaseFolded();
}

void ChartLibraryModel::setPack(const QString& value)
{
    if (pack_ != value) { pack_ = value; rebuildFilter(); }
}

void ChartLibraryModel::setGenre(const QString& value)
{
    if (genre_ != value) { genre_ = value; rebuildFilter(); }
}

void ChartLibraryModel::setVersionCategory(const QString& value)
{
    if (versionCategory_ != value) { versionCategory_ = value; rebuildFilter(); }
}

void ChartLibraryModel::rebuildFilter(QVector<Entry>* replacement)
{
    beginResetModel();
    if (replacement) {
        entries_.swap(*replacement);
        QSet<QString> packs, genres, versions;
        if (!root_.isEmpty()) {
            const QDir packRoot(QDir(root_).filePath("packs"));
            for (const auto& folder : packRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
                if (!folder.isJunction()) packs.insert(folder.fileName());
        }
        for (const auto& name : catalogNames("genre")) genres.insert(name);
        for (const auto& name : catalogNames("miacode_version_category")) versions.insert(name);
        for (const auto& entry : entries_) {
            if (!entry.pack.isEmpty()) packs.insert(entry.pack);
            if (!entry.versionCategory.isEmpty()) versions.insert(entry.versionCategory);
            if (!entry.genre.isEmpty()) genres.insert(entry.genre);
        }
        packs_ = packs.values(); packs_.sort();
        genres_ = genres.values(); genres_.sort();
        versions_ = versions.values(); versions_.sort();
        if (!packs.contains(pack_)) pack_.clear();
        if (!genres.contains(genre_)) genre_.clear();
        if (!versions.contains(versionCategory_)) versionCategory_.clear();
    }
    visible_.clear();
    const auto words=query_.toCaseFolded().split(QLatin1Char(' '),Qt::SkipEmptyParts);
    for(int i=0;i<entries_.size();++i) {
        const auto& e=entries_[i]; bool keep=!duplicatesOnly_ || e.copies>1;
        keep = keep && (pack_.isEmpty() || e.pack == pack_) && (genre_.isEmpty() || e.genre == genre_)
            && (versionCategory_.isEmpty() || e.versionCategory == versionCategory_);
        for(const auto& word:words) keep=keep&&e.search.contains(word);
        if(keep)visible_.append(i);
    }
    endResetModel(); emit changed();
}
void ChartLibraryModel::setQuery(const QString& value) {if(query_!=value){query_=value;rebuildFilter();}}
void ChartLibraryModel::setDuplicatesOnly(bool value){if(duplicatesOnly_!=value){duplicatesOnly_=value;rebuildFilter();}}
QString ChartLibraryModel::cachePath() const
{return QDir(storageDirectory("cache")).filePath("chart-library-"+QString::fromLatin1(library::digest(root_.toUtf8()).toHex())+".json");}
QString ChartLibraryModel::journalPath() const
{return QDir(storageDirectory("logs")).filePath("library-"+QUuid::createUuid().toString(QUuid::Id128)+".json");}
void ChartLibraryModel::chooseRoot()
{
    if(busy_)return;
    miacode::FileRequest request;request.nameFilters={QStringLiteral("Better Miacode (*.miabundle miacode-library.json)")};request.title=qtTrId("library.choose_root");request.startPath=root_;
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request,[guard](const QString& path){if(guard&&!path.isEmpty())guard->setRoot(path);});
}
void ChartLibraryModel::reportProgress(int percent)
{
    QMetaObject::invokeMethod(this,[this,percent]{progress_.report(percent,status_);},Qt::QueuedConnection);
}

void ChartLibraryModel::chooseExport()
{
    if (busy_ || root_.isEmpty()) return;
    FileRequest request;
    request.selectFolder=true; request.title=qtTrId("library.export");
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request,[guard,root=root_](const QString& parent){
        if (!guard || guard->busy_ || parent.isEmpty() || guard->root_!=root) return;
        guard->busy_=true; guard->status_=qtTrId("library.export");
        guard->progress_.begin(localizedText("library.title"),localizedText("library.export"),true);
        guard->cancel_=std::make_shared<std::atomic_bool>(false);
        emit guard->changed();
        guard->worker_=QThread::create([guard,root,parent,canceled=guard->cancel_]{
            QString error;
            const auto path=library::exportLibrary(root,parent,*canceled,[guard](int p){guard->reportProgress(p);},&error);
            QMetaObject::invokeMethod(guard,[guard,path,error]{
                guard->status_=error.isEmpty()?qtTrId("library.export_done").arg(path):error;
                guard->finishThread();
            },Qt::QueuedConnection);
        });
        guard->worker_->start();
    });
}

bool ChartLibraryModel::writeChartChanges(QVector<library::Change>* changes,QString* error)
{
    QSet<QString> allowed;
    for (auto& change:*changes) {
        if (change.path==QDir(root_).filePath("categories.json")) continue;
        const auto* entry=entryFor(change.path);
        const auto path=entry ? library::editableChartPath(root_,change.path,entry->record) : QString{};
        if (path.isEmpty()) { *error=qtTrId("library.conflict"); return false; }
        change.path=path; allowed.insert(path);
    }
    return library::apply(root_,changes,journalPath(),error,{},allowed);
}
void ChartLibraryModel::chooseNewLibrary()
{
    if (busy_) return;
    if (!sessions_.isEmpty()) { status_ = qtTrId("library.close_sessions_first"); emit changed(); return; }
    FileRequest request;
    request.selectFolder = true;
    request.title = qtTrId("library.choose_parent");
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request, [guard](const QString& parent) {
        if (!guard || parent.isEmpty() || guard->busy_) return;
        if (!guard->sessions_.isEmpty()) {
            guard->status_ = qtTrId("library.close_sessions_first"); emit guard->changed(); return;
        }
        QString error;
        const QString path = library::createLibrary(parent, &error);
        if (!path.isEmpty()) guard->setRoot(path);
        else { guard->status_ = error; emit guard->changed(); }
    });
}

void ChartLibraryModel::chooseImportFolders(const QString& targetPack, bool archives, bool manualSong)
{
    if (busy_ || root_.isEmpty()) return;
    FileRequest request;
    request.selectFolder = !archives;
    if (archives) request.nameFilters = {QStringLiteral("ZIP (*.zip)")};
    request.title = archives ? qtTrId("library.choose_zip") : targetPack.isEmpty() ? qtTrId("library.choose_import") : qtTrId("library.import_to").arg(targetPack);
    const QString root = root_;
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFiles(request, [guard, root, targetPack, manualSong](const QStringList& folders) {
        if (guard && root == guard->root_) guard->importFolders(folders, targetPack, manualSong);
    });
}

void ChartLibraryModel::importFolders(const QStringList& folders, const QString& targetPack, bool manualSong)
{
    runImport(folders, targetPack, manualSong, false);
}

void ChartLibraryModel::consolidateSongs()
{
    runImport({}, {}, false, true);
}

void ChartLibraryModel::runImport(const QStringList& folders, const QString& targetPack, bool manualSong, bool consolidate)
{
    if (busy_ || (!consolidate && folders.isEmpty()) || !library::isManagedLibrary(root_)) return;
    busy_ = true;
    importInProgress_ = true;
    status_ = qtTrId("library.importing");
    emit changed();
    progress_.begin(localizedText("library.title"), localizedText("library.importing"), true);
    progress_.report(0,localizedText("library.importing"));
    cancel_ = std::make_shared<std::atomic_bool>(false);
    QSet<QString> protectedPaths;
    for (const auto* session : sessions_) protectedPaths.insert(session->path());
    if (document_) for (const auto& path : document_->openSongPaths()) protectedPaths.insert(QFileInfo(path).canonicalFilePath());
    worker_ = QThread::create([this, root = root_, folders, targetPack, manualSong, consolidate, protectedPaths, canceled = cancel_] {
        const auto result = library::importChartFolders(root, folders, *canceled, targetPack, protectedPaths, consolidate, manualSong, [this](int percent){ reportProgress(percent); });
        QMetaObject::invokeMethod(this, [this, result, canceled] {
            importInProgress_ = false;
            finishThread();
            refreshTimer_.stop();
            importStatus_ = qtTrId("library.import_summary").arg(result.imported).arg(result.merged).arg(result.existing).arg(result.missingMedia).arg(result.conflicts).arg(result.failed);
            if (canceled->load()) importStatus_.prepend(qtTrId("library.import_stopped") + " ");
            if (!result.firstError.isEmpty()) importStatus_ += " (" + result.firstError + ")";
            refresh();
        }, Qt::QueuedConnection);
    });
    worker_->start();
}

bool ChartLibraryModel::setRoot(const QString& path)
{
    if(busy_)return false;
    const QFileInfo selection(path);
    const QFileInfo dir(selection.isFile() ? selection.absolutePath() : path);
    if(!dir.isDir()||dir.isSymLink()||!library::isManagedLibrary(dir.absoluteFilePath()) || (selection.isFile() && selection.fileName()!="miacode-library.json" && selection.canonicalFilePath()!=QFileInfo(library::bundleFile(dir.absoluteFilePath())).canonicalFilePath())){status_=qtTrId("library.invalid_root");emit changed();return false;}
    const QString canonical=dir.canonicalFilePath();
    if(canonical.isEmpty())return false;
    if(root_!=canonical && !sessions_.isEmpty()){status_=qtTrId("library.close_sessions_first");emit changed();return false;}
    if(root_!=canonical && !unavailableRecovery_.isEmpty()) {
        const QString archive=QDir(storageDirectory("sessions")).filePath("library-unavailable-"+QUuid::createUuid().toString(QUuid::Id128)+".json");
        QString error;
        if(!saveJson(archive,{{"schema",1},{"root",root_.isEmpty()?deferredRoot_:root_},{"sessions",unavailableRecovery_}},&error)) {
            status_=qtTrId("library.recovery_failed").arg(error);emit changed();return false;
        }
        unavailableRecovery_={}; deferredRoot_.clear();
        requests_->postNotice(NoticeSeverity::Information,qtTrId("library.title"),qtTrId("library.recovery_archived").arg(archive));
    }
    if (root_ != canonical) { pack_.clear(); genre_.clear(); versionCategory_.clear(); importStatus_.clear(); }
    root_=canonical;selected_.clear();prepared_.clear();undo_.clear();QVector<Entry> restored;
    rebuildFilter(&restored); refresh(); return true;
}
void ChartLibraryModel::finishThread()
{ if(worker_){worker_->wait();delete worker_;worker_=nullptr;}progress_.end();busy_=false;emit changed(); }
void ChartLibraryModel::cancel(){if(cancel_)cancel_->store(true);}
void ChartLibraryModel::refresh()
{
    if(busy_||root_.isEmpty())return;
    if(!library::isManagedLibrary(root_)){status_=qtTrId("library.recovery_unavailable");emit changed();return;}
    busy_=true;status_=qtTrId("library.scanning");prepared_.clear();emit changed();
    const auto root=root_; const auto generation=++generation_;cancel_=std::make_shared<std::atomic_bool>(false);
    progress_.begin(localizedText("library.title"),localizedText("library.scanning"),true);
    progress_.report(0,localizedText("library.scanning"));
    const auto canceled=cancel_; QHash<QString,Entry> cached;for(const auto& e:entries_)cached.insert(e.path,e);
    worker_=QThread::create([this,root,generation,canceled,cached]{
        QVector<Entry> found;QHash<QString,int> hashes;int skipped=0;qint64 indexBytes=0;
        QStringList watchedDirectories{root};
        const auto rows=library::bundleSongs(root,&skipped,false,canceled.get(),[this](int percent){reportProgress(percent);});
        for (const auto& row:rows) {
            if (canceled->load()) break;
            const auto path=row.path;
            const QFileInfo info(path);
            const auto watchDirectory=QFileInfo(row.origin.section("!/",0,0)).absolutePath();
            if (watchedDirectories.size()<512 && !watchedDirectories.contains(watchDirectory)) watchedDirectories.append(watchDirectory);
            if (found.size()>=100000) {++skipped;break;}
            Entry e=cached.value(path);
            if(e.path.isEmpty()||e.size!=info.size()||e.modified!=info.lastModified().toMSecsSinceEpoch()){
                QByteArray bytes;QString text,error;
                if(!library::read(path,&bytes,&error)||!library::decode(bytes,&text,&error)){++skipped;continue;}
                const auto doc=SimaiDocument::fromText(text);QStringList levels;
                for(int id:doc.difficultyIds())if(const auto* d=doc.difficulty(id))levels.append(SimaiDocument::difficultyShortName(id)+" "+d->level);
                const QString version = metadata::fieldValue(doc, "version").trimmed();
                e={path,doc.title.left(512),doc.artist.left(512),levels.join(" / ").left(256),version.left(256),QString::fromLatin1(library::digest(bytes).toHex()),{},info.size(),info.lastModified().toMSecsSinceEpoch(),1};
                e.pack = row.pack;
                e.versionCategory = metadata::fieldValue(doc, "miacode_version_category").trimmed().left(256);
                e.genre = metadata::fieldValue(doc, "genre").trimmed().left(256);
                e.songId = metadata::fieldValue(doc, "shortid").trimmed();
                {
                    const QString original=row.origin;
                    if (e.title.isEmpty()) e.title = QFileInfo(original).dir().dirName().left(512);
                    if (e.songId.isEmpty()) {
                        // Recognized archive and game folder layouts, never an arbitrary numeric parent.
                        static const QRegularExpression catalogId(
                            QStringLiteral("(?:/谱面/|/music)([0-9]{1,6})/"), QRegularExpression::CaseInsensitiveOption);
                        e.songId = catalogId.match(original).captured(1);
                    }
                }
                const auto cabinet = metadata::fieldValue(doc, "cabinet").trimmed().toUpper();
                bool knownId = false;
                int id = e.songId.toInt(&knownId);
                knownId = knownId && id >= 0 && id <= 999999;
                const bool banquet = e.title.endsWith("[宴]") || e.genre == QStringLiteral("宴会場")
                    || (knownId && id >= 100000 && id < 200000);
                if (banquet) e.kind = "UTAGE";
                else if ((knownId && id >= 10000 && id < 20000) || cabinet == "DX"
                         || e.title.endsWith("[DX]", Qt::CaseInsensitive)) e.kind = "DX";
                else if (cabinet == "SD" || e.title.endsWith("[SD]", Qt::CaseInsensitive)
                         || (knownId && id < 10000)) e.kind = "SD";
                if (knownId) {
                    if (banquet && id < 20000) id += 100000;
                    else if (e.kind == "DX" && id < 10000) id += 10000;
                    e.songId = QString::number(id);
                } else e.songId.clear();
                if (e.songId.isEmpty()) e.songId=row.id;
                e.updateSearch();
            }
            e.record=row.record;
            e.pack=row.pack;
            if (!row.id.isEmpty()) e.songId=row.id;
            e.updateSearch();
            indexBytes += 2 * (e.path.size()+e.title.size()+e.artist.size()+e.levels.size()+e.version.size()+e.hash.size()+e.search.size()) + 128;
            if(indexBytes>64*1024*1024){++skipped;break;}
            ++hashes[e.hash];found.append(e);
        }
        for(auto& e:found)e.copies=hashes[e.hash];
        std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
            if (a.pack != b.pack) return a.pack < b.pack;
            if (a.songId.isEmpty() != b.songId.isEmpty()) return !a.songId.isEmpty();
            if (a.songId != b.songId) return a.songId.toInt() < b.songId.toInt();
            if (a.title != b.title) return a.title < b.title;
            return a.path < b.path;
        });
        QMetaObject::invokeMethod(this,[this,generation,canceled,found=std::move(found),watchedDirectories,skipped]()mutable{
            if(generation!=generation_)return;
            finishThread();
            if(canceled->load()){status_=qtTrId("library.canceled");emit changed();return;}
            QSet<QString> existing;for(const auto& e:found)existing.insert(e.path);
            selected_.intersect(existing);rebuildFilter(&found);persistIndex();
            watcher_.removePaths(watcher_.directories());watcher_.removePaths(watcher_.files());
            watcher_.addPaths(watchedDirectories);
            QStringList watchedFiles{library::bundleFile(root_)};for(const auto& e:entries_){if(watchedFiles.size()>=2048)break;watchedFiles.append(e.path);}
            if(!watchedFiles.isEmpty())watcher_.addPaths(watchedFiles);
            status_=importStatus_.isEmpty() ? qtTrId("library.indexed").arg(entries_.size()).arg(skipped) : importStatus_;
            importStatus_.clear();emit changed();
        },Qt::QueuedConnection);
    });worker_->start();
}
void ChartLibraryModel::persistIndex()
{
    QJsonArray rows;for(const auto&e:entries_)rows.append(QJsonObject{{"path",e.path},{"title",e.title},{"artist",e.artist},{"levels",e.levels},{"version",e.version},{"hash",e.hash},{"pack",e.pack},{"genre",e.genre},{"songId",e.songId},{"kind",e.kind},{"versionCategory",e.versionCategory},{"size",e.size},{"modified",e.modified}});
    saveJson(cachePath(),{{"schema",4},{"root",root_},{"entries",rows}});recoveryTimer_.start();
}
void ChartLibraryModel::selectPath(const QString& path,bool value)
{
    if(busy_)return;
    if(value)selected_.insert(path);else selected_.remove(path);
    prepared_.clear();if(rowCount())emit dataChanged(index(0),index(rowCount()-1),{SelectedRole});emit changed();
}
void ChartLibraryModel::selectVisible(bool value)
{if(busy_)return;for(int i:visible_){if(value)selected_.insert(entries_[i].path);else selected_.remove(entries_[i].path);}prepared_.clear();if(rowCount())emit dataChanged(index(0),index(rowCount()-1),{SelectedRole});emit changed();}
QString ChartLibraryModel::chartCaption(const QString& path) const
{
    for (const auto& entry : entries_) {
        if (entry.path != path) continue;
        QStringList parts{entry.title.isEmpty() ? qtTrId("library.untitled") : entry.title};
        const bool banquet = entry.kind == "UTAGE";
        const QString tag = banquet ? QStringLiteral("[宴]") : QStringLiteral("[%1]").arg(entry.kind);
        if (!entry.kind.isEmpty() && !entry.title.endsWith(tag, Qt::CaseInsensitive))
            parts << (banquet ? qtTrId("library.utage") : entry.kind);
        if (!entry.songId.isEmpty()) parts << entry.songId;
        return parts.join(QStringLiteral(" · "));
    }
    return QDir(root_).relativeFilePath(path);
}

QVariantList ChartLibraryModel::sessions()const{QVariantList result;for(auto* s:sessions_)result.append(QVariant::fromValue(static_cast<QObject*>(s)));return result;}
void ChartLibraryModel::openBuffer(const QString& path)
{
    if (importInProgress_) return;
    for(auto* s:sessions_)if(s->path()==path){formDrafts_.remove(s);active_=s;emit changed();return;}
    if(sessions_.size()>=5){status_=qtTrId("library.session_limit");emit changed();return;}
    QByteArray bytes;QString text,error;
    if((!entryFor(path) && !recoveryPaths_.contains(path))||!library::read(path,&bytes,&error)||!library::decode(bytes,&text,&error)){
        status_=error.isEmpty()?qtTrId("library.invalid_root"):error;emit changed();return;
    }
    auto* session=new LibraryDocument(path,bytes,this);sessions_.append(session);active_=session;
    connect(session,&LibraryDocument::changed,this,[this]{recoveryTimer_.start();});
    recoveryTimer_.start();emit changed();
}
void ChartLibraryModel::activateBuffer(int index){if(index>=0&&index<sessions_.size()){active_=sessions_[index];emit changed();}}
void ChartLibraryModel::closeActive()
{
    QPointer<LibraryDocument> session=active_;if(!session)return;
    const auto close=[this,session](bool yes){if(!session||!yes)return;formDrafts_.remove(session);sessions_.removeAll(session);if(active_==session)active_=sessions_.isEmpty()?nullptr:sessions_.first();delete session;saveRecovery();emit changed();};
    if(session->dirty()) requests_->requestConfirmation(qtTrId("library.close_buffer"),qtTrId("library.discard_prompt"),qtTrId("library.discard"),close);
    else close(true);
}
bool ChartLibraryModel::pathOpen(const QString& path) const
{
    if (bufferFor(path)) return true;
    if (document_) {
        for (const auto& open : document_->openSongPaths())
            if (QFileInfo(open) == QFileInfo(path)) return true;
    }
    return false;
}
bool ChartLibraryModel::protectedChanges(const QVector<library::Change>& changes)const
{for(const auto&c:changes)if(pathOpen(c.path))return true;return false;}
void ChartLibraryModel::requestSave(LibraryDocument* session)
{
    if(!session||!session->dirty()||busy_)return;
    if(document_ && document_->openSongPaths().contains(session->path())){
        status_=qtTrId("library.close_editor_first");emit changed();return;
    }
    QByteArray bytes;QString error;
    if(!library::read(session->path(),&bytes,&error)||library::digest(bytes)!=session->expectedHash){status_=qtTrId("library.conflict");emit changed();return;}
    QVector<library::Change> change{{session->path(),bytes,library::encode(session->text(),session->original),{}}};
    if(writeChartChanges(&change,&error)){session->markSaved(change.first().after);status_=qtTrId("library.saved");saveRecovery();refresh();}
    else status_=error;
    emit changed();
}
void ChartLibraryModel::saveActive(){requestSave(active_);}
bool ChartLibraryModel::additionalOpenModifierHeld() const
{
    return QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
}
void ChartLibraryModel::openFromList(const QString& path)
{
    if (!additionalOpenModifierHeld()) { openInEditor(path); return; }
    const auto* entry = entryFor(path);
    if (busy_ || !document_ || !entry) return;
    if (auto* draft = bufferFor(path); draft && draft->dirty()) {
        status_ = qtTrId("library.save_buffer_first"); emit changed(); return;
    }
    const auto editable = library::editableChartPath(root_, path, entry->record);
    if (document_->openAdditionalSong(editable)) emit editorOpened();
    else if (editable.isEmpty()) { status_ = qtTrId("library.open_failed").arg(path); emit changed(); }

}
void ChartLibraryModel::openInEditor(const QString& path, int difficulty)
{
    if(importInProgress_||!document_||!entryFor(path))return;
    for(auto*s:sessions_)if(s->path()==path&&s->dirty()){status_=qtTrId("library.save_buffer_first");emit changed();return;}
    QPointer<ChartLibraryModel> guard(this);
    document_->requestLeaveDocument([guard,path,difficulty](bool yes) {
        if (!guard || !yes || !guard->document_ || !guard->entryFor(path)) return;
        const auto editable=library::editableChartPath(guard->root_,path,guard->entryFor(path)->record);
        if (!editable.isEmpty() && guard->document_->openFile(QUrl::fromLocalFile(editable))) {
            if (SimaiDocument::isDifficultyId(difficulty)) guard->document_->selectDifficulty(difficulty);
            emit guard->editorOpened();
        }
    });
}
QVariantList ChartLibraryModel::proposals()const
{
    QVariantList list;
    for(const auto& c:prepared_) {
        const QString before=QString::fromUtf8(c.before),after=QString::fromUtf8(c.after);
        int start=0;while(start<before.size()&&start<after.size()&&before[start]==after[start])++start;
        start=qMax(0,before.lastIndexOf(QLatin1Char('\n'),qMax(0,start-1))+1);
        list.append(QVariantMap{{"path",c.path},{"before",before.mid(start,2000)},{"after",after.mid(start,2000)}});
    }
    return list;
}
void ChartLibraryModel::prepareNormalization()
{
    if(busy_||selected_.isEmpty())return;
    if(selected_.size()>100){status_=qtTrId("library.batch_limit");emit changed();return;}
    busy_=true;prepared_.clear();status_=qtTrId("library.preparing");emit changed();
    const QStringList paths=selected_.values();const auto root=root_;const auto generation=++generation_;
    const auto stored=PreferenceDocument::loadPreferencesObject().value("app").toObject().value("preview").toObject();
    const auto options=app_preferences::chartNormalizationOptionsFromPreferences(stored, chart_transform::ChartNormalizationOptions{true,false,true});
    cancel_=std::make_shared<std::atomic_bool>(false);const auto canceled=cancel_;
    progress_.begin(localizedText("library.title"),localizedText("library.preparing"),true);
    progress_.reportIndeterminate(localizedText("library.preparing"));
    worker_=QThread::create([this,paths,root,generation,canceled,options]{
        QVector<library::Change> changes;QString error;qint64 total=0;
        for(const auto&path:paths){
            if(canceled->load())break;
            QByteArray bytes,normalized;
            if(!library::withinRoot(QFileInfo(path).absolutePath(),path)||!library::read(path,&bytes,&error)){if(error.isEmpty())error="Unsafe path";break;}
            total+=bytes.size();if(total>16*1024*1024){error=QStringLiteral("Batch exceeds 16 MiB; select fewer charts.");break;}
            if(!library::normalize(bytes,&normalized,&error,options))break;
            if(normalized!=bytes)changes.append({path,bytes,normalized,{}});
        }
        QMetaObject::invokeMethod(this,[this,generation,canceled,changes=std::move(changes),error]()mutable{
            if(generation!=generation_)return;finishThread();
            if(canceled->load())status_=qtTrId("library.canceled");
            else if(!error.isEmpty()) {
                status_ = error;
                requests_->postNotice(NoticeSeverity::Warning, localizedText("library.normalize_warning"), error);
            }
            else{prepared_=std::move(changes);status_=qtTrId("library.prepared").arg(prepared_.size());}
            emit changed();
        },Qt::QueuedConnection);
    });worker_->start();
}
void ChartLibraryModel::applyPrepared()
{
    if(busy_||prepared_.isEmpty())return;
    if(protectedChanges(prepared_)){status_=qtTrId("library.close_editor_first");emit changed();return;}
    const auto generation=generation_;const auto changes=prepared_;QPointer<ChartLibraryModel> guard(this);
    requests_->requestConfirmation(qtTrId("library.apply"),qtTrId("library.apply_prompt").arg(changes.size()),qtTrId("library.apply"),
        [guard,generation,changes](bool yes)mutable{
            if(!guard||!yes)return;
            if(guard->busy_||guard->generation_!=generation||guard->prepared_.size()!=changes.size()||guard->protectedChanges(changes)){
                guard->status_=qtTrId("library.conflict");emit guard->changed();return;
            }
            auto transaction=changes;QString error;
            if(guard->writeChartChanges(&transaction,&error)){guard->undo_=transaction;guard->prepared_.clear();guard->refresh();}
            else{guard->status_=error;guard->prepared_.clear();emit guard->changed();}
        });
}
void ChartLibraryModel::undoBatch()
{
    if(!canUndoBatch())return;
    if(protectedChanges(undo_)){status_=qtTrId("library.close_editor_first");emit changed();return;}
    QString error;
    QVector<library::Change> reverse; for (const auto& change:undo_) reverse.append({change.path,change.after,change.before,{}});
    if(writeChartChanges(&reverse,&error)){undo_.clear();refresh();}else{status_=error;emit changed();}
}
void ChartLibraryModel::saveRecovery() { writeRecovery(); }
bool ChartLibraryModel::checkpoint()
{
    recoveryTimer_.stop();
    if(writeRecovery())return true;
    return std::none_of(sessions_.cbegin(),sessions_.cend(),[](const LibraryDocument* session){return session->dirty();})
        && std::none_of(unavailableRecovery_.cbegin(),unavailableRecovery_.cend(),[](const QJsonValue& value){return value.toObject().value("dirty").toBool();});
}
bool ChartLibraryModel::writeRecovery()
{
    QJsonArray sessions=unavailableRecovery_;for(auto*s:sessions_)sessions.append(QJsonObject{{"path",s->path()},{"text",s->text()},
        {"expected",QString::fromLatin1(s->expectedHash.toHex())},{"dirty",s->dirty()}});
    QString error;
    if(!saveJson(recoveryPath(),{{"schema",1},{"root",root_.isEmpty()?deferredRoot_:root_},{"sessions",sessions}},&error)) {
        recoveryFailed_=true;status_=qtTrId("library.recovery_failed").arg(error);emit changed();return false;
    }
    if(recoveryFailed_){recoveryFailed_=false;status_.clear();emit changed();}
    return true;
}
void ChartLibraryModel::restoreRecovery()
{
    const auto saved=loadJson(recoveryPath());if(saved.value("schema").toInt()!=1)return;
    root_=saved.value("root").toString();
    if(!library::isManagedLibrary(root_)){
        deferredRoot_=root_; root_.clear();
        unavailableRecovery_=saved.value("sessions").toArray();
        recoveryPaths_.clear();
    if(!unavailableRecovery_.isEmpty())status_=qtTrId("library.recovery_unavailable");
        return;
    }
    if (!saved.value("sessions").toArray().isEmpty())
        for (const auto& song:library::bundleSongs(root_)) recoveryPaths_.insert(song.path);
    for(const auto&value:saved.value("sessions").toArray()){
        const auto object=value.toObject();const auto path=object.value("path").toString();
        if(sessions_.size()>=5){unavailableRecovery_.append(object);continue;}
        openBuffer(path);
        if(!active_||active_->path()!=path){unavailableRecovery_.append(object);continue;}
        if(object.value("dirty").toBool()){
            active_->expectedHash=QByteArray::fromHex(object.value("expected").toString().toLatin1());
            active_->setText(object.value("text").toString());
        }
    }
    recoveryPaths_.clear();
    if(!unavailableRecovery_.isEmpty())status_=qtTrId("library.recovery_unavailable");
    else if(!sessions_.isEmpty())status_=qtTrId("library.recovered");
    refresh();
}
}
