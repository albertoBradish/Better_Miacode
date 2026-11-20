#include "app/services/HandOrderExperimentService.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTest>
#include <QTextStream>
#include <QTranslator>

namespace {
class ExperimentSpecTranslator final : public QTranslator {
public:
    QString prefix = QStringLiteral("A");
    QString translate(const char*, const char* id, const char*, int) const override {
        if (qstrcmp(id, "hand_order.experiment.failed") == 0) return QStringLiteral("%1");
        if (qstrcmp(id, "hand_order.experiment.input_limit") == 0) return prefix + QStringLiteral(" limit");
        return {};
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool value, const char* message) {
        QTextStream(value ? stdout : stderr) << (value ? "PASS " : "FAIL ") << message << Qt::endl; ok &= value;
    };
    miacode::ChartWorkspace workspace;
    const QString source = QStringLiteral("&inote_5=(120){4}1~l,2~r,3~l,4~r,E\n&inote_6=(120){4}1,E\n");
    workspace.openSource(source, {}, 5);
    miacode::HandOrderExperimentService service(workspace);
    service.setFeaturesEnabled(true);
    miacode::JobProgressService progress;
    service.setJobProgress(&progress);
    const auto settle = [&] {
        for (int i = 0; i < 1000 && service.pending(); ++i) QTest::qWait(10);
    };
    const auto original = workspace.snapshot();
    expect(service.requestAnalysis(), "read-only analysis accepts the active difficulty");
    expect(progress.active() && progress.cancellable(), "experiment uses the shared cancellable progress surface");
    settle();
    expect(!progress.active(), "completed experiment releases its progress ownership");
    expect(service.hasAnalysis() && !service.rows().isEmpty(), "asynchronous current evidence is published");
    expect(workspace.snapshot().sourceText == original.sourceText && workspace.snapshot().revision == original.revision
        && !workspace.snapshot().dirty, "analysis leaves both source and save point unchanged");
    expect(!service.navigationTarget(0).isEmpty(), "current evidence has guarded source navigation coordinates");
    const auto oldParameters = service.simulationParameters();
    expect(!service.setSimulationParameter("seed", 2.5) && !service.setSimulationParameter("reactionMs", -1)
        && service.simulationParameters() == oldParameters, "invalid parameters leave valid settings intact");
    expect(service.requestSimulation(), "simulation uses the frozen current analysis");
    settle();
    const auto first = service.report();
    expect(service.hasSimulation() && service.requestSimulation(), "completed simulation can be repeated");
    settle();
    expect(first == service.report(), "fixed seed reproduction survives asynchronous service dispatch");
    bool replaced = false;
    quint64 foreignToken = 0;
    const auto takeover = QObject::connect(&progress, &miacode::JobProgressService::changed, &progress, [&] {
        if (!replaced && progress.active()) {
            replaced = true;
            foreignToken = progress.begin(QStringLiteral("Other job"), QStringLiteral("Untouched label"), true);
        }
    });
    expect(service.requestSimulation(), "simulation can run without claiming a replacement progress job");
    expect(replaced && progress.token() == foreignToken && !progress.indeterminate()
        && progress.label() == QStringLiteral("Untouched label"),
        "synchronous begin reentry cannot overwrite the replacement job's label or mode");
    service.cancel();
    expect(progress.active() && progress.token() == foreignToken && progress.label() == QStringLiteral("Untouched label"),
        "cancelling the experiment does not end a replacement progress job");
    QObject::disconnect(takeover); progress.end();
    bool cancelledDuringBegin = false;
    const auto cancelDuringBegin = QObject::connect(&progress, &miacode::JobProgressService::changed, &progress, [&] {
        if (!cancelledDuringBegin && progress.active()) {
            cancelledDuringBegin = true; progress.requestCancel();
        }
    });
    expect(!service.requestAnalysis() && !service.pending() && !progress.active(),
        "cancellation during begin cannot dispatch an orphaned worker");
    QObject::disconnect(cancelDuringBegin);
    workspace.replaceActiveDifficultyChart(QStringLiteral("(120){4}1~l,2~r,3~l,4~r,E "));
    workspace.replaceActiveDifficultyChart(QStringLiteral("(120){4}1~l,2~r,3~l,4~r,E"));
    expect(!service.hasAnalysis() && service.report().isEmpty() && service.navigationTarget(0).isEmpty(),
        "returning to identical chart text cannot resurrect an old revision");
    service.requestAnalysis(); progress.requestCancel(); QTest::qWait(40);
    expect(!service.pending() && !service.hasAnalysis(), "cancelled worker cannot publish evidence");
    service.requestAnalysis(); workspace.selectDifficulty(6); QTest::qWait(40);
    expect(!service.hasAnalysis(), "difficulty changes reject in-flight evidence");
    workspace.selectDifficulty(5); service.requestAnalysis(); workspace.openSource(source, {}, 5); QTest::qWait(40);
    expect(!service.hasAnalysis(), "reopening the same source rejects the prior document generation");
    service.requestAnalysis(); settle();
    auto profile = miacode::hand_order::PlayerProfile{}; profile.staminaCapacity = 0.5;
    service.setPlayerProfile(profile);
    expect(!service.hasAnalysis(), "player-profile changes invalidate baseline analysis");
    service.requestAnalysis();
    const bool priorPolicy = SimaiParser::allowNegativeHsEnabled();
    SimaiParser::setAllowNegativeHsEnabled(!priorPolicy); service.refreshParserPolicy();
    SimaiParser::setAllowNegativeHsEnabled(priorPolicy); service.refreshParserPolicy();
    QTest::qWait(40);
    expect(!service.hasAnalysis() && !service.pending(), "parser-policy changes reject older asynchronous results");
    service.requestAnalysis(); settle();
    expect(service.hasAnalysis(), "current parser policy can create a new analysis");
    SimaiParser::setAllowNegativeHsEnabled(!priorPolicy);
    expect(!service.hasAnalysis() && service.report().isEmpty() && service.navigationTarget(0).isEmpty(),
        "getters reject policy-stale evidence even when the owner did not refresh it");
    expect(!service.requestSimulation() && !service.pending(),
        "simulation request independently rejects a changed global parser policy");
    SimaiParser::setAllowNegativeHsEnabled(priorPolicy);
    service.requestAnalysis(); settle();
    expect(service.requestSimulation(), "fresh-policy baseline allows simulation");
    SimaiParser::setAllowNegativeHsEnabled(!priorPolicy);
    settle();
    expect(!service.hasAnalysis() && !service.hasSimulation() && !service.pending() && service.report().isEmpty(),
        "simulation completion rechecks policy without an owner notification");
    SimaiParser::setAllowNegativeHsEnabled(priorPolicy); service.refreshParserPolicy();

    ExperimentSpecTranslator translator;
    app.installTranslator(&translator);
    const QString largeUnicode(miacode::hand_order::experiment::maximumInputUtf8Bytes / 2, QChar(0x4e00));
    workspace.openSource(QStringLiteral("&inote_5=") + largeUnicode + QLatin1Char('\n'), {}, 5);
    expect(!service.requestAnalysis() && !service.pending() && service.status() == QStringLiteral("A limit"),
        "service enforces the UTF-8 limit before dispatching its parser");
    translator.prefix = QStringLiteral("B"); service.retranslate();
    expect(service.status() == QStringLiteral("B limit"),
        "input-limit diagnostics retain their translation key across language changes");
    app.removeTranslator(&translator);
    return ok ? 0 : 1;
}
