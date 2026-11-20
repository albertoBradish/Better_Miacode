#pragma once

#include "common/RuntimePaths.h"

#include "export/video_export/EncoderProbeCache.h"
#include "common/DebugLog.h"
#include "common/RuntimePaths.h"
#include <QDir>
#include <QSettings>

namespace miacode::runtime {

// Encoder probing is a machine cache, separate from portable user preferences.
class SettingsEncoderProbeCache final : public miacode::video_export::EncoderProbeCache {
public:
    QString preferredHardwareEncoder() const override
    {
        const QString path = cachePath();
        if (path.isEmpty()) return {};
        QSettings settings(path, QSettings::IniFormat);
        const QString codec = settings.value(key()).toString().trimmed();
        if (settings.status() != QSettings::NoError) {
            miacode::debug_log::appendLine(miacode::debug_log::Channel::Export,
                QStringLiteral("encoder_probe_cache"),
                QStringLiteral("encoder_probe_cache_read_failed path=%1 status=%2")
                    .arg(settings.fileName()).arg(static_cast<int>(settings.status())),
                true, miacode::debug_log::Level::Error);
            return {};
        }
        return codec;
    }
    void rememberPreferredHardwareEncoder(const QString& codec) override
    {
        const QString path = cachePath();
        if (path.isEmpty()) return;
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(key(), codec);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            miacode::debug_log::appendLine(miacode::debug_log::Channel::Export,
                QStringLiteral("encoder_probe_cache"), QStringLiteral("encoder_probe_cache_write_failed path=%1 status=%2")
                                .arg(settings.fileName()).arg(static_cast<int>(settings.status())),
                true, miacode::debug_log::Level::Error);
        }
    }
private:
    static QString cachePath()
    {
        const QString root = miacode::runtime_paths::cacheDirectory();
        return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("video_export_runtime.ini"));
    }
    static QString key()
    {
        return QStringLiteral("video_export/runtime_probe/preferred_hardware_encoder");
    }
};

} // namespace miacode::runtime
