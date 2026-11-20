#include "app/services/HandOrderAnalysisService.h"
#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTextStream>
#include <cmath>

using namespace miacode;
namespace {
double elapsedMs(const QElapsedTimer& timer) { return timer.nsecsElapsed() / 1000000.0; }
QString edited(const QString& source, const editor::SimaiTextEditResult& edit)
{
    QString result = source;
    if (edit.consumed && edit.transaction.hasEdit)
        result.replace(edit.transaction.replacementStart, edit.transaction.replacementEnd - edit.transaction.replacementStart,
            edit.transaction.replacementText);
    return result;
}
QString patched(const QString& source, const hand_order::BasicResult& plan)
{
    QString result = source;
    for (auto patch = plan.patches.crbegin(); patch != plan.patches.crend(); ++patch)
        result.replace(patch->start, patch->end - patch->start, patch->text);
    return result;
}
hand_order::BasicAssignmentEvaluation fixed(const QString& source)
{
    hand_order::BasicParameters p; p.evaluateCompleteAssignment = true; p.counterfactualBudget = 0;
    return hand_order::inferBasicHandOrder(source, SimaiParser::validateSyntax(source), 0, source.size(), p).evaluation;
}
bool sameEvaluation(const hand_order::BasicAssignmentEvaluation& a, const hand_order::BasicAssignmentEvaluation& b)
{
    return a.complete && b.complete && a.ergonomicBreaks == b.ergonomicBreaks
        && std::abs(a.cost - b.cost) < 1e-6 && std::abs(a.risk - b.risk) < 1e-8
        && std::abs(a.playerCost - b.playerCost) < 1e-6
        && std::abs(a.peakSpeed - b.peakSpeed) < 1e-6 && std::abs(a.peakTapRate - b.peakTapRate) < 1e-6
        && std::abs(a.peakLoad - b.peakLoad) < 1e-6;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) {
        ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n';
    };
    QJsonObject measurements;
    QString original = "(210)";
    for (int block = 0; block < 60; ++block) original += "{16}1,3,5,7,1,3,5,7,1,3,5,7,{1},,{16}";
    QElapsedTimer timer; timer.start();
    const QString stage1 = chart_transform::basicHandOrderLayoutInSelection(original, 0, 0).edit.replacement;
    measurements["layout_elapsed_ms"] = elapsedMs(timer);
    ChartWorkspace workspace;
    workspace.openSource("&title=Synthetic T10 pipeline service regression\n&inote_5=" + stage1 + "\n", {}, 5);
    const auto identity = workspace.snapshot();
    HandOrderAnalysisService cold(workspace);
    cold.setFeaturesEnabled(true);
    timer.restart(); cold.request(stage1, 0, 0, false);
    expect(QTest::qWaitFor([&] { return !cold.pending(); }, 30000), "cold real stage-one output completes contextual optimization");
    measurements["context_cold_elapsed_ms"] = elapsedMs(timer);
    const auto coldRows = cold.rows();
    const auto coldEdit = cold.canApply() ? cold.takeEdit(stage1) : editor::SimaiTextEditResult{};
    const QString coldStage2 = edited(stage1, coldEdit);
    measurements["cold_dispatches"] = qint64(cold.computationCount());

    HandOrderAnalysisService warm(workspace);

