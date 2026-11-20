#pragma once
#include "core/chart/ChartScoringEvents.h"
#include <QString>

namespace miacode::chart {
struct ScoreLossRow {
    QString object;
    QString judgment;
    int quantity = 0;
    int baseLoss = 0;
    int extraLoss = 0;
    double lossPercent = 0.0;
    int maximumSss = 0;
    int maximumSssPlus = 0;
};
struct ScoreLossResult {
    bool calculable = false;
    bool hasUnofficialExtensions = false;
    qint64 baseTotal = 0;
    int breakTotal = 0;
    int total = 0;
    int mineCount = 0;
    int simpleGreatSss = 0;
    int simpleGreatSssPlus = -1;
    QVector<int> counts = QVector<int>(9, 0); // six kinds, Break, Ex, Firework; attributes overlap
    QVector<ScoreLossRow> rows;
};

enum class ScoreScenarioError { None, Unavailable, InvalidQuantity, InventoryExceeded };
struct ScoreScenarioResult {
    ScoreScenarioError error = ScoreScenarioError::Unavailable;
    QString object;
    double maximumAchievement = 0.0;
    double lossPercent = 0.0;
    double achievement = 0.0;
    qint64 enteredJudgments = 0;
    bool valid() const { return error == ScoreScenarioError::None; }
};

inline ScoreScenarioResult calculateScoreScenario(const ScoreLossResult& score, const QVector<int>& quantities)
{
    ScoreScenarioResult result;
    if (!score.calculable) return result;
    if (quantities.size() != score.rows.size()) {
        result.error = ScoreScenarioError::InvalidQuantity;
        return result;
    }
    for (int index = 0; index < quantities.size(); ++index) {
        if (quantities[index] < 0 || quantities[index] > score.rows[index].quantity) {
            result.error = ScoreScenarioError::InvalidQuantity;
            result.object = score.rows[index].object;
            return result;
        }
    }
    // Eligible inventories are nested for each object: non-EX <= degrading EX
    // hold tails <= all notes (Miss). Check every threshold, not just row limits,
    // so the same scarce non-EX note cannot supply both Great and Good.
    for (const auto& limit : score.rows) {
        qint64 used = 0;
        for (int index = 0; index < score.rows.size(); ++index) {
            const auto& row = score.rows[index];
            if (row.object == limit.object && row.quantity <= limit.quantity) used += quantities[index];
        }
        if (used > limit.quantity) {
            result.error = ScoreScenarioError::InventoryExceeded;
            result.object = limit.object;
            return result;
        }
    }
    result.error = ScoreScenarioError::None;
    result.maximumAchievement = score.breakTotal > 0 ? 101.0 : 100.0;
    for (int index = 0; index < score.rows.size(); ++index) {
        result.lossPercent += quantities[index] * score.rows[index].lossPercent;
        result.enteredJudgments += quantities[index];
    }
    result.achievement = std::max(0.0, result.maximumAchievement - result.lossPercent);
    return result;
}
inline ScoreLossResult calculateScoreLoss(const ScoringEvents& source)
{
    ScoreLossResult result;
    result.hasUnofficialExtensions = source.hasUnofficialExtensions;
    result.mineCount = source.mineCount;
    QVector<int> normalCounts(6, 0), normalExLocked(6, 0);
    int breakExLocked = 0, breakExHolds = 0;
    for (const auto& event : source.events) {
        ++result.total;
        ++result.counts[static_cast<int>(event.kind)];
        if (event.isBreak) { ++result.breakTotal; ++result.counts[6]; }
        else {
            ++normalCounts[static_cast<int>(event.kind)];
            if (event.isEx && !event.exTailCanDegrade) ++normalExLocked[static_cast<int>(event.kind)];
        }
        if (event.isBreak && event.isEx) {
            ++breakExLocked;
            if (event.exTailCanDegrade) ++breakExHolds;
        }
        if (event.isEx) ++result.counts[7];
        if (event.isFirework) ++result.counts[8];
        result.baseTotal += event.baseScore();
    }
    result.calculable = result.total > 0 && !source.hasUnofficialExtensions;
    if (!result.calculable) return result;
    // A normal Tap Great loses 100 base points. Express the whole remaining
    // achievement budget in these units, independently of the Tap inventory.
    if (result.breakTotal) {
        result.simpleGreatSss = static_cast<int>(result.baseTotal / 10000);
        result.simpleGreatSssPlus = static_cast<int>(result.baseTotal / 20000);
    }
    const auto append = [&](const QString& object, const QString& judgment, int count,
                            int baseLoss, int extraLoss) {
        ScoreLossRow row{object, judgment, count, baseLoss, extraLoss};
        if (count <= 0 || (baseLoss == 0 && extraLoss == 0)) return;
        row.lossPercent = 100.0 * baseLoss / result.baseTotal
            + (result.breakTotal ? static_cast<double>(extraLoss) / (100 * result.breakTotal) : 0.0);
        // All official base scores are multiples of 500; all losses multiples of 50.
        // Keep the capacity comparison rational/integer; round only the displayed percent.
        const qint64 units = result.baseTotal / 500;
        const qint64 breaks = std::max(1, result.breakTotal);
        const qint64 loss = static_cast<qint64>(baseLoss / 50) * 1000 * breaks
            + static_cast<qint64>(extraLoss) * units;
        const auto capacity = [&](int targetHalfPercent) {
            const int maximumHalfPercent = result.breakTotal ? 202 : 200;
            const qint64 budget = static_cast<qint64>(maximumHalfPercent - targetHalfPercent)
                * units * 50 * breaks;
            // Scale: rate loss = loss / (units * breaks * 100).
            if (budget < 0) return 0;
            return loss == 0 ? count : static_cast<int>(std::min<qint64>(count, budget / loss));
        };
        row.maximumSss = capacity(200);
        row.maximumSssPlus = capacity(201);
        result.rows.append(row);
    };
    const QStringList names{QStringLiteral("Tap"), QStringLiteral("Hold"), QStringLiteral("Star"),
        QStringLiteral("Slide"), QStringLiteral("Touch"), QStringLiteral("TouchHold")};
    for (int kind = 0; kind < normalCounts.size(); ++kind) {
        if (!normalCounts[kind]) continue;
        const int base = (kind == 3 ? 1500 : (kind == 1 || kind == 5 ? 1000 : 500));
        append(names[kind], QStringLiteral("Perfect"), normalCounts[kind], 0, 0);
        append(names[kind], QStringLiteral("Great"), normalCounts[kind] - normalExLocked[kind], base / 5, 0);
        append(names[kind], QStringLiteral("Good"), normalCounts[kind] - normalExLocked[kind], base / 2, 0);
        append(names[kind], QStringLiteral("Miss"), normalCounts[kind], base, 0);
    }
    if (result.breakTotal) {
        const int nonEx = result.breakTotal - breakExLocked;
        const auto addBreak = [&](const char* judgment, int base, int extra, int eligible) {
            append(QStringLiteral("Break"), QString::fromLatin1(judgment), eligible, base, extra);
        };
        addBreak("Critical Perfect 2600", 0, 0, result.breakTotal);
        addBreak("Perfect 2550", 0, 25, nonEx + breakExHolds);
        addBreak("Perfect 2500", 0, 50, nonEx);
        addBreak("Great 2000", 500, 60, nonEx);
        addBreak("Great 1500", 1000, 60, nonEx + breakExHolds);
        addBreak("Great 1250", 1250, 60, nonEx);
        addBreak("Good 1000", 1500, 70, nonEx + breakExHolds);
        addBreak("Miss", 2500, 100, result.breakTotal);
    }
    return result;
}
} // namespace miacode::chart
