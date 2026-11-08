#include "app/services/AutomaticChartPreparation.h"
#include "app/services/ChartFileSafety.h"
#include "app/services/PreferenceDocument.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "editor/SpaceInputGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
