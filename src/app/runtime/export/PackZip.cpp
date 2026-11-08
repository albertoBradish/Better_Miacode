#include "app/services/AutomaticChartPreparation.h"
#include "app/services/PreferenceDocument.h"
#include "common/LocalizedText.h"

#include "app/runtime/export/VideoExportHost.h"
#include "app/runtime/Shared.h"
#include "app/runtime/document/DocumentSessionHost.h"
#include "app/runtime/shell/ShellHost.h"

#include "app/services/JobProgressService.h"
#include "app/services/UiRequestService.h"
#include "common/DebugLog.h"
#include "common/OperationLog.h"
#include "media_tools/zip_export/ChartZipPackager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QUrl>

using namespace miacode::runtime::shared;

void miacode::runtime::VideoExportHost::onPackAsZip()
{
    MC_OP("miacode::runtime::VideoExportHost::onPackAsZip");

    miacode::UiRequestService* const requests = session_.uiRequestService();
    if (requests == nullptr) {
        _mc_op_.fail(QStringLiteral("ui request service unavailable"));
        return;
    }

    const miacode::LocalizedText dialogTitle = miacode::localizedText("export.export_as_zip");

    auto& workspace = session_.applicationServices_.workspace();
    const auto before = workspace.snapshot();
    if (!before.hasDocument) return;
    const auto unchanged = [this, before] {
        const auto current = session_.applicationServices_.workspace().snapshot();
        return current.documentOpenGeneration == before.documentOpenGeneration && current.revision == before.revision;
    };
    const auto choosePath = [this, requests, dialogTitle, before, unchanged] {
        if (!unchanged()) {
            requests->postNotice(miacode::NoticeSeverity::Warning, dialogTitle, miacode::localizedText("export.document_changed_retry"));
            return;
        }
        const auto& document = session_.applicationServices_.workspace().document();
        const QString defaultName = miacode::zip_export::sanitizedZipStem(document.title) + QStringLiteral(".zip");
        miacode::FileRequest request;
        request.title = dialogTitle;
        request.startPath = before.filePath.isEmpty() ? defaultName : QFileInfo(before.filePath).absoluteDir().filePath(defaultName);
        request.nameFilters = {miacode::localizedText("file_filter.zip")};
        request.saveMode = true;
        requests->requestFile(request, [this, requests, dialogTitle, before, unchanged](const QString& picked) {
            if (picked.isEmpty()) return;
            if (!unchanged()) {
                requests->postNotice(miacode::NoticeSeverity::Warning, dialogTitle, miacode::localizedText("export.document_changed_retry"));
                return;
            }
            auto& services = session_.applicationServices_;
            const auto prepared = miacode::automatic_chart::prepare(services.workspace(), services.files(), miacode::automatic_chart::Trigger::Zip);
            if (!prepared.accepted) {
                requests->postNotice(miacode::NoticeSeverity::Error, dialogTitle, prepared.error);
                return;
            }
            const QString chartText = services.workspace().document().toText();
            packChartToZipAtPath(chartText, before.filePath, dialogTitle, picked);
        });
    };
    bool hasEmptyDifficulty = false;
    const auto timing = miacode::simai::buildTimingMetadata(workspace.document());
    for (const int id : workspace.document().difficultyIds()) {
        const auto* difficulty = workspace.document().difficulty(id);
        if (!difficulty) continue;
        const auto parsed = SimaiParser::parseForTimeline(difficulty->chart, timing);
        if (!parsed.ok) {
            requests->postNotice(miacode::NoticeSeverity::Warning, dialogTitle, miacode::localizedText("export.syntax_before_zip"));
            return;
        }
        if (parsed.noteMarkers.isEmpty()) hasEmptyDifficulty = true;
    }
    const bool warn = PreferenceDocument::loadPreferencesObject().value(QStringLiteral("ui")).toObject()
        .value(QStringLiteral("warn_empty_difficulty")).toBool(true);
    if (warn && hasEmptyDifficulty) {
        const QVariantList choices{
            QVariantMap{{"id", "continue"}, {"label", qtTrId("export.empty_difficulty_continue")}, {"role", ""}},
            QVariantMap{{"id", "reject"}, {"label", qtTrId("action.cancel")}, {"role", "reject"}, {"default", true}}
        };
        requests->requestChoice(dialogTitle, miacode::localizedText("export.empty_difficulty_warning"), choices,
            QStringLiteral("reject"), [choosePath](const QString& id) { if (id == QLatin1String("continue")) choosePath(); });
    } else choosePath();
}

void miacode::runtime::VideoExportHost::packChartToZipAtPath(
    const QString& chartText,
    const QString& chartPath,
    const miacode::LocalizedText& dialogTitle,
    const QString& pickedPath)
{
    MC_OP("miacode::runtime::VideoExportHost::packChartToZipAtPath");
    miacode::UiRequestService* const requests = session_.uiRequestService();
    miacode::JobProgressService* const jobProgress = session_.jobProgressService();
    if (requests == nullptr || jobProgress == nullptr || pickedPath.isEmpty()) {
        return;
    }

    QString outputPath = pickedPath;
    if (!outputPath.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        outputPath += QStringLiteral(".zip");
    }
    _mc_op_.note(QStringLiteral("out=%1").arg(outputPath));

    miacode::zip_export::ChartZipInput input;
    input.chartText = chartText;
    input.chartPath = chartPath;
    input.videoFieldValue = SimaiDocument::fromText(chartText).videoPath;
    input.outputZipPath = outputPath;

    jobProgress->begin(
        dialogTitle,
        miacode::localizedText("export.preparing_package"),
        /*cancellable=*/true);

    // The packager is synchronous and reports from the UI thread, so pump the
    // event loop at each entry: that is what repaints the overlay and delivers
    // the cancel click. Cancellation stays cooperative — we return false and the
    // packager unwinds itself.
    const auto onProgress = [jobProgress](int current, int total, const QString& entryName) -> bool {
        const int safeTotal = qMax(1, total);
        jobProgress->report(
            qRound(static_cast<double>(current - 1) * 100.0 / safeTotal),
            miacode::localizedText("export.packaging_1_2_3")
                .arg(current)
                .arg(total)
                .arg(entryName));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        return !jobProgress->cancelRequested();
    };

    const miacode::zip_export::ChartZipResult result =
        miacode::zip_export::packChartToZip(input, onProgress);
    jobProgress->end();

    if (result.canceled) {
        _mc_op_.note(QStringLiteral("canceled"));
        requests->postNotice(
            miacode::NoticeSeverity::Information,
            dialogTitle,
            miacode::localizedText("export.packaging_canceled"));
        return;
    }

    if (!result.ok) {
        _mc_op_.fail(result.errorMessage);
        requests->postNotice(
            miacode::NoticeSeverity::Error,
            dialogTitle,
            miacode::localizedText("export.packaging_failed_1").arg(result.errorMessage));
        return;
    }

    QString details = result.includedEntries.join(QLatin1Char('\n'));
    if (details.size() > 3000) {
        details = details.left(3000) + QStringLiteral("\n...");
    }
    requests->requestNoticeAction(
        miacode::NoticeSeverity::Information,
        dialogTitle,
        miacode::localizedText("export.exported_to_1_2_file")
            .arg(QDir::toNativeSeparators(outputPath))
            .arg(result.includedEntries.size())
            .arg(QString()),
        details,
        miacode::localizedText("action.open_folder"),
        [outputPath](bool openFolder) {
            if (!openFolder) {
                return;
            }
            const QString dir = QFileInfo(outputPath).absoluteDir().absolutePath();
            if (!dir.isEmpty()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            }
        });
}
