#include "core/analysis/hand_order/HandPlayPose.h"

#include "core/analysis/hand_order/HandPlayContacts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-6;
constexpr double armExtensionTarget = 0.88;
constexpr double tilt = playCabinetTiltDegrees * std::numbers::pi / 180.0;
const double verticalProjection = std::cos(tilt);
const double normalProjection = std::sin(tilt);

double upperArmLength(const PlayProfile& profile)
{
    return profile.heightCm * PlayProfile::upperArmRatio * (profile.armReachPercent / 100.0) / playCabinetFieldRadiusCm;
}

double forearmLength(const PlayProfile& profile)
{
    // Forearm plus the usable part of the hand between the wrist and contact.
    return profile.heightCm * 0.15 * (profile.armReachPercent / 100.0) / playCabinetFieldRadiusCm;
}

QVector3D shoulderPosition(int hand, const PlayProfile& profile, double offsetCm, double depthCm = 0.0)
{
    const double overCenter = profile.shoulderHeightCm() + offsetCm - playCabinetCenterCm;
    const double distance = profile.standingDistanceCm + depthCm;
    return QVector3D((hand == 0 ? -1.0 : 1.0) * profile.heightCm * 0.115 / playCabinetFieldRadiusCm,
        (distance * normalProjection - overCenter * verticalProjection) / playCabinetFieldRadiusCm,
        (distance * verticalProjection + overCenter * normalProjection) / playCabinetFieldRadiusCm);
}

QVector3D interpolate(const QVector3D& a, const QVector3D& b, double t)
{
    return a + (b - a) * static_cast<float>(t);
}

PlayArmPose armPose(int hand, const PlayHandMotion& motion, double palmRadius,
                   const PlayProfile& profile, double bodyOffsetCm, double distanceOffsetCm, bool raised)
{
    const double side = hand == 0 ? -1 : 1;
    PlayArmPose arm;
    arm.shoulder = shoulderPosition(hand, profile, bodyOffsetCm, distanceOffsetCm);
    arm.palmRadius = motion.palmRadius > 0.0 ? motion.palmRadius : palmRadius;
    arm.lifted = raised;
    arm.palm = QVector3D(motion.contact.x(), motion.contact.y(),
        motion.touching ? (motion.buttonLane > 0 ? 0.075 : 0.025) : raised ? 0.29 : 0.105);
    const QPointF towardBody = arm.shoulder.toVector2D().toPointF() - motion.contact;
    const double distance = std::hypot(towardBody.x(), towardBody.y());
    const QPointF offset = distance > epsilon ? towardBody * (playPalmLength / distance) : QPointF(0, playPalmLength);
    arm.wrist = arm.palm + QVector3D(offset.x(), offset.y(), raised ? 0.22 : 0.07);

    const QVector3D axis = arm.wrist - arm.shoulder;
    const double actualReach = axis.length();
    const double upper = upperArmLength(profile), lower = forearmLength(profile);
    const double reach = qBound(std::abs(upper - lower) + epsilon, actualReach, upper + lower - epsilon);
    const QVector3D direction = actualReach > epsilon ? axis.normalized() : QVector3D(0, -1, 0);
    arm.reachable = actualReach <= upper + lower + epsilon;
    if (!arm.reachable) {
        const QVector3D change = arm.shoulder + direction * static_cast<float>(reach) - arm.wrist;
        arm.wrist += change;
        arm.palm += change;
    }
    const double along = (upper * upper - lower * lower + reach * reach) / (2 * reach);
    const double bend = std::sqrt(qMax(0.0, upper * upper - along * along));
    const QVector3D outward(0.63 * side, 0.72, raised ? 0.92 : 0.18);
    auto bendDirection = outward - direction * QVector3D::dotProduct(direction, outward);
    if (bendDirection.lengthSquared() < epsilon) bendDirection = QVector3D::crossProduct(direction, QVector3D(0, 0, 1));
    bendDirection.normalize();
    arm.elbow = arm.shoulder + direction * static_cast<float>(along) + bendDirection * static_cast<float>(bend);
    return arm;
}

struct Capsule {
    QVector3D start;
    QVector3D end;
    double radius;
};

