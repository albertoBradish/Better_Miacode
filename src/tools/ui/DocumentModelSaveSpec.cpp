#include "app/services/HandOrderFeaturePreferences.h"
#include "app/ui/document/DocumentModel.h"
#include "app/services/PreferenceDocument.h"
#include "common/RuntimePaths.h"
#include "app/services/StickingPublicationMetadata.h"
#include <QDir>

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
class FakeBridge final : public miacode::DocumentBridge
{
public:
    QString sourceText() const override { return {}; }
    QString filePath() const override { return {}; }
    int activeDifficultyId() const override { return 5; }
    bool applyCommittedDocument(const QString&, const QString&, int, bool, quint64, CommitKind, bool) override
    { return true; }
    QVariantList recentDocumentEntries() override { return {}; }
    void noteRecentDocument(const QString&) override {}
    void removeRecentDocument(const QString&) override {}
    QVariantList backupDocumentEntries() override { return {}; }
    void restoreBackupDocument(const QString&) override {}
    miacode::chart_transform::ChartNormalizationOptions normalizationOptions() const override { return {}; }
    void setNormalizationOptions(const miacode::chart_transform::ChartNormalizationOptions&) override {}
    bool requestEditorNavigation(int, int, int, int, bool, bool, bool) override { return true; }
    void setDocumentSaveHandler(std::function<bool(const QString&)> value) override { save = std::move(value); }
    void setChartTextHandler(std::function<bool(const QString&)> value) override { chart = std::move(value); }
    void setLeaveDocumentHandler(std::function<void(std::function<void(bool)>)> value) override
    { leave = std::move(value); }
    void requestLeaveDocument(std::function<void(bool)> decided) override { leave(std::move(decided)); }
    bool applyDocumentDesignerSlots(const QVector<QPair<int, QString>>&, bool, const QString&) override
    { return false; }
    void reconcileUnifiedDocumentDesigner(UnifiedDesignerReconcileReason) override {}
    std::function<bool(const QString&)> save;
    std::function<bool(const QString&)> chart;
    std::function<void(std::function<void(bool)>)> leave;
};

