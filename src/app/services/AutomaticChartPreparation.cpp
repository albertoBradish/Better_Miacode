#include "app/services/AutomaticChartPreparation.h"
#include "app/services/ChartFileSafety.h"
#include "app/services/ChartNormalizationPreferences.h"
#include "app/services/PreferenceDocument.h"
#include "core/chart/document/AdvancedMetadata.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace miacode::automatic_chart {
namespace {

QByteArray gameplaySignature(const SimaiParseResult& parsed)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    const auto time = [](double value) { return qRound64(value * 10000000.0); };
    // durationSeconds includes the last empty comma's grid sample. Formatting
    // changes that sample density; E and the actual note tails define musical time.
    out << time(parsed.endSecond) << time(parsed.endQuarterBeat);
    out << qint64(parsed.noteMarkers.size());
    for (const auto& n : parsed.noteMarkers) {
        out << time(n.second) << time(n.endSecond) << time(n.slideTraceSecond)
            << n.type << n.lane << n.endLane << n.slideDisplayKey << n.slideTrackKey << n.slideSegmentKeys
            << n.touchPad << n.touchPoint << n.tapUsesStarMaterial << n.tapStarDouble
            << n.isEach << n.isBreak << n.isEx << n.isFirework << n.isMine
            << n.headBreak << n.headEx << n.trackBreak << n.trackMine << n.headMine
            << n.hasHeadStar << n.headlessImmediate << n.slideHeadUsesTapMaterial << time(n.hsMultiplier);
        out << qint64(n.slideSegmentDurations.size());
        for (const double t : n.slideSegmentDurations) out << time(t);
        for (const double t : n.slideSegmentShootSeconds) out << time(t);
        out << n.slideSegmentPoints << n.wifiLanePoints;
    }
    out << qint64(parsed.timingPoints.size());
    for (const auto& p : parsed.timingPoints)
        out << time(p.second) << time(p.quarterBeat) << time(p.bpm)
            << p.meterNumerator << p.meterDenominator << p.meterExplicit;
    return bytes;
}

bool normalize(SimaiDocument* document, const chart_transform::ChartNormalizationOptions& options,
               QMap<int, QString>* originals, QString* error)
{
    const auto timing = simai::buildTimingMetadata(*document);
    for (const int id : document->difficultyIds()) {
        auto* difficulty = document->difficulty(id);
        if (!difficulty || difficulty->chart.trimmed().isEmpty()) continue;
        const auto report = SimaiParser::buildValidationReport(difficulty->chart, SimaiValidationLocale::English, timing);
        if (report.errorCount > 0) {
            *error = QStringLiteral("Automatic normalization stopped: difficulty %1 has syntax errors.").arg(id);
            return false;
        }
        const auto before = SimaiParser::parseForTimeline(difficulty->chart, timing);
        // An intentionally empty difficulty is handled by ZIP confirmation,
        // not turned into an automatic-normalization error or a phantom chart.
        if (before.noteMarkers.isEmpty()) continue;
        const auto result = chart_transform::normalizeChartText(difficulty->chart, timing, options);
        if (!result.ok) { *error = result.errorMessage; return false; }
        if (result.text == difficulty->chart) continue;
        const auto after = SimaiParser::parseForTimeline(result.text, timing);
        if (gameplaySignature(before) != gameplaySignature(after)) {
            *error = QStringLiteral("Automatic normalization would change timing or note semantics in difficulty %1.").arg(id);
            return false;
        }
        if (originals) originals->insert(id, difficulty->chart);
        difficulty->chart = result.text;
    }
    return true;
}

