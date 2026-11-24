#include "app/services/ChartZipExport.h"
#include "app/services/ChartLibraryFiles.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "app/services/PreferenceDocument.h"
#include "core/chart/document/AdvancedMetadata.h"
#include "core/chart/document/SimaiDocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>

namespace miacode::library {
zip_export::ChartZipResult exportChartZip(zip_export::ChartZipInput input, const QVariantMap& options,
    const zip_export::ChartZipProgressFn& progress)
{
    if (options.value("normalize").toBool()) {
        const auto preview = PreferenceDocument::loadPreferencesObject().value("app").toObject().value("preview").toObject();
        const auto settings = app_preferences::chartNormalizationOptionsFromPreferences(preview, {true, false, true});
        QByteArray output;
        QString error;
        if (!normalize(input.chartText.toUtf8(), &output, &error, settings)) return {false, false, error};
        input.chartText = QString::fromUtf8(output);
    }
    const bool basic = options.value("basicMetadata").toBool();
    const bool paired = options.value("withoutPvCopy").toBool();
    if (!basic && !paired) return zip_export::packChartToZip(input, progress);

    auto document = SimaiDocument::fromText(input.chartText);
    if (basic) {
        static const QSet<QString> playbackFields{"wholebpm", "whole_time_signature", "clock_count",
            "seek", "wait", "pvstart", "pvlen", "demo_seek", "demo_len"};
        document.extraFields.removeIf([](const SimaiRawField& field) {
            return !playbackFields.contains(field.key.toLower());
        });
        for (const int slot : document.difficultyIds()) {
            auto& level = document.difficulty(slot)->level;
            if (!level.contains('.')) continue;
            const auto displayed = metadata::displayLevelForConstant(level);
            if (!displayed.isEmpty()) level = displayed;
        }
        input.chartText = document.toText();
    }
    if (!paired) return zip_export::packChartToZip(input, progress);

    const QFileInfo target(input.outputZipPath);
    QTemporaryDir stage(target.dir().filePath(".miacode-zip-pair-XXXXXX"));
    if (!stage.isValid()) return {false, false, stage.errorString()};
    auto companion = input;
    companion.outputZipPath = stage.filePath("without-pv.zip");
    companion.includeVideo = false;
    document.videoPath.clear();
    companion.chartText = document.toText();
    auto result = zip_export::packChartToZip(companion, progress);
    if (!result.ok) return result;

    // The picker only confirms overwriting the main file. Never overwrite an existing companion.
    const auto stem = target.completeBaseName() + QStringLiteral("_noPV");
    QString destination = target.dir().filePath(stem + ".zip");
    for (int suffix = 2; QFileInfo::exists(destination); ++suffix)
        destination = target.dir().filePath(QStringLiteral("%1_%2.zip").arg(stem).arg(suffix));
    if (!QFile::rename(companion.outputZipPath, destination))
        return {false, false, QStringLiteral("Cannot create %1").arg(destination)};
    result = zip_export::packChartToZip(input, progress);
    if (result.ok) result.companionPath = destination;
    else if (!QFile::remove(destination)) {
        result.canceled = false;
        result.errorMessage += QStringLiteral("\n%1").arg(destination);
    }
    return result;
}
}