bool expect(bool condition, const char* message)
{
    if (!condition) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return condition;
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool verifyDifficultyCardProjection()
{
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    workspace.openSource("&title=Song\n&artist=Artist\n&des=Shared\n"
                         "&lv_4=12\n&des_4=Expert designer\n&inote_4=(174.5){4}1,E\n"
                         "&lv_5=14+\n&inote_5=(200){4}A1,E\n", {}, 5);
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    const auto revision = workspace.snapshot().revision;
    const auto expert = model.difficultyCardTrack(4);
    const auto master = model.difficultyCardTrack(5);
    bool ok = expect(expert.value("difficulty") == "EXPERT" && expert.value("level") == "12"
        && expert.value("designer") == "Expert designer" && expert.value("bpm") == "174.5"
        && expert.value("mode") == "Standard" && master.value("difficulty") == "MASTER"
        && master.value("level") == "14+" && master.value("designer") == "Shared"
        && master.value("bpm") == "200" && master.value("mode") == "DX",
        "card projection uses the requested difficulty's level, designer, BPM and chart type");
    ok &= expect(model.currentDifficultyId() == 5 && workspace.snapshot().revision == revision
        && !workspace.snapshot().dirty && model.difficultyCardTrack(7).isEmpty(),
        "previewing a card neither selects an editor difficulty nor changes the document");
    workspace.updateDocumentField(miacode::ChartWorkspaceDocumentField::Title, "Renamed");
    workspace.updateDifficultyField(5, miacode::ChartWorkspaceDifficultyField::Level, "15");
    workspace.replaceDifficultyChart(5, "(180){4}1,E");
    const auto updated = model.difficultyCardTrack(5);
    ok &= expect(updated.value("title") == "Renamed" && updated.value("level") == "15"
        && updated.value("bpm") == "180" && updated.value("mode") == "Standard",
        "metadata and chart edits invalidate the card cache");
    workspace.openSource("&title=Next\n&wholebpm=150\n&lv_5=8\n&inote_5=(100){4}1,E\n", {}, 5);
    const auto replacement = model.difficultyCardTrack(5);
    ok &= expect(replacement.value("title") == "Next" && replacement.value("level") == "8"
        && replacement.value("bpm") == "150",
        "replacement documents refresh the card and preserve the whole-BPM priority");
    workspace.removeDifficulty(5);
    ok &= expect(model.difficultyCardTrack(5).isEmpty(), "deleted difficulties have no stale card");
    analysis.shutdown();
    return ok;
}

bool verifyUnsupportedOpenShowsError()
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const QString path = directory.filePath(QStringLiteral("非谱面 file.mp4"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("synthetic non-chart file") < 0) return false;
    file.close();
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    workspace.openSource(QStringLiteral("&title=keep\n&inote_5=(120){4}1,E\n"));
    workspace.replaceActiveDifficultyChart(QStringLiteral("(120){4}2,E"));
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    int notices = 0;
    int replacements = 0;
    QVariantMap notice;
    QObject::connect(&requests, &miacode::UiRequestService::noticeRequested,
        &model, [&](const QString&, const QVariantMap& value) { ++notices; notice = value; });
    QObject::connect(&model, &miacode::ui::DocumentModel::documentReplaced,
        &model, [&] { ++replacements; });
    const auto before = workspace.snapshot();
    bool ok = expect(!model.openFile(QUrl::fromLocalFile(path)) && notices == 1
        && notice.value("severity") == QLatin1String("error")
        && notice.value("title") == qtTrId("document.open_failed")
        && notice.value("text").toString().startsWith(qtTrId("document.unsupported_file_type"))
        && notice.value("text").toString().endsWith(QDir::toNativeSeparators(path)),
        "a pasted unsupported path returns false and presents a file-type error with its path");
    const auto after = workspace.snapshot();
    ok &= expect(after.revision == before.revision && after.sourceText == before.sourceText
        && after.filePath == before.filePath && after.dirty == before.dirty
        && after.documentOpenGeneration == before.documentOpenGeneration && replacements == 0,
        "a rejected open never replaces the edited document or publishes it to the runtime");
    const QString validPath = directory.filePath(QStringLiteral("chart.simai"));
    QFile validFile(validPath);
    if (!validFile.open(QIODevice::WriteOnly) || validFile.write("&title=next\n&inote_5=(120){4}1,E\n") < 0)
        return false;
    validFile.close();
    ok &= expect(model.openFile(QUrl::fromLocalFile(validPath)) && notices == 1
        && workspace.document().title == QLatin1String("next") && replacements == 1,
        "a supported chart can still be opened after dismissing the error");
    analysis.shutdown();
    return ok;
}

bool verifyStickingCopyOnOpen()
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const QString original = directory.filePath(QStringLiteral("maidata.txt"));
    const QString sticking = directory.filePath(QStringLiteral("maidata_sticking.txt"));
    const QByteArray initial = "&title=Original\n&lv_5=12\n&inote_5=(150){16}2,1,2,1,E\n";
    QFile source(original);
    if (!source.open(QIODevice::WriteOnly) || source.write(initial) != initial.size()) return false;
    source.close();
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    bool ok = expect(model.openFile(QUrl::fromLocalFile(original))
        && workspace.snapshot().filePath == sticking && read(sticking) == initial,
        "opening maidata creates and selects the Sticking copy");
    const QString infoPath = directory.filePath(QStringLiteral("Original MAS 拆分信息.txt"));
    ok &= expect(QString::fromUtf8(read(infoPath)).contains(QStringLiteral("BPM：00:00:000/150.00")),
        "creating the Sticking copy also writes per-difficulty publication information");
    QFile info(infoPath);
    if (!info.open(QIODevice::WriteOnly) || info.write("User edited") != 11) return false;
    info.close();
    model.setChartText(QStringLiteral("(150){16}2~r,1~l,2~r,1~l,E"));
    ok &= expect(model.saveWholeDocument() && read(original) == initial
        && read(sticking).contains("2~r,1~l,2~r,1~l"),
        "saving edits the Sticking copy and preserves original bytes");
    const QByteArray saved = read(sticking);
    ok &= expect(model.openFile(QUrl::fromLocalFile(original))
        && workspace.snapshot().filePath == sticking && read(sticking) == saved
        && model.chartText().contains("2~r,1~l,2~r,1~l"),
        "reopening maidata resumes the existing Sticking copy without replacing it");
    ok &= expect(read(infoPath) == "User edited", "reopening preserves edited publication information");
    ok &= expect(model.openFile(QUrl::fromLocalFile(directory.path()))
        && workspace.snapshot().filePath == sticking && read(original) == initial,
        "opening the project folder also selects its existing Sticking copy");
    analysis.shutdown();
    return ok;
}

