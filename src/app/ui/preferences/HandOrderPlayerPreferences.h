#pragma once

#include <QJsonObject>
#include <QVariantMap>
#include <cmath>
#include <iterator>
#include "core/analysis/hand_order/HandOrderPlayerModel.h"

namespace miacode::ui::hand_order_player_preferences {
struct Parameter {
    const char* name;
    double minimum, maximum, defaultValue;
};
inline constexpr Parameter parameters[] = {
    {"maxComfortableTapRate", hand_order::PlayerProfile::minTapRate, hand_order::PlayerProfile::maxTapRate, hand_order::PlayerProfile{}.maxComfortableTapRate},
    {"comfortableMoveSpeed", hand_order::PlayerProfile::minMoveSpeed, hand_order::PlayerProfile::maxMoveSpeed, hand_order::PlayerProfile{}.comfortableMoveSpeed},
    {"staminaCapacity", hand_order::PlayerProfile::minStamina, hand_order::PlayerProfile::maxStamina, hand_order::PlayerProfile{}.staminaCapacity},
    {"recoveryHalfLife", hand_order::PlayerProfile::minRecoveryHalfLife, hand_order::PlayerProfile::maxRecoveryHalfLife, hand_order::PlayerProfile{}.recoveryHalfLife},
    {"techniquePreference", hand_order::PlayerProfile::minTechniquePreference, hand_order::PlayerProfile::maxTechniquePreference, hand_order::PlayerProfile{}.techniquePreference},
    {"postureWeight", hand_order::PlayerProfile::minPostureWeight, hand_order::PlayerProfile::maxPostureWeight, hand_order::PlayerProfile{}.postureWeight},
};

inline QString styleToken(const QJsonObject& root)
{
    const auto app = root.value(QStringLiteral("app")).toObject();
    const QString stored = app.value(QStringLiteral("hand_order_player_style")).toString();
    if (stored == QLatin1String("comprehensive") || stored == QLatin1String("hard")
        || stored == QLatin1String("pagination") || stored == QLatin1String("decomposition")
        || stored == QLatin1String("custom")) return stored;
    const auto profile = app.value(QStringLiteral("hand_order_player")).toObject();
    for (const auto& parameter : parameters)
        if (profile.value(QLatin1String(parameter.name)).isDouble())
            return QStringLiteral("custom");
    return QStringLiteral("comprehensive");
}

inline hand_order::PlayerProfile preset(const QString& style)
{
    hand_order::PlayerProfile profile;
    if (style == QLatin1String("hard")) {
        profile.maxComfortableTapRate = 8.5;
        profile.comfortableMoveSpeed = 5.5;
        profile.staminaCapacity = 1.4;
        profile.recoveryHalfLife = 3.0;
        profile.techniquePreference = 0.8;
        profile.postureWeight = 3.0;
    } else if (style == QLatin1String("pagination")) {
        profile.maxComfortableTapRate = 6.5;
        profile.comfortableMoveSpeed = 7.5;
        profile.staminaCapacity = 1.1;
        profile.recoveryHalfLife = 2.5;
        profile.techniquePreference = 0.25;
        profile.postureWeight = 5.0;
    } else if (style == QLatin1String("decomposition")) {
        profile.maxComfortableTapRate = 5.0;
        profile.comfortableMoveSpeed = 8.0;
        profile.staminaCapacity = 1.0;
        profile.recoveryHalfLife = 2.0;
        profile.techniquePreference = -0.9;
        profile.postureWeight = 3.0;
    }
    return profile;
}

inline bool applyPreset(QJsonObject* root, const QString& style)
{
    if (root == nullptr || (style != QLatin1String("comprehensive")
        && style != QLatin1String("hard") && style != QLatin1String("pagination")
        && style != QLatin1String("decomposition"))) return false;
    const auto values = preset(style);
    const double chosen[] = {values.maxComfortableTapRate, values.comfortableMoveSpeed,
        values.staminaCapacity, values.recoveryHalfLife, values.techniquePreference,
        values.postureWeight};
    QJsonObject app = root->value(QStringLiteral("app")).toObject();
    QJsonObject profile = app.value(QStringLiteral("hand_order_player")).toObject();
    bool changed = styleToken(*root) != style;
    for (int i = 0; i < int(std::size(parameters)); ++i) {
        const QString name = QString::fromLatin1(parameters[i].name);
        changed |= !profile.value(name).isDouble() || profile.value(name).toDouble() != chosen[i];
        profile.insert(name, chosen[i]);
    }
    if (!changed) return false;
    app.insert(QStringLiteral("hand_order_player"), profile);
    app.insert(QStringLiteral("hand_order_player_style"), style);
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline QVariantMap read(const QJsonObject& root)
{
    const auto profile = root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_player")).toObject();
    const auto style = styleToken(root);
    const auto fallback = preset(style);
    const double defaults[] = {fallback.maxComfortableTapRate, fallback.comfortableMoveSpeed,
        fallback.staminaCapacity, fallback.recoveryHalfLife, fallback.techniquePreference,
        fallback.postureWeight};
    QVariantMap values;
    for (int i = 0; i < int(std::size(parameters)); ++i) {
        const auto& parameter = parameters[i];
        const QString name = QString::fromLatin1(parameter.name);
        const auto stored = profile.value(name);
        const double value = stored.toDouble(defaults[i]);
        const bool valid = stored.isDouble() && std::isfinite(value)
            && value >= parameter.minimum && value <= parameter.maximum;
        // The named comprehensive style follows the current defaults.
        // Values edited individually use the custom style.
        values.insert(name, valid && style != QLatin1String("comprehensive") ? value : defaults[i]);
    }
    return values;
}

inline bool setStyle(QJsonObject* root, const QString& style)
{
    if (style != QLatin1String("custom")) return applyPreset(root, style);
    if (root == nullptr || styleToken(*root) == style) return false;
    auto app = root->value(QStringLiteral("app")).toObject();
    auto profile = app.value(QStringLiteral("hand_order_player")).toObject();
    const auto effective = read(*root);
    for (auto it = effective.cbegin(); it != effective.cend(); ++it) profile.insert(it.key(), it.value().toDouble());
    app.insert(QStringLiteral("hand_order_player"), profile);
    app.insert(QStringLiteral("hand_order_player_style"), style);
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline double displayValue(const QString& name, double stored)
{
    if (name == QLatin1String("maxComfortableTapRate")) return stored * 15.0;
    if (name == QLatin1String("comfortableMoveSpeed")) return stored / 2.0;
    return stored;
}
inline double storedValue(const QString& name, double displayed)
{
    if (name == QLatin1String("maxComfortableTapRate")) return displayed / 15.0;
    if (name == QLatin1String("comfortableMoveSpeed")) return displayed * 2.0;
    return displayed;
}
inline QVariantMap readDisplay(const QJsonObject& root)
{
    auto values = read(root);
    for (auto it = values.begin(); it != values.end(); ++it)
        it.value() = displayValue(it.key(), it.value().toDouble());
    return values;
}

inline bool set(QJsonObject* root, const QString& name, double value)
{
    if (root == nullptr || !std::isfinite(value)) return false;
    const Parameter* selected = nullptr;
    for (const auto& parameter : parameters)
        if (name == QLatin1String(parameter.name)) { selected = &parameter; break; }
    if (selected == nullptr) return false;
    value = qBound(selected->minimum, value, selected->maximum);
    const auto effective = read(*root);
    if (effective.value(name).toDouble() == value) return false;
    if (styleToken(*root) != QLatin1String("custom")) setStyle(root, QStringLiteral("custom"));
    QJsonObject app = root->value(QStringLiteral("app")).toObject();
    QJsonObject profile = app.value(QStringLiteral("hand_order_player")).toObject();
    profile.insert(name, value);
    app.insert(QStringLiteral("hand_order_player"), profile);
    app.insert(QStringLiteral("hand_order_player_style"), QStringLiteral("custom"));
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline bool reset(QJsonObject* root)
{
    return applyPreset(root, QStringLiteral("comprehensive"));
}
inline bool setDisplay(QJsonObject* root, const QString& name, double displayed)
{
    return set(root, name, storedValue(name, displayed));
}
} // namespace miacode::ui::hand_order_player_preferences
