#pragma once

#include <QStringList>
#include <QSet>
#include <atomic>
#include <functional>
#include <QJsonArray>
#include <QJsonObject>

namespace miacode::library {
bool isManagedLibrary(const QString& root);
QString createLibrary(const QString& parent, QString* error);

QString bundleFile(const QString& root);
struct BundleSong {
    QString path, pack, origin, id;
    QJsonObject record;
};
QList<BundleSong> bundleSongs(const QString& root, int* skipped = nullptr, bool fullMedia = false,
    const std::atomic_bool* canceled = nullptr, const std::function<void(int)>& progress = {},
    QSet<QString>* packs = nullptr);
bool isLibraryChartText(const QString& text);
QString playbackChartPath(const QString& root, const QJsonObject& record, const std::atomic_bool* canceled = nullptr);
QString editableChartPath(const QString& root, const QString& path, const QJsonObject& record);
bool registerSong(const QString& root, const QString& path, const QString& pack, bool owned = false);
bool registerSavedCharts(const QString& root, const QSet<QString>& paths);
bool unlinkSongs(const QString& root, const QSet<QString>& paths);
enum class CollectionRemoval { Removed, InUse, Failed };
CollectionRemoval removeCollection(const QString& root, const QString& pack, const QSet<QString>& protectedPaths);
QString safeFolderName(QString name);
QString exportLibrary(const QString& root, const QString& parent, const std::atomic_bool& canceled,
    const std::function<void(int)>& progress, QString* error);
struct ImportResult {
    int imported = 0;
    int existing = 0;
    int merged = 0;
    int conflicts = 0;
    int failed = 0;
    QString firstError;
};

ImportResult importChartFolders(const QString& root, const QStringList& folders, const std::atomic_bool& canceled,
    const QString& targetPack = {}, const QSet<QString>& protectedPaths = {}, bool consolidate = false,
    const std::function<void(int)>& progress = {});
}
