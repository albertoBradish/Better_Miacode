#pragma once
#include <QObject>
#include <QVariantList>
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/ChartScoreLoss.h"
namespace miacode { class UiRequestService; }
namespace miacode::ui {
class ChartToolsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList objectCounts READ objectCounts NOTIFY changed)
    Q_PROPERTY(QVariantList scoreLossRows READ scoreLossRows NOTIFY changed)
    Q_PROPERTY(bool calculable READ calculable NOTIFY changed)
    Q_PROPERTY(QString calculationStatus READ calculationStatus NOTIFY changed)
    Q_PROPERTY(int bpmFormat READ bpmFormat WRITE setBpmFormat NOTIFY changed)
    Q_PROPERTY(QString bpmPreview READ bpmPreview NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit ChartToolsModel(miacode::UiRequestService& requests, QObject* parent = nullptr);
    void setChart(const QString& text, const miacode::simai::SimaiTimingMetadata& timing, const QString& first);
    QVariantList objectCounts() const;
    QVariantList scoreLossRows() const;
    bool calculable() const;
    QString calculationStatus() const;
    int bpmFormat() const { return format_; }
    void setBpmFormat(int format);
    QString bpmPreview() const;
    QString error() const;
    Q_INVOKABLE void copyBpm();
    Q_INVOKABLE void saveBpm();
signals:
    void changed();
private:
    miacode::UiRequestService* requests_;
    SimaiParseResult parsed_;
    chart::ScoreLossResult score_;
    QString chartText_;
    QString firstText_;
    miacode::simai::SimaiTimingMetadata timing_;
    bool initialized_ = false;
    double first_ = 0.0;
    int format_ = 0;
    QString error_;
    QString saveError_;
};
}
