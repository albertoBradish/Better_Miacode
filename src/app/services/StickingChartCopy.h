#pragma once

#include "app/services/StickingPublicationMetadata.h"
#include "app/services/HandOrderFeaturePreferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringDecoder>

namespace miacode::sticking {

inline constexpr auto chartFileName = "maidata_sticking.txt";

// Local project opens always edit a byte-for-byte copy of maidata.txt.
// An existing Sticking chart is authoritative and is never replaced here.
inline QString editableChartPath(const QString& requestedPath)
{
    if (requestedPath.isEmpty()) return {};
    const QFileInfo requested(requestedPath);
    QString sourcePath = requested.absoluteFilePath();
    if (!miacode::hand_order_features::enabled())
        return requested.isDir() ? QDir(sourcePath).filePath(QStringLiteral("maidata.txt")) : sourcePath;
    if (requested.isDir()) {
        const QDir directory(sourcePath);
        const QString copyPath = directory.filePath(QLatin1String(chartFileName));
        if (QFileInfo(copyPath).isFile()) return copyPath;
        sourcePath = directory.filePath(QStringLiteral("maidata.txt"));
    }
    const QFileInfo source(sourcePath);
    if (source.fileName().compare(QStringLiteral("maidata.txt"), Qt::CaseInsensitive) != 0)
        return sourcePath;
    const QString copyPath = source.dir().filePath(QLatin1String(chartFileName));
    if (QFileInfo::exists(copyPath)) return QFileInfo(copyPath).isFile() ? copyPath : QString();
    if (!source.isFile()) return {};
    if (!QFile::copy(sourcePath, copyPath))
        return QFileInfo(copyPath).isFile() ? copyPath : QString();
    QFile copy(copyPath);
    if (!copy.open(QIODevice::ReadOnly)) {
        QFile::remove(copyPath);
        return {};
    }
    const QByteArray bytes = copy.readAll();
    copy.close();
    QStringDecoder utf8(QStringConverter::Utf8);
    QString text = utf8.decode(bytes);
    if (utf8.hasError()) {
        QStringDecoder system(QStringConverter::System);
        text = system.decode(bytes);
    }
    const SimaiDocument document = SimaiDocument::fromText(text);
    QStringList createdInfoPaths;
    for (const int id : document.difficultyIds()) {
        const auto* difficulty = document.difficulty(id);
        if (!difficulty || difficulty->chart.trimmed().isEmpty()) continue;
        const QString infoPath = source.dir().filePath(publicationInfoName(document, id));
        if (QFileInfo(infoPath).isFile()) continue;
        QFile info(infoPath);
        const QByteArray content = publicationInfoText(document, id).toUtf8();
        const bool opened = info.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        const bool written = opened && info.write(content) == content.size() && info.flush();
        info.close();
        if (!written) {
            if (opened) QFile::remove(infoPath);
            for (const auto& createdPath : createdInfoPaths) QFile::remove(createdPath);
            QFile::remove(copyPath);
            return {};
        }
        createdInfoPaths.append(infoPath);
    }
    return copyPath;
}

} // namespace miacode::sticking
