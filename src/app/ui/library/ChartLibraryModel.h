#pragma once
#include "app/services/ChartLibraryFiles.h"
#include "app/services/JobProgressService.h"
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QSet>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>

namespace miacode { class UiRequestService; }
namespace miacode::ui {
class DocumentModel;
class LibraryDocument final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path CONSTANT)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed)
public:
    LibraryDocument(QString path, QByteArray bytes, QObject* parent);
    QString path() const { return path_; }
    QString text() const { return document_.toPlainText(); }
    bool dirty() const { return text() != savedText_; }
    bool canUndo() const { return document_.isUndoAvailable(); }
    bool canRedo() const { return document_.isRedoAvailable(); }
    quint64 revision() const { return revision_; }
    void setText(const QString& value);
    Q_INVOKABLE bool replaceText(const QString& value);
    QString error() const { return error_; }
    Q_INVOKABLE void undo() { document_.undo(); }
    Q_INVOKABLE void redo() { document_.redo(); }
    QByteArray original;
    QByteArray expectedHash;
    void markSaved(QByteArray bytes);
signals:
    void changed();
private:
    QString path_, savedText_, error_;
    QTextDocument document_;
    quint64 revision_ = 0;
    qint64 historyBytes_ = 0;
};

class ChartLibraryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QObject* progress READ progress CONSTANT)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY changed)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY changed)
    Q_PROPERTY(QStringList packs READ packs NOTIFY changed)
    Q_PROPERTY(QStringList versions READ versions NOTIFY changed)
    Q_PROPERTY(QStringList genres READ genres NOTIFY changed)
    Q_PROPERTY(QString pack READ pack WRITE setPack NOTIFY changed)
    Q_PROPERTY(QString versionCategory READ versionCategory WRITE setVersionCategory NOTIFY changed)
    Q_PROPERTY(QString genre READ genre WRITE setGenre NOTIFY changed)
    Q_PROPERTY(bool duplicatesOnly READ duplicatesOnly WRITE setDuplicatesOnly NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY changed)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantList proposals READ proposals NOTIFY changed)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY changed)
    Q_PROPERTY(QObject* activeSession READ activeSession NOTIFY changed)
    Q_PROPERTY(bool canUndoBatch READ canUndoBatch NOTIFY changed)
