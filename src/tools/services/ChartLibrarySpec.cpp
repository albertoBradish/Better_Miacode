#include "app/services/ChartLibraryFiles.h"
#include "app/services/ChartZipExport.h"
#include <miniz.h>
#include <QDirIterator>
#include "app/services/ManagedChartLibrary.h"
#include "app/services/AutomaticChartPreparation.h"
#include "app/services/PreferenceDocument.h"
#include "app/ui/document/DocumentModel.h"
#include "app/ui/library/ChartLibraryModel.h"
#include "common/RuntimePaths.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/document/SimaiTimingMetadata.h"
#include "core/chart/parser/SimaiParser.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <cmath>
#include <QGuiApplication>
#include <QWindow>
#include <QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace files = miacode::library;
using miacode::ui::ChartLibraryModel;
using miacode::ui::LibraryDocument;
int failures = 0;

void check(bool value, const char* description)
{
    QTextStream(stdout) << (value ? "PASS " : "FAIL ") << description << Qt::endl;
    if (!value) ++failures;
}

bool put(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray get(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray chart(const QString& title)
{
    return (QStringLiteral("&title=") + title
        + QStringLiteral("\n&artist=Generated artist\n&version=QA\n&lv_5=12\n&inote_5=(120){4}1,2h[4:1],3-5[4:1],A1,E\n")).toUtf8();
}

void zipExportOptions(const QString& base)
{
    QDir folder(QDir(base).filePath("zip-options"));
    QDir().mkpath(folder.path());
    const QString path = folder.filePath("maidata.txt");
    const QByteArray bytes = "&title=Test\n&artist=Artist\n&des=Designer\n&first=0.25\n&clock_count=4\n"
        "&wholebpm=120\n&video=pv.mp4\n&version=Custom\n&level_constant_5=14.8\n&lv_5=14+\n"
        "&inote_5=(120){4}1,2,3,4,E\n&lv_4=13\n&inote_4=(120){4}4,3,2,1,E\n";
    put(path, bytes);
    put(folder.filePath("pv.mp4"), "video");
    put(folder.filePath("bg.jpg"), "image");
    put(folder.filePath("track.mp3"), "audio");
    const QString archive = folder.filePath("song.zip");
    const miacode::zip_export::ChartZipInput input{QString::fromUtf8(bytes), path, "pv.mp4", archive};
    const QVariantMap options{{"basicMetadata", true}, {"normalize", true}, {"withoutPvCopy", true}};
    const auto result = files::exportChartZip(input, options);
    check(result.ok && QFile::exists(result.companionPath) && get(path) == bytes,
        "paired normalized ZIP keeps the original chart byte-for-byte");
    for (const auto& zipPath : {archive, result.companionPath}) {
        mz_zip_archive zip{};
        const bool open = mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0);
        if (!open) { check(false, "exported ZIP opens"); continue; }
        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, "maidata.txt", &size, 0);
        const QByteArray text(static_cast<const char*>(data), qsizetype(size));
        mz_free(data);
        const bool hasVideo = zipPath == archive;
        check(text.contains("&inote_4=") && text.contains("&inote_5=") && text.contains("&first=0.25")
            && text.contains("&wholebpm=120") && text.contains("&lv_5=14+") && text.contains("&des=Designer")
            && !text.contains("&level_constant") && !text.contains("&version="), "basic ZIP retains timing and both charts");
        check((mz_zip_reader_locate_file(&zip, "pv.mp4", nullptr, 0) >= 0) == hasVideo
            && text.contains("&video=pv.mp4") == hasVideo && mz_zip_reader_locate_file(&zip, "bg.jpg", nullptr, 0) >= 0
            && mz_zip_reader_locate_file(&zip, "track.mp3", nullptr, 0) >= 0, "companion only omits video and its reference");
        mz_zip_reader_end(&zip);
    }
    const auto before = get(archive);
    const auto canceled = files::exportChartZip(input, options, [](int, int, const QString&) { return false; });
    check(canceled.canceled && get(archive) == before && folder.entryList({"*.zip"}, QDir::Files).size() == 2,
        "canceling paired ZIP preserves previous archives");
}

void clearRecovery()
{
    QDir runtime(QFileInfo(miacode::runtime_paths::configFile("preferences_v2.json")).absolutePath());
    runtime.cdUp();
    QFile::remove(runtime.filePath("sessions/chart-library.json"));
}