    warm.setFeaturesEnabled(true);
    timer.restart(); warm.setBackgroundEnabled(true);
    const bool reachedDecisions = QTest::qWaitFor([&] {
        return warm.backgroundActive() && warm.backgroundPercent() > 0 && warm.backgroundPercent() < 100
            && warm.backgroundDetail().contains(qtTrId("hand_order.progress.decisions"));
    }, 30000);
    expect(reachedDecisions, "background solver reaches positive actual decision progress before pause");
    measurements["pause_at_elapsed_ms"] = elapsedMs(timer);
    measurements["pause_at_decision_percent"] = warm.backgroundPercent();
    measurements["pause_at_detail"] = warm.backgroundDetail();
    const auto dispatches = warm.computationCount();
    warm.pause();
    QElapsedTimer hold; hold.start();
    // This is a real user pause interval, not a substitute for solver work.
    QTest::qWait(80);
    measurements["pause_hold_elapsed_ms"] = elapsedMs(hold);
    expect(warm.backgroundActive() && warm.paused(), "mid-search pause retains the running solver instead of publishing a completed result");
    warm.resume(); QElapsedTimer afterResume; afterResume.start();
    expect(QTest::qWaitFor([&] { return !warm.backgroundActive(); }, 30000), "resumed background solver reaches a complete cache result");
    measurements["resume_to_complete_elapsed_ms"] = elapsedMs(afterResume);
    measurements["background_wall_with_idle_and_pause_ms"] = elapsedMs(timer);
    measurements["background_dispatches"] = qint64(warm.computationCount());
    expect(warm.computationCount() == dispatches && dispatches == 1, "pause and continue do not dispatch another calculation");
    const auto afterBackground = workspace.snapshot();
    expect(afterBackground.revision == identity.revision && afterBackground.sourceText == identity.sourceText
        && afterBackground.dirty == identity.dirty, "the real background pipeline leaves workspace and save identity unchanged");
    timer.restart(); warm.prepare(stage1, 0, 0, false);
    measurements["cache_prepare_elapsed_ms"] = elapsedMs(timer);
    expect(warm.cacheHit() && warm.rows() == coldRows, "exact cached rows retain every cold-search evidence field");
    timer.restart(); warm.request(stage1, 0, 0, false);
    measurements["cache_request_elapsed_ms"] = elapsedMs(timer);
    expect(warm.cacheHit() && !warm.pending() && warm.computationCount() == dispatches,
        "explicit cache reuse has no new worker dispatch");
    const auto warmEdit = warm.canApply() ? warm.takeEdit(stage1) : editor::SimaiTextEditResult{};
    const QString warmStage2 = edited(stage1, warmEdit);
    expect(coldEdit.consumed == warmEdit.consumed
        && coldEdit.transaction.hasEdit == warmEdit.transaction.hasEdit
        && coldEdit.transaction.replacementStart == warmEdit.transaction.replacementStart
        && coldEdit.transaction.replacementEnd == warmEdit.transaction.replacementEnd
        && coldEdit.transaction.replacementText == warmEdit.transaction.replacementText && coldStage2 == warmStage2,
        "resumed cached application transaction equals cold contextual patches exactly");
    timer.restart();
    const auto coldEvaluation = fixed(coldStage2), warmEvaluation = fixed(warmStage2);
    measurements["independent_context_evaluations_elapsed_ms"] = elapsedMs(timer);
    measurements["context_evaluation_complete"] = coldEvaluation.complete && warmEvaluation.complete;
    measurements["context_cost"] = coldEvaluation.complete ? QJsonValue(coldEvaluation.cost) : QJsonValue(QJsonValue::Null);
    expect(sameEvaluation(coldEvaluation, warmEvaluation), "cold and paused/cache contextual outputs have identical complete reconstructed evaluations");
    warm.setBackgroundEnabled(false);
    hand_order::BasicParameters standard; standard.overwriteExisting = true;
    timer.restart();
    const auto a = hand_order::optimizeHandOrderPatterns(coldStage2, SimaiParser::validateSyntax(coldStage2),
        0, coldStage2.size(), standard);
    measurements["pattern_cold_elapsed_ms"] = elapsedMs(timer);
    timer.restart();
    const auto b = hand_order::optimizeHandOrderPatterns(warmStage2, SimaiParser::validateSyntax(warmStage2),
        0, warmStage2.size(), standard);
    measurements["pattern_after_resumed_cache_elapsed_ms"] = elapsedMs(timer);
    expect(patched(coldStage2, a) == patched(warmStage2, b) && a.patternOutcome == b.patternOutcome
        && a.workUsed == b.workUsed && a.patternSuggestions.size() == b.patternSuggestions.size()
        && !a.evaluation.complete && !b.evaluation.complete,
        "both actual pipeline branches retain identical structural recommendations without claiming physical proof");
    workspace.updateDocumentField(ChartWorkspaceDocumentField::Title, "Updated metadata");
    warm.prepare(stage1, 0, 0, false);
    expect(!warm.cacheHit(), "metadata edits invalidate cached pipeline evidence");
    warm.setMaxWorkUnits(600000000); warm.prepare(stage1, 0, 0, false);
    expect(!warm.cacheHit(), "parameter/budget edits cannot reuse the earlier pipeline result");
    // Playback starts after a new background solve has entered actual work.
    workspace.openSource("&inote_5=" + stage1 + "\n", {}, 5);
    warm.setBackgroundEnabled(true);
    expect(QTest::qWaitFor([&] { return warm.backgroundActive() && warm.backgroundPercent() > 0; }, 30000),
        "playback invalidation scenario enters real background computation");
    warm.setPlaybackActive(true);
    const auto beforePlayback = warm.computationCount();
    QTest::qWait(2200);
    warm.prepare(stage1, 0, 0, false);
    expect(!warm.backgroundActive() && !warm.cacheHit() && warm.computationCount() == beforePlayback,
        "playback cancels in-flight work and cannot publish or restart its stale cache");
    warm.setBackgroundEnabled(false);
    measurements["all_assertions_passed"] = ok;
    QTextStream(stdout) << "MEASURE_PIPELINE_SERVICE " << QJsonDocument(measurements).toJson(QJsonDocument::Compact) << '\n';
    return ok ? 0 : 1;
}
