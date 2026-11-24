#include "app/services/LibraryImportIndex.h"
#include <miniz.h>
#include "app/services/ChartLibraryFiles.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/document/SimaiDocument.h"
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStorageInfo>

namespace miacode::library {
namespace {
    size_t readZip(void* opaque, mz_uint64 offset, void* bytes, size_t count)
    {
        auto* file = static_cast<QFile*>(opaque);
        if (!file->seek(qint64(offset)))
            return 0;
        return size_t(qMax<qint64>(0, file->read(static_cast<char*>(bytes), qint64(count))));
    }
    struct Output {
        QFile& file;
        const std::atomic_bool& canceled;
        mz_uint64 size;
    };
    size_t writeMember(void* opaque, mz_uint64 offset, const void* bytes, size_t count)
    {
        auto& out = *static_cast<Output*>(opaque);
        if (out.canceled.load() || offset > out.size || count > out.size - offset || out.file.pos() != qint64(offset))
            return 0;
        return size_t(qMax<qint64>(0, out.file.write(static_cast<const char*>(bytes), qint64(count))));
    }

    bool safeName(const QString& name)
    {
        if (name.isEmpty() || name.contains(QChar::ReplacementCharacter) || name.contains(QChar::Null)
            || name.contains(':') || QDir::isAbsolutePath(name))
            return false;
        for (const auto& part : name.split('/')) {
            if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' '))
                return false;
            for (const auto ch : part)
                if (ch.unicode() < 32 || QStringLiteral("<>\"|?*").contains(ch))
                    return false;
            const auto stem = part.section('.', 0, 0).toUpper();
            if (stem == "CONIN$" || stem == "CONOUT$" || stem == "CON" || stem == "PRN" || stem == "AUX"
                || stem == "NUL"
                || (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem.back().isDigit()))
                return false;
        }
        return true;
    }
}

static bool visitZip(const QString& archive, const std::atomic_bool& canceled,
    const std::function<bool(mz_zip_archive&, mz_uint, const QString&, mz_uint64)>& visit)
{
    QFile source(archive);
    if (!source.open(QIODevice::ReadOnly))
        return false;
    mz_zip_archive zip {};
    zip.m_pRead = readZip;
    zip.m_pIO_opaque = &source;
    if (!mz_zip_reader_init(&zip, mz_uint64(source.size()), 0))
        return false;
    struct End {
        mz_zip_archive& zip;
        ~End() { mz_zip_reader_end(&zip); }
    } end { zip };
    const auto count = mz_zip_reader_get_num_files(&zip);
    mz_uint64 remaining = 16ULL * 1024 * 1024 * 1024;
    if (count > 100000)
        return false;
    QSet<QString> seen;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat {};
        if (canceled.load() || !mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_encrypted
            || ((stat.m_external_attr >> 16) & 0170000) == 0120000 || stat.m_uncomp_size > remaining)
            return false;
        const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (!length || length > 4096)
            return false;
        QByteArray encoded(int(length), Qt::Uninitialized);
        if (mz_zip_reader_get_filename(&zip, i, encoded.data(), length) != length)
            return false;
        encoded.chop(1);
        QString name = QString::fromUtf8(encoded).replace('\\', '/');
        while (name.startsWith("./"))
            name.remove(0, 2);
        if (stat.m_is_directory && (name.isEmpty() || name == "."))
            continue;
        if (stat.m_is_directory && name.endsWith('/'))
            name.chop(1);
        if (!safeName(name) || seen.contains(name.toCaseFolded()))
            return false;
        seen.insert(name.toCaseFolded());
        if (stat.m_is_directory)
            continue;
        remaining -= stat.m_uncomp_size;
        bool ignored = false;
        for (const auto& part : name.split('/')) {
            if (part.startsWith('.') || part.startsWith(QStringLiteral("转谱核验"))) {
                ignored = true;
                break;
            }
        }
        if (!ignored && !visit(zip, i, name, stat.m_uncomp_size))
            return false;
    }
    return !canceled.load();
}
bool inspectChartZip(const QString& archive, const std::atomic_bool& canceled, QList<ZipChart>* charts,
    const std::function<void(int)>& progress)
{
    QList<ZipChart> found;
    int last = -1;
    const bool ok
        = visitZip(archive, canceled, [&](mz_zip_archive& zip, mz_uint i, const QString& name, mz_uint64 size) {
              const int pct = int(100ULL * i / qMax(1u, mz_zip_reader_get_num_files(&zip)));
              if (progress && pct != last) {
                  progress(pct);
                  last = pct;
              }
              if (size > kMaximumFileBytes
                  || !chart_assets::supportedChartFileExtensions().contains(QFileInfo(name).suffix().toLower()))
                  return true;
              QByteArray bytes(qsizetype(size), Qt::Uninitialized);
              if (size && !mz_zip_reader_extract_to_mem(&zip, i, bytes.data(), size_t(size), 0))
                  return false;
              QString text;
              if (bytes.contains("&inote_") && decode(bytes, &text, nullptr) && isLibraryChartText(text))
                  found.append({ name, std::move(bytes) });
              return true;
          });
    if (!ok)
        return false;
    *charts = std::move(found);
    return true;
}
bool unpackChartZip(const QString& archive, const QString& directory, const std::atomic_bool& canceled,
    const QString& member, bool fullMedia)
{
    const QDir target(directory);
    const auto prefix = member.left(member.lastIndexOf('/') + 1);
    QSet<QString> wanted { member };
    QByteArray chart;
    // Read just the document first so an explicit relative PV path can be included.
    if (!visitZip(archive, canceled,
            [&](mz_zip_archive& zip, mz_uint i, const QString& name, mz_uint64 size) {
                if (name != member)
                    return true;
                if (size > kMaximumFileBytes)
                    return false;
                chart.resize(qsizetype(size));
                return size && mz_zip_reader_extract_to_mem(&zip, i, chart.data(), size_t(size), 0);
            })
        || chart.isEmpty())
        return false;
    for (const auto& name : (fullMedia ? chart_assets::trackCandidateFileNames() : QStringList {})
            + chart_assets::backgroundMediaCandidateFileNames(fullMedia))
        wanted.insert(prefix + name);
    const auto video
        = QDir::cleanPath(QDir::fromNativeSeparators(SimaiDocument::fromText(QString::fromUtf8(chart)).videoPath));
    if (fullMedia && safeName(video))
        wanted.insert(prefix + video);
    return visitZip(archive, canceled, [&](mz_zip_archive& zip, mz_uint i, const QString& name, mz_uint64 size) {
        if (!wanted.contains(name))
            return true;
        const QString path = target.filePath(name);
        QFile file(path);
        if (!target.mkpath(QFileInfo(name).path()) || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return false;
        Output output { file, canceled, size };
        return mz_zip_reader_extract_to_callback(&zip, i, writeMember, &output, 0) && file.flush()
            && mz_uint64(file.size()) == size;
    });
}
}
