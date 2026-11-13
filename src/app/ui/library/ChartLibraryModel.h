#pragma once
#include "app/services/ChartLibraryFiles.h"
#include "app/services/JobProgressService.h"
#include <QAbstractListModel>
#include <QJsonArray>
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
    enum Role { PathRole=Qt::UserRole+1, TitleRole, ArtistRole, LevelsRole, VersionRole, DigestRole, DuplicateRole, SelectedRole };
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
    Q_INVOKABLE void chooseRoot();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void selectPath(const QString& path, bool selected);
    Q_INVOKABLE void selectVisible(bool selected);
    Q_INVOKABLE void openBuffer(const QString& path);
    Q_INVOKABLE void activateBuffer(int index);
    Q_INVOKABLE void closeActive();
    Q_INVOKABLE void saveActive();
    Q_INVOKABLE bool checkpoint();
    Q_INVOKABLE void openInEditor(const QString& path);
    Q_INVOKABLE void prepareNormalization();
    Q_INVOKABLE void applyPrepared();
    Q_INVOKABLE void undoBatch();
    // Explicit API for deterministic non-GUI tests; choosing a root never writes it.
    bool setRoot(const QString& path);
signals:
    void changed();
private:
    struct Entry { QString path,title,artist,levels,version,hash,search; qint64 size=0,modified=0; int copies=1; };
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
    QString root_,query_,status_;
    bool duplicatesOnly_=false,busy_=false,recoveryFailed_=false;
    quint64 generation_=0;
    QVector<Entry> entries_;
    QVector<int> visible_;
    QSet<QString> selected_;
    QVector<library::Change> prepared_,undo_;
    QList<LibraryDocument*> sessions_;
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
