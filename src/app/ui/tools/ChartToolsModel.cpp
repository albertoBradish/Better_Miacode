#include "app/ui/tools/ChartToolsModel.h"
#include "app/services/UiRequestService.h"
#include "app/ui/preferences/LocaleService.h"
#include "core/chart/ChartBpmExport.h"
#include "core/chart/ChartBpmImport.h"
#include <QFile>
#include "core/chart/ChartScoreLoss.h"
#include "core/video/AssetPaths.h"
#include <QUrl>
#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QSaveFile>
#include <cmath>

namespace miacode::ui {
ChartToolsModel::ChartToolsModel(miacode::UiRequestService& requests, QObject* parent)
    : QObject(parent), requests_(&requests)
{
    connect(&miacode::LocaleService::instance(), &miacode::LocaleService::languageChanged,
        this, [this](const QString&) { emit changed(); });
}
QString ChartToolsModel::error() const
{
    if (error_ == QLatin1String("tools.chart.invalid_first")) return qtTrId("tools.chart.invalid_first");
    if (error_ == QLatin1String("tools.chart.parse_error")) return qtTrId("tools.chart.parse_error");
    return error_.isEmpty() ? saveError_ : error_;
}
void ChartToolsModel::setChart(const QString& text, const miacode::simai::SimaiTimingMetadata& timing, const QString& first,
    int difficulty, quint64 revision)
{
    if (initialized_ && (difficulty_ != difficulty || revision_ != revision) && !importText_.isEmpty()) {
        importPreview_.clear();
        importError_ = QStringLiteral("tools.bpm.import_stale");
    }
    const bool identityChanged = difficulty_ != difficulty || revision_ != revision;
    difficulty_ = difficulty; revision_ = revision;
    if (identityChanged) emit changed();
    if (initialized_ && chartText_ == text && timing_ == timing && firstText_ == first) return;
    initialized_ = true;
    chartText_ = text;
    timing_ = timing;
    firstText_ = first;
    saveError_.clear();
    bool ok = true;
    first_ = first.trimmed().isEmpty() ? 0.0 : first.toDouble(&ok);
    error_ = (!ok || !std::isfinite(first_)) ? QStringLiteral("tools.chart.invalid_first") : QString();
    parsed_ = SimaiParser::parseForTimeline(text, timing);
    if (!parsed_.ok) error_ = QStringLiteral("tools.chart.parse_error");
    score_ = chart::calculateScoreLoss(chart::buildScoringEvents(parsed_.noteMarkers));
    emit changed();
}

QString ChartToolsModel::bpmImportSummary() const
{
    if (!importError_.isEmpty()) return qtTrId(qPrintable(importError_));
    if (importPreview_.isEmpty()) return {};
    QString summary = qtTrId("tools.bpm.import_summary").arg(importChanges_).arg(importNotes_);
    if (!importSubdivisionWarning_.isEmpty())
        summary += QLatin1Char('\n') + qtTrId("tools.bpm.import_subdivisions").arg(importSubdivisionWarning_);
    return summary;
}

void ChartToolsModel::setBpmImportText(const QString& text)
{
    importText_ = text;
    importPreview_.clear(); importError_.clear(); importSubdivisionWarning_.clear();
    importDifficulty_ = difficulty_; importRevision_ = revision_;
    if (!text.trimmed().isEmpty() && !error_.isEmpty()) importError_ = error_;
    else if (!text.trimmed().isEmpty()) {
        const auto result = chart::importChartBpm(text, chartText_, timing_, first_);
        importPreview_ = result.chart; importError_ = result.error;
        importChanges_ = result.changes; importNotes_ = result.notes;
        importSubdivisionWarning_ = result.nonStandardSubdivisions.join(QStringLiteral(", "));
    }
    emit changed();
}

void ChartToolsModel::loadBpm()
{
    FileRequest request;
    request.title = qtTrId("tools.bpm.import");
    request.nameFilters = {QStringLiteral("BPM / maidata (*.txt)")};
    QPointer<ChartToolsModel> guard(this);
    const quint64 revision = revision_;
    const int difficulty = difficulty_;
    requests_->requestFile(request, [guard, revision, difficulty](const QString& path) {
        if (!guard || path.isEmpty()) return;
        if (guard->revision_ != revision || guard->difficulty_ != difficulty) {
            guard->importError_ = QStringLiteral("tools.bpm.import_stale");
            guard->importPreview_.clear(); emit guard->changed(); return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
            guard->importError_ = QStringLiteral("tools.bpm.import_invalid");
            guard->importPreview_.clear(); emit guard->changed(); return;
        }
        guard->setBpmImportText(QString::fromUtf8(file.readAll()));
    });
}
void ChartToolsModel::pasteBpm() { setBpmImportText(QGuiApplication::clipboard()->text()); }
void ChartToolsModel::applyBpm()
{
    if (canApplyBpm()) emit applyBpmRequested(chartText_, importPreview_, importDifficulty_, importRevision_);
}
void ChartToolsModel::completeBpmApply(bool success)
{
    importPreview_.clear();
    importError_ = success ? QStringLiteral("tools.bpm.import_applied") : QStringLiteral("tools.bpm.import_stale");
    importText_.clear(); emit changed();
}
QVariantList ChartToolsModel::objectCounts() const
{
    const auto& stats = score_;
    const QStringList names{QStringLiteral("Tap"), QStringLiteral("Hold"), QStringLiteral("Star"),
        QStringLiteral("Slide"), QStringLiteral("Touch"), QStringLiteral("TouchHold"),
        QStringLiteral("Break"), QStringLiteral("Ex"), QStringLiteral("Firework"), QStringLiteral("Mine"), QStringLiteral("Total")};
    const QStringList icons{QStringLiteral("tap.png"), QStringLiteral("hold.png"), QStringLiteral("star.png"),
        QStringLiteral("slide.png"), QStringLiteral("touch.png"), QStringLiteral("touchhold_border.png"),
        QStringLiteral("tap_break.png"), QStringLiteral("tap_ex.png"), QString(), QStringLiteral("tap_mine.png"), QString()};
    QVariantList rows;
    for (int index = 0; index < names.size(); ++index) {
        if (index == 9 && stats.mineCount == 0) continue;
        rows.append(QVariantMap{{QStringLiteral("name"), names[index]},
            {QStringLiteral("count"), index == 10 ? stats.total : (index == 9 ? stats.mineCount : stats.counts[index])},
            {QStringLiteral("attribute"), index >= 6 && index <= 9},
            {QStringLiteral("icon"), index == 8 ? QUrl(QStringLiteral("qrc:/preview/judge_effects/judge_effect_firework.png"))
                : icons[index].isEmpty() ? QUrl() : QUrl::fromLocalFile(
                miacode::assets::assetPath(QStringLiteral("skin/skinDX/") + icons[index]))}});
    }
    return rows;
}
QVariantList ChartToolsModel::scoreLossRows() const
{
    QVariantList rows;
    if (!error_.isEmpty()) return rows;
    const auto& stats = score_;
    for (const auto& row : stats.rows)
        rows.append(QVariantMap{{QStringLiteral("object"), row.object}, {QStringLiteral("judgment"), row.judgment},
            {QStringLiteral("loss"), QString::number(row.lossPercent, 'f', 6) + QLatin1Char('%')},
            {QStringLiteral("sss"), row.maximumSss}, {QStringLiteral("sssPlus"), row.maximumSssPlus},
            {QStringLiteral("count"), row.quantity}});
    return rows;
}
bool ChartToolsModel::calculable() const
{
    return error_.isEmpty() && score_.calculable;
}
QString ChartToolsModel::calculationStatus() const
{
    if (!error_.isEmpty()) return error();
    const auto& stats = score_;
    if (stats.hasUnofficialExtensions) return qtTrId("tools.dx_loss.extensions");
    if (stats.total == 0) return qtTrId("tools.dx_loss.empty");
    return {};
}
QString ChartToolsModel::simpleLossSummary() const
{
    if (!calculable()) return {};
    return qtTrId("tools.dx_loss.simple_summary").arg(score_.simpleGreatSss)
        .arg(score_.simpleGreatSssPlus < 0 ? qtTrId("tools.dx_loss.unreachable")
                                         : QString::number(score_.simpleGreatSssPlus));
}
void ChartToolsModel::setBpmFormat(int format)
{
    format = qBound(0, format, 2);
    if (format_ == format) return;
    format_ = format;
    emit changed();
}
QString ChartToolsModel::bpmPreview() const
{
    if (!error_.isEmpty()) return {};
    return chart::exportChartBpm(parsed_, first_, static_cast<chart::BpmExportFormat>(format_));
}
void ChartToolsModel::copyBpm()
{
    if (!bpmPreview().isEmpty()) QGuiApplication::clipboard()->setText(bpmPreview());
}
void ChartToolsModel::saveBpm()
{
    const QString content = bpmPreview();
    if (content.isEmpty()) return;
    FileRequest request;
    request.title = qtTrId("tools.bpm.title");
    request.startPath = format_ == 2 ? QStringLiteral("maidata.txt") : QStringLiteral("bpm.txt");
    request.saveMode = true;
    request.nameFilters = {QStringLiteral("Text (*.txt)")};
    QPointer<ChartToolsModel> guard(this);
    requests_->requestFile(request, [guard, content](const QString& path) {
        if (!guard || path.isEmpty()) return;
        QSaveFile output(path);
        const QByteArray bytes = content.toUtf8();
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
            guard->saveError_ = qtTrId("tools.bpm.save_error").arg(output.errorString());
            emit guard->changed();
        } else if (!guard->saveError_.isEmpty()) {
            guard->saveError_.clear();
            emit guard->changed();
        }
    });
}
} // namespace miacode::ui
