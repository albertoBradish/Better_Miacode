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
    Q_PROPERTY(QString simpleLossSummary READ simpleLossSummary NOTIFY changed)
    Q_PROPERTY(QString scoreBasisSummary READ scoreBasisSummary NOTIFY changed)
    Q_PROPERTY(QString scenarioSummary READ scenarioSummary NOTIFY scenarioChanged)
    Q_PROPERTY(QString scenarioError READ scenarioError NOTIFY scenarioChanged)
    Q_PROPERTY(int bpmFormat READ bpmFormat WRITE setBpmFormat NOTIFY changed)
    Q_PROPERTY(QString bpmPreview READ bpmPreview NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString bpmImportText READ bpmImportText WRITE setBpmImportText NOTIFY changed)
    Q_PROPERTY(QString bpmImportPreview READ bpmImportPreview NOTIFY changed)
    Q_PROPERTY(QString bpmImportSummary READ bpmImportSummary NOTIFY changed)
    Q_PROPERTY(bool canApplyBpm READ canApplyBpm NOTIFY changed)
public:
    explicit ChartToolsModel(miacode::UiRequestService& requests, QObject* parent = nullptr);
    void setChart(const QString& text, const miacode::simai::SimaiTimingMetadata& timing, const QString& first,
        int difficulty = 0, quint64 revision = 0);
    QVariantList objectCounts() const;
    QVariantList scoreLossRows() const;
    bool calculable() const;
    QString calculationStatus() const;
    QString simpleLossSummary() const;
    QString scoreBasisSummary() const;
    QString scenarioSummary() const;
    QString scenarioError() const;
    Q_INVOKABLE QString lossQuantity(int row) const;
    Q_INVOKABLE void setLossQuantity(int row, const QString& value);
    Q_INVOKABLE void resetLossQuantities();
    int bpmFormat() const { return format_; }
    void setBpmFormat(int format);
    QString bpmPreview() const;
    QString error() const;
    Q_INVOKABLE void copyBpm();
    Q_INVOKABLE void saveBpm();
    QString bpmImportText() const { return importText_; }
    QString bpmImportPreview() const { return importPreview_; }
    QString bpmImportSummary() const;
    bool canApplyBpm() const { return !importPreview_.isEmpty() && importRevision_ == revision_ && importDifficulty_ == difficulty_; }
    void setBpmImportText(const QString& text);
    Q_INVOKABLE void loadBpm();
    Q_INVOKABLE void pasteBpm();
    Q_INVOKABLE void applyBpm();
    Q_INVOKABLE void completeBpmApply(bool success);
signals:
    void changed();
    void scenarioChanged();
    void applyBpmRequested(const QString& original, const QString& replacement, int difficulty, quint64 revision);
private:
    miacode::UiRequestService* requests_;
    SimaiParseResult parsed_;
    chart::ScoreLossResult score_;
    QVector<QString> lossInputs_;
    chart::ScoreScenarioResult scoreScenario() const;
    QString chartText_;
    QString firstText_;
    miacode::simai::SimaiTimingMetadata timing_;
    bool initialized_ = false;
    double first_ = 0.0;
    int format_ = 0;
    QString error_;
    QString saveError_;
    QString importText_, importPreview_, importError_, importSubdivisionWarning_;
    int difficulty_ = 0, importDifficulty_ = 0, importChanges_ = 0, importNotes_ = 0;
    quint64 revision_ = 0, importRevision_ = 0;
};
}