bool settled(ChartLibraryModel& model, int limitMs = 20000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (model.busy() && elapsed.elapsed() < limitMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return !model.busy();
}

class LibraryTestBridge final : public miacode::DocumentBridge
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

struct Host {
    miacode::ChartWorkspace workspace;
    miacode::ChartWorkspaceFileService fileService{workspace};
    miacode::AnalysisService analysis{workspace};
    miacode::ShellNotifications notifications;
    miacode::UiRequestService requests;
    LibraryTestBridge bridgeImpl;
    miacode::DocumentBridge* bridge = &bridgeImpl;
    miacode::PreviewSurface* preview = nullptr;
    miacode::ui::DocumentModel document{notifications, workspace, fileService, analysis, requests, bridge, preview};
    std::unique_ptr<ChartLibraryModel> library;
    Host()
    {
        analysis.setDiagnosticsDeferred(true);
        library = std::make_unique<ChartLibraryModel>(requests, document);
    }
    ~Host() { library.reset(); analysis.shutdown(); }
};

void independentSongTabs(const QString& base)
{
    QString error;
    const auto root = files::createLibrary(base,&error);
    const auto pathA = QDir(root).filePath("A/maidata.txt");
    const auto pathB = QDir(root).filePath("B/maidata.txt");
    const QByteArray a = "&title=Song A\n&inote_5=(120){4}1,E\n&inote_6=(120){4}2,E\n";
    const QByteArray b = "&title=Song B\n&inote_5=(140){4}3,E\n";
    check(put(pathA,a) && put(pathB,b), "independent song fixtures created");
    Host host;
    auto& document = host.document;
    QString pending;
    QObject::connect(&host.requests,&miacode::UiRequestService::choiceRequested,&document,
        [&](const QString& id,const QVariantMap&) {pending=id;});
    check(document.openFile(QUrl::fromLocalFile(pathA)), "first song opens normally");
    document.selectDifficulty(6);
    const QString editA = "(120){4}4,5,6,E";
    const QString editB = "(140){4}7,8,E";
    document.setChartText(editA);
    check(files::registerSong(root,pathA,"Test") && files::registerSong(root,pathB,"Test")
        && host.library->setRoot(root) && settled(*host.library), "both songs available through library list");
    QWindow window;
    window.show();
    QTest::keyPress(&window, Qt::Key_Control, Qt::ControlModifier);
    host.library->openFromList(pathB);
    QTest::keyRelease(&window, Qt::Key_Control);
    check(document.currentFilePath()==pathB && document.songTabs().size()==2 && pending.isEmpty(),
        "additive open retains the outgoing dirty song in the same document model");
    document.setChartText(editB);
    check(!document.openAdditionalSong(QDir(base).filePath("missing/maidata.txt"))
        && document.songTabs().size()==2 && document.currentFilePath()==pathB && document.chartText()==editB,
        "failed additive open leaves both tabs intact");
    check(document.openAdditionalSong(pathA) && document.currentSongTab()==0 && document.songTabs().size()==2
        && document.currentDifficultyId()==6 && document.chartText()==editA && document.dirty()
        && get(pathA)==a && get(pathB)==b, "reopening a song restores its unsaved difficulty without a duplicate tab");
    check(!document.saveAs(QUrl::fromLocalFile(pathB)) && get(pathB)==b,
        "Save As cannot overwrite the other open song");
    check(document.activateSongTab(1) && document.chartText()==editB && document.saveWholeDocument()
        && get(pathB).contains(editB.toUtf8()) && get(pathA)==a, "saving B leaves parked A untouched");
    bool exitApproved=true;
    host.bridgeImpl.requestLeaveAllSongs([&](bool accepted) {exitApproved=accepted;});
    check(!pending.isEmpty() && document.currentSongTab()==0, "window close visits unsaved background song");
    host.requests.submitChoiceResult(pending,"cancel");
    check(!exitApproved && document.dirty() && document.songTabs().size()==2, "cancel preserves both tabs");
    pending.clear();
    document.requestCloseSongTab(0);
    host.requests.submitChoiceResult(pending,"discard");
    check(document.songTabs().size()==1 && document.currentFilePath()==pathB && document.chartText().trimmed()==editB
        && get(pathA)==a, "discard closes only A and returns to B at its own save point");
}

void encodingAndNormalization(const QString& base)
{
    QString error, decoded;
    const QByteArray original = QByteArray("\xef\xbb\xbf")
        + "&title=Encoding fixture\r\n&unknown=keep = value\r\n"
          "&custom_multiline=first line\r\nsecond line\r\n"
          "&lv_5=12\r\n&inote_5=(120){4}1,2h[4:1],3-5[4:1],A1,E\r\n";
    check(files::decode(original, &decoded, &error), "UTF-8 BOM and CRLF decode");
    check(!decoded.startsWith(QChar(0xfeff)) && !decoded.contains('\r'), "buffer has no BOM glyph or CRLF duplication");
    check(files::encode(decoded, original) == original, "unchanged buffer round-trips BOM and CRLF exactly");
    QByteArray normalized;
    check(files::normalize(original, &normalized, &error), "valid chart normalization prepares bytes");
    check(normalized.startsWith("\xef\xbb\xbf") && !normalized.contains("\r\r\n"), "normalization retains BOM and valid CRLF");
    check(normalized.contains("&unknown=keep = value\r\n&custom_multiline=first line\r\nsecond line\r\n"),
        "unknown fields, multiline values and ordering survive normalization");
    QString normalizedText;
    files::decode(normalized, &normalizedText, &error);
    const auto before = SimaiDocument::fromText(decoded);
    const auto after = SimaiDocument::fromText(normalizedText);
    const auto* beforeDifficulty = before.difficulty(5);
    const auto* afterDifficulty = after.difficulty(5);
    check(beforeDifficulty && afterDifficulty, "normalization retains the difficulty field");
    if (!beforeDifficulty || !afterDifficulty) return;
    const auto beforeNotes = SimaiParser::parseForTimeline(beforeDifficulty->chart, miacode::simai::buildTimingMetadata(before));
    const auto afterNotes = SimaiParser::parseForTimeline(afterDifficulty->chart, miacode::simai::buildTimingMetadata(after));
    bool preserved = beforeNotes.ok && afterNotes.ok && beforeNotes.noteMarkers.size() == afterNotes.noteMarkers.size();
    for (int i = 0; preserved && i < beforeNotes.noteMarkers.size(); ++i) {
        const auto& a = beforeNotes.noteMarkers[i];
        const auto& b = afterNotes.noteMarkers[i];
        preserved = a.type == b.type && a.lane == b.lane && a.endLane == b.endLane
            && std::abs(a.second - b.second) < 1e-8 && std::abs(a.endSecond - b.endSecond) < 1e-8
            && a.slideDisplayKey == b.slideDisplayKey && a.isBreak == b.isBreak && a.isEx == b.isEx
            && a.headBreak == b.headBreak && a.trackBreak == b.trackBreak && a.isMine == b.isMine;
    }
    check(preserved, "normalization preserves representative note types, lanes, timings and attributes");
    const auto native = SimaiParser::parseForTimeline(QStringLiteral("(120){4}1,2,3,4,E"));
    const auto moved = SimaiParser::parseForTimeline(QStringLiteral("(120){4}8,2,3,4,E"));
    check(!miacode::automatic_chart::normalizationPreservesGameplay(native, moved),
        "same count and end time do not permit a changed lane");
    auto retimed = native;
    if (!retimed.noteMarkers.isEmpty()) retimed.noteMarkers[0].second += 0.1;
    check(!miacode::automatic_chart::normalizationPreservesGameplay(native, retimed),
        "same count and end time do not permit shifted note timing");
    check(!files::normalize("&title=A\n&title=B\n&inote_5=(120){4}1,E\n", &normalized, &error),
        "duplicate fields block normalization instead of silently collapsing values");
    check(!files::decode(QByteArray("\xc3\x28", 2), &decoded, &error), "invalid UTF-8 is rejected");
    const QString oversized = QDir(base).filePath("oversized.txt");
    QFile large(oversized);
    check(large.open(QIODevice::WriteOnly) && large.resize(files::kMaximumFileBytes + 1), "oversized fixture created");
    large.close();
    QByteArray bytes;
    check(!files::read(oversized, &bytes, &error), "files over the 4 MiB edit cap are rejected");
}

void fileTransactions(const QString& base)
{
    const QString root = QDir(base).filePath("transactions");
    const QString first = QDir(root).filePath("first/maidata.txt");
    const QString second = QDir(root).filePath("second/maidata.txt");
    const QString outside = QDir(base).filePath("outside/maidata.txt");
    const QByteArray a = chart(QStringLiteral("First")), b = chart(QStringLiteral("Second"));
    check(put(first, a) && put(second, b) && put(outside, a), "transaction fixtures created");
    check(files::withinRoot(root, first) && !files::withinRoot(root, outside), "canonical path boundary excludes sibling directories");
    QString error;
    QVector<files::Change> changes{{first, a, a + "|| A\n", {}}, {second, b + "stale", b + "|| B\n", {}}};
    const QString journal = QDir(base).filePath("journals/transaction.json");
    check(!files::apply(root, &changes, journal, &error) && get(first) == a && get(second) == b,
        "external conflict in a later file prevents every batch write");
    check(!QFileInfo::exists(QDir(root).filePath("first/.autosave")), "full preflight happens before backup writes");

    changes[1].before = b;
    const QString blocked = QDir(root).filePath("second/.autosave");
    check(put(blocked, "blocked"), "backup failure fixture created");
    check(!files::apply(root, &changes, journal, &error) && get(first) == a && get(second) == b,
        "backup failure leaves all source bytes unchanged");
    QFile::remove(blocked);
    const QString invalidJournal = QDir(base).filePath("journal_directory");
    QDir().mkpath(invalidJournal);
    check(!files::apply(root, &changes, invalidJournal, &error) && get(first) == a && get(second) == b,
        "journal creation failure occurs before source mutation");

    QVector<int> checkpoints;
    check(!files::apply(root, &changes, journal + ".cancel", &error, [&](int index) {
        checkpoints.append(index);
        return index == 0;
    }), "canceling at the second write stops a partially applied transaction");
    check(checkpoints == QVector<int>{0, 1} && get(first) == a && get(second) == b,
        "mid-batch cancellation restores the first file and leaves the next file untouched");
    check(QJsonDocument::fromJson(get(journal + ".cancel")).object().value("state") == "rolled_back",
        "cancellation journal records completed rollback");
    const QByteArray intervening = chart(QStringLiteral("Outside edit during cancellation"));
    check(!files::apply(root, &changes, journal + ".external", &error, [&](int index) {
        if (index == 0) return true;
        check(put(first, intervening), "external-write fixture replaces a previously committed file");
        return false;
    }), "transaction stops when cancellation follows an outside edit");
    check(get(first) == intervening && get(second) == b,
        "rollback never overwrites bytes modified outside the transaction");
    check(QJsonDocument::fromJson(get(journal + ".external")).object().value("state") == "recovery_required",
        "rollback conflict is recorded for manual recovery");
    put(first, a);

    check(files::apply(root, &changes, journal, &error), "batch writes all prepared changes");
    check(get(changes[0].backup) == a && get(changes[1].backup) == b, "each backup contains original bytes");
    const auto logged = QJsonDocument::fromJson(get(journal)).object();
    check(logged.value("state") == "complete" && logged.value("written").toInt() == 2,
        "journal records completed file count");
    check(get(first) == changes[0].after && get(second) == changes[1].after, "committed bytes match the preview");
    const QByteArray external = chart(QStringLiteral("External edit"));
    put(second, external);
    check(!files::undo(root, changes, journal + ".undo", &error)
        && get(first) == changes[0].after && get(second) == external,
        "undo preflights all files and preserves an outside edit");
    put(second, changes[1].after);
    check(files::undo(root, changes, journal + ".undo", &error) && get(first) == a && get(second) == b,
        "undo restores all original bytes after a successful preflight");

    const QString link = QDir(root).filePath("redirect.txt");
#ifdef Q_OS_WIN
    const bool linked = CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(outside).utf16()), 0x2) != 0;
