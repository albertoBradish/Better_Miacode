#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/analysis/hand_order/CombinedHandOrder.h"
#include "core/chart/transform/HandOrderPatternPriority.h"
#include "app/services/HandOrderProgressEstimate.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QMap>

using namespace miacode::hand_order;
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    bool ok = true;
    const auto expect = [&](bool passed, const char* message) {
        ok &= passed; QTextStream(passed ? stdout : stderr) << (passed ? "PASS " : "FAIL ") << message << '\n';
    };
    miacode::HandOrderProgressEstimate estimate;
    HandOrderProgress stage{HandOrderProgress::Decisions, 0, 10, 1, 1, 1};
    expect(estimate.update(stage, 0).remainingSeconds == -1, "ETA starts unknown without completed work");
    stage.completed = 2;
    expect(estimate.update(stage, 1400).remainingSeconds == -1, "two decisions cannot establish an ETA");
    stage.completed = 3;
    const auto sample = estimate.update(stage, 1800);
    expect(sample.percent == 30 && sample.remainingSeconds == 5, "three measured decisions estimate remaining current-stage time");
    stage.completed = 1;
    expect(estimate.update(stage, 1900).percent == 30, "one stage never moves backwards on a stale sample");
    stage.candidate = 1; stage.completed = 0;
    const auto nextCandidate = estimate.update(stage, 2000);
    expect(nextCandidate.percent == 0 && nextCandidate.remainingSeconds == -1, "a new candidate discards the previous stage ETA");
    stage.attempt = 2;
    expect(estimate.update(stage, 3000).remainingSeconds == -1, "a new search attempt resets its estimate");
    stage.phase = HandOrderProgress::Replay; stage.total = 0;
    expect(estimate.update(stage, 3500).percent == -1, "unmeasurable full replay stays indeterminate");
    miacode::HandOrderProgressEstimate interleaved;
    HandOrderProgress series{HandOrderProgress::Windows, 0, 6};
    interleaved.update(series, 0);
    interleaved.update({HandOrderProgress::Decisions, 0, 10, 1, 6, 1}, 100);
    interleaved.update({HandOrderProgress::Replay}, 1500);
    series.completed = 3;
    const auto windowsEstimate = interleaved.update(series, 2000);
    expect(windowsEstimate.percent == 50 && windowsEstimate.remainingSeconds == 2,
        "window-series ETA retains real elapsed work across interleaved search stages");
    miacode::HandOrderProgressEstimate resumedEstimate;
    HandOrderProgress resumedStage{HandOrderProgress::Decisions, 50, 100, 1, 1, 1};
    expect(resumedEstimate.update(resumedStage, 300000).remainingSeconds == -1,
        "resume/promotion starts with unknown ETA even after a long paused wall time");
    resumedStage.completed = 53;
    expect(resumedEstimate.update(resumedStage, 301500).remainingSeconds == 24,
        "fresh ETA uses only decisions completed after the new sampling origin");
    miacode::HandOrderWorkflowProgress workflow;
    using Work = HandOrderProgress;
    expect(workflow.update({Work::Normalizing, 0, 1}, 0).stages == QVector<int>(5, 0),
        "combined job lists all five work items from startup");
    workflow.update({Work::Normalizing, 1, 1}, 100);
    workflow.update({Work::Patterns, 1, 1}, 300);
    Work searchProgress{Work::Windows, 2, 6};
    searchProgress.pass = Work::ContextSearch;
    const auto searching = workflow.update(searchProgress, 2500);
    expect(searching.percent > 12 && searching.stages.at(2) > 0 && searching.stages.at(4) == 0
        && searching.remainingSeconds > 0, "overall ETA accounts for work after the active search");
    searchProgress.completed = 1;
    expect(workflow.update(searchProgress, 2600).percent == searching.percent,
        "a retry cannot reduce aggregate progress");
    workflow.resetClock(300000);
    expect(workflow.update(searchProgress, 300100).remainingSeconds == -1,
        "resuming keeps stage completion and starts a fresh ETA");
    expect(workflow.update({Work::Finished, 1, 1}, 303000).stages == QVector<int>(5, 100),
        "all listed work completes together");
    const QString source = "(150){16}1~l,5~r,1~l,5~r,1~l,5~r,1~l,5~r,";
    BasicParameters parameters; parameters.counterfactualBudget = 0;
    QVector<HandOrderProgress> updates;
    const auto progress = [&](const HandOrderProgress& update) { updates.append(update); };
    const auto parsed = SimaiParser::validateSyntax(source);
    inferBasicHandOrder(source, parsed, 0, source.size(), parameters, {}, progress);
    bool decisions = false, windows = false, monotonic = true;
    QMap<QString, int> previous;
    for (const auto& update : updates) {
        decisions |= update.phase == HandOrderProgress::Decisions && update.completed > 0;
        windows |= update.phase == HandOrderProgress::Windows && update.completed > 0;
        const QString key = QStringLiteral("%1:%2:%3:%4:%5").arg(update.phase).arg(update.candidate)
            .arg(update.window).arg(update.attempt).arg(update.total);
        if (update.total > 0) {
            monotonic &= update.completed >= previous.value(key, 0) && update.completed <= update.total;
            previous[key] = update.completed;
        }
    }
    expect(decisions && windows && monotonic, "real automatic Sticking emits monotonic decision and window work counts");
    updates.clear();
    const auto result = optimizeHandOrderPatterns(source, parsed, 0, source.size(), parameters, {}, {}, progress);
    bool baseline = false, candidate = false;
    for (const auto& update : updates) {
        baseline |= update.candidate == 0 && update.phase == HandOrderProgress::Decisions;
        candidate |= update.candidate > 0 && update.phase == HandOrderProgress::Decisions;
    }
    expect(result.patternAccepted > 0 && !baseline && !candidate && result.workUsed == 0,
        "structural shortcuts finish without launching baseline or candidate search work");
    const QString combined = QStringLiteral("|| retain these original offsets\n(150){8}1/4, 1/5, 1/6, 1/7, 1/8,,\nE");
    const auto combinedParsed = SimaiParser::validateSyntax(combined);
    const auto merged = optimizeHandOrder(combined, combinedParsed, 0, combined.size(), parameters, {}, {}, {}, progress);
    expect(merged.combinedOptimization && merged.combinedNormalizationUsed
        && !merged.patternSuggestions.isEmpty() && merged.patternSuggestions.first().registryId == "T25"
        && merged.patternSuggestions.first().line == 2
        && handOrderPatternPriority(QStringLiteral("T25")) < handOrderPatternPriority(QStringLiteral("T19")),
        "combined optimization recognizes the fixed axis over overlapping Eachs at the original source");
    const auto classify = [](const QString& text) {
        const auto chart = SimaiParser::validateSyntax(text);
        return recognizeHandOrderPatterns(text, chart, 0, text.size(), PatternRecognitionMode::Prioritized);
    };
    const auto steering = classify("(150){16}1,5,2,6,3,7,4,8,1,5,E");
    const auto circle = classify("(150){16}1,2,3,4,5,6,7,8,1,E");
    const auto triangle = classify("(150){16}1,3,5,3,1,3,5,3,E");
    const auto axis = classify("(150){16}2,5,2,7,2,5,2,7,E");
    const auto staircase = classify("(150){16}1,2,3,4,5,6,E");
    const auto sweep = classify("(180){16}1,2,3,4,5,6,E");
    const auto returnSweep = classify("(150){32}2,3,4,5,4,3,2,E");
    const auto variable = classify("(150){8}1,2,{16}3,4,{32}5,6,7,{16}8,1,{8}2,E");
    const auto shortJack = classify("(150){8}5,5,E");
    const auto rotatedBursts = classify("(150){32}1/5,1/5,,2/6,2/6,,3/7,3/7,,4/8,4/8,E");
    expect(steering.size() == 1 && steering.first().registryId == "T09"
        && circle.size() == 1 && circle.first().registryId == "T30"
        && triangle.size() == 1 && triangle.first().registryId == "T06"
        && axis.size() == 1 && axis.first().registryId == "T08"
        && staircase.size() == 1 && staircase.first().registryId == "T05"
        && sweep.size() == 1 && sweep.first().registryId == "T28",
        "steering, single-hand rotation, triangle, axis and staircase use their own geometric and timing rules");
    expect(returnSweep.size() == 1 && returnSweep.first().registryId == "T29"
        && variable.size() == 1 && variable.first().registryId == "T31"
        && shortJack.size() == 1 && shortJack.first().registryId == "T14" && shortJack.first().preferred >= 0
        && rotatedBursts.size() == 1 && rotatedBursts.first().registryId == "T18",
        "return sweeps, speed changes, comfortable jacks and rotating burst pairs win their overlaps");
    return ok ? 0 : 1;
}