std::array<Capsule, 3> armVolumes(const PlayArmPose& arm)
{
    return {{{arm.shoulder, arm.elbow, playUpperArmRadius},
             {arm.elbow, arm.wrist, playForearmRadius},
             {arm.wrist, arm.palm, arm.palmRadius}}};
}

std::pair<QVector3D, QVector3D> closestPoints(const Capsule& a, const Capsule& b)
{
    const auto u = a.end - a.start;
    const auto v = b.end - b.start;
    const auto w = a.start - b.start;
    const double aa = QVector3D::dotProduct(u, u), bb = QVector3D::dotProduct(u, v);
    const double cc = QVector3D::dotProduct(v, v), dd = QVector3D::dotProduct(u, w);
    const double ee = QVector3D::dotProduct(v, w);
    const double determinant = aa * cc - bb * bb;
    double s = determinant > epsilon ? qBound(0.0, (bb * ee - cc * dd) / determinant, 1.0) : 0.0;
    double t = cc > epsilon ? (bb * s + ee) / cc : 0.0;
    if (t < 0.0 || t > 1.0 || cc <= epsilon) {
        t = qBound(0.0, t, 1.0);
        s = aa > epsilon ? qBound(0.0, (bb * t - dd) / aa, 1.0) : 0.0;
    }
    return {interpolate(a.start, a.end, s), interpolate(b.start, b.end, t)};
}

struct Clearance {
    double gap = std::numeric_limits<double>::infinity();
    QPointF point;
};

Clearance armClearance(const std::array<PlayArmPose, 2>& arms)
{
    Clearance result;
    for (const auto& left : armVolumes(arms[0])) for (const auto& right : armVolumes(arms[1])) {
        const auto [a, b] = closestPoints(left, right);
        const double gap = (a - b).length() - left.radius - right.radius;
        if (gap < result.gap) {
            result.gap = gap;
            result.point = ((a + b) * 0.5f).toVector2D().toPointF();
        }
    }
    return result;
}

} // namespace

double playReadyY(const PlayProfile& profile)
{
    const double offsetCm = playCabinetCenterCm - profile.readyHandHeightCm();
    return qBound(-0.84, offsetCm / (playCabinetFieldRadiusCm * verticalProjection), 0.84);
}

bool playArmCanPress(const PlayArmFrame& pose, const std::array<PlayHandMotion, 2>& hands, int hand)
{
    if (!pose.arms[hand].reachable) return false;
    const int other = 1 - hand;
    if (pose.interference != PlayInterference::Collision || !hands[other].touching || !pose.arms[other].reachable) return true;
    const auto priority = [](PlayContactKind kind) {
        return kind == PlayContactKind::Hold ? 2 : kind == PlayContactKind::Trace ? 1 : 0;
    };
    if (hands[hand].kind != hands[other].kind)
        return priority(hands[hand].kind) > priority(hands[other].kind);
    const double reach = (pose.arms[hand].wrist - pose.arms[hand].shoulder).length();
    const double otherReach = (pose.arms[other].wrist - pose.arms[other].shoulder).length();
    return std::abs(reach - otherReach) < epsilon ? hand == 0 : reach < otherReach;
}

