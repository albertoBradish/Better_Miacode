#include "app/ui/library/ChartLibraryModel.h"
#include "app/ui/document/DocumentModel.h"
#include "app/services/UiRequestService.h"
#include "common/RuntimePaths.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "core/chart/document/SimaiDocument.h"
#include <QDateTime>
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
    case PathRole:return e.path; case TitleRole:return e.title; case ArtistRole:return e.artist;
    case LevelsRole:return e.levels; case VersionRole:return e.version; case DigestRole:return e.hash;
    case DuplicateRole:return e.copies; case SelectedRole:return selected_.contains(e.path); default:return {};
    }
}
QHash<int,QByteArray> ChartLibraryModel::roleNames() const
{ return {{PathRole,"chartPath"},{TitleRole,"chartTitle"},{ArtistRole,"artist"},{LevelsRole,"levels"},
    {VersionRole,"chartVersion"},{DigestRole,"sha256"},{DuplicateRole,"copies"},{SelectedRole,"selected"}}; }
void ChartLibraryModel::rebuildFilter(QVector<Entry>* replacement)
{
    beginResetModel(); if(replacement)entries_.swap(*replacement); visible_.clear(); const auto words=query_.toCaseFolded().split(QLatin1Char(' '),Qt::SkipEmptyParts);
    for(int i=0;i<entries_.size();++i) {
        const auto& e=entries_[i]; bool keep=!duplicatesOnly_ || e.copies>1;
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
    miacode::FileRequest request;request.selectFolder=true;request.title=qtTrId("library.choose_root");request.startPath=root_;
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request,[guard](const QString& path){if(guard&&!path.isEmpty())guard->setRoot(path);});
}
bool ChartLibraryModel::setRoot(const QString& path)
{
    if(busy_)return false;
    const QFileInfo dir(path);
    if(!dir.isDir()||dir.isSymLink()){status_=qtTrId("library.invalid_root");emit changed();return false;}
    const QString canonical=dir.canonicalFilePath();
    if(canonical.isEmpty())return false;
    if(root_!=canonical && !sessions_.isEmpty()){status_=qtTrId("library.close_sessions_first");emit changed();return false;}
    if(root_!=canonical && !unavailableRecovery_.isEmpty()) {
        const QString archive=QDir(storageDirectory("sessions")).filePath("library-unavailable-"+QUuid::createUuid().toString(QUuid::Id128)+".json");
        QString error;
        if(!saveJson(archive,{{"schema",1},{"root",root_},{"sessions",unavailableRecovery_}},&error)) {
            status_=qtTrId("library.recovery_failed").arg(error);emit changed();return false;
        }
        unavailableRecovery_={};
        requests_->postNotice(NoticeSeverity::Information,qtTrId("library.title"),qtTrId("library.recovery_archived").arg(archive));
    }
    root_=canonical;selected_.clear();prepared_.clear();undo_.clear();QVector<Entry> restored;
    const auto cached=loadJson(cachePath());
    if(cached.value("schema").toInt()==1 && cached.value("root").toString()==root_) {
        for(const auto& value:cached.value("entries").toArray()){
            auto o=value.toObject();Entry e{ o["path"].toString(),o["title"].toString(),o["artist"].toString(),
                o["levels"].toString(),o["version"].toString(),o["hash"].toString(),{},o["size"].toVariant().toLongLong(),o["modified"].toVariant().toLongLong(),1};
            e.search=(e.path+" "+e.title+" "+e.artist+" "+e.levels+" "+e.version).toCaseFolded();
            if(library::withinRoot(root_,e.path))restored.append(e);
        }
    }
    rebuildFilter(&restored); refresh(); return true;
}
void ChartLibraryModel::finishThread()
{ if(worker_){worker_->wait();delete worker_;worker_=nullptr;}progress_.end();busy_=false;emit changed(); }
void ChartLibraryModel::cancel(){if(cancel_)cancel_->store(true);}
void ChartLibraryModel::refresh()
{
    if(busy_||root_.isEmpty())return;
    if(!QFileInfo(root_).isDir()){status_=qtTrId("library.recovery_unavailable");emit changed();return;}
    busy_=true;status_=qtTrId("library.scanning");prepared_.clear();emit changed();
    const auto root=root_; const auto generation=++generation_;cancel_=std::make_shared<std::atomic_bool>(false);
    progress_.begin(localizedText("library.title"),localizedText("library.scanning"),true);
    progress_.reportIndeterminate(localizedText("library.scanning"));
    const auto canceled=cancel_; QHash<QString,Entry> cached;for(const auto& e:entries_)cached.insert(e.path,e);
    worker_=QThread::create([this,root,generation,canceled,cached]{
        QVector<Entry> found;QHash<QString,int> hashes;int skipped=0;qint64 indexBytes=0;
        QStringList watchedDirectories{root};
        QDirIterator directories(root,QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDirIterator::Subdirectories);
        while(directories.hasNext() && watchedDirectories.size()<512 && !canceled->load()) watchedDirectories.append(directories.next());
        QDirIterator it(root,{"maidata*.txt"},QDir::Files|QDir::NoSymLinks,QDirIterator::Subdirectories);
        while(it.hasNext()&&!canceled->load()){
            const QString path=it.next();const QFileInfo info(path);
            if(found.size()>=100000){++skipped;break;}
            if(!library::withinRoot(root,path)){++skipped;continue;}
            Entry e=cached.value(path);
            if(e.path.isEmpty()||e.size!=info.size()||e.modified!=info.lastModified().toMSecsSinceEpoch()){
                QByteArray bytes;QString text,error;
                if(!library::read(path,&bytes,&error)||!library::decode(bytes,&text,&error)){++skipped;continue;}
                const auto doc=SimaiDocument::fromText(text);QStringList levels;
                for(int id:doc.difficultyIds())if(const auto* d=doc.difficulty(id))levels.append(QString::number(id)+":"+d->level);
                QString version;for(const auto& f:SimaiDocument::parseRawFields(text))if(f.key=="version")version=f.value.trimmed();
                e={path,doc.title.left(512),doc.artist.left(512),levels.join(" / ").left(256),version.left(256),QString::fromLatin1(library::digest(bytes).toHex()),{},info.size(),info.lastModified().toMSecsSinceEpoch(),1};
                e.search=(e.path+" "+e.title+" "+e.artist+" "+e.levels+" "+e.version).toCaseFolded();
            }
            indexBytes += 2 * (e.path.size()+e.title.size()+e.artist.size()+e.levels.size()+e.version.size()+e.hash.size()+e.search.size()) + 128;
            if(indexBytes>64*1024*1024){++skipped;break;}
            ++hashes[e.hash];found.append(e);
        }
        for(auto& e:found)e.copies=hashes[e.hash];
        std::sort(found.begin(),found.end(),[](const Entry& a,const Entry& b){return a.path<b.path;});
        QMetaObject::invokeMethod(this,[this,generation,canceled,found=std::move(found),watchedDirectories,skipped]()mutable{
            if(generation!=generation_)return;
            finishThread();
            if(canceled->load()){status_=qtTrId("library.canceled");emit changed();return;}
            QSet<QString> existing;for(const auto& e:found)existing.insert(e.path);
            selected_.intersect(existing);rebuildFilter(&found);persistIndex();
            watcher_.removePaths(watcher_.directories());watcher_.removePaths(watcher_.files());
            watcher_.addPaths(watchedDirectories);
            QStringList watchedFiles;for(const auto& e:entries_){if(watchedFiles.size()>=2048)break;watchedFiles.append(e.path);}
            if(!watchedFiles.isEmpty())watcher_.addPaths(watchedFiles);
            status_=qtTrId("library.indexed").arg(entries_.size()).arg(skipped);emit changed();
        },Qt::QueuedConnection);
    });worker_->start();
}
void ChartLibraryModel::persistIndex()
{
    QJsonArray rows;for(const auto&e:entries_)rows.append(QJsonObject{{"path",e.path},{"title",e.title},{"artist",e.artist},{"levels",e.levels},{"version",e.version},{"hash",e.hash},{"size",e.size},{"modified",e.modified}});
    saveJson(cachePath(),{{"schema",1},{"root",root_},{"entries",rows}});recoveryTimer_.start();
}
void ChartLibraryModel::selectPath(const QString& path,bool value)
{
    if(busy_)return;
    if(value)selected_.insert(path);else selected_.remove(path);
    prepared_.clear();if(rowCount())emit dataChanged(index(0),index(rowCount()-1),{SelectedRole});emit changed();
}
void ChartLibraryModel::selectVisible(bool value)
{if(busy_)return;for(int i:visible_){if(value)selected_.insert(entries_[i].path);else selected_.remove(entries_[i].path);}prepared_.clear();if(rowCount())emit dataChanged(index(0),index(rowCount()-1),{SelectedRole});emit changed();}
QVariantList ChartLibraryModel::sessions()const{QVariantList result;for(auto* s:sessions_)result.append(QVariant::fromValue(static_cast<QObject*>(s)));return result;}
void ChartLibraryModel::openBuffer(const QString& path)
{
    for(auto* s:sessions_)if(s->path()==path){active_=s;emit changed();return;}
    if(sessions_.size()>=5){status_=qtTrId("library.session_limit");emit changed();return;}
    QByteArray bytes;QString text,error;
    if(!library::withinRoot(root_,path)||!library::read(path,&bytes,&error)||!library::decode(bytes,&text,&error)){
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
    const auto close=[this,session](bool yes){if(!session||!yes)return;sessions_.removeAll(session);if(active_==session)active_=sessions_.isEmpty()?nullptr:sessions_.first();delete session;saveRecovery();emit changed();};
    if(session->dirty()) requests_->requestConfirmation(qtTrId("library.close_buffer"),qtTrId("library.discard_prompt"),qtTrId("library.discard"),close);
    else close(true);
}
bool ChartLibraryModel::pathOpen(const QString& path)const
{for(auto*s:sessions_)if(s->path()==path)return true;return document_&&QFileInfo(document_->currentFilePath()).canonicalFilePath()==QFileInfo(path).canonicalFilePath();}
bool ChartLibraryModel::protectedChanges(const QVector<library::Change>& changes)const
{for(const auto&c:changes)if(pathOpen(c.path))return true;return false;}
void ChartLibraryModel::requestSave(LibraryDocument* session)
{
    if(!session||!session->dirty()||busy_)return;
    if(document_&&QFileInfo(document_->currentFilePath()).canonicalFilePath()==QFileInfo(session->path()).canonicalFilePath()){
        status_=qtTrId("library.close_editor_first");emit changed();return;
    }
    QByteArray bytes;QString error;
    if(!library::read(session->path(),&bytes,&error)||library::digest(bytes)!=session->expectedHash){status_=qtTrId("library.conflict");emit changed();return;}
    QVector<library::Change> change{{session->path(),bytes,library::encode(session->text(),session->original),{}}};
    if(library::apply(root_,&change,journalPath(),&error)){session->markSaved(change.first().after);status_=qtTrId("library.saved");saveRecovery();refresh();}
    else status_=error;
    emit changed();
}
void ChartLibraryModel::saveActive(){requestSave(active_);}
void ChartLibraryModel::openInEditor(const QString& path)
{
    if(!document_||!library::withinRoot(root_,path))return;
    for(auto*s:sessions_)if(s->path()==path&&s->dirty()){status_=qtTrId("library.save_buffer_first");emit changed();return;}
    QPointer<ChartLibraryModel> guard(this);
    document_->requestLeaveDocument([guard,path](bool yes){if(guard&&yes&&guard->document_)guard->document_->openFile(QUrl::fromLocalFile(path));});
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
            if(!library::withinRoot(root,path)||!library::read(path,&bytes,&error)){if(error.isEmpty())error="Unsafe path";break;}
            total+=bytes.size();if(total>16*1024*1024){error=QStringLiteral("Batch exceeds 16 MiB; select fewer charts.");break;}
            if(!library::normalize(bytes,&normalized,&error,options))break;
            if(normalized!=bytes)changes.append({path,bytes,normalized,{}});
        }
        QMetaObject::invokeMethod(this,[this,generation,canceled,changes=std::move(changes),error]()mutable{
            if(generation!=generation_)return;finishThread();
            if(canceled->load())status_=qtTrId("library.canceled");
            else if(!error.isEmpty())status_=error;
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
            if(library::apply(guard->root_,&transaction,guard->journalPath(),&error)){guard->undo_=transaction;guard->prepared_.clear();guard->refresh();}
            else{guard->status_=error;guard->prepared_.clear();emit guard->changed();}
        });
}
void ChartLibraryModel::undoBatch()
{
    if(!canUndoBatch())return;
    if(protectedChanges(undo_)){status_=qtTrId("library.close_editor_first");emit changed();return;}
    QString error;
    if(library::undo(root_,undo_,journalPath(),&error)){undo_.clear();refresh();}else{status_=error;emit changed();}
}
void ChartLibraryModel::saveRecovery() { writeRecovery(); }
bool ChartLibraryModel::checkpoint()
{
    recoveryTimer_.stop();
    if(writeRecovery())return true;
    return std::none_of(sessions_.cbegin(),sessions_.cend(),[](const LibraryDocument* session){return session->dirty();});
}
bool ChartLibraryModel::writeRecovery()
{
    QJsonArray sessions=unavailableRecovery_;for(auto*s:sessions_)sessions.append(QJsonObject{{"path",s->path()},{"text",s->text()},
        {"expected",QString::fromLatin1(s->expectedHash.toHex())},{"dirty",s->dirty()}});
    QString error;
    if(!saveJson(recoveryPath(),{{"schema",1},{"root",root_},{"sessions",sessions}},&error)) {
        recoveryFailed_=true;status_=qtTrId("library.recovery_failed").arg(error);emit changed();return false;
    }
    if(recoveryFailed_){recoveryFailed_=false;status_.clear();emit changed();}
    return true;
}
void ChartLibraryModel::restoreRecovery()
{
    const auto saved=loadJson(recoveryPath());if(saved.value("schema").toInt()!=1)return;
    root_=saved.value("root").toString();
    if(!QFileInfo(root_).isDir()){
        unavailableRecovery_=saved.value("sessions").toArray();
        if(!unavailableRecovery_.isEmpty())status_=qtTrId("library.recovery_unavailable");
        return;
    }
    for(const auto&value:saved.value("sessions").toArray()){
        const auto object=value.toObject();const auto path=object.value("path").toString();
        if(!library::withinRoot(root_,path)||sessions_.size()>=5){unavailableRecovery_.append(object);continue;}
        openBuffer(path);
        if(!active_||active_->path()!=path){unavailableRecovery_.append(object);continue;}
        if(object.value("dirty").toBool()){
            active_->expectedHash=QByteArray::fromHex(object.value("expected").toString().toLatin1());
            active_->setText(object.value("text").toString());
        }
    }
    if(!unavailableRecovery_.isEmpty())status_=qtTrId("library.recovery_unavailable");
    else if(!sessions_.isEmpty())status_=qtTrId("library.recovered");
}
}
