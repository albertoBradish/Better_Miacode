#include "app/ui/library/ChartLibraryModel.h"

#include "app/services/ChartNormalizationPreferences.h"
#include "app/services/ChartZipExport.h"
#include "app/services/ManagedChartLibrary.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/UiRequestService.h"
#include "core/chart/ChartAssetPaths.h"
#include "core/chart/ChartVersion.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/document/ChartClockCount.h"
#include "core/chart/parser/SimaiParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <optional>

namespace miacode::ui {
namespace {
QVariantMap metadataEdits(const SimaiDocument& doc)
{
    QVariantMap fields;
    bool fillBpm = metadata::fieldValue(doc, "wholebpm").trimmed().isEmpty();
    const bool fillVersion = metadata::fieldValue(doc, "version").trimmed().isEmpty();
    const auto timing = simai::buildTimingMetadata(doc);
    std::optional<chart_version::Version> latestVersion;
    for (const int slot : doc.difficultyIds()) {
        const auto& difficulty = *doc.difficulty(slot);
        if (difficulty.chart.trimmed().isEmpty()) continue;
        if (fillBpm) {
            const double bpm = chart_clock::firstBpmFromChart(difficulty.chart);
            if (bpm > 0.0) {
                fields.insert("wholebpm", QString::number(bpm, 'g', 15));
                fillBpm = false;
            }
        }
        if (fillVersion) {
            const auto parsed = SimaiParser::parseForTimeline(difficulty.chart, timing);
            if (parsed.ok && !parsed.noteMarkers.isEmpty()) {
                const auto version = chart_version::detect(doc, parsed.noteMarkers);
                if (!latestVersion || version.family > latestVersion->family
                    || (version.family == latestVersion->family && version.plus)) {
                    latestVersion = version;
                }
            }
        }
        const auto grade = difficulty.level.trimmed();
        if (!grade.isEmpty() && !grade.contains('.')) continue;
        const auto suffix = QString::number(slot);
        const auto constantKey = "level_constant_" + suffix;
        const auto constant = metadata::fieldValue(doc, constantKey).trimmed();
        const auto next = metadata::displayLevelForConstant(grade.isEmpty() ? constant : grade);
        if (next.isEmpty()) continue;
        if (!grade.isEmpty()) {
            // Keep two existing values intact when their decimal constants disagree.
            if (!constant.isEmpty() && constant.toDouble() != grade.toDouble()) continue;
            if (constant.isEmpty()) fields.insert(constantKey, grade);
        }
        fields.insert("lv_" + suffix, next);
    }
    if (latestVersion) fields.insert("version", latestVersion->name());
    return fields;
}

QString zipName(const QString& path, bool withoutPv)
{
    return withoutPv ? QFileInfo(path).absolutePath() + '/' + QFileInfo(path).completeBaseName() + "_noPV.zip" : path;
}

QString availableZip(const QDir& folder, const QString& stem)
{
    QString path = folder.filePath(stem + ".zip");
    for (int suffix = 2; QFileInfo::exists(path) || QFileInfo::exists(zipName(path, true)); ++suffix)
        path = folder.filePath(stem + " (" + QString::number(suffix) + ").zip");
    return path;
}
}

QList<ChartLibraryModel::BatchSong> ChartLibraryModel::selectedSongs() const
{
    QList<BatchSong> songs;
    songs.reserve(selected_.size());
    for (const auto& entry : entries_) {
        if (selected_.contains(entry.path))
            songs.append({entry.path, entry.title, entry.record});
    }
    return songs;
}

void ChartLibraryModel::prepareNormalization()
{
    prepareBatchChanges(false);
}

void ChartLibraryModel::prepareMetadataCleanup()
{
    prepareBatchChanges(true);
}

void ChartLibraryModel::prepareBatchChanges(bool metadataOnly)
{
    if (busy_ || selected_.isEmpty()) return;
    if (selected_.size() > 100) {
        status_ = qtTrId("library.batch_limit");
        emit changed();
        return;
    }
    const auto songs = selectedSongs();
    for (const auto& song : songs) {
        if (!pathOpen(song.path)) continue;
        status_ = qtTrId("library.close_editor_first");
        emit changed();
        return;
    }
    const auto settings = PreferenceDocument::loadPreferencesObject().value("app").toObject().value("preview").toObject();
    const auto options = app_preferences::chartNormalizationOptionsFromPreferences(
        settings, chart_transform::ChartNormalizationOptions{true, false, true});
    const auto generation = ++generation_;
    const auto canceled = cancel_ = std::make_shared<std::atomic_bool>(false);
    busy_ = true;
    prepared_.clear();
    status_ = qtTrId("library.preparing");
    progress_.begin(localizedText("library.title"), localizedText("library.preparing"), true);
    progress_.report(0, status_);
    emit changed();

    worker_ = QThread::create([this, songs, generation, canceled, metadataOnly, options] {
        QVector<library::Change> changes;
        qint64 total = 0;
        int processed = 0;
        QString error;
        for (const auto& song : songs) {
            if (canceled->load()) break;
            QByteArray bytes, edited;
            if (!library::withinRoot(QFileInfo(song.path).absolutePath(), song.path)
                || !library::read(song.path, &bytes, &error)) {
                if (error.isEmpty()) error = qtTrId("library.conflict");
                break;
            }
            total += bytes.size();
            if (total > 16 * 1024 * 1024) {
                error = qtTrId("library.batch_size_limit");
                break;
            }
            if (!metadataOnly) {
                if (!library::normalize(bytes, &edited, &error, options)) break;
            } else {
                QString text;
                if (!library::decode(bytes, &text, &error)) break;
                const auto fields = metadataEdits(SimaiDocument::fromText(text));
                if (!fields.isEmpty() && !patchFields(text, fields, error)) break;
                edited = fields.isEmpty() ? bytes : library::encode(text, bytes);
            }
            if (edited != bytes) changes.append({song.path, bytes, edited, {}});
            reportProgress(100 * ++processed / songs.size());
        }
        QMetaObject::invokeMethod(this, [this, generation, canceled, changes = std::move(changes), error]() mutable {
            if (generation != generation_) return;
            finishThread();
            if (canceled->load()) status_ = qtTrId("library.canceled");
            else if (!error.isEmpty()) {
                status_ = error;
                requests_->postNotice(NoticeSeverity::Warning, localizedText("library.title"), error);
            } else {
                prepared_ = std::move(changes);
                status_ = qtTrId("library.prepared").arg(prepared_.size());
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
    worker_->start();
}

void ChartLibraryModel::chooseSelectedZipOutput(const QVariantMap& options)
{
    if (busy_ || selected_.isEmpty()) return;
    FileRequest request;
    request.selectFolder = true;
    request.title = qtTrId("library.batch_zip");
    QPointer<ChartLibraryModel> guard(this);
    requests_->requestFile(request, [guard, generation = generation_, songs = selectedSongs(), options](const QString& output) {
        if (!guard || output.isEmpty() || guard->busy_ || generation != guard->generation_) return;
        if (songs.isEmpty()) return;
        const auto root = guard->root_;
        const auto active = ++guard->generation_;
        const auto canceled = guard->cancel_ = std::make_shared<std::atomic_bool>(false);
        guard->busy_ = true;
        guard->status_ = qtTrId("library.batch_zip");
        guard->progress_.begin(localizedText("library.title"), localizedText("library.batch_zip"), true);
        guard->progress_.report(0, guard->status_);
        emit guard->changed();

        guard->worker_ = QThread::create([guard, songs, output, root, active, canceled, options] {
            int success = 0, failed = 0, processed = 0;
            QString firstError;
            const QDir destination(output);
            for (const auto& song : songs) {
                if (canceled->load()) break;
                const auto source = library::playbackChartPath(root, song.record, canceled.get());
                const auto target = availableZip(destination, zip_export::sanitizedZipStem(song.title));
                const QTemporaryDir stage(destination.filePath(".miacode-zip-XXXXXX"));
                const auto stagedZip = QDir(stage.path()).filePath(QFileInfo(target).fileName());
                QByteArray bytes;
                QString text, error;
                bool published = false;
                if (stage.isValid() && !source.isEmpty()
                    && library::read(source, &bytes, &error) && library::decode(bytes, &text, &error)) {
                    const auto doc = SimaiDocument::fromText(text);
                    const zip_export::ChartZipInput input{text, source, doc.videoPath, stagedZip};
                    const auto result = library::exportChartZip(input, options, [canceled](int, int, const QString&) {
                        return !canceled->load();
                    });
                    error = result.errorMessage;
                    if (result.ok && !canceled->load() && QFile::rename(stagedZip, target)) {
                        published = result.companionPath.isEmpty()
                            || QFile::rename(result.companionPath, zipName(target, true));
                        if (!published) QFile::remove(target);
                    }
                }
                if (published) ++success;
                else if (!canceled->load()) {
                    ++failed;
                    if (firstError.isEmpty()) {
                        firstError = song.title + " · " + (error.isEmpty() ? qtTrId("library.batch_zip_failed") : error);
                    }
                }
                if (canceled->load()) break;
                guard->reportProgress(100 * ++processed / songs.size());
            }
            QMetaObject::invokeMethod(guard, [guard, active, success, failed, canceled, firstError] {
                if (!guard || active != guard->generation_) return;
                guard->finishThread();
                guard->status_ = canceled->load() ? qtTrId("library.batch_zip_canceled").arg(success)
                    : qtTrId("library.batch_zip_done").arg(success).arg(failed);
                if (!firstError.isEmpty()) guard->status_ += QLatin1Char('\n') + firstError;
                emit guard->changed();
            }, Qt::QueuedConnection);
        });
        guard->worker_->start();
    });
}

void ChartLibraryModel::queueSelectedForRender(const QVariantList& difficulties)
{
    if (busy_ || selected_.isEmpty()) return;
    QList<int> requested;
    for (const auto& difficulty : difficulties) {
        const int slot = difficulty.toInt();
        if (SimaiDocument::isDifficultyId(slot) && !requested.contains(slot)) requested.append(slot);
    }
    if (requested.isEmpty()) return;
    const auto songs = selectedSongs();
    const auto generation = ++generation_;
    const auto canceled = cancel_ = std::make_shared<std::atomic_bool>(false);
    busy_ = true;
    status_ = qtTrId("library.batch_queue");
    progress_.begin(localizedText("library.title"), localizedText("library.batch_queue"), true);
    progress_.report(0, status_);
    emit changed();
    worker_ = QThread::create([this, songs, requested, root = root_, generation, canceled] {
        QVariantMap charts;
        int processed = 0, skipped = 0;
        for (const auto& song : songs) {
            if (canceled->load()) break;
            const auto path = library::playbackChartPath(root, song.record, canceled.get());
            QByteArray bytes;
            QString text;
            QVariantList matched;
            if (!path.isEmpty() && !chart_assets::resolveTrackPath(path).isEmpty()
                && library::read(path, &bytes, nullptr) && library::decode(bytes, &text, nullptr)) {
                const auto doc = SimaiDocument::fromText(text);
                for (const int slot : requested) {
                    const auto* difficulty = doc.difficulty(slot);
                    if (difficulty && !difficulty->chart.trimmed().isEmpty()) matched.append(slot);
                }
            }
            if (!matched.isEmpty()) charts.insert(path, matched);
            skipped += requested.size() - matched.size();
            reportProgress(100 * ++processed / songs.size());
        }
        QMetaObject::invokeMethod(this, [this, generation, canceled, charts, skipped] {
            if (generation != generation_) return;
            finishThread();
            if (canceled->load()) status_ = qtTrId("library.canceled");
            else {
                status_ = qtTrId("library.batch_queue_done").arg(charts.size()).arg(skipped);
                if (!charts.isEmpty()) emit renderQueuePrepared(charts);
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
    worker_->start();
}
}