#else
    const bool linked = QFile::link(outside, link);
#endif
    if (linked) {
        check(!files::withinRoot(root, link), "symbolic-link chart path is rejected");
        QVector<files::Change> redirected{{link, a, b, {}}};
        check(!files::apply(root, &redirected, journal + ".link", &error) && get(outside) == a,
            "transaction cannot write through a symbolic link");
        bool swapped = false;
        check(!files::apply(root, &changes, journal + ".redirected", &error, [&](int index) {
            if (index == 0) return true;
            swapped = QFile::remove(second) && QFile::rename(link, second);
            return swapped;
        }), "path replacement after preflight is rejected at the write boundary");
        check(swapped && get(first) == a && get(outside) == a,
            "late symlink replacement cannot escape the root and prior writes roll back");
        QFile::remove(second);
        put(second, b);
        QFile::remove(link);
    } else QTextStream(stdout) << "SKIP symbolic-link creation is unavailable on this host" << Qt::endl;
}

void indexAndBuffers(const QString& base)
{
    clearRecovery();
    const QString parent = QDir(base).filePath("library");
    QDir().mkpath(parent);
    QString error;
    const QString root = files::createLibrary(parent, &error);
    // A pre-bundle library remains readable until its first catalog mutation.
    QFile::remove(files::bundleFile(root));
    put(QDir(root).filePath("miacode-library.json"),R"({"format":"BetterMiacode.ChartLibrary","version":1})");

    check(!root.isEmpty(), "managed test library created");
    const QByteArray identical = chart(QStringLiteral("Duplicate song"));
    for (int i = 0; i < 6; ++i)
        check(put(QDir(root).filePath(QStringLiteral("packs/Test/%1/maidata.txt").arg(i)), i < 2 ? identical : chart(i == 4 ? QStringLiteral("SongUniqueFour") : QStringLiteral("Song %1").arg(i))),
            "index fixture created");
    check(put(QDir(base).filePath("not_selected/maidata.txt"), chart(QStringLiteral("Outside"))), "unselected fixture created");
    Host host;
    auto& model = *host.library;
    check(model.rootPath().isEmpty() && model.totalCount() == 0 && !model.busy(), "new library does not scan an implicit directory");
    check(model.setRoot(root) && settled(model), "explicit root scan completes");
    check(model.totalCount() == 6, "only charts below the chosen root are indexed");
    model.setDuplicatesOnly(true);
    check(model.rowCount() == 2, "duplicate filter uses identical content hashes");
    model.setDuplicatesOnly(false);
    model.setQuery(QStringLiteral("SongUniqueFour"));
    check(model.rowCount() == 1, "search intersects title and other metadata terms");
    model.setQuery({});
    const int previousCount = model.totalCount();
    put(QDir(root).filePath("packs/Test/new/maidata.txt"), chart(QStringLiteral("Newly added")));
    model.refresh(); model.cancel();
    check(settled(model) && model.totalCount() == previousCount, "canceled refresh retains the previous complete index");
    model.refresh();
    check(settled(model) && model.totalCount() == 7, "explicit refresh discovers newly created directories");
    const auto oldLast = model.index(model.rowCount() - 1, 0);
    const QString oldTitle = model.data(oldLast, ChartLibraryModel::TitleRole).toString();
    QString observedOldTitle;
    bool resetObserved = false;
    const auto resetConnection = QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&] {
        resetObserved = true;
        observedOldTitle = model.data(oldLast, ChartLibraryModel::TitleRole).toString();
    });
    check(QFile::remove(QDir(root).filePath("packs/Test/new/maidata.txt")), "shrinking-index fixture removes one generated chart");
    model.refresh();
    check(settled(model) && model.totalCount() == 6, "refresh removes deleted entries");
    QObject::disconnect(resetConnection);
    check(resetObserved && observedOldTitle == oldTitle,
        "about-to-reset observers can still read the old index before backing storage shrinks");
    const QString first = QDir(root).filePath("packs/Test/0/maidata.txt"), second = QDir(root).filePath("packs/Test/1/maidata.txt");
    model.openBuffer(first);
    auto* a = qobject_cast<LibraryDocument*>(model.activeSession());
    model.openBuffer(second);
    auto* b = qobject_cast<LibraryDocument*>(model.activeSession());
    check(a && b && a != b && model.sessions().size() == 2, "two documents own independent live buffers");
    if (!a || !b) return;
    const QString before = a->text();
    a->setText(before + "|| draft A\n");
    const auto editedRevision = a->revision();
    check(a->dirty() && a->canUndo() && !b->dirty() && b->text() == before, "editing one buffer leaves the other untouched");
    a->undo();
    check(!a->dirty() && a->text() == before && a->canRedo() && a->revision() > editedRevision,
        "undo changes revision and returns to the saved baseline");
    a->redo();
    b->setText(b->text() + "|| draft B\n"); b->undo();
    check(a->dirty() && !b->dirty(), "undo stacks remain independent across active-tab switches");
    check(!model.setRoot(QDir(base).filePath("not_selected")) && model.rootPath() == QFileInfo(root).canonicalFilePath(),
        "changing root cannot discard open documents");
    for (int i = 2; i < 6; ++i) model.openBuffer(QDir(root).filePath(QStringLiteral("packs/Test/%1/maidata.txt").arg(i)));
    check(model.sessions().size() == 5, "buffer count is bounded at five documents");
    model.activateBuffer(0);
    model.saveActive();
    check(settled(model) && !a->dirty() && get(first).contains("draft A"), "saving a buffer writes only its own content");
    a->setText(a->text() + "|| newer A\n");
    const QByteArray external = chart(QStringLiteral("External replacement"));
    put(first, external);
    model.saveActive();
    check(a->dirty() && get(first) == external, "external disk modification blocks buffered save");
    model.selectPath(first, true);
    model.prepareNormalization();
    const int preparedSelectionCount = model.selectedCount();
    model.selectPath(second, true);
    const bool changedDuringPreparation = model.selectedCount() != preparedSelectionCount;
    check(settled(model), "normalization preview preparation completes asynchronously");
    check(!changedDuringPreparation || model.proposals().isEmpty(),
        "selection changes during preparation cannot install an obsolete batch proposal");
    model.applyPrepared();
    check(get(first) == external, "batch apply cannot overwrite an open buffered document");
    model.activateBuffer(0);
    QString requestId;
    QObject::connect(&host.requests, &miacode::UiRequestService::noticeRequested, &host.requests,
        [&](const QString& id, const QVariantMap&) { requestId = id; });
    const auto sessionCount = model.sessions().size();
    model.closeActive();
    check(!requestId.isEmpty(), "dirty buffer close requires an explicit decision");
    host.requests.submitNoticeResult(requestId, false);
    check(model.sessions().size() == sessionCount && model.activeSession() == a,
        "canceling dirty close preserves the buffer and its edits");
}

