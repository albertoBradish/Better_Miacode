#include "core/chart/ChartVersion.h"
#include "core/chart/document/AdvancedMetadata.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <iterator>
#include <optional>

namespace miacode::chart_version {
namespace {
constexpr const char* families[] = {"maimai", "GreeN", "ORANGE", "PiNK", "MURASAKi",
    "MiLK", "FiNALE", "DX", "Splash", "UNiVERSE", "FESTiVAL", "BUDDiES", "PRiSM",
    "CiRCLE", "LightDX", "Gamble", "MAGiCAL"};

std::optional<Version> namedVersion(QString token)
{
    token = token.normalized(QString::NormalizationForm_KC).simplified().toLower();
    token.remove(' ');
    token.replace(QStringLiteral("でらっくす"), QStringLiteral("dx"));
    token.replace(QStringLiteral("deluxe"), QStringLiteral("dx"));
    const bool plus = token.endsWith('+') || token.endsWith(QStringLiteral("plus"));
    if (plus) token.chop(token.endsWith('+') ? 1 : 4);
    if (token.startsWith(QStringLiteral("maimai")) && token.size() > 6) token.remove(0, 6);
    if (token.startsWith(QStringLiteral("dx")) && token.size() > 2) token.remove(0, 2);
    if (token == QLatin1String("pri")) token = QStringLiteral("prism");
    if (token == QLatin1String("fes")) token = QStringLiteral("festival");
    for (int index = 0; index < static_cast<int>(std::size(families)); ++index) {
        if (token.compare(QLatin1String(families[index]), Qt::CaseInsensitive) == 0)
            return Version{static_cast<Family>(index + 1), plus};
    }
    return std::nullopt;
}

Version minimumVersion(const TimelineNoteMarker& marker)
{
    const bool touch = marker.type == QLatin1String("touch");
    const bool touchHold = marker.type == QLatin1String("touch_hold");
    if (touchHold && !marker.touchPad.isEmpty() && !marker.touchPad.startsWith('C'))
        return {Family::Prism, true};
    if (marker.slideSegmentKeys.size() > 1 || marker.trackBreak
        || (marker.type == QLatin1String("hold") && marker.isBreak)
        || (marker.isBreak && marker.isEx) || (marker.headBreak && marker.headEx)
        || (touch && (marker.touchPad.startsWith('A') || marker.touchPad.startsWith('D'))))
        return {Family::Festival};
    if (touch || touchHold || marker.isEx || marker.headEx) return {Family::Dx};

    const QString& shape = marker.slideTrackKey;
    if (marker.type == QLatin1String("wifi") || marker.slideBranchIndex > 0 || marker.sameHeadSlide || marker.headBreak
        || shape.contains(QLatin1String("pp")) || shape.contains(QLatin1String("qq")) || shape.contains('V'))
        return {Family::Pink};
    const bool fullCircle = shape.size() == 3 && shape.front() == shape.back()
        && (shape.at(1) == '<' || shape.at(1) == '>');
    if (fullCircle || shape.contains('s') || shape.contains('z') || shape.contains('v')
        || shape.contains('p') || shape.contains('q')) return {Family::Green, true};
    return {};
}
} // namespace

QString Version::outlineFileName() const
{
    // MAGiCAL has no bundled line. Use the last available arcade family's artwork.
    const int asset = static_cast<int>(family == Family::Magical ? Family::Circle : family);
    return QStringLiteral("%1.%2.png").arg(asset).arg(QLatin1String(families[asset - 1]));
}

Version detect(const SimaiDocument& document, const QVector<TimelineNoteMarker>& markers)
{
    const QString metadata = miacode::metadata::fieldValue(document, QStringLiteral("metadata"));
    if (!metadata.isEmpty()) {
        const auto fields = QJsonDocument::fromJson(metadata.toUtf8()).object();
        if (const auto version = namedVersion(fields.value(QStringLiteral("version")).toString())) return *version;
    }
    if (const auto version = namedVersion(miacode::metadata::fieldValue(document, QStringLiteral("version"))))
        return *version;
    Version latest;
    for (const auto& marker : markers) {
        const Version candidate = minimumVersion(marker);
        if (candidate.family > latest.family || (candidate.family == latest.family && candidate.plus))
            latest = candidate;
    }
    return latest;
}
} // namespace miacode::chart_version
