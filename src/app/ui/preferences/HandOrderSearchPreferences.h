#pragma once

#include "core/analysis/hand_order/BasicHandOrder.h"

#include <QJsonObject>
#include <QString>
#include <cmath>

namespace miacode::ui::hand_order_search_preferences {

inline constexpr int lowMillions = 50;
inline constexpr int standardMillions = 300;
inline constexpr int highMillions = 600;
inline constexpr int maximumMillions = int(hand_order::BasicParameters::maxAllowedWorkUnits / 1000000);

inline QString mode(const QJsonObject& root)
{
    const QString token = root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_search_mode")).toString();
    if (token == QLatin1String("low") || token == QLatin1String("standard")
        || token == QLatin1String("high") || token == QLatin1String("custom")) return token;
    return QStringLiteral("standard");
}

inline int customMillions(const QJsonObject& root)
{
    const auto stored = root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_search_custom_millions"));
    const double number = stored.toDouble(standardMillions);
    if (!stored.isDouble() || !std::isfinite(number) || std::floor(number) != number)
        return standardMillions;
    if (number < 10) return 10;
    if (number > maximumMillions) return maximumMillions;
    return int(number);
}

inline int millions(const QJsonObject& root)
{
    const QString selected = mode(root);
    if (selected == QLatin1String("low")) return lowMillions;
    if (selected == QLatin1String("high")) return highMillions;
    if (selected == QLatin1String("custom")) return customMillions(root);
    return standardMillions;
}

inline bool setMode(QJsonObject* root, const QString& token)
{
    if (root == nullptr || (token != QLatin1String("low")
        && token != QLatin1String("standard") && token != QLatin1String("high")
        && token != QLatin1String("custom")) || mode(*root) == token) return false;
    auto app = root->value(QStringLiteral("app")).toObject();
    if (token == QLatin1String("custom")
        && !app.value(QStringLiteral("hand_order_search_custom_millions")).isDouble())
        app.insert(QStringLiteral("hand_order_search_custom_millions"), millions(*root));
    app.insert(QStringLiteral("hand_order_search_mode"), token);
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline bool setCustomMillions(QJsonObject* root, int value)
{
    if (root == nullptr) return false;
    value = qBound(10, value, maximumMillions);
    auto app = root->value(QStringLiteral("app")).toObject();
    const bool changed = mode(*root) != QLatin1String("custom")
        || customMillions(*root) != value;
    if (!changed) return false;
    app.insert(QStringLiteral("hand_order_search_mode"), QStringLiteral("custom"));
    app.insert(QStringLiteral("hand_order_search_custom_millions"), value);
    root->insert(QStringLiteral("app"), app);
    return true;
}

} // namespace miacode::ui::hand_order_search_preferences
