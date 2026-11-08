#include "core/chart/ChartPreviewRegion.h"

#include "core/chart/IntroConfig.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/chart/parser/SimaiParser.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>

namespace miacode::chart_preview {
namespace {

QString dropKey(const char* property, int difficultyId)
{
    return QStringLiteral("miacode_drop_%1_%2").arg(QString::fromLatin1(property)).arg(difficultyId);
}

QString extraValue(const SimaiDocument& document, const QString& key)
{
    for (auto it = document.extraFields.crbegin(); it != document.extraFields.crend(); ++it) {
        if (it->key.compare(key, Qt::CaseInsensitive) == 0) return it->value.trimmed();
    }
    return {};
}

double number(const SimaiDocument& document, const QString& key, double fallback = -1.0)
{
    bool ok = false;
    const double value = extraValue(document, key).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

bool fitsMedia(MediaRange range, double mediaDuration)
{
    return range.isValid() && (!std::isfinite(mediaDuration) || mediaDuration <= 0.0
        || range.endSeconds <= mediaDuration + 0.001);
}

QString sha256(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

MediaRange previewWindow(MediaRange range, double mediaDuration)
{
    if (std::isfinite(mediaDuration) && mediaDuration > 0.0) {
        range.startSeconds = std::min(range.startSeconds, mediaDuration);
        range.endSeconds = std::min(range.endSeconds, mediaDuration);
    }
    range.endSeconds = std::min(range.endSeconds, range.startSeconds + intro::kPvPreviewHoldSeconds);
    return range;
}

MediaRange demoWindow(const SimaiDocument& document, double mediaDuration)
{
    const double start = number(document, QStringLiteral("demo_seek"));
    const double length = number(document, QStringLiteral("demo_len"));
    if (start < 0.0 || length <= 0.0 || !std::isfinite(start + length)) return {};
    return previewWindow({start, start + length}, mediaDuration);
}

} // namespace

bool MediaRange::isValid() const
{
    return std::isfinite(startSeconds) && std::isfinite(endSeconds)
        && startSeconds >= 0.0 && endSeconds > startSeconds;
}

double toMediaSeconds(double chartSeconds, double firstSeconds)
{
    return chartSeconds + firstSeconds;
}

double toChartSeconds(double mediaSeconds, double firstSeconds)
{
    return mediaSeconds - firstSeconds;
}

QString dropChartFingerprint(const SimaiDocument& document, int difficultyId)
{
    const auto* difficulty = document.difficulty(difficultyId);
    if (difficulty == nullptr) return {};
    const auto timing = simai::buildTimingMetadata(document);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    QString logicalChart = difficulty->chart;
    logicalChart.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    stream << logicalChart << timeline::offset::parsedFirstSeconds(document.first)
        << timing.wholeTimeSignatureText << timing.wholeTimeSignatureNumerator
        << timing.wholeTimeSignatureDenominator << timing.wholeTimeSignatureValid
        << SimaiParser::allowNegativeHsEnabled();
    return sha256(bytes);
}

QString combineDropFingerprints(const QString& chartFingerprint, const QString& audioFingerprint)
{
    if (chartFingerprint.isEmpty() || audioFingerprint.isEmpty()) return {};
    return sha256(QStringLiteral("%1|%2|%3").arg(kDropAlgorithmVersion)
        .arg(audioFingerprint, chartFingerprint).toUtf8());
}

QString dropInputFingerprint(const SimaiDocument& document, int difficultyId, const QString& audioFingerprint)
{
    return combineDropFingerprints(dropChartFingerprint(document, difficultyId), audioFingerprint);
}

std::optional<DropSelection> storedDropSelection(const SimaiDocument& document, int difficultyId)
{
    if (document.difficulty(difficultyId) == nullptr) return {};
    const MediaRange range{number(document, dropKey("start", difficultyId)),
        number(document, dropKey("end", difficultyId))};
    if (!range.isValid()) return {};
    const QString source = extraValue(document, dropKey("source", difficultyId));
    const QString fingerprint = extraValue(document, dropKey("fingerprint", difficultyId));
    if (source == QLatin1String("manual")) return DropSelection{range, DropSource::Manual, fingerprint};
    if (source == QLatin1String("auto") && !fingerprint.isEmpty()
        && number(document, dropKey("version", difficultyId)) == kDropAlgorithmVersion) {
        return DropSelection{range, DropSource::Auto, fingerprint};
    }
    return {};
}

bool hasManualDropMarker(const SimaiDocument& document, int difficultyId)
{
    return document.difficulty(difficultyId) != nullptr
        && extraValue(document, dropKey("source", difficultyId)) == QLatin1String("manual");
}

std::optional<DropSelection> readDropSelection(const SimaiDocument& document, int difficultyId,
    const QString& currentFingerprint, double mediaDuration)
{
    const auto selection = storedDropSelection(document, difficultyId);
    if (!selection || !fitsMedia(selection->range, mediaDuration)) return {};
    if (selection->source == DropSource::Auto && selection->inputFingerprint != currentFingerprint) return {};
    return selection;
}

bool writeDropSelection(SimaiDocument& document, int difficultyId, std::optional<DropSelection> selection)
{
    if (document.difficulty(difficultyId) == nullptr || (selection && (!selection->range.isValid()
        || (selection->source == DropSource::Auto && selection->inputFingerprint.isEmpty())))) return false;

    const auto existing = storedDropSelection(document, difficultyId);
    if (selection && existing == selection) return false;
    const QString start = dropKey("start", difficultyId);
    const QString end = dropKey("end", difficultyId);
    const QString source = dropKey("source", difficultyId);
    const QString version = dropKey("version", difficultyId);
    const QString fingerprint = dropKey("fingerprint", difficultyId);
    const auto removed = document.extraFields.removeIf([&](const SimaiRawField& field) {
        const QString key = field.key.toLower();
        return key == start || key == end || key == source || key == version || key == fingerprint;
    });
    if (!selection) return removed > 0;

    document.extraFields.append({start, QString::number(selection->range.startSeconds, 'g', 17)});
    document.extraFields.append({end, QString::number(selection->range.endSeconds, 'g', 17)});
    document.extraFields.append({source, selection->source == DropSource::Manual
        ? QStringLiteral("manual") : QStringLiteral("auto")});
    document.extraFields.append({version, QString::number(kDropAlgorithmVersion)});
    if (!selection->inputFingerprint.isEmpty()) document.extraFields.append({fingerprint, selection->inputFingerprint});
    return true;
}

bool seedDemoWindow(SimaiDocument& document, MediaRange range, double mediaDuration)
{
    if (demoWindow(document, mediaDuration).isValid() || !range.isValid()) return false;
    const QString source = extraValue(document, QStringLiteral("miacode_demo_source"));
    if (source != QLatin1String("auto") && (source == QLatin1String("manual")
        || !extraValue(document, QStringLiteral("demo_seek")).isEmpty()
        || !extraValue(document, QStringLiteral("demo_len")).isEmpty())) return false;
    const auto window = previewWindow(range, mediaDuration);
    if (!window.isValid()) return false;
    document.extraFields.removeIf([](const SimaiRawField& field) {
        const QString key = field.key.toLower();
        return key == QLatin1String("demo_seek") || key == QLatin1String("demo_len")
            || key == QLatin1String("miacode_demo_source");
    });
    document.extraFields.append({QStringLiteral("demo_seek"), QString::number(window.startSeconds, 'g', 17)});
    document.extraFields.append({QStringLiteral("demo_len"), QString::number(window.endSeconds - window.startSeconds, 'g', 17)});
    document.extraFields.append({QStringLiteral("miacode_demo_source"), QStringLiteral("auto")});
    return true;
}

IntroWindow resolveIntroWindow(const SimaiDocument& document, int difficultyId,
    std::optional<MediaRange> explicitOverride, IntroWindowMode mode, double mediaDuration,
    const QString& currentFingerprint, std::optional<DropSelection> sessionDrop, bool preferSessionDrop)
{
    if (explicitOverride && explicitOverride->isValid()) {
        const MediaRange window = previewWindow(*explicitOverride, mediaDuration);
        if (window.isValid()) return {window, IntroWindowSource::Explicit};
    }
    if (mode == IntroWindowMode::Automatic && extraValue(document, QStringLiteral("miacode_demo_source")) != QLatin1String("auto")) {
        const MediaRange demo = demoWindow(document, mediaDuration);
        if (demo.isValid()) return {demo, IntroWindowSource::Demo};
    }
    if (mode != IntroWindowMode::Default) {
        const auto stored = preferSessionDrop ? std::optional<DropSelection>{}
            : readDropSelection(document, difficultyId, currentFingerprint, mediaDuration);
        if (!sessionDrop || !fitsMedia(sessionDrop->range, mediaDuration)
            || (sessionDrop->source == DropSource::Auto && (currentFingerprint.isEmpty()
                || sessionDrop->inputFingerprint != currentFingerprint))
            || (stored && stored->source == DropSource::Manual && sessionDrop->source == DropSource::Auto)) {
            sessionDrop = stored;
        }
        if (sessionDrop) return {previewWindow(sessionDrop->range, mediaDuration), IntroWindowSource::Drop};
    }
    return {previewWindow({0.0, intro::kPvPreviewHoldSeconds}, mediaDuration), IntroWindowSource::Default};
}

} // namespace miacode::chart_preview