void importPacks(const QString& base)
{
    clearRecovery();
    QString error;
    const QString root = files::createLibrary(base, &error);
    const QDir source(QDir(base).filePath("Pack A"));
    const QString nested = source.filePath("unsorted/sub/renamed.txt");
    const QByteArray dx = chart("Twin[DX]") + "&shortid=18\n&genre=maimai\n&video=media/clip.mp4\n";
    put(nested, dx);
    put(source.filePath("unsorted/sub/track.ogg"), "audio");
    put(source.filePath("unsorted/sub/media/clip.mp4"), "video");
    put(source.filePath("readme.txt"), "ordinary text");
    put(source.filePath("谱面/18/version/other/maidata.txt"), chart("Twin[SD]"));
    const QString packB = QDir(base).filePath("Pack B");
    put(QDir(packB).filePath("odd.simai"), chart("Party") + "&shortid=161852\n&genre=宴会場\n");
    put(source.filePath("unsorted/sub/bg.png"), "art");
    const QDir standard(QFileInfo(source.filePath("谱面/18/version/other/maidata.txt")).dir());
    put(standard.filePath("track.wav"), "audio");
    put(standard.filePath("bg.png"), "art");
    put(QDir(packB).filePath("track.wav"), "audio");
    put(QDir(packB).filePath("bg.png"), "art");
    Host host;
    auto& model = *host.library;
    check(!model.setRoot(source.path()), "ordinary folders cannot become libraries");
    check(model.setRoot(root) && settled(model), "empty managed library opens");
    model.importFolders({source.path(), packB});
    check(settled(model) && model.totalCount() == 3 && model.packs().size() == 2,
        "batch import detects renamed Simai charts and excludes unrelated files");
    model.setPack("Pack A");
    model.setGenre("maimai");
    check(model.rowCount() == 1 && model.data(model.index(0), ChartLibraryModel::SongIdRole) == "10018",
        "combined pack and genre filters distinguish the DX ID");
    const auto imported = model.data(model.index(0), ChartLibraryModel::PathRole).toString();
    check(get(imported) == dx && get(nested) == dx && get(QFileInfo(imported).dir().filePath("track.ogg")) == "audio"
        && get(QFileInfo(imported).dir().filePath("media/clip.mp4")) == "video",
        "import preserves chart bytes, track and nested referenced video");
    model.setPack({});
    model.setGenre({});
    model.setQuery("Twin[SD]");
    check(model.rowCount() == 1 && model.data(model.index(0), ChartLibraryModel::SongIdRole) == "18",
        "a recognized archive directory supplies the missing standard ID");
    model.setQuery("161852");
    check(model.rowCount() == 1 && model.data(model.index(0), ChartLibraryModel::KindRole) == "UTAGE",
        "extended banquet ID survives import and search");
    model.setQuery({});
    model.importFolders({source.path(), packB});
    check(settled(model) && model.totalCount() == 3, "repeat import is idempotent");
}

