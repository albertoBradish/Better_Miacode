#pragma once

#include "core/analysis/hand_order/HandPlaySettings.h"

#include <QPointF>
#include <QVector3D>
#include <array>

namespace miacode::hand_order {

struct PlayHandMotion;

enum class PlayInterference { Clear, Lifted, Collision, OutOfReach };

struct PlayArmPose {
    QVector3D shoulder;
    QVector3D elbow;
    QVector3D wrist;
    QVector3D palm;
    double palmRadius = 0.08;
    bool lifted = false;
    bool reachable = true;
};

struct PlayArmFrame {
    std::array<PlayArmPose, 2> arms;
    PlayInterference interference = PlayInterference::Clear;
    QPointF interferencePoint;
    int interferenceHand = -1;
};

// Positions and lengths are in field radii. z is normal to the tilted screen.
inline constexpr double playUpperArmRadius = 0.105;
inline constexpr double playForearmRadius = 0.075;
inline constexpr double playPalmLength = 0.115;

double playReadyY(const PlayProfile& profile);
struct PlayBodyOffset { double verticalCm = 0.0; double distanceCm = 0.0; };
PlayBodyOffset preferredBodyOffset(const std::array<PlayHandMotion, 2>& hands, const PlayProfile& profile);
PlayArmFrame resolvePlayArms(const std::array<PlayHandMotion, 2>& hands, double palmRadius,
                            const PlayProfile& profile = {}, double bodyOffsetCm = 0.0, double distanceOffsetCm = 0.0);
bool playArmCanPress(const PlayArmFrame& pose, const std::array<PlayHandMotion, 2>& hands, int hand);

} // namespace miacode::hand_order