bool verifyStickingPublicationMetadata()
{
    const auto document = SimaiDocument::fromText(QStringLiteral(
        "&title=曲名😀/A:B?\n&first=1.25\n&artist=曲师\n&des=默认谱师\n"
        "&lv_6=14+\n&des_6=难度谱师\n&inote_6=|| (888)\n(150){4}1,// (999)\n"
        "(75)2,|| (777)\n(75)3,(87.5)4,(150)5,E\n"));
    bool ok = expect(miacode::sticking::publicationVideoName(document, 6)
        == QStringLiteral("【舞萌DX手序拆解／读谱辅助】曲名😀／A：B？ REMAS 14+.mp4"),
        "publication video names retain Unicode, use REMAS, and replace illegal filename characters");
    const QString text = miacode::sticking::publicationInfoText(document, 6);
    ok &= expect(text.contains(QStringLiteral("曲名：曲名😀/A:B?\n曲师：曲师\n谱师：难度谱师\n"))
        && text.contains(QStringLiteral("BPM：00:01:250/150.00 - 00:01:650/75.00"))
        && text.contains(QStringLiteral("蓝色为左手，红色为右手"))
        && text.contains(QStringLiteral("星星自暗变为全亮时"))
        && !text.contains(QStringLiteral("999")) && !text.contains(QStringLiteral("888"))
        && !text.contains(QStringLiteral("777")),
        "publication information uses raw metadata and ordered BPM changes, excluding comments and repeated BPM");
    return ok;
}

