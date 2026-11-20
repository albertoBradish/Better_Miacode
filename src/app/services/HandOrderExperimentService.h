#pragma once
#include "app/services/ChartWorkspace.h"
#include "app/services/JobProgressService.h"
#include <QPointer>
#include "common/TaskCancellation.h"
#include "core/analysis/hand_order/HandOrderExperiment.h"
#include <QObject>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>

namespace miacode {
class HandOrderExperimentService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool hasAnalysis READ hasAnalysis NOTIFY changed)
    Q_PROPERTY(bool hasSimulation READ hasSimulation NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QVariantMap simulationParameters READ simulationParameters NOTIFY changed)
    Q_PROPERTY(QString report READ report NOTIFY changed)
public:
    explicit HandOrderExperimentService(ChartWorkspace& workspace, QObject* parent = nullptr);
    ~HandOrderExperimentService() override;
    void setFeaturesEnabled(bool enabled) { featuresEnabled_ = enabled; if (!enabled) cancel(); }
    bool pending() const { return pending_; }
    bool hasAnalysis() const;
    bool hasSimulation() const { return hasAnalysis() && simulation_.valid; }
    QString status() const;
    QVariantList rows() const;
    QVariantMap simulationParameters() const;
    QString report() const;
    void setPlayerProfile(const hand_order::PlayerProfile& profile);
    void setJobProgress(JobProgressService* progress);
    void refreshParserPolicy();
    void retranslate() { emit changed(); }
    Q_INVOKABLE bool requestAnalysis();
    Q_INVOKABLE bool requestSimulation();
    Q_INVOKABLE bool setSimulationParameter(const QString& key, const QVariant& value);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QVariantMap navigationTarget(int row) const;
private:
    bool featuresEnabled_ = false;
    void stopWork();
    void beginProgress(const char* label);
    void endProgress();
    bool current(const ChartWorkspaceSnapshot& identity) const;
    ChartWorkspace* workspace_;
    ChartWorkspaceSnapshot identity_;
    hand_order::PlayerProfile profile_;
    hand_order::experiment::Analysis analysis_;
    hand_order::experiment::Simulation simulation_;
    hand_order::experiment::SimulationParameters parameters_;
    QPointer<JobProgressService> progress_;
    quint64 progressToken_ = 0;
    QThreadPool worker_;
    task::CancellationFlag cancellation_;
    quint64 sequence_ = 0;
    bool pending_ = false;
    bool parserPolicy_ = SimaiParser::allowNegativeHsEnabled();
    QString state_ = QStringLiteral("idle"), error_;
signals:
    void changed();
};
} // namespace miacode
