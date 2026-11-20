#pragma once

#include "app/services/PreferenceDocument.h"
#include "core/analysis/MuriRenderOptions.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace miacode::hand_order_features {

inline QString grantPath()
{
    return QFileInfo(PreferenceDocument::preferencesFilePath()).dir().filePath("hand-order-access-v1");
}

inline bool validGrant(const QByteArray& grant)
{
    // The persisted preimage can only be derived from the password. A legacy
    // preference flag, or a copy of this verifier, cannot unlock the feature.
#ifdef MIACODE_HAND_ORDER_FIXTURE_VERIFIER
    constexpr char verifier[] = MIACODE_HAND_ORDER_FIXTURE_VERIFIER;
#else
    constexpr char verifier[] = "0ab69b472431d9150eca8e7c6e344026dce085aff7bdb9aa3aba69e4c762a94b";
#endif
    return grant.size() == 32
        && QCryptographicHash::hash(grant, QCryptographicHash::Sha256).toHex() == verifier;
}

inline bool enabled()
{
    QFile grant(grantPath());
    return grant.open(QIODevice::ReadOnly) && validGrant(grant.read(33));
}

enum class UnlockResult { Enabled, IncorrectPassword, StorageError };

inline UnlockResult unlock(const QString& password)
{
    const auto grant = QCryptographicHash::hash(
        QByteArrayLiteral("Better Miacode / hand-order / v1:") + password.toUtf8(), QCryptographicHash::Sha256);
    if (!validGrant(grant)) return UnlockResult::IncorrectPassword;
    const QString target = grantPath();
    if (!QFileInfo(target).dir().mkpath(QStringLiteral("."))) return UnlockResult::StorageError;
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(grant) != grant.size() || !file.commit())
        return UnlockResult::StorageError;
    return UnlockResult::Enabled;
}

inline bool disable()
{
    const QString target = grantPath();
    return !QFileInfo::exists(target) || QFile::remove(target);
}

inline bool watermark(const QJsonObject& root)
{
    return root.value("app").toObject().value("hand_order_watermark_enabled").toBool(false);
}

inline bool watermark() { return watermark(PreferenceDocument::loadPreferencesObject()); }

template<class Task>
void applyExportPolicy(Task& task)
{
    const auto preferences = PreferenceDocument::loadPreferencesObject();
    const bool authorized = enabled();
    if (!authorized) {
        task.showHandOrderAnnotations = false;
        task.muriRenderOptions.playAnalysisEnabled = false;
        if (renderModeShowsHandOrder(task.muriRenderOptions.renderMode))
            task.muriRenderOptions.renderMode = RenderMode::Native;
    }
    task.showHandOrderWatermark = authorized && watermark(preferences)
        && task.showHandOrderAnnotations;
}

} // namespace miacode::hand_order_features