void smartImport(const QString& base)
{
    QString error;
    const QString root = files::createLibrary(base, &error);
    const QDir inputs(QDir(base).filePath("smart-inputs"));
    const QByteArray common = "&title=Innocent\n&artist=QA\n&des=Test Author\n&genre=Original\n";
    const QByteArray master = common + "&lv_5=13+\n&inote_5=(120){4}1,2,E\n";
    const QByteArray remaster = common + "&lv_6=14\n&inote_6=(120){4}4,3,E\n";
    const auto putSong = [&](const QString& folder, const QByteArray& bytes) {
        put(inputs.filePath(folder + "/maidata.txt"), bytes);
        put(inputs.filePath(folder + "/bg.png"), "test jacket");
        put(inputs.filePath(folder + "/track.wav"), "test audio");
    };
    const std::atomic_bool cancel{false};
    putSong("Pack A", master);
    QList<int> percent;
    const auto first = files::importChartFolders(root, {inputs.filePath("Pack A")}, cancel, {}, {}, false,
        [&](int value){percent.append(value);});
    check(!percent.isEmpty() && percent.first()==0 && percent.last()==100 && std::is_sorted(percent.begin(),percent.end()),
        "import reports monotone percentages through completion");
    check(first.imported == 1, "batch import accepts a chart with both media assets");
    const auto indexed=files::bundleSongs(root);
    if (indexed.size()!=1) {check(false,"master is indexed");return;}
    QString canonical=indexed.first().path;
    check(canonical==inputs.filePath("Pack A/maidata.txt") && QDir(root).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot).size()==1,
        "import stores a bundle reference only");
    const auto makeZip = [&](const QString& filename, const QList<QPair<QByteArray,QByteArray>>& members) {
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_heap(&archive, 0, 0)) return false;
        bool ok = true;
        for (const auto& [name, bytes] : members)
            ok = ok && mz_zip_writer_add_mem(&archive, name.constData(), bytes.constData(), bytes.size(), MZ_BEST_SPEED);
        void* data = nullptr; size_t size = 0;
        ok = ok && mz_zip_writer_finalize_heap_archive(&archive, &data, &size)
            && put(filename, QByteArray(static_cast<const char*>(data), qsizetype(size)));
        mz_free(data); mz_zip_writer_end(&archive);
        return ok;
    };
    const QString zip = inputs.filePath("对照曲包.zip");
    check(makeZip(zip, {{"music/maidata.txt", remaster}, {"music/bg.png","art"}, {"music/track.wav","track"}}), "ZIP fixture created");
    auto merged = files::importChartFolders(root, {zip}, cancel);
    canonical=files::bundleSongs(root).first().path;
    const auto combined = get(canonical);
    check(get(inputs.filePath("Pack A/maidata.txt"))==master,"merge preserves the external master");
    check(merged.merged == 1 && !merged.imported && combined.startsWith(master)
        && combined.count("&title=") == 1 && combined.contains("&inote_6="), "ZIP complements a chart without rewriting its original difficulty");
    auto again = files::importChartFolders(root, {zip}, cancel);
    check(again.existing == 1 && !again.merged && !again.imported, "ZIP reimport skips the same difficulty");
    putSong("Different source", master);
    auto duplicate = files::importChartFolders(root, {inputs.filePath("Different source")}, cancel);
    check(duplicate.existing == 1 && !duplicate.imported, "duplicate detection is independent of path and pack");
    putSong("Conflict", QByteArray(master).replace("1,2,E", "8,7,E"));
    auto conflict = files::importChartFolders(root, {inputs.filePath("Conflict")}, cancel);
    check(conflict.conflicts == 1 && get(canonical) == combined, "overlapping difficulty conflict preserves indexed chart");
    putSong("Metadata", QByteArray(remaster).replace("genre=Original", "genre=Changed"));
    check(files::importChartFolders(root, {inputs.filePath("Metadata")}, cancel).conflicts == 1, "disagreeing global metadata blocks a merge");
    putSong("Other author", QByteArray(master).replace("Test Author", "Other Author"));
    check(files::importChartFolders(root, {inputs.filePath("Other author")}, cancel).imported == 1, "different chart credits remain independent");
    put(inputs.filePath("Missing/alternate.simai"), "&title=No media\n&inote_5=(120){4}1,E\n");
    put(inputs.filePath("Missing/no-fields.txt"), "ordinary text accepted by the standalone editor");
    put(inputs.filePath("Missing/no-title.txt"), "&inote_5=(120){4}1,E\n");
    put(inputs.filePath("Missing/metadata-only.txt"), "&title=No chart\n&lv_5=13\n&inote_5= \n&artist=QA\n");
    const QString secondZip = inputs.filePath("Mixed.zip");
    check(makeZip(secondZip, {{"normal/alternate.txt", "&title=ZIP without media\n&inote_4=(120){4}1,E\n"},
                              {"normal/readme.txt", "&title=No chart\n&inote_5= \n&artist=QA\n"}}),
        "content-validation ZIP fixture created");
    const auto mixed = files::importChartFolders(root, {inputs.filePath("Missing"), secondZip}, cancel);
    check(mixed.imported == 2 && mixed.failed == 0,
        "mixed batch imports valid alternate filenames without requiring a designer or media");
    const auto extra = QDir(root).filePath("packs/legacy/copy/maidata.txt");
    put(extra,master);
    check(files::registerSong(root,extra,"legacy"),"register legacy edition");
    auto cleaned=files::importChartFolders(root,{},cancel,{},{canonical},true);
    check(cleaned.existing==1 && QFileInfo::exists(extra),"consolidating references preserves original files");
    const QDir out(QDir(base).filePath("sorted-export"));QDir().mkpath(out.path());
    const auto exported=files::exportLibrary(root,out.path(),cancel,{},&error);
    check(!exported.isEmpty() && files::isManagedLibrary(exported),"export writes a self-contained bundle");
    check(QFileInfo(QDir(exported).filePath("按谱师查找/Test Author")).isDir()
        && QFileInfo(QDir(exported).filePath("按难度查找/Master")).isDir(),"export has both browse indexes");
#ifdef Q_OS_WIN
    const QDir authorsDir(QDir(exported).filePath("按谱师查找/Test Author"));
    const auto links=authorsDir.entryInfoList({"*.lnk"},QDir::AllEntries|QDir::NoDotAndDotDot);
    check(!links.isEmpty() && QFileInfo(links.first().symLinkTarget()).isDir()
        && QFileInfo::exists(QDir(links.first().symLinkTarget()).filePath("maidata.txt")),
        "a published author shortcut resolves to the song folder");
#endif
    const auto unsafeZip = inputs.filePath("unsafe.zip");
    check(makeZip(unsafeZip, {{"../escape.txt","blocked"}})
        && files::importChartFolders(root, {unsafeZip}, cancel).failed == 1, "archive path outside extraction folder is rejected");

    for (const auto& song : files::bundleSongs(root)) {
        if (song.pack != "Pack A") continue;
        const auto editing = files::editableChartPath(root, song.path, song.record);
        const auto unrelated = QFileInfo(editing).dir().filePath("notes.md");
        check(!editing.isEmpty() && put(unrelated, "keep me"), "packed chart has a private working copy");
        check(files::removeCollection(root, "Pack A", {editing}) == files::CollectionRemoval::InUse,
            "open working copies protect packs against deletion");
        check(files::removeCollection(root, "Pack A", {}) == files::CollectionRemoval::Removed
                && !QFileInfo::exists(editing) && get(unrelated) == "keep me"
                && get(inputs.filePath("Pack A/maidata.txt")) == master && QFileInfo::exists(zip),
            "pack deletion removes private copies and preserves external originals and unrelated files");
        break;
    }
}

