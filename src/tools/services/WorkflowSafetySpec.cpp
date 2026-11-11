#include "core/video/PreviewFrameRatePolicy.h"
#include "app/services/PausedPreviewPreferences.h"
#include "app/services/AutomaticChartPreparation.h"
#include "app/services/ChartFileSafety.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "app/services/PreviewModePreferences.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "editor/SpaceInputGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; QTextStream(stderr) << "FAIL: " << message << Qt::endl; }
}
bool write(const QString& path, const QByteArray& value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}
QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        namespace profiles = miacode::preview_mode_preferences;
        QJsonObject store;
        QJsonObject regular = profiles::normalized({});
        regular.insert("showTimestamp", false);
        regular.insert("outlineVariant", static_cast<int>(PreviewOutlineVariant::Point));
        regular.insert("outlineAutomatic", false);
        regular.insert("outlineCustomFile", "custom.png");
        store = profiles::withCurrent(store, RenderMode::Native, regular);
        QJsonObject debug = profiles::restored(store, RenderMode::Debug);
        check(debug.value("showTimestamp").toBool() && !debug.value("showBpmHud").toBool(),
              "unused render modes start from independent defaults");
        for (const char* key : {"showBpmHud", "showCpsHud", "showJudgeMarkers", "showTouchTrail"})
            debug.insert(QString::fromLatin1(key), true);
        debug.insert("outlineVariant", static_cast<int>(PreviewOutlineVariant::JudgeAreaLabeled));
        store = profiles::withCurrent(store, RenderMode::Debug, debug);
        check(profiles::restored(store, RenderMode::EraseByArea) == regular,
              "leaving debug restores all regular flags and the exact custom outline");
        QJsonObject muri = profiles::restored(store, RenderMode::MaimuriDxStyle);
        check(!muri.value("showTouchTrail").toBool(),
              "debug diagnostics never leak into an unused Muri profile");
        const auto legacyProfile = profiles::normalized({{"showMeasureLines", true},
            {"showJudgementSkeleton", true}, {"showSlideJudgementTiming", true},
            {"showObjectStatsHud", true}, {"showChartInfoHud", true}});
        check(!legacyProfile.contains("showMeasureLines") && !legacyProfile.contains("showJudgementSkeleton")
              && !legacyProfile.contains("showSlideJudgementTiming") && !legacyProfile.contains("showObjectStatsHud")
              && !legacyProfile.contains("showChartInfoHud"),
              "mode profiles do not own shared visual aids or independent video statistics");
        muri.insert("showJudgeMarkers", true);
        store = profiles::withCurrent(store, RenderMode::MaimuriDxStyle, muri);
        const auto reloaded = QJsonDocument::fromJson(QJsonDocument(store).toJson()).object();
        check(profiles::restored(reloaded, RenderMode::Debug) == debug
              && profiles::restored(reloaded, RenderMode::Native) == regular
              && profiles::restored(reloaded, RenderMode::MaimuriDxStyle) == muri,
              "three rendering profiles survive a preference serialization round trip");
        const auto malformed = profiles::normalized({{"showBpmHud", "true"}, {"outlineVariant", 99}});
        check(!malformed.value("showBpmHud").toBool()
              && malformed.value("outlineVariant").toInt() == static_cast<int>(PreviewOutlineVariant::Line),
              "invalid profile flags and outline types fall back safely");
    }
    miacode::preview_video::FrameRateGate gate;
    int shown = 0;
    for (int i = 0; i < 120; ++i) shown += gate.accept(i / 60.0, 30, 1, false);
    check(shown == 60, "manual PV limit keeps 30 of 60 frames each second");
    gate.reset(); shown = 0;
    for (int i = 0; i < 240; ++i) shown += gate.accept(i / 23.976, 0, 1, false);
    check(shown == 240, "auto detection keeps fractional source frame timing");
    for (double pts : {0.0, 0.04, 0.10, 0.11, 0.4})
        check(gate.accept(pts, 0, 1, false), "auto detection retains variable frame timestamps");
    gate.reset(); gate.accept(10, 30, 1, false);
    check(gate.accept(10.001, 30, 1, true), "paused seek bypasses PV presentation limit");
    check(gate.accept(1, 30, 1, false), "backward seek resets the PV gate");
    gate.reset(); shown = 0;
    for (int i = 0; i < 120; ++i) shown += gate.accept(i / 60.0, 30, 2, false);
    check(shown == 30, "playback speed keeps the wall-time presentation limit");
    using miacode::paused_preview::showStageMedia;
    check(showStageMedia({}), "new preferences show PV while paused");
    check(!showStageMedia({{"force_labeled_judge_line_when_paused", true}}), "legacy explicit hidden PV stays hidden");
    check(showStageMedia({{"hide_stage_media_when_paused", false}}), "legacy explicit visible PV stays visible");
    check(showStageMedia({{"show_stage_media_when_paused", true}, {"force_labeled_judge_line_when_paused", true}}),
          "PV and judge area can both be enabled");
    check(!showStageMedia({{"show_stage_media_when_paused", false}, {"force_labeled_judge_line_when_paused", false}}),
          "PV and judge area can both be disabled");
    check(showStageMedia({{"show_stage_media_when_paused", "false"}, {"hide_stage_media_when_paused", "true"}}),
          "invalid pause preferences use safe defaults");
    using miacode::editor::SpaceInputGuard;
    SpaceInputGuard guard;
    check(guard.press(0, false) == SpaceInputGuard::Action::TogglePlayback, "single space plays immediately");
    check(guard.press(50, true) == SpaceInputGuard::Action::Ignore, "held-space repeats do not unlock");
    check(guard.press(250, false) == SpaceInputGuard::Action::TogglePlayback, "second physical press toggles");
    check(guard.press(500, false) == SpaceInputGuard::Action::Unlock, "third physical press unlocks without insertion");
    check(guard.press(600, false) == SpaceInputGuard::Action::Insert, "subsequent spaces edit text");
    guard.reset();
    guard.press(1000, false); guard.press(1400, false);
    check(guard.press(1500, false) == SpaceInputGuard::Action::TogglePlayback, "expired sequence stays locked");
    guard.interruptSequence();
    check(guard.press(1550, false) == SpaceInputGuard::Action::TogglePlayback, "another key breaks the gesture");

    using namespace miacode::metadata;
    check(validateField("desid_6", "001, 1065").isEmpty(), "collaboration IDs retain leading zeros");
    check(!validateField("demo_seek", "nan").isEmpty(), "nonfinite time rejected");
    check(!validateField("create_date", "20260230").isEmpty(), "invalid calendar date rejected");
    check(validateField("creatorscomment", "line one\nline two").isEmpty(), "multiline comments retained");
    check(!validateField("creatorscomment", "test\n&inote_5=1").isEmpty(), "metadata cannot inject another field");
    check(conflictingKeys("&title=A\n&title=B\n").contains("title"), "duplicate managed values detected before parse loss");

    QTemporaryDir directory;
    check(directory.isValid(), "temporary fixture directory created");
    const QString path = directory.filePath("maidata.txt");
    const QByteArray original = QByteArray("\xEF\xBB\xBF") +
        "&title=Fixture\r\n&lv_2=\r\n&inote_2=E\r\n&lv_5=12\r\n&lv_6=13\r\n&desid_6=001\r\n&unknown=keep & this\r\n"
        "&inote_5=(120){4}1,2,3,4,E\r\n&inote_6=(120){4}5,6,7,8,E\r\n";
    check(write(path, original), "raw BOM/CRLF fixture written");
    QString backup;
    check(miacode::chart_file_safety::backupOriginal(path, &backup), "raw backup succeeds");
    check(read(backup) == original, "backup is byte-identical, including BOM and CRLF");
    QString repeatedBackup;
    check(miacode::chart_file_safety::backupOriginal(path, &repeatedBackup) && backup == repeatedBackup,
          "unchanged content reuses verified backup");

    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    check(files.open(path).accepted, "workspace fixture opened");
    const QString unsaved = "(120){4}8,7,6,5,E\n";
    workspace.replaceDifficultyChart(6, unsaved);
    auto preferences = PreferenceDocument::loadPreferencesObject();
    auto ui = preferences.value("ui").toObject(); ui.insert("auto_normalize_enabled", true);
    preferences.insert("ui", ui); PreferenceDocument::savePreferencesObject(preferences);
    const auto prepared = miacode::automatic_chart::prepare(workspace, files, miacode::automatic_chart::Trigger::Zip);
    if (!prepared.accepted) {
        QTextStream(stderr) << prepared.error << Qt::endl;
        const QString sample = workspace.document().difficulty(5)->chart;
        const auto norm = miacode::chart_transform::normalizeChartText(sample, {}, {true, false, true});
        const auto a = SimaiParser::parseForTimeline(sample), b = SimaiParser::parseForTimeline(norm.text);
        QTextStream(stderr) << "NORMALIZED: " << norm.text << "\nEND " << a.endSecond << "/" << b.endSecond
            << " duration " << a.durationSeconds << "/" << b.durationSeconds << " timing " << a.timingPoints.size() << "/" << b.timingPoints.size() << Qt::endl;
    }
    check(prepared.accepted && prepared.changed, "automatic normalization applies a real formatting change");
    check(workspace.snapshot().dirtyDifficultyIds.contains(6), "unsaved difficulty remains dirty");
    check(workspace.document().difficulty(2) && workspace.document().difficulty(2)->chart.trimmed() == "E",
          "intentional empty difficulty survives automatic preparation");
    const auto disk = SimaiDocument::fromText(QString::fromUtf8(read(path)));
    check(disk.difficulty(6) && disk.difficulty(6)->chart != workspace.document().difficulty(6)->chart,
          "normalization never silently saves unrelated unsaved notes");
    check(fieldValue(disk, "desid_6") == "001" && fieldValue(disk, "unknown").contains("& this"),
          "normalization preserves IDs and unknown metadata");
    const auto again = miacode::automatic_chart::prepare(workspace, files, miacode::automatic_chart::Trigger::Zip);
    check(again.accepted && !again.changed, "normalization is idempotent");
    const QByteArray external = read(path) + "\n&external=edit\n";
    check(write(path, external), "external edit staged");
    check(!files.save(5).accepted && read(path) == external, "section save refuses external modification");

    const QString duplicatePath = directory.filePath("duplicate.txt");
    const QByteArray duplicate = "&title=A\n&title=B\n&lv_5=12\n&inote_5=(120){4}1,E\n";
    write(duplicatePath, duplicate);
    miacode::ChartWorkspace duplicateWorkspace;
    miacode::ChartWorkspaceFileService duplicateFiles(duplicateWorkspace);
    duplicateFiles.open(duplicatePath);
    duplicateWorkspace.upsertExtraField("miacode_drop_start_5", "1");
    check(!duplicateFiles.save(miacode::ChartWorkspace::MetadataSection).accepted && read(duplicatePath) == duplicate,
          "automatic metadata cannot erase conflicting original fields");

    const QString otherName = directory.filePath("notes.simai");
    const QByteArray plainNotes = "(120){4}1,2,3,4,E\n";
    write(otherName, plainNotes);
    miacode::ChartWorkspace otherWorkspace;
    miacode::ChartWorkspaceFileService otherFiles(otherWorkspace);
    otherFiles.open(otherName);
    const auto otherOpen = miacode::automatic_chart::prepare(otherWorkspace, otherFiles, miacode::automatic_chart::Trigger::FirstOpen);
    check(otherOpen.accepted && !otherOpen.changed && read(otherName) == plainNotes,
          "first-open preparation is limited to maidata.txt, preserving other chart text files");

    const QString windowsPath = directory.filePath("windows-save.txt");
    const QByteArray windowsBytes = "&title=Windows round trip\r\n&lv_5=12\r\n&inote_5=(120)\r\n{4}1,2,3,4,E\r\n";
    write(windowsPath, windowsBytes);
    miacode::ChartWorkspace windowsWorkspace;
    miacode::ChartWorkspaceFileService windowsFiles(windowsWorkspace);
    // The runtime loader historically reads in QIODevice::Text mode.
    windowsWorkspace.openSource(QString::fromUtf8(windowsBytes).replace("\r\n", "\n"), windowsPath, 5);
    windowsFiles.observeCurrentDisk();
    windowsWorkspace.upsertExtraField("artistid", "0007");
    check(windowsFiles.save(miacode::ChartWorkspace::MetadataSection).accepted,
          "metadata saves from the runtime's LF representation");
    const auto afterMetadataSave = miacode::automatic_chart::prepare(windowsWorkspace, windowsFiles, miacode::automatic_chart::Trigger::Render);
    check(afterMetadataSave.accepted, "Windows CRLF output is not mistaken for an external edit before rendering");
    miacode::ChartWorkspace binaryOpened;
    miacode::ChartWorkspaceFileService binaryFiles(binaryOpened);
    write(windowsPath, windowsBytes);
    check(binaryFiles.open(windowsPath).accepted && !binaryOpened.document().difficulty(5)->chart.contains('\r'),
          "binary reads retain raw hashes but normalize logical CRLF chart text");
    check(binaryFiles.save(5).accepted && !read(windowsPath).contains("\r\r\n"),
          "saving a CRLF chart never doubles carriage returns");

    QTemporaryDir blocked;
    const QString blockedPath = blocked.filePath("maidata.txt");
    check(write(blockedPath, original) && write(blocked.filePath(".autosave"), "not a directory"), "blocked backup fixture prepared");
    miacode::ChartWorkspace blockedWorkspace;
    miacode::ChartWorkspaceFileService blockedFiles(blockedWorkspace);
    blockedFiles.open(blockedPath);
    const auto failed = miacode::automatic_chart::prepare(blockedWorkspace, blockedFiles, miacode::automatic_chart::Trigger::Render);
    check(!failed.accepted && read(blockedPath) == original && !blockedWorkspace.snapshot().dirty,
          "backup failure preserves both disk and workspace");
    QTextStream(stdout) << (failures ? "FAILED" : "ALL PASS") << Qt::endl;
    return failures ? 1 : 0;
}
