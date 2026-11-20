#include "app/services/HandOrderAnalysisService.h"
#include <QCoreApplication>
#include <QTest>
#include <QTextStream>

using namespace miacode;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) {
        ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n';
    };
    ChartWorkspace workspace;
    const QString chart = "(150){16}1~r,5~l,1~r,5~l,1~r,5~l,";
    workspace.openSource("&inote_5=" + chart + "\n&inote_6=2~r,\n", {}, 5);
    const auto original = workspace.snapshot();
    HandOrderAnalysisService service(workspace);
    service.setFeaturesEnabled(true);
    JobProgressService progress; service.setJobProgress(&progress);
    expect(service.overwriteExisting(), "step two defaults to refining existing basic-layout annotations");
    expect(service.prepare(chart, 0, 0, false) && !service.pending() && service.computationCount() == 0,
        "opening contextual settings starts no foreground calculation");
    expect(service.prepare(chart, 0, 0, true) && !service.pending() && service.computationCount() == 0,
        "opening specialized settings starts no foreground calculation");
    service.setBackgroundEnabled(true);
    expect(QTest::qWaitFor([&] { return service.computationCount() > 0 && !service.backgroundActive(); }, 6000),
        "idle current-chart context calculation completes silently");
    const auto after = workspace.snapshot();
    expect(!progress.active() && service.rows().isEmpty() && !service.pending()
        && after.revision == original.revision && after.sourceText == original.sourceText && after.dirty == original.dirty,
        "background work neither publishes edits nor changes document/save identity");
    const auto runs = service.computationCount();
    expect(service.prepare(chart, 0, 0, false) && service.cacheHit() && !service.rows().isEmpty()
        && service.computationCount() == runs, "settings can preview an exact background cache without calculation");
    expect(service.request(chart, 0, 0, false) && service.cacheHit() && !service.pending()
        && service.computationCount() == runs, "explicit calculation reuses the exact whole-chart cache");
    service.setBeamWidth(32);
    expect(QTest::qWaitFor([&] { return service.computationCount() > runs && !service.backgroundActive(); }, 6000),
        "changing a parameter after a completed cache schedules fresh idle work");
    service.prepare(chart, chart.indexOf("1~r"), chart.indexOf(',') , false);
    expect(!service.cacheHit() && !service.canApply(), "a selected scope cannot reuse a whole-chart result");
    service.setMaxWorkUnits(600000000);
    service.prepare(chart, 0, 0, false);
    expect(!service.cacheHit(), "budget/parameter changes invalidate background results");
    service.request(chart, 0, 0, false); service.pause();
    const auto pausedRun = service.computationCount();
    QTest::qWait(30);
    expect(service.pending() && service.paused(), "foreground computation can remain paused without a result");
    service.resume();
    expect(QTest::qWaitFor([&] { return !service.pending(); }, 5000)
        && service.computationCount() == pausedRun, "continue resumes the same dispatched computation");
    service.request(chart, 0, 0, false);
    service.cancel(); const auto stoppedRuns = service.computationCount();
    QTest::qWait(2300);
    expect(!service.backgroundActive() && service.computationCount() == stoppedRuns,
        "explicit exit is not silently restarted by idle background work");
    workspace.replaceActiveDifficultyChart(chart + " ");
    service.setPlaybackActive(true);
    const auto playingRuns = service.computationCount(); QTest::qWait(2300);
    expect(service.computationCount() == playingRuns, "playback suppresses idle precomputation");
    service.setPlaybackActive(false);
    expect(QTest::qWaitFor([&] { return service.computationCount() > playingRuns; }, 6000),
        "idle work becomes eligible again after playback stops");
    service.setBackgroundEnabled(false);
    workspace.updateDocumentField(ChartWorkspaceDocumentField::Title, "Changed timing/document metadata");
    service.prepare(chart + " ", 0, 0, false);
    expect(!service.cacheHit(), "metadata changes invalidate current-chart evidence");
    workspace.selectDifficulty(6);
    service.prepare("2~r,", 0, 0, false);
    expect(!service.cacheHit(), "active difficulty changes invalidate cached evidence");
    workspace.openSource("&inote_5=" + chart + "\n", {}, 5);
    service.prepare(chart, 0, 0, false);
    expect(!service.cacheHit(), "same text in a new document generation cannot reuse old cached evidence");
    // A real sufficiently long context solve remains in flight through the
    // 20%-duty background worker and is promoted without dispatching again.
    QString longChart = "(180){16}";
    for (int i = 0; i < 600; ++i) longChart += (i % 2 ? "5," : "1,");
    workspace.openSource("&inote_5=" + longChart + "\n", {}, 5);
    service.setBackgroundEnabled(true);
    expect(QTest::qWaitFor([&] { return service.backgroundActive(); }, 6000), "a long real background solve starts");
    const auto backgroundRun = service.computationCount();
    service.pause(); QTest::qWait(30);
    expect(service.backgroundActive() && service.paused() && !progress.active(),
        "background work can be paused without opening the foreground progress card");
    service.resume();
    service.request(longChart, 0, 0, false);
    expect(service.pending() && !service.backgroundActive() && progress.active(),
        "an exact explicit request promotes background work to the foreground surface");
    expect(QTest::qWaitFor([&] { return !service.pending(); }, 15000)
        && service.computationCount() == backgroundRun, "promotion keeps the existing worker stack and search state");
    service.setBackgroundEnabled(false);
    return ok ? 0 : 1;
}
