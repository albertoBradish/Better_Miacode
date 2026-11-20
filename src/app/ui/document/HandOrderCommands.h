#pragma once

#include <QString>
#include <QVector>

namespace miacode::ui {
struct HandOrderCommandSpec { QString id; QString labelKey; bool requiresSelection = true; bool showInMenu = true; };
inline QVector<HandOrderCommandSpec> handOrderCommandSpecs()
{
    return {{QStringLiteral("hand_order.basic_layout"), QStringLiteral("hand_order.basic_layout"), false},
        {QStringLiteral("hand_order.set_left"), QStringLiteral("hand_order.set_left")},
        {QStringLiteral("hand_order.set_right"), QStringLiteral("hand_order.set_right")},
        {QStringLiteral("hand_order.set_either"), QStringLiteral("hand_order.set_either")},
        {QStringLiteral("hand_order.split_annotation"), QStringLiteral("hand_order.split_annotation")},
        {QStringLiteral("hand_order.bisect_annotation"), QStringLiteral("hand_order.bisect_annotation"), false},
        {QStringLiteral("hand_order.swap_hands"), QStringLiteral("hand_order.swap_hands")},
        {QStringLiteral("hand_order.alternate"), QStringLiteral("hand_order.alternate")},
        {QStringLiteral("hand_order.screen_split"), QStringLiteral("hand_order.screen_split")},
        {QStringLiteral("hand_order.split_native_slide"), QStringLiteral("hand_order.split_native_slide")},
        {QStringLiteral("hand_order.analyze"), QStringLiteral("hand_order.analyze"), false},
        {QStringLiteral("hand_order.optimize"), QStringLiteral("hand_order.analyze"), false, false},
        {QStringLiteral("hand_order.play_review"), QStringLiteral("hand_order.play_review"), false}};
}
} // namespace miacode::ui
