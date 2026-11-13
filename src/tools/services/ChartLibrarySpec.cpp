#include "app/services/ChartLibraryFiles.h"
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
    const QString root = QDir(base).filePath("library");
    const QByteArray identical = chart(QStringLiteral("Duplicate song"));
    for (int i = 0; i < 6; ++i)
        check(put(QDir(root).filePath(QStringLiteral("%1/maidata.txt").arg(i)), i < 2 ? identical : chart(i == 4 ? QStringLiteral("SongUniqueFour") : QStringLiteral("Song %1").arg(i))),
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
    put(QDir(root).filePath("new/maidata.txt"), chart(QStringLiteral("Newly added")));
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
    check(QFile::remove(QDir(root).filePath("new/maidata.txt")), "shrinking-index fixture removes one generated chart");
    model.refresh();
    check(settled(model) && model.totalCount() == 6, "refresh removes deleted entries");
    QObject::disconnect(resetConnection);
    check(resetObserved && observedOldTitle == oldTitle,
        "about-to-reset observers can still read the old index before backing storage shrinks");
    const QString first = QDir(root).filePath("0/maidata.txt"), second = QDir(root).filePath("1/maidata.txt");
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
    for (int i = 2; i < 6; ++i) model.openBuffer(QDir(root).filePath(QStringLiteral("%1/maidata.txt").arg(i)));
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

void recovery(const QString& base)
{
    clearRecovery();
    const QString root = QDir(base).filePath("recovery");
    const QString path = QDir(root).filePath("maidata.txt");
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
    }
    check(QDir().rename(offline,root), "recovery source directory returns");
    {
        Host host;
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

void benchmark(const QString& base)
{
    for (const int count : {1000, 10000}) {
        clearRecovery();
        const QString root = QDir(base).filePath(QStringLiteral("benchmark_%1").arg(count));
        QElapsedTimer creation; creation.start();
        bool created = true;
        for (int i = 0; i < count; ++i)
            created = put(QDir(root).filePath(QStringLiteral("maidata_%1.txt").arg(i)), chart(QStringLiteral("Generated %1").arg(i))) && created;
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
    const QString runtime = QDir(fixtures.path()).filePath("runtime");
    QDir().mkpath(runtime);
    put(QDir(runtime).filePath(".better-miacode-runtime"), "BetterMiacode.runtime.v1\n");
    put(QDir(runtime).filePath(".hand-order-runtime"), "MiaCodeHandOrder.runtime.v1\n");
    put(QDir(runtime).filePath(".sticking-runtime"), "MiaCodeSticking.runtime.v1\n");
    qputenv("BETTER_MIACODE_RUNTIME_ROOT", runtime.toUtf8());
    qputenv("MIACODE_HAND_ORDER_RUNTIME_ROOT", runtime.toUtf8());
    QString runtimeError;
    if (!miacode::runtime_paths::initialize(&runtimeError)) {
        QTextStream(stderr) << runtimeError << Qt::endl;
        return 2;
    }
    auto preferences = PreferenceDocument::loadPreferencesObject();
    preferences.insert("ui", QJsonObject{{"auto_normalize_enabled",false}});
    PreferenceDocument::savePreferencesObject(preferences);
    encodingAndNormalization(fixtures.path());
    fileTransactions(fixtures.path());
    indexAndBuffers(fixtures.path());
    recovery(fixtures.path());
    if (app.arguments().contains(QStringLiteral("--performance"))) benchmark(fixtures.path());
    QTextStream(stdout) << (failures ? "FAILED" : "ALL PASS") << " chart library specs: " << failures << " failures" << Qt::endl;
    return failures ? 1 : 0;
}