public:
    enum Role { PathRole=Qt::UserRole+1, TitleRole, ArtistRole, LevelsRole, VersionRole, DigestRole, DuplicateRole, SelectedRole, PackRole, GenreRole, SongIdRole, KindRole, JacketRole, BadgesRole };
    ChartLibraryModel(miacode::UiRequestService& requests, DocumentModel& document, QObject* parent=nullptr);
    ~ChartLibraryModel() override;
    int rowCount(const QModelIndex& parent={}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    QObject* progress() { return &progress_; }
    QString rootPath() const { return root_; }
    QString query() const { return query_; }
    void setQuery(const QString& text);
    bool duplicatesOnly() const { return duplicatesOnly_; }
    void setDuplicatesOnly(bool value);
    bool busy() const { return busy_; }
    int totalCount() const { return entries_.size(); }
    int selectedCount() const { return selected_.size(); }
    QString status() const { return status_; }
    QVariantList proposals() const;
    QVariantList sessions() const;
    QObject* activeSession() const { return active_; }
    bool canUndoBatch() const { return !undo_.isEmpty() && !busy_; }
    QStringList packs() const { return packs_; }
    QStringList versions() const { return versions_; }
    QStringList genres() const { return genres_; }
    QString pack() const { return pack_; }
    QString genre() const { return genre_; }
    void setPack(const QString& value);
    void setGenre(const QString& value);
    QString versionCategory() const { return versionCategory_; }
    void setVersionCategory(const QString& value);
    Q_INVOKABLE void chooseRoot();
    Q_INVOKABLE void chooseExport();
    void reportProgress(int percent);
    bool writeChartChanges(QVector<library::Change>* changes, QString* error);
    Q_INVOKABLE void chooseNewLibrary();
    Q_INVOKABLE void chooseImportFolders(const QString& targetPack = {}, bool archives = false, bool manualSong = false);
    Q_INVOKABLE void consolidateSongs();
    void importFolders(const QStringList& folders, const QString& targetPack = {}, bool manualSong = false);
    void runImport(const QStringList& folders, const QString& targetPack, bool manualSong, bool consolidate);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void selectPath(const QString& path, bool selected);
    Q_INVOKABLE void selectVisible(bool selected);
    Q_INVOKABLE QString chartCaption(const QString& path) const;
    Q_INVOKABLE bool hasSong(const QString& path) const { return entryFor(path) != nullptr; }
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE int rowForPath(const QString& path) const;
    Q_INVOKABLE QVariantMap songDetails(const QString& path);
    Q_INVOKABLE QString stageSongFields(const QString& path, const QString& stamp, const QVariantMap& changes);
    Q_INVOKABLE void discardSongEdits(const QString& path);
    Q_INVOKABLE bool saveSong(const QString& path);
    Q_INVOKABLE void requestNoteCount(const QString& path, int difficulty);
    Q_INVOKABLE QString createSong(const QString& pack, const QString& title, int difficulty);
    Q_INVOKABLE void changeCategory(const QString& key, const QString& from, const QString& to);
    Q_INVOKABLE int categoryUsage(const QString& key, const QString& name) const;
    Q_INVOKABLE void chooseSongAudio(const QString& path);
    void replaceSongAudio(const QString& path, const QString& source);
    Q_INVOKABLE void removeSong(const QString& path);
    Q_INVOKABLE void openBuffer(const QString& path);
    Q_INVOKABLE void activateBuffer(int index);
    Q_INVOKABLE void closeActive();
    Q_INVOKABLE void saveActive();
    Q_INVOKABLE bool checkpoint();
    Q_INVOKABLE bool additionalOpenModifierHeld() const;
    Q_INVOKABLE void openFromList(const QString& path);
    Q_INVOKABLE void openInEditor(const QString& path, int difficulty = 0);
    Q_INVOKABLE void prepareNormalization();
    Q_INVOKABLE void applyPrepared();
    Q_INVOKABLE void undoBatch();
    // Explicit API for deterministic non-GUI tests; choosing a root never writes it.
    bool setRoot(const QString& path);
signals:
    void changed();
    void editorOpened();
    void songAudioReady(const QString& path);
    void noteCountReady(const QString& path, int difficulty, int notes);
private:
    LibraryDocument* bufferFor(const QString& path) const;
    struct Entry { QString path,title,artist,levels,version,hash,search; qint64 size=0,modified=0; int copies=1; QString pack,genre,songId,kind,versionCategory; QJsonObject record;
        void updateSearch(); };
    const Entry* entryFor(const QString& path) const;
    QStringList catalogNames(const QString& key) const;
    QString songText(const QString& path, QString* error) const;
    void rebuildFilter(QVector<Entry>* replacement = nullptr);
    void finishThread();
    void saveRecovery();
    bool writeRecovery();
    void restoreRecovery();
    void persistIndex();
    QString cachePath() const;
    QString journalPath() const;
    bool pathOpen(const QString& path) const;
    bool protectedChanges(const QVector<library::Change>& changes) const;
    void requestSave(LibraryDocument* session);
    miacode::UiRequestService* requests_;
    QPointer<DocumentModel> document_;
    QString root_,query_,status_,pack_,genre_,versionCategory_,importStatus_,deferredRoot_;
    QStringList packs_,genres_,versions_;
    quint64 countRequest_ = 0;
    bool duplicatesOnly_=false,busy_=false,recoveryFailed_=false;
    bool importInProgress_ = false;
    quint64 generation_=0;
    QVector<Entry> entries_;
    QVector<int> visible_;
    QSet<QString> selected_, recoveryPaths_;
    QVector<library::Change> prepared_,undo_;
    QList<LibraryDocument*> sessions_;
    QSet<LibraryDocument*> formDrafts_;
    QJsonArray unavailableRecovery_;
    QPointer<LibraryDocument> active_;
    QThread* worker_=nullptr;
    std::shared_ptr<std::atomic_bool> cancel_;
    // Background indexing owns its progress rather than blocking the main editor.
    miacode::JobProgressService progress_{this};
    QFileSystemWatcher watcher_;
    QTimer refreshTimer_,recoveryTimer_;
};
}
