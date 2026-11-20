#pragma once
#include "app/services/ManagedChartLibrary.h"
#include <QHash>
#include <QSet>

namespace miacode::library {
enum class ImportMatch { NewSong, Existing, Merged, Conflict, Protected, Failed };

QString songSignature(const QString& text);
ImportMatch compareCharts(const QString& existing, const QString& incoming, QString& combined, QString& detail);

struct ZipChart { QString member; QByteArray bytes; bool hasMedia=false; };
bool inspectChartZip(const QString& archive, const std::atomic_bool& canceled, QList<ZipChart>* charts,
                     const std::function<void(int)>& progress);
bool unpackChartZip(const QString& archive, const QString& directory, const std::atomic_bool& canceled, const QString& member, bool fullMedia);
}
