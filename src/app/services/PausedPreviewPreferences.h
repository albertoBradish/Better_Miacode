#pragma once

#include <QJsonObject>

namespace miacode::paused_preview {

inline bool showStageMedia(const QJsonObject& preview)
{
    const auto explicitValue = preview.value("show_stage_media_when_paused");
    if (explicitValue.isBool()) return explicitValue.toBool();
    for (const char* key : {"force_labeled_judge_line_when_paused", "hide_stage_media_when_paused"}) {
        const auto legacy = preview.value(QLatin1String(key));
        if (legacy.isBool()) return !legacy.toBool();
    }
    return true;
}

} // namespace miacode::paused_preview
