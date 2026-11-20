#include "app/services/HandOrderAnalysisService.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCoreApplication>
#include <QTest>
#include <QTextStream>
using namespace miacode;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message) { ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n'; };
    ChartWorkspace workspace;
    workspace.openSource("&inote_5=|| cache review \n(150){8}4,4,4,5,5,5,6,3,5,,,,{16}3,4,1,3,4,1,3,4,1,\n&inote_6=(150){4}1,\n", {}, 5);
    HandOrderAnalysisService service(workspace);
    service.setFeaturesEnabled(true);
    const auto chart = [&] { return workspace.document().difficulty(workspace.snapshot().activeDifficultyId)->chart; };
    const auto settle = [&] { for (int i = 0; service.pending() && i < 1000; ++i) QTest::qWait(5); };
    service.requestCurrentPatterns(); settle();
    const int count = service.patternCount();
    const auto target = service.patternNavigationTarget(0);
    expect(target.value("revision").toULongLong() == workspace.snapshot().revision
        && target.value("difficultyId").toInt() == 5 && target.value("line").toInt() > 0
        && target.value("end").toInt() > target.value("start").toInt(),
        "candidate navigation includes the current editor identity, complete selection and playback location");
    expect(count >= 3 && service.patternChoicesEnabled(), "real worker retains independent named pattern choices");
    const auto computations = service.computationCount();
    service.requestCurrentPatterns(); settle();
    expect(service.cacheHit() && service.computationCount() == computations, "reopening/rerequesting exact structural results does not dispatch work");
    const QString before = chart();
    const auto edit = service.takePatternEdit(0,0,before);
    expect(edit.consumed && edit.transaction.hasEdit, "one selected default produces one editor transaction");
    QString updated = before;
    if (edit.transaction.hasEdit) updated.replace(edit.transaction.replacementStart,
        edit.transaction.replacementEnd - edit.transaction.replacementStart, edit.transaction.replacementText);
    workspace.replaceActiveDifficultyChart(updated);
    expect(service.patternCount() == count && service.patternSuggestions().first().toMap().value("variants").toList().first().toMap().value("applied").toBool(),
        "annotation-only application preserves choices and marks the chosen syntax applied");
    int triangle = -1;
    for (int i = 0; i < service.patternCount(); ++i)
        if (service.patternSuggestions().at(i).toMap().value("registryId") == "T06") triangle = i;
    const auto location = service.patternNavigationRange(triangle, updated);
    expect(!location.isEmpty() && updated.mid(location.value("start").toInt(),1) == "3",
        "later cached ranges rebase after earlier annotation insertion");
    const auto later = service.takePatternEdit(triangle,1,updated);
    expect(later.consumed && later.transaction.replacementStart == location.value("start").toInt(),
        "later mirror choice applies at the rebased UTF-16 range");
    service.setMaxWorkUnits(1000);
    expect(service.patternCount() == count, "changing a solver budget retains independent structural alternatives");
    const auto beforeRefresh = service.computationCount();
    service.requestCurrentPatterns(); settle();
    expect(!service.cacheHit() && service.computationCount() == beforeRefresh + 1,
        "an explicit calculation refreshes edited hand labels instead of treating rebasing as a fresh calculation");
    service.setMaxWorkUnits(2000); const auto beforeBudget = service.computationCount();
    service.requestCurrentPatterns(); settle();
    expect(service.cacheHit() && service.computationCount() == beforeBudget,
        "budget-only changes still reuse identical structural input");
    service.setPreservePatternAnnotations(true);
    expect(!service.patternChoicesEnabled() && !service.takePatternEdit(0,1,updated).consumed, "the explicit label lock also blocks individual choices");
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count, "a locked scan cannot overwrite retained candidates with an empty result");
    service.setPreservePatternAnnotations(false);
    workspace.replaceActiveDifficultyChart(before);
    expect(service.patternCount() == count, "undoing annotations preserves the same alternatives");
    service.ignorePattern(0);
    expect(service.patternCount() == count - 1, "ignoring removes one candidate without changing chart text");
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count - 1, "reopening a cached scan does not resurrect an ignored candidate");
    service.setBackgroundEnabled(true);
    const auto manualRuns = service.computationCount();
    const QString extended = before + "{1},2,";
    workspace.replaceActiveDifficultyChart(extended);
    expect(service.patternCount() == count - 1 && !service.takePatternEdit(0,0,before).consumed,
        "native edits outside saved candidates retain the list and reject stale input text");
    QTest::qWait(2300);
    expect(service.computationCount() == manualRuns && !service.backgroundActive(),
        "editing a manually reviewed chart never schedules an automatic recalculation");
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count - 1 && service.computationCount() == manualRuns + 1,
        "only an explicit calculation refreshes edited native input and keeps ignored groups hidden");
    workspace.replaceActiveDifficultyChart(extended + "1~[");
    expect(service.patternCount() == count - 1 && !service.patternChoicesEnabled(),
        "an incomplete edit preserves review decisions while disabling stale application");
    workspace.replaceActiveDifficultyChart(extended);
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count - 1, "correcting incomplete syntax does not resurrect ignored groups");
    QString shifted = extended;
    shifted.insert(shifted.indexOf("(150){8}") + 8, "7,");
    workspace.replaceActiveDifficultyChart(shifted);
    expect(service.patternCount() == count - 1, "inserting an unrelated note rebases saved candidates without detecting new ones");
    service.requestCurrentPatterns(); settle();
    bool ignoredReturned = false;
    for (const auto& group : service.patternSuggestions())
        ignoredReturned |= group.toMap().value("line").toInt() == 2
            && group.toMap().value("col").toInt() == shifted.indexOf("4,4") - shifted.lastIndexOf('\n',shifted.indexOf("4,4"));
    expect(!ignoredReturned, "manual recalculation preserves ignores after note ordinals and absolute times shift");
    QString formatted = shifted; formatted.replace(",",",\n");
    workspace.replaceActiveDifficultyChart(formatted);
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count - 1, "formatting the chart does not restore ignored configurations during an explicit scan");
    const auto beforePaste = service.computationCount();
    workspace.replaceActiveDifficultyChart("");
    workspace.replaceActiveDifficultyChart(formatted);
    QTest::qWait(2300);
    expect(service.computationCount() == beforePaste && service.patternCount() == 0,
        "clearing and pasting native text still waits for an explicit review calculation");
    service.requestCurrentPatterns(); settle();
    expect(service.patternCount() == count - 1, "explicit calculation after pasting retains the session's ignore decisions");
    const auto kept = service.patternCount();
    workspace.selectDifficulty(6);
    expect(service.patternCount() == 0, "another difficulty does not show this difficulty's review list");
    workspace.selectDifficulty(5);
    expect(service.patternCount() == kept, "returning to a difficulty restores its retained review list");
    workspace.selectDifficulty(5); service.requestCurrentPatterns();
    workspace.openSource("&inote_5=" + before + "\n", {}, 5); settle(); QTest::qWait(20);
    expect(service.patternCount() == 0, "reopened documents reject late worker recommendations");
    service.setBackgroundEnabled(false);
    ChartWorkspace batchWorkspace;
    const QString mirrored = "(150){16}3,4,1,3,4,1,,,,6,5,8,6,5,8,,,,{8}3,4,1,3,4,1,";
    batchWorkspace.openSource("&inote_5="+mirrored+"\n",{},5);
    HandOrderAnalysisService batchService(batchWorkspace);
    batchService.setFeaturesEnabled(true);
    batchService.requestCurrentPatterns();
    for(int i=0;batchService.pending()&&i<1000;++i) QTest::qWait(5);
    const auto batchGroups=batchService.patternSuggestions();
    expect(batchGroups.size()==3,"batch fixture retains three distinct instances");
    if(batchGroups.size()==3) {
        const auto a=batchGroups[0].toMap(),b=batchGroups[1].toMap(),c=batchGroups[2].toMap();
        const QStringList keys{a.value("key").toString(),b.value("key").toString()};
        const QString choice=a.value("variants").toList()[0].toMap().value("choiceKey").toString();
        const QString native=batchWorkspace.document().difficulty(5)->chart;
        const auto tx=batchService.takePatternBatchEdit(keys,choice,native);
        expect(a.value("similarityKey")==b.value("similarityKey")&&a.value("similarityKey")!=c.value("similarityKey")&&tx.consumed,
            "mirrored native shapes batch by strategy while different real timing stays separate");
        QString result=native;
        if(tx.consumed) result.replace(tx.transaction.replacementStart,tx.transaction.replacementEnd-tx.transaction.replacementStart,tx.transaction.replacementText);
        const auto parsed=SimaiParser::validateSyntax(result);bool roles=parsed.noteMarkers.size()==18;
        for(int i=0;roles&&i<12;++i) roles&=parsed.noteMarkers[i].handOrderHead==((i<6?(i%2==0):(i%2!=0))?HandOrderHand::Right:HandOrderHand::Left);
        expect(roles&&hand_order::stripAnnotations(result)==native,"batch transforms the local entry hand for each mirror and preserves native data");
        expect(!batchService.takePatternBatchEdit({keys.first(),c.value("key").toString()},choice,native).consumed,
            "mixed families are rejected atomically");
        batchWorkspace.replaceActiveDifficultyChart(result);
        expect(!batchService.takePatternBatchEdit(keys,choice,native).consumed,"batch rejects stale editor text");
        const auto runs=batchService.computationCount();batchService.ignorePattern(1);
        expect(!batchService.takePatternBatchEdit(keys,"mirror",result).consumed&&batchService.computationCount()==runs,
            "an ignored selected instance rejects the entire batch without recalculating");
    }
    return ok ? 0 : 1;
}