void collectionLifecycle(const QString& base)
{
    clearRecovery();
    QString error;
    const QString root = files::createLibrary(base, &error);
    const QDir library(root);
    const auto native = library.filePath("local-project/alternate.simai");
    const auto original = QDir(base).filePath("native-original/maidata.txt");
    const auto invalid = library.filePath("registered-before-upgrade/maidata.txt");
    const auto privateDirectory = library.filePath("packs/Legacy/0123456789abcdef-fedcba9876543210");
    const auto legacy = QDir(privateDirectory).filePath("maidata.txt");
    const auto relatedTrack = QDir(privateDirectory).filePath("track.wav");
    const auto unrelated = QDir(privateDirectory).filePath("notes.md");
    check(put(original, chart("Native save")) && put(library.filePath("local-project/notes.md"), "project notes")
            && put(invalid, "not a Simai chart") && files::registerSong(root, invalid, "Old")
            && put(legacy, chart("Legacy copy")) && put(relatedTrack, "owned track")
            && put(unrelated, "private folder note") && put(library.filePath("packs/Legacy/.source"),
                QFileInfo(original).absolutePath().toUtf8())
            && put(QDir(privateDirectory).filePath("source.json"),
                QJsonDocument(QJsonObject{{"path", original}}).toJson())
            && files::registerSong(root, legacy, "Legacy"),
        "legacy and modern library lifecycle fixtures created");
    Host host;
    auto& model = *host.library;
    check(model.setRoot(root) && settled(model) && model.totalCount() == 1 && !model.hasSong(invalid),
        "refresh hides invalid existing registrations");
    check(host.document.openFile(QUrl::fromLocalFile(original)) && host.document.saveAs(QUrl::fromLocalFile(native)),
        "native editor saves a chart under the open library");
    check(QTest::qWaitFor([&] { return model.hasSong(native) && !model.busy(); }, 6000),
        "successful native Save As registers an eligible chart automatically");
    check(!model.deleteCollection("Legacy", "wrong") && QFileInfo::exists(legacy),
        "pack deletion requires its exact typed name");
    check(model.deleteCollection("Legacy", "Legacy") && settled(model) && !model.packs().contains("Legacy")
            && !QFileInfo::exists(legacy) && !QFileInfo::exists(relatedTrack) && get(unrelated) == "private folder note",
        "legacy pack deletion removes verified managed copies and preserves unrelated siblings");
    check(model.deleteCollection({}, model.collectionName()) && model.rootPath().isEmpty()
            && !files::isManagedLibrary(root) && QFileInfo::exists(native) && QFileInfo::exists(original)
            && QFileInfo::exists(invalid) && get(unrelated) == "private folder note",
        "library deletion unregisters the collection and keeps native and external files");
}

