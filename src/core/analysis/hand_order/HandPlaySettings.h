#pragma once

#include <QJsonObject>
#include <QtGlobal>

namespace miacode::hand_order {

// Scaled from MMT-P0026 v2.00, printed p. 6 (front and side drawings).
inline constexpr double playCabinetCenterCm = 126.5;
inline constexpr double playCabinetButtonRadiusCm = 32.0;
inline constexpr double playCabinetTiltDegrees = 19.0;
inline constexpr double playExternalButtonRadius = 1.065;
inline constexpr double playCabinetFieldRadiusCm = playCabinetButtonRadiusCm / playExternalButtonRadius;

struct PlayProfile {
    static constexpr double shoulderRatio = 0.82;
    static constexpr double upperArmRatio = 0.19;
    int heightCm = 170;
    int standingDistanceCm = 35;
    int standingOffsetCm = 0;
    int armReachPercent = 100;
    int lowerLimitCm = 25;
    int raiseLimitCm = 8;
    int verticalSpeedCmPerSecond = 85;
    int reactionMs = 140;
    int forwardLimitCm = 14;
    int backwardLimitCm = 8;
    int foreAftSpeedCmPerSecond = 65;
    bool automaticHeight = true;
    bool automaticDistance = true;
    bool adaptivePalm = true;
    bool preferButtons = true;

    bool operator==(const PlayProfile&) const = default;

    double shoulderHeightCm() const { return heightCm * shoulderRatio + standingOffsetCm; }
    double readyHandHeightCm() const { return shoulderHeightCm() - heightCm * upperArmRatio * armReachPercent / 100.0; }

    QJsonObject toJson() const
    {
        return {{"heightCm", heightCm}, {"standingDistanceCm", standingDistanceCm}, {"standingOffsetCm", standingOffsetCm},
            {"armReachPercent", armReachPercent}, {"lowerLimitCm", lowerLimitCm}, {"raiseLimitCm", raiseLimitCm},
            {"verticalSpeedCmPerSecond", verticalSpeedCmPerSecond}, {"automaticHeight", automaticHeight},
            {"automaticDistance", automaticDistance}, {"reactionMs", reactionMs},
            {"forwardLimitCm", forwardLimitCm}, {"backwardLimitCm", backwardLimitCm},
            {"foreAftSpeedCmPerSecond", foreAftSpeedCmPerSecond}, {"adaptivePalm", adaptivePalm}, {"preferButtons", preferButtons}};
    }

    static PlayProfile fromJson(const QJsonObject& values)
    {
        PlayProfile p;
        p.heightCm = qBound(120, values.value("heightCm").toInt(p.heightCm), 220);
        p.standingDistanceCm = qBound(15, values.value("standingDistanceCm").toInt(p.standingDistanceCm), 65);
        p.standingOffsetCm = qBound(-30, values.value("standingOffsetCm").toInt(p.standingOffsetCm), 20);
        p.armReachPercent = qBound(80, values.value("armReachPercent").toInt(p.armReachPercent), 120);
        p.lowerLimitCm = qBound(0, values.value("lowerLimitCm").toInt(p.lowerLimitCm), 45);
        p.raiseLimitCm = qBound(0, values.value("raiseLimitCm").toInt(p.raiseLimitCm), 20);
        p.verticalSpeedCmPerSecond = qBound(20, values.value("verticalSpeedCmPerSecond").toInt(p.verticalSpeedCmPerSecond), 160);
        p.automaticHeight = values.value("automaticHeight").toBool(p.automaticHeight);
        p.automaticDistance = values.value("automaticDistance").toBool(p.automaticDistance);
        p.reactionMs = qBound(0, values.value("reactionMs").toInt(p.reactionMs), 350);
        p.forwardLimitCm = qBound(0, values.value("forwardLimitCm").toInt(p.forwardLimitCm), 28);
        p.backwardLimitCm = qBound(0, values.value("backwardLimitCm").toInt(p.backwardLimitCm), 30);
        p.foreAftSpeedCmPerSecond = qBound(20, values.value("foreAftSpeedCmPerSecond").toInt(p.foreAftSpeedCmPerSecond), 150);
        p.adaptivePalm = values.value("adaptivePalm").toBool(p.adaptivePalm);
        p.preferButtons = values.value("preferButtons").toBool(p.preferButtons);
        return p;
    }
};

} // namespace miacode::hand_order
