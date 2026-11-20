#pragma once

#include "app/services/PreferenceDocument.h"
#include "export/video_export/VideoWatermarkSettings.h"

namespace miacode::video_watermark_preferences {

inline video_export::VideoWatermarkSettings load()
{
    return video_export::VideoWatermarkSettings::fromJson(
        PreferenceDocument::loadPreferencesObject().value(QStringLiteral("app")).toObject()
            .value(QStringLiteral("video_watermark")).toObject());
}

inline void save(const video_export::VideoWatermarkSettings& settings)
{
    auto root = PreferenceDocument::loadPreferencesObject();
    auto app = root.value(QStringLiteral("app")).toObject();
    app.insert(QStringLiteral("video_watermark"), settings.toJson());
    root.insert(QStringLiteral("app"), app);
    PreferenceDocument::savePreferencesObject(root);
}

}  // namespace miacode::video_watermark_preferences
