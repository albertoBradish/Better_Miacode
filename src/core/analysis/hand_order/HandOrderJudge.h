#pragma once
#include "core/analysis/hand_order/BasicHandOrder.h"
#include <limits>

namespace miacode::hand_order {
// Frozen, auditable community-reference profile. This is a computation grid,
// not a claim about the timing of a particular cabinet or hardware version.
struct TouchJudgeProfile {
    static constexpr double ticksPerSecond = 180.0;
    static constexpr double criticalSeconds = 27.0 / ticksPerSecond;
    static constexpr double availableSeconds = 27.0 / ticksPerSecond;
    static constexpr double handRadius = 15.0 / 240.0;
};
struct ContactAction {
    int tokenStart = -1, branch = 0, hand = 0;
    QString kind;
    double start = 0, end = 0;
    double motionStart = 0, motionEnd = 0;
    QVector<QPointF> points;
    QVector<double> times;
    double radius = TouchJudgeProfile::handRadius;
};
struct TouchTarget {
    int tokenStart = -1, parseOrder = 0, eachGroupId = -1;
    QString pad;
    double second = 0;
    // Touch Hold participates in the input state but never in Touch groups.
    bool hold = false;
    int lock = -1;
    int sweepPrevious = -1;
    double tempoBpm = 0;
    int subdivision = 4;
    int layoutBasis = -1;
    bool gestureDefault = false;
};
struct TouchJudgement {
    bool judged = false, highQuality = false;
    bool stableResponsibility = true;
    int handMask = 0;
    double second = 0, margin = 0;
    QString kind, quality;
    QVector<BasicCause> causes;
};
struct TouchReplay {
    QVector<TouchJudgement> judgements;
    QVector<QVector<int>> groups;
    bool exhausted = false;
};
QPointF contactPosition(const ContactAction& action, double second);
QPointF touchPadPoint(const QString& pad);
bool touchPadsAdjacent(const QString& a, const QString& b);
QVector<QVector<int>> buildHandOrderTouchGroups(const QVector<TouchTarget>& targets);
TouchReplay replayHandOrderTouches(const QVector<TouchTarget>& targets, const QVector<ContactAction>& contacts,
    double throughSecond = std::numeric_limits<double>::infinity(), qint64* workRemaining = nullptr);
} // namespace miacode::hand_order