PlayBodyOffset preferredBodyOffset(const std::array<PlayHandMotion, 2>& hands, const PlayProfile& profile)
{
    if (!profile.automaticHeight && !profile.automaticDistance) return {};
    const double total = upperArmLength(profile) + forearmLength(profile);
    const double comfortable = armExtensionTarget * total;
    // Lower or raise the shoulders only when the current reach benefits.
    // A free hand approaching an imminent target is considered before impact.
    struct Target { double vertical; double depth; double horizontalSquared; };
    std::array<Target, 2> targets{};
    int count = 0;
    const QVector3D vertical(0, -verticalProjection, normalProjection);
    const QVector3D depth(0, normalProjection, verticalProjection);
    for (int hand = 0; hand < 2; ++hand) {
        const auto& motion = hands[hand];
        if (motion.source < 0) continue;
        const bool grounded = motion.touching || motion.blocked || !std::isfinite(motion.targetSecond);
        const auto point = grounded ? motion.contact : motion.nextTarget;
        const auto delta = QVector3D(point.x(), point.y(), 0.06) - shoulderPosition(hand, profile, 0.0);
        const double along = QVector3D::dotProduct(delta, vertical);
        targets[count++] = {along, QVector3D::dotProduct(delta, depth), double(delta.x()) * delta.x()};
    }
    if (count == 0) return {};
    const auto cost = [&](double offsetCm, double distanceCm) {
        const double offset = offsetCm / playCabinetFieldRadiusCm;
        const double standOff = distanceCm / playCabinetFieldRadiusCm;
        double result = 0.22 * offset * offset + 0.49 * standOff * standOff;
        for (int i = 0; i < count; ++i) {
            const auto target = targets[i];
            const double gap = target.vertical - offset;
            const double forwardGap = target.depth - standOff;
            const double reach = std::sqrt(target.horizontalSquared + gap * gap + forwardGap * forwardGap) - playPalmLength;
            const double strain = qMax(0.0, reach - comfortable);
            const double excess = qMax(0.0, reach - total);
            result += strain * strain * 2.0 + excess * excess * 40.0;
            // Leave room to bend the elbows instead of standing against the glass.
            const double cramped = qMax(0.0, 0.53 * total - reach);
            result += cramped * cramped * 3.0;
        }
        return result;
    };
    const auto minimize = [](double first, double last, const auto& evaluate) {
        for (int i = 0; i < 9; ++i) {
            const double a = (2 * first + last) / 3.0;
            const double b = (first + 2 * last) / 3.0;
            if (evaluate(a) < evaluate(b)) last = b;
            else first = a;
        }
        const double chosen = (first + last) / 2.0;
        return std::abs(chosen) < 0.5 ? 0.0 : chosen;
    };
    PlayBodyOffset result;
    for (int pass = 0; pass < 2; ++pass) {
        if (profile.automaticHeight) result.verticalCm = minimize(-profile.lowerLimitCm, profile.raiseLimitCm,
            [&](double y) { return cost(y, result.distanceCm); });
        if (profile.automaticDistance) result.distanceCm = minimize(
            -qMin(profile.forwardLimitCm, qMax(0, profile.standingDistanceCm - 15)), profile.backwardLimitCm,
            [&](double z) { return cost(result.verticalCm, z); });
    }
    return result;
}

PlayArmFrame resolvePlayArms(const std::array<PlayHandMotion, 2>& hands, double palmRadius,
                            const PlayProfile& profile, double bodyOffsetCm, double distanceOffsetCm)
{
    PlayArmFrame frame;
    frame.arms = {armPose(0, hands[0], palmRadius, profile, bodyOffsetCm, distanceOffsetCm, false),
                  armPose(1, hands[1], palmRadius, profile, bodyOffsetCm, distanceOffsetCm, false)};
    for (int hand = 0; hand < 2; ++hand) {
        if (!frame.arms[hand].reachable && hands[hand].source >= 0) {
            frame.interference = PlayInterference::OutOfReach;
            frame.interferencePoint = hands[hand].contact;
            frame.interferenceHand = hand;
            return frame;
        }
    }
    if (hands[0].source < 0 && hands[1].source < 0) return frame;
    const auto flat = armClearance(frame.arms);
    if (flat.gap >= -epsilon) return frame;
    frame.interferencePoint = flat.point;

    // Try raising the wrist and elbow with a fixed palm for a sustained
    // contact. A travelling hand may lift its palm as well.
    double best = -epsilon;
    auto raised = frame.arms;
    for (int hand : {1, 0}) {
        auto candidate = frame.arms;
        candidate[hand] = armPose(hand, hands[hand], palmRadius, profile, bodyOffsetCm, distanceOffsetCm, true);
        if (!candidate[hand].reachable) continue;
        const double clearance = armClearance(candidate).gap;
        if (clearance <= best) continue;
        best = clearance;
        raised = candidate;
    }
    if (best > -epsilon) {
        frame.arms = raised;
        frame.interference = PlayInterference::Lifted;
    } else {
        frame.interference = PlayInterference::Collision;
    }
    return frame;
}

} // namespace miacode::hand_order