QString identityKey(const QString& path)
{
    const QFileInfo info(path);
    QString identity = info.canonicalFilePath();
    if (identity.isEmpty()) identity = info.absoluteFilePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace

bool normalizationPreservesGameplay(const SimaiParseResult& before, const SimaiParseResult& after)
{ return gameplaySignature(before) == gameplaySignature(after); }

Result prepare(ChartWorkspace& workspace, ChartWorkspaceFileService& files, Trigger trigger)
{
    const auto snapshot = workspace.snapshot();
    if (!snapshot.hasDocument || workspace.isNetPreview() || snapshot.filePath.isEmpty()) return {};
    if (trigger == Trigger::FirstOpen
        && QFileInfo(snapshot.filePath).fileName().compare(QStringLiteral("maidata.txt"), Qt::CaseInsensitive) != 0)
        return {};
    auto preferences = PreferenceDocument::loadPreferencesObject();
    const auto ui = preferences.value(QStringLiteral("ui")).toObject();
    auto seen = preferences.value(QStringLiteral("auto_normalize_opened")).toObject();
    const QString key = identityKey(snapshot.filePath);
    if (trigger == Trigger::FirstOpen && seen.value(key).toBool()) return {};
    const auto markOpened = [&] {
        // Reload so a concurrent preference edit is not replaced by this snapshot.
        auto current = PreferenceDocument::loadPreferencesObject();
        auto opened = current.value(QStringLiteral("auto_normalize_opened")).toObject();
        opened.insert(key, true);
        current.insert(QStringLiteral("auto_normalize_opened"), opened);
        PreferenceDocument::savePreferencesObject(current);
    };
    if (!ui.value(QStringLiteral("auto_normalize_enabled")).toBool(true)) {
        if (trigger == Trigger::FirstOpen) markOpened();
        return {};
    }
    QFile source(snapshot.filePath);
    if (!source.open(QIODevice::ReadOnly)) return {false, false, source.errorString()};
    const QByteArray originalBytes = source.readAll();
    if (source.error() != QFileDevice::NoError) return {false, false, source.errorString()};
    source.close();
    const auto originalText = ChartWorkspaceFileService::decodeDocumentText(originalBytes);
    if (!originalText) return {false, false, QStringLiteral("The chart text could not be decoded.")};
    const QStringList conflicts = metadata::conflictingKeys(*originalText);
    if (!conflicts.isEmpty()) return {false, false, QStringLiteral("Conflicting metadata must be resolved before automatic normalization: %1").arg(conflicts.join(QStringLiteral(", ")))};
    const SimaiDocument originalDisk = SimaiDocument::fromText(*originalText);
    if (originalDisk.toText() != SimaiDocument::fromText(workspace.savedSourceText()).toText())
        return {false, false, QStringLiteral("The chart was modified outside this session. Reopen it before automatic normalization.")};
    const auto preview = preferences.value(QStringLiteral("app")).toObject().value(QStringLiteral("preview")).toObject();
    const auto options = app_preferences::chartNormalizationOptionsFromPreferences(preview, chart_transform::ChartNormalizationOptions{true, false, true});
    SimaiDocument normalizedWorkspace = workspace.document();
    SimaiDocument normalizedDisk = originalDisk;
    QMap<int, QString> originals;
    QString error;
    if (!normalize(&normalizedWorkspace, options, &originals, &error)
        || !normalize(&normalizedDisk, options, nullptr, &error)) return {false, false, error};
    const bool diskChanged = normalizedDisk.toText() != originalDisk.toText();
    if (originals.isEmpty() && !diskChanged) {
        if (trigger == Trigger::FirstOpen) markOpened();
        return {};
    }
    if (!chart_file_safety::backupBytes(snapshot.filePath, originalBytes, nullptr, &error)) return {false, false, error};
    if (snapshot.dirty && !chart_file_safety::backupBytes(snapshot.filePath, snapshot.sourceText.toUtf8(), nullptr, &error))
        return {false, false, error};
    const QByteArray expected = QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256);
    if (workspace.snapshot().revision != snapshot.revision || chart_file_safety::fileDigest(snapshot.filePath) != expected)
        return {false, false, QStringLiteral("The chart changed while preparing normalization.")};
    if (diskChanged) {
        QSaveFile target(snapshot.filePath);
        target.setDirectWriteFallback(false);
        const QByteArray bytes = normalizedDisk.toText().toUtf8();
        if (!target.open(QIODevice::WriteOnly) || target.write(bytes) != bytes.size()) return {false, false, target.errorString()};
        if (chart_file_safety::fileDigest(snapshot.filePath) != expected) {
            target.cancelWriting();
            return {false, false, QStringLiteral("The chart changed before normalization could be saved.")};
        }
        if (!target.commit()) return {false, false, target.errorString()};
    }
    workspace.applyNormalizedDocument(normalizedWorkspace, normalizedDisk.toText(), originals);
    files.observeCurrentDisk();
    if (trigger == Trigger::FirstOpen) markOpened();
    return {true, true, {}};
}
} // namespace miacode::automatic_chart
