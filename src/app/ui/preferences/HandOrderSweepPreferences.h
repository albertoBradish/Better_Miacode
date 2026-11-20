#pragma once

#include "core/chart/model/HandOrderSweepThreshold.h"

#include <QJsonObject>
#include <cmath>

namespace miacode::ui::hand_order_sweep_preferences {

inline int threshold(const QJsonObject& root)
{
    const auto value = root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_sweep_threshold"));
    if (!value.isDouble()) return chart::handOrderDefaultSweepThreshold;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < chart::handOrderMinSweepThreshold
        || number > chart::handOrderMaxSweepThreshold)
        return chart::handOrderDefaultSweepThreshold;
    return int(number);
}

inline bool setThreshold(QJsonObject* root, int value)
{
    if (root == nullptr) return false;
    value = qBound(chart::handOrderMinSweepThreshold, value,
        chart::handOrderMaxSweepThreshold);
    QJsonObject app = root->value(QStringLiteral("app")).toObject();
    const auto stored = app.value(QStringLiteral("hand_order_sweep_threshold"));
    if (stored.isDouble() && stored.toDouble() == value) return false;
    app.insert(QStringLiteral("hand_order_sweep_threshold"), value);
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline bool colorUnresolved(const QJsonObject& root)
{
    return root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_color_unresolved")).toBool(true);
}

inline bool setColorUnresolved(QJsonObject* root, bool enabled)
{
    if (root == nullptr) return false;
    QJsonObject app = root->value(QStringLiteral("app")).toObject();
    const auto stored = app.value(QStringLiteral("hand_order_color_unresolved"));
    if (stored.isBool() && stored.toBool() == enabled) return false;
    app.insert(QStringLiteral("hand_order_color_unresolved"), enabled);
    root->insert(QStringLiteral("app"), app);
    return true;
}

} // namespace miacode::ui::hand_order_sweep_preferences