bool verifyRejectedDraftBlocksPersistenceAndLeave()
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const QString path = directory.filePath("maidata.txt");
    const QString copy = directory.filePath("copy.txt");
    const QString source = "&title=old\n&custom=old\n&lv_5=12\n&inote_5=(120){4}1,E\n";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(source.toUtf8()) != source.toUtf8().size()) return false;
    file.close();
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    if (!files.open(path).accepted) return false;
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    model.setWholeSourceEditorActive(true);
    const auto revision = workspace.snapshot().revision;
    const QByteArray originalBytes = read(path);
    const QString rejected = "&custom=new\n&missing_equals\n&another=new";
    bool submitted = false;
    const auto submitDraft = QObject::connect(&model, &miacode::ui::DocumentModel::editingFinishedRequested,
        &model, [&] { if (!submitted) { submitted = true; model.setMetadataExtraText(rejected); } });
    bool ok = expect(!model.save() && submitted && model.metadataExtraText() == rejected
        && !model.metadataExtraError().isEmpty() && model.dirty()
        && model.dirtyEditorKeys().contains("metadata") && !workspace.snapshot().dirty
        && workspace.snapshot().revision == revision && read(path) == originalBytes,
        "input rejected during flush stays visible and dirty without changing workspace or file");
    QObject::disconnect(submitDraft);
    model.setMetadataTitle("new title");
    QCoreApplication::processEvents();
    ok &= expect(model.metadataExtraText() == rejected && read(path) == originalBytes,
        "unrelated metadata updates and autosave preserve the rejected draft");
    ok &= expect(!model.saveWholeDocument() && !model.saveDifficultySection(5)
        && !model.saveAs(QUrl::fromLocalFile(copy)) && !fake.save(copy)
        && !QFile::exists(copy) && read(path) == originalBytes,
        "all manual and bridge save entrances reject invalid input without publishing files");
    bool mayLeave = true;
    int decisions = 0;
    model.requestLeaveDocument([&](bool value) { mayLeave = value; ++decisions; });
    ok &= expect(!mayLeave && decisions == 1 && requests.pendingChoiceCount() == 0,
        "invalid input prevents document close before the clean-workspace fast path");
    model.requestLeaveCurrentField([&](bool value) { mayLeave = value; });
    ok &= expect(!mayLeave && model.metadataExtraText() == rejected,
        "invalid input prevents page navigation and retains its draft");
    model.requestCloseDifficulty(5);
    model.closeDocument();
    ok &= expect(requests.pendingChoiceCount() == 0 && model.hasDocument(),
        "invalid metadata prevents a difficulty close from hiding the input");
    model.setMetadataExtraText("&custom=new\n&another=new");
    ok &= expect(model.metadataExtraError().isEmpty() && model.saveWholeDocument()
        && !model.dirty() && read(path).contains("&custom=new") && read(path).contains("&another=new")
        && read(path).contains("&title=new title"),
        "correcting the draft saves every accepted field and clears its dirty state");
    model.setMetadataExtraText(rejected);
    ok &= expect(model.discardChanges() && model.metadataExtraError().isEmpty() && !model.dirty()
        && model.metadataExtraText().contains("&custom=new"),
        "explicit discard restores the save point and clears the rejected draft");
    model.setMetadataExtraText(rejected);
    workspace.openSource(source, path, 5, miacode::ChartDocumentOrigin::NetPreview);
    ok &= expect(model.metadataExtraError().isEmpty() && !model.dirty()
        && model.metadataExtraText().contains("&custom=old"),
        "a new document generation does not inherit another document's draft");
    model.setMetadataExtraText(rejected);
    ok &= expect(!model.save() && requests.pendingFileRequestCount() == 0 && workspace.isNetPreview(),
        "invalid online-preview input is rejected before requesting a save-as path");
    model.setMetadataExtraText("&custom=online copy");
    ok &= expect(model.saveAs(QUrl::fromLocalFile(copy)) && !workspace.isNetPreview()
        && !model.dirty() && read(copy).contains("&custom=online copy"),
        "corrected online-preview input can still be saved as a local document");
    int metadataNotifications = 0;
    QObject::connect(&model, &miacode::ui::DocumentModel::metadataChanged,
                     &model, [&] { ++metadataNotifications; });
    workspace.updateDocumentField(miacode::ChartWorkspaceDocumentField::First,
                                  QStringLiteral("0.375"));
    ok &= expect(metadataNotifications > 0 && model.currentDifficultyOffset() == QLatin1String("0.375"),
                 "a calibration write through the workspace updates the header offset projection");
    analysis.shutdown();
    return ok;
}
bool verifyPickerDirectories()
{
    QTemporaryDir directory;
    QDir root(directory.path());
    root.mkpath("charts"); root.mkpath("images"); root.mkpath("videos"); root.mkpath("documents");
    const QString chart = directory.filePath("charts/maidata.txt");
    const QString source = "&title=test\n&lv_5=12\n&inote_5=(120){4}1,E\n";
    PreferenceDocument::savePreferencesObject({{"app", QJsonObject{{"last_open_dir", directory.filePath("documents")}}}});
    miacode::ChartWorkspace workspace;
    workspace.openSource(source, chart, 5, miacode::ChartDocumentOrigin::NetPreview);
    miacode::ChartWorkspaceFileService files(workspace);
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    QString id;
    QVariantMap request;
    QObject::connect(&requests, &miacode::UiRequestService::fileRequested,
                     [&](const QString& nextId, const QVariantMap& value) { id = nextId; request = value; });
    const auto folder = [&] { return request.value("startFolder").toUrl().toLocalFile(); };
    int invalidAudioNotices = 0;
    QObject::connect(&requests, &miacode::UiRequestService::noticeRequested,
                     [&](const QString&, const QVariantMap&) { ++invalidAudioNotices; });
    model.createDocumentFromPickedAudio();
    const auto newFilters = request.value("nameFilters").toStringList();
    const auto openFilters = model.openFileNameFilters();
    bool ok = expect(newFilters.size() == 1 && openFilters.size() == 1
        && openFilters.first() == qtTrId("qml.simai_files_txt_simai"),
        "new picker offers audio only; open picker offers charts only");
    requests.submitFileResult(id, QUrl::fromLocalFile(directory.filePath("documents/not_audio.txt")));
    ok &= expect(invalidAudioNotices == 1 && !QFileInfo::exists(directory.filePath("documents/maidata.txt")),
                 "new chart rejects a non-audio path without creating a chart");
    ok &= expect(!model.openChartFile(QUrl::fromLocalFile(directory.filePath("documents/song.mp3")))
        && invalidAudioNotices == 2 && !QFileInfo::exists(directory.filePath("documents/maidata.txt")),
        "open chart rejects an audio path without creating a chart");
    model.saveWholeDocument();
    ok &= expect(folder() == directory.filePath("documents")
        && request.value("startFile").toUrl().fileName() == QLatin1String("maidata.txt"),
        "first network-preview save uses remembered documents folder and maidata name");
    requests.cancelFileRequest(id);
    ok &= expect(model.fileDialogLocation(false).value("folder").toUrl().toLocalFile() == directory.filePath("documents"),
                 "main open uses the persisted document folder");
    PreferenceDocument::savePreferencesObject({});
    ok &= expect(model.fileDialogLocation(true).value("folder").toUrl().toLocalFile() != QFileInfo(chart).absolutePath(),
                 "network preview with no history never suggests its cache directory");
    PreferenceDocument::savePreferencesObject({{"app", QJsonObject{{"last_open_dir", QFileInfo(chart).absolutePath()}}}});
    ok &= expect(model.fileDialogLocation(true).value("folder").toUrl().toLocalFile() != QFileInfo(chart).absolutePath(),
                 "a network cache stored by an older runtime is not proposed as a save destination");
    workspace.openSource(source, chart, 5);
    ok &= expect(model.fileDialogLocation(true).value("file").toUrl().toLocalFile() == chart,
                 "local save-as proposes the current chart path");
    const auto pick = [&](bool video, const QString& sourcePath) {
        if (video) model.importChartBackgroundVideo(); else model.importChartBackgroundImage();
        requests.submitFileResult(id, QUrl::fromLocalFile(sourcePath));
        if (video) model.importChartBackgroundVideo(); else model.importChartBackgroundImage();
        ok &= expect(folder() == QFileInfo(sourcePath).absolutePath(),
                     video ? "chart video import remembers its source" : "chart image import remembers its source");
        requests.cancelFileRequest(id);
    };
    pick(false, directory.filePath("images/picture.png"));
    pick(true, directory.filePath("videos/background.mp4"));
    model.importChartBackgroundImage();
    ok &= expect(folder() == directory.filePath("images"), "video selection does not overwrite image source history");
    requests.cancelFileRequest(id);
    root.mkpath("new_chart");
    const QString audioPath = directory.filePath("new_chart/song.mp3");
    QFile audio(audioPath);
    if (!audio.open(QIODevice::WriteOnly) || audio.write("audio fixture") < 0) return false;
    audio.close();
    model.createDocumentFromPickedAudio();
    requests.submitFileResult(id, QUrl::fromLocalFile(audioPath));
    ok &= expect(QFileInfo::exists(directory.filePath("new_chart/track.mp3"))
        && QFileInfo::exists(directory.filePath("new_chart/maidata.txt")),
        "new chart from an audio file creates a track and chart beside it");
    analysis.shutdown();
    return ok;
}