void songManager(const QString& base)
{
    clearRecovery();
    QString error;
    const QString root = files::createLibrary(base, &error);
    // A pre-bundle library remains readable until its first catalog mutation.
    QFile::remove(files::bundleFile(root));
    put(QDir(root).filePath("miacode-library.json"),R"({"format":"BetterMiacode.ChartLibrary","version":1})");

    const QString file = QDir(root).filePath("packs/Pack/song/maidata.txt");
    const QByteArray original = QByteArray("\xef\xbb\xbf") + "&title=First\r\n&artist=QA\r\n&shortid=10018\r\n"
        "&genre=Old genre\r\n&version=One\r\n&keep=custom\r\n&lv_4=10\r\n&inote_4=(120){4}1,2,E\r\n"
        "&lv_5=13+\r\n&inote_5=(120){4}1,2,3,4,E\r\n";
    put(file, original);
    const QString track = QFileInfo(file).dir().filePath("track.wav");
    put(track, "original audio");
    Host host;
    auto& model = *host.library;
    check(model.setRoot(root) && settled(model), "managed song fixture indexed");
    const auto detail = model.songDetails(file);
    check(detail["songId"] == "10018" && detail["difficulties"].toList().size() == 2
        && detail["track"].toUrl().toLocalFile() == track, "song details bind identity, difficulties and track");
    int noteCount = -1;
    QObject::connect(&model, &ChartLibraryModel::noteCountReady, &model, [&](const QString&, int id, int notes) {
        if (id == 5) noteCount = notes;
    });
    model.requestNoteCount(file, 5);
    QElapsedTimer wait; wait.start();
    while (noteCount < 0 && wait.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    check(noteCount == 4, "count is derived from the selected difficulty");
    const auto stamp = detail["stamp"].toString();
    const auto next = model.stageSongFields(file, stamp, {{"title","Edited"},{"level_constant_5","13.8"},{"des_4","Author"}});
    check(!next.isEmpty() && get(file) == original && model.checkpoint(), "form edits stay in a recoverable draft until saved");
    check(model.stageSongFields(file, stamp, {{"artist","stale"}}).isEmpty(), "stale form cannot overwrite newer draft");
    check(model.stageSongFields(file, next, {{"title","injected\n&artist=bad"}}).isEmpty(), "multiline metadata rejected");
    check(model.saveSong(file) && settled(model), "explicit save writes and refreshes the song");
    auto expected = original;
    expected.replace("&title=First", "&title=Edited");
    expected.insert(3, "&des_4=Author\r\n&level_constant_5=13.8\r\n");
    check(get(file) == expected && model.sessions().isEmpty(), "save preserves notes, other fields, BOM and line endings");
    model.changeCategory("genre", {}, "Unused");
    check(settled(model) && model.genres().contains("Unused"), "unused genre is stored in the library");
    model.changeCategory("genre", "Old genre", "New genre");
    check(settled(model) && get(file).contains("&genre=New genre") && model.categoryUsage("genre","New genre") == 1,
        "category rename updates member songs");
    model.changeCategory("genre", "New genre", {});
    check(model.genres().contains("New genre"), "referenced category cannot be removed");
    model.undoBatch();
    check(settled(model) && get(file) == expected, "undo restores category and song together");
    const auto categorised = model.stageSongFields(file, model.songDetails(file)["stamp"].toString(), {{"miacode_version_category","Custom"}});
    check(!categorised.isEmpty() && model.saveSong(file) && settled(model), "version category can be assigned independently");
    model.changeCategory("miacode_version_category", "Custom", "Local");
    check(settled(model) && model.versions().contains("Local") && get(file).contains("&version=One")
        && get(file).contains("&miacode_version_category=Local"), "renaming a version category preserves the source release");
    model.stageSongFields(file, model.songDetails(file)["stamp"].toString(), {{"title","Discard me"}});
    model.discardSongEdits(file);
    check(model.songDetails(file)["fields"].toMap()["title"] == "Edited", "discard returns to the saved song");
    const QString extra = model.createSong("Pack", "Second", 4);
    check(!extra.isEmpty() && settled(model) && model.totalCount() == 2, "create song in selected pack");
    const QString source = QDir(base).filePath("Imported Folder");
    put(QDir(source).filePath("random.txt"), chart("Third"));
    model.importFolders({source}, "Pack");
    check(settled(model) && model.totalCount() == 3 && model.packs() == QStringList{"Pack"},
        "import songs into an existing pack without creating another pack");
    model.importFolders({source}, "Pack");
    check(settled(model) && model.totalCount() == 3, "repeat targeted import is idempotent");
    const QString audio = QDir(base).filePath("replacement.wav");
    put(audio, "replacement audio");
    model.replaceSongAudio(file, audio);
    check(settled(model) && get(track) == "replacement audio" && get(audio) == "replacement audio",
        "audio replacement copies into managed storage");
    const QDir backups(QDir(root).filePath("media-backups"));
    const auto backupList = backups.entryList(QDir::Files);
    check(backupList.size() == 1 && get(backups.filePath(backupList.value(0))) == "original audio", "old audio is recoverable");
    QString request;
    QObject::connect(&host.requests, &miacode::UiRequestService::noticeRequested, &model,
        [&](const QString& id, const QVariantMap&) { request = id; });
    model.removeSong(extra);
    host.requests.submitNoticeResult(request, false);
    check(QFileInfo::exists(extra), "canceling removal keeps the song");
    model.removeSong(extra);
    host.requests.submitNoticeResult(request, true);
    check(settled(model) && QFileInfo::exists(extra) && model.totalCount() == 2, "removal unlinks the record and preserves its source");
}

void recovery(const QString& base)
{
    clearRecovery();
    const QString parent = QDir(base).filePath("recovery");
    QDir().mkpath(parent);
    QString error;
    const QString root = files::createLibrary(parent, &error);
    // A pre-bundle library remains readable until its first catalog mutation.
    QFile::remove(files::bundleFile(root));
    put(QDir(root).filePath("miacode-library.json"),R"({"format":"BetterMiacode.ChartLibrary","version":1})");

    check(!root.isEmpty(), "managed test library created");
    const QString path = QDir(root).filePath("packs/Test/maidata.txt");
    const QByteArray original = QByteArray("\xef\xbb\xbf") + "&title=Recover\r\n&lv_5=1\r\n&inote_5=(120){4}1,E\r\n";
    put(path, original);
    QString draft;
    {
        Host host;
        check(host.library->setRoot(root) && settled(*host.library), "recovery fixture indexes");
        host.library->openBuffer(path);
        auto* buffer = qobject_cast<LibraryDocument*>(host.library->activeSession());
        check(buffer != nullptr, "recovery fixture opens independently");
        if (!buffer) return;
        draft = buffer->text() + "|| recovered draft\n";
        buffer->setText(draft);
        QDir runtime(QFileInfo(miacode::runtime_paths::configFile("preferences_v2.json")).absolutePath());
        runtime.cdUp();
        const QString recoveryFile=runtime.filePath("sessions/chart-library.json");
        QFile::remove(recoveryFile);
        check(QDir().mkpath(recoveryFile), "blocked recovery destination prepared");
        check(!host.library->checkpoint() && buffer->dirty() && buffer->text()==draft && get(path)==original,
            "failed recovery checkpoint blocks closing unsaved drafts without touching source bytes");
        check(QDir().rmdir(recoveryFile) && host.library->checkpoint(), "recovery checkpoint can be retried after fixing storage");
    }
    check(get(path) == original, "saving session recovery never writes the source chart");
    const QString offline=root+"_offline";
    check(QDir().rename(root,offline), "recovery source directory becomes temporarily unavailable");
    {
        Host unavailable;
        check(unavailable.library->sessions().isEmpty() && unavailable.library->checkpoint(),
            "unavailable sources retain deferred recovery without creating empty live buffers");
        QDir runtime(QFileInfo(miacode::runtime_paths::configFile("preferences_v2.json")).absolutePath()); runtime.cdUp();
        const QString record=runtime.filePath("sessions/chart-library.json");
        QFile::remove(record);check(QDir().mkpath(record),"deferred recovery failure fixture created");
        check(!unavailable.library->checkpoint(),"unavailable dirty drafts also block closing if recovery cannot be saved");
        check(QDir().rmdir(record) && unavailable.library->checkpoint(),"deferred draft checkpoint retries without source files");
    }
    check(QDir().rename(offline,root), "recovery source directory returns");
    {
        Host host;
        check(settled(*host.library), "restored library indexed");
        auto* restored = qobject_cast<LibraryDocument*>(host.library->activeSession());
        check(restored && restored->dirty() && restored->text() == draft, "dirty text and path recover from isolated session storage");
        host.library->saveActive();
        check(settled(*host.library) && restored && !restored->dirty(), "recovered document can save against its original hash");
        const auto saved = get(path);
        check(saved.startsWith("\xef\xbb\xbf") && saved.contains("\r\n") && !saved.contains("\r\r\n"),
            "recovered save preserves BOM and CRLF style");
    }
    clearRecovery();
}

void batchLibraryActions(const QString& base)
{
    clearRecovery();
    QString error;
    const QString root = files::createLibrary(base, &error);
    QDir library(root);
    const QString two = library.filePath("tracks/two/alternate.simai");
    const QString three = library.filePath("tracks/three/maidata.txt");
    const QString ten = library.filePath("tracks/ten/maidata.txt");
    const QString twenty = library.filePath("tracks/twenty/maidata.txt");
    check(put(two, "&title=Song 2\n&artist=Beta\n&version=GreeN PLUS\n&genre=maimai\n"
        "&lv_5=13+\n&des_5=Beta\n&inote_5=(160){4}1,E\n")
        && put(three, "&title=Song 3\n&artist=Alpha\n&version=Custom\n&wholebpm=190\n"
            "&lv_4=13+\n&level_constant_4=14.1\n&des_4=Alpha\n&inote_4=(90){4}2,E\n"
            "&lv_5=13\n&des_5=Wrong designer\n&inote_5=(90){4}3,E\n")
        && put(ten, "&title=Song 10\n&artist=Gamma\n&version=maimai DX PRiSM\n&genre=maimai\n"
            "&lv_4=13?\n&des_4=Gamma\n&inote_4=(140){4}4,E\n")
        && put(twenty, "&title=Song 20\n&des=Default designer\n&lv_4=13.6\n&inote_4=(172){4}1,E\n"
            "&lv_5=14\n&level_constant_5=13.9\n&inote_5=(172){4}A1,E\n")
        && put(QFileInfo(two).dir().filePath("track.mp3"), "audio")
        && put(QFileInfo(ten).dir().filePath("track.mp3"), "audio"), "batch action fixtures created");
    for (const auto& path : {two, three, ten, twenty})
        check(files::registerSong(root, path, "Batch"), "batch fixture registered");

    Host host;
    auto& model = *host.library;
    check(model.setRoot(root) && settled(model) && model.rowCount() == 4, "batch action fixtures indexed");
    const auto order = [&model] {
        QStringList paths;
        for (int i = 0; i < model.rowCount(); ++i) paths.append(model.pathAt(i));
        return paths;
    };
    check(order() == QStringList{two, three, ten, twenty}, "title sort uses natural digit order");
    model.setSortColumn(ChartLibraryModel::SortVersion);
    check(order() == QStringList{two, ten, three, twenty}, "ascending version order puts custom releases after official versions");
    model.setSortDescending(true);
    check(order() == QStringList{three, ten, two, twenty}, "descending treats custom versions as newest and keeps missing versions last");
    model.setSortDescending(false);
    model.setSortColumn(ChartLibraryModel::SortConstant);
    check(order() == QStringList{ten, two, twenty, three},
        "highest constant uses 13? as 13.0 and 13+ as 13.6 when explicit constants are missing");
    model.setSortColumn(ChartLibraryModel::SortDesigner);
    check(order().value(0) == three, "designer is selected using the highest chart constant, including lower slots");
    model.setSortColumn(ChartLibraryModel::SortBpm);
    check(order() == QStringList{ten, two, twenty, three}, "BPM sort prefers explicit track BPM and falls back to chart BPM");
    model.setGenre(model.uncategorized());
    check(order() == QStringList{twenty, three}, "uncategorized filter selects songs without a genre");
    model.setGenre({});
    check(model.genreSuggestions().contains("maimai") && model.versionSuggestions().contains("PRiSM")
        && model.versionCategorySuggestions().contains("CiRCLE PLUS"), "empty catalog fields offer official suggestions");

    model.setSortColumn(ChartLibraryModel::SortTitle);
    model.toggleBatchRow(two, false);
    model.toggleBatchRow(ten, true);
    check(model.selectedCount() == 3, "Shift selection includes the anchor and intervening visible rows");
    QVariantMap queued;
    QObject::connect(&model, &ChartLibraryModel::renderQueuePrepared, &model,
        [&](const QVariantMap& charts) { queued = charts; });
    model.queueSelectedForRender({4, 5});
    check(settled(model) && queued.size() == 2 && queued.value(two).toList() == QVariantList{5}
        && queued.value(ten).toList() == QVariantList{4}, "render queue uses exact chart paths and skips missing audio and difficulties");
    model.selectVisible(false);
    model.selectPath(twenty, true);
    QString requestId;
    QObject::connect(&host.requests, &miacode::UiRequestService::noticeRequested, &host.requests,
        [&](const QString& id, const QVariantMap&) { requestId = id; });
    const auto beforeCleanup = get(twenty);
    model.prepareMetadataCleanup();
    check(settled(model) && model.proposals().size() == 1 && get(twenty) == beforeCleanup,
        "metadata cleanup stages every correction in one proposal without modifying the chart");
    model.applyPrepared();
    check(!requestId.isEmpty(), "applying metadata cleanup requests confirmation once");
    host.requests.submitNoticeResult(requestId, true);
    check(settled(model), "applied metadata cleanup refreshes the library");
    const auto cleaned = get(twenty);
    check(cleaned.contains("&wholebpm=172"), "batch BPM comes from the first chart tempo");
    check(cleaned.contains("&version=FESTiVAL"), "batch version detects the newest required chart syntax");
    check(cleaned.contains("&level_constant_4=13.6") && cleaned.contains("&lv_4=13+"),
        "batch decimal migration moves the value and derives a display level");
    check(cleaned.contains("&lv_5=13+") && cleaned.contains("&inote_5=(172){4}A1,E"),
        "batch level derivation preserves chart bodies");
    model.undoBatch();
    check(settled(model) && get(twenty) == beforeCleanup, "one undo restores every automatic metadata correction");

    const QDir output(library.filePath("exported"));
    check(put(output.filePath("Song 20.zip"), "previous ZIP"), "ZIP naming collision fixture created");
    QString picker;
    QObject::connect(&host.requests, &miacode::UiRequestService::fileRequested, &host.requests,
        [&](const QString& id, const QVariantMap&) { picker = id; });
    model.chooseSelectedZipOutput({{"withoutPvCopy", true}});
    host.requests.submitFileResult(picker, QUrl::fromLocalFile(output.path()));
    const bool zipped = settled(model);
    const auto archive = output.filePath("Song 20 (2).zip");
    mz_zip_archive zip{};
    const bool opened = zipped && mz_zip_reader_init_file(&zip, archive.toUtf8().constData(), 0);
    QByteArray contents;
    if (opened) {
        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, "maidata.txt", &size, 0);
        contents = QByteArray(static_cast<const char*>(data), qsizetype(size));
        mz_free(data);
        mz_zip_reader_end(&zip);
    }
    check(opened && contents.contains("&inote_4=") && contents.contains("&inote_5=")
        && get(output.filePath("Song 20.zip")) == "previous ZIP"
        && QFile::exists(output.filePath("Song 20 (2)_noPV.zip")),
        "batch ZIP includes every difficulty and preserves existing archives");
}

