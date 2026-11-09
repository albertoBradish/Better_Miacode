#pragma once

#include <QFileInfo>
#include <QJsonObject>

#include "core/analysis/MuriRenderOptions.h"

namespace miacode::preview_mode_preferences {

inline QString profileKey(RenderMode mode)
{
    if (mode == RenderMode::Debug) return QStringLiteral("debug");
    if (mode == RenderMode::MaimuriDxStyle) return QStringLiteral("muri");
    return QStringLiteral("regular");
}

inline QJsonObject normalized(const QJsonObject& values)
{
    QJsonObject result;
    for (const char* key : {"showTimestamp", "showBpmHud", "showCpsHud", "showMeterHud", "showModsHud",
                           "showJudgeMarkers", "showTouchTrail", "showDebugInfo",
                           "showChartInfoHud", "outlineAutomatic"}) {
        const QString name = QString::fromLatin1(key);
        const bool defaultValue = name == QLatin1String("showTimestamp") || name == QLatin1String("outlineAutomatic");
        result.insert(name, values.value(name).toBool(defaultValue));
    }
    const int variant = values.value(QStringLiteral("outlineVariant")).toInt(static_cast<int>(PreviewOutlineVariant::Line));
    result.insert(QStringLiteral("outlineVariant"), variant >= 0 && variant <= static_cast<int>(PreviewOutlineVariant::None)
        ? variant : static_cast<int>(PreviewOutlineVariant::Line));
    const QString customFile = values.value(QStringLiteral("outlineCustomFile")).toString();
    result.insert(QStringLiteral("outlineCustomFile"), QFileInfo(customFile).fileName());
    return result;
}

inline QJsonObject withCurrent(QJsonObject profiles, RenderMode mode, const QJsonObject& current)
{
    profiles.insert(profileKey(mode), normalized(current));
    return profiles;
}

inline QJsonObject restored(const QJsonObject& profiles, RenderMode mode)
{
    // A previously unused mode starts from its clean defaults, never from the
    // debug options currently visible in the shared rendering runtime.
    return normalized(profiles.value(profileKey(mode)).toObject());
}

} // namespace miacode::preview_mode_preferences