bool verifyPlainSimaiExport()
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const QString path = directory.filePath(QStringLiteral("maidata.txt"));
    const QString plainPath = directory.filePath(QStringLiteral("plain.txt"));
    const QByteArray annotated =
        "&title=A~B\n&lv_5=12\n&inote_5=(120){4}1~l,E\n";
    QFile source(path);
    if (!source.open(QIODevice::WriteOnly) || source.write(annotated) != annotated.size()) {
        return false;
    }
    source.close();

    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    if (!files.open(path).accepted) return false;
    miacode::AnalysisService analysis(workspace);
    analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    const auto before = workspace.snapshot();
    bool ok = expect(model.exportPlainSimai(QUrl::fromLocalFile(plainPath)),
                     "plain Simai export writes a separate file");
    const QByteArray exported = read(plainPath);
    ok &= expect(exported.contains("&title=A~B") && exported.contains("&inote_5=(120){4}1,E")
                     && !exported.contains("1~l") && read(path) == annotated,
                 "plain export strips chart annotations and preserves source metadata and file");
    ok &= expect(!model.exportPlainSimai(QUrl::fromLocalFile(path)) && read(path) == annotated,
                 "plain export refuses to overwrite the annotated source");
    ok &= expect(workspace.snapshot().revision == before.revision
                     && workspace.snapshot().dirty == before.dirty
                     && workspace.snapshot().filePath == before.filePath,
                 "plain export leaves workspace revision, dirty state and path unchanged");
    analysis.shutdown();
    return ok;
}
bool verifyFirstOpenAutomaticPreparation()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("maidata.txt");
    const QByteArray source = "&title=First open\n&lv_5=12\n&inote_5=(120){4}1,2,3,4,E\n";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size()) return false;
    file.close();
    auto preferences = PreferenceDocument::loadPreferencesObject();
    auto ui = preferences.value("ui").toObject(); ui.insert("auto_normalize_enabled", true);
    preferences.insert("ui", ui); PreferenceDocument::savePreferencesObject(preferences);
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService files(workspace);
    miacode::AnalysisService analysis(workspace); analysis.setDiagnosticsDeferred(true);
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    FakeBridge fake;
    miacode::DocumentBridge* bridge = &fake;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel model(notifications, workspace, files, analysis, requests, bridge, preview);
    model.setHandOrderFeaturesEnabled(true);
    bool ok = expect(model.openChartFile(QUrl::fromLocalFile(path)), "real document-model open path accepts fixture");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    const QString copy = directory.filePath("maidata_sticking.txt");
    ok &= expect(read(path) == source && read(copy) == source && workspace.snapshot().filePath == copy
        && !workspace.snapshot().dirty, "first open preserves source bytes and selects an unchanged Sticking copy");
    ok &= expect(!QDir(directory.filePath(".autosave")).exists(),
        "copy-on-open does not normalize the original or create a misleading backup");
    ok &= expect(model.openChartFile(QUrl::fromLocalFile(path)), "already-opened chart reopens");
    QCoreApplication::processEvents();
    ok &= expect(read(path) == source && read(copy) == source && workspace.snapshot().filePath == copy,
        "reopen retains both the original and existing Sticking copy");
    analysis.shutdown();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    app.setApplicationName(QStringLiteral("MiaCodeDocumentModelSaveSpec"));
    QTemporaryDir profile;
    if (!profile.isValid()) return 1;
    QDir().mkpath(profile.filePath("source")); QDir().mkpath(profile.filePath("runtime/spec"));
    const auto fixtureFile = [&](const QString& name, const QByteArray& data) {
        QFile file(profile.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    };
    if (!fixtureFile("baseline.json", "{}") || !fixtureFile("source/AGENTS.md", "synthetic fixture")
        || !fixtureFile("runtime/spec/.better-miacode-runtime", "BetterMiacode.runtime.v1\n")) return 1;
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", profile.filePath("runtime/spec").toUtf8());
    QString runtimeError;
    if (!miacode::runtime_paths::initializeFromEnvironment(&runtimeError)) {
        QTextStream(stderr) << runtimeError << Qt::endl;
        return 1;
    }
    if (miacode::hand_order_features::unlock(QStringLiteral("test-hand-order"))
        != miacode::hand_order_features::UnlockResult::Enabled) return 1;
    return verifyFirstOpenAutomaticPreparation() && verifyDifficultyCardProjection() && verifyUnsupportedOpenShowsError()
        && verifyStickingCopyOnOpen() && verifyStickingPublicationMetadata()
        && verifyRejectedDraftBlocksPersistenceAndLeave() && verifyPickerDirectories() && verifyPlainSimaiExport() ? 0 : 1;
}