void benchmark(const QString& base)
{
    for (const int count : {1000, 10000}) {
        clearRecovery();
        const QString parent = QDir(base).filePath(QStringLiteral("benchmark_%1").arg(count));
        QDir().mkpath(parent);
        QString error;
        const QString root = files::createLibrary(parent, &error);
    // A pre-bundle library remains readable until its first catalog mutation.
    QFile::remove(files::bundleFile(root));
    put(QDir(root).filePath("miacode-library.json"),R"({"format":"BetterMiacode.ChartLibrary","version":1})");

        check(!root.isEmpty(), "managed test library created");
        QElapsedTimer creation; creation.start();
        bool created = true;
        for (int i = 0; i < count; ++i)
            created = put(QDir(root).filePath(QStringLiteral("packs/Test/maidata_%1.txt").arg(i)), chart(QStringLiteral("Generated %1").arg(i))) && created;
        check(created, "performance fixtures contain generated charts only");
        const auto createMs = creation.elapsed();
        Host host;
        QElapsedTimer timer; timer.start();
        check(host.library->setRoot(root) && settled(*host.library, 120000) && host.library->totalCount() == count,
            "performance index count matches the synthetic directory");
        const auto initialMs = timer.restart();
        host.library->refresh(); check(settled(*host.library, 120000), "incremental performance refresh completes");
        const auto refreshMs = timer.restart();
        host.library->setQuery(QStringLiteral("Generated 987"));
        const auto filterMs = timer.elapsed();
        QTextStream(stdout) << "BENCHMARK " << QJsonDocument(QJsonObject{{"files",count},{"create_ms",createMs},
            {"initial_scan_ms",initialMs},{"cached_refresh_ms",refreshMs},{"filter_ms",filterMs}}).toJson(QJsonDocument::Compact) << Qt::endl;
    }
    clearRecovery();
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir fixtures(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT "/chart-library-spec-XXXXXX"));
    if (!fixtures.isValid()) return 2;
    const QString runtime = qEnvironmentVariable("BETTER_MIACODE_RUNTIME_ROOT");
    if (runtime.isEmpty()) return 2;
    QDir().mkpath(runtime);
    put(QDir(runtime).filePath(".better-miacode-runtime"), "BetterMiacode.runtime.v1\n");
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", runtime.toUtf8());
    QString runtimeError;
    if (!miacode::runtime_paths::initializeFromEnvironment(&runtimeError)
        || !miacode::runtime_paths::configureProcessEnvironment(&runtimeError)) {
        QTextStream(stderr) << runtimeError << Qt::endl;
        return 2;
    }
    auto preferences = PreferenceDocument::loadPreferencesObject();
    preferences.insert("ui", QJsonObject{{"auto_normalize_enabled",false}});
    PreferenceDocument::savePreferencesObject(preferences);
    independentSongTabs(fixtures.path());
    encodingAndNormalization(fixtures.path());
    zipExportOptions(fixtures.path());
    fileTransactions(fixtures.path());
    importPacks(fixtures.path());
    smartImport(fixtures.path());
    collectionLifecycle(fixtures.path());
    songManager(fixtures.path());
    indexAndBuffers(fixtures.path());
    batchLibraryActions(fixtures.path());
    recovery(fixtures.path());
    if (app.arguments().contains(QStringLiteral("--performance"))) benchmark(fixtures.path());
    QTextStream(stdout) << (failures ? "FAILED" : "ALL PASS") << " chart library specs: " << failures << " failures" << Qt::endl;
    return failures ? 1 : 0;
}
