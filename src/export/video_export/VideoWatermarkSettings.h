#pragma once

#include <QJsonObject>

namespace miacode::video_export {

struct VideoWatermarkSettings {
    static constexpr int kMinimumFontSize = 16;
    static constexpr int kMaximumFontSize = 120;

    bool enabled = false;
    QString text = QStringLiteral("Better Miacode");
    int fontSize = 48;

    QJsonObject toJson() const
    {
        return {{QStringLiteral("enabled"), enabled},
                {QStringLiteral("text"), text},
                {QStringLiteral("font_size"), fontSize}};
    }

    static VideoWatermarkSettings fromJson(const QJsonObject& object)
    {
        VideoWatermarkSettings settings;
        settings.enabled = object.value(QStringLiteral("enabled")).toBool();
        settings.text = object.value(QStringLiteral("text")).toString(settings.text);
        settings.fontSize = qBound(kMinimumFontSize,
            object.value(QStringLiteral("font_size")).toInt(settings.fontSize), kMaximumFontSize);
        return settings;
    }
};

}  // namespace miacode::video_export
