#include "app/ui/tools/ChartToolsModel.h"
#include "app/services/UiRequestService.h"
#include "app/ui/preferences/LocaleService.h"
#include "core/chart/ChartBpmExport.h"
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
void ChartToolsModel::setChart(const QString& text, const miacode::simai::SimaiTimingMetadata& timing, const QString& first)
{
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
    for (int index = 0; index < names.size(); ++index)
        rows.append(QVariantMap{{QStringLiteral("name"), names[index]},
            {QStringLiteral("count"), index == 10 ? stats.total : (index == 9 ? stats.mineCount : stats.counts[index])},
            {QStringLiteral("attribute"), index >= 6 && index <= 9},
            {QStringLiteral("icon"), index == 8 ? QUrl(QStringLiteral("qrc:/preview/judge_effects/judge_effect_firework.png"))
                : icons[index].isEmpty() ? QUrl() : QUrl::fromLocalFile(
                miacode::assets::assetPath(QStringLiteral("skin/skinDX/") + icons[index]))}});
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
    return stats.breakTotal ? qtTrId("tools.dx_loss.maximum_break")
        : qtTrId("tools.dx_loss.maximum_no_break");
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
