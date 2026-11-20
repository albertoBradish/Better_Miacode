#include "core/analysis/hand_order/BasicHandOrder.h"
#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace miacode::hand_order;

namespace {
BasicResult solve(const QString& text, BasicParameters parameters = {})
{
    parameters.counterfactualBudget = 0;
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), 0, text.size(), parameters);
}
bool verified(const BasicResult& result)
{
    return result.error.isEmpty() && !result.rows.isEmpty()
        && std::all_of(result.rows.begin(), result.rows.end(), [](const auto& row) {
            return row.fullReplayVerified;
        });
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message, const BasicResult& result = {}) {
        QTextStream out(pass ? stdout : stderr);
        out << (pass ? "PASS: " : "FAIL: ") << message << '\n';
        ok &= pass;
        if (!pass) {
            out << "error=" << result.error << " work=" << result.workUsed << '\n';
            for (const auto& row : result.rows)
                out << row.start << " status=" << row.status << " reason=" << row.reason
                    << " hand=" << int(row.head) << " cost=" << row.playerCost
                    << " tapRate=" << row.peakTapRate << " load=" << row.peakLoad << '\n';
        }
    };
    BasicParameters lowSpeed, highSpeed;
    lowSpeed.player.maxComfortableTapRate = 2.0;
    highSpeed.player.maxComfortableTapRate = 12.0;
    const QString pair = QStringLiteral("(180){16}1,1,");
    const auto low = solve(pair, lowSpeed), high = solve(pair, highSpeed);
    expect(verified(low) && verified(high) && low.rows.size() == 2 && high.rows.size() == 2,
        "both player profiles retain full replay feasibility", low);
    if (low.rows.size() == 2 && high.rows.size() == 2)
        expect(low.rows[0].head != low.rows[1].head && high.rows[0].head == high.rows[1].head,
            "configured hand speed changes a feasible rapid pair between alternating and bracing", low);
    expect(std::any_of(high.rows.begin(), high.rows.end(), [](const auto& row) {
        return row.playerCost > 0 && row.peakTapRate > 0 && row.peakLoad > 0;
    }), "ordinary button fast path emits player effort evidence", high);
    bool preferenceChangesHands = false;
    for (double reference = 2.0; reference <= 6.0 && !preferenceChangesHands; reference += 0.1) {
        auto decompose = highSpeed, brace = highSpeed;
        decompose.player.maxComfortableTapRate = brace.player.maxComfortableTapRate = reference;
        decompose.player.techniquePreference = -1.0; brace.player.techniquePreference = 1.0;
        const auto a = solve(pair, decompose), b = solve(pair, brace);
        if (!verified(a) || !verified(b) || a.rows.size() != 2 || b.rows.size() != 2) continue;
        preferenceChangesHands = a.rows[0].head != a.rows[1].head && b.rows[0].head == b.rows[1].head;
    }
    expect(preferenceChangesHands,
        "technique preference can choose alternating or bracing at the same configured ability");

    auto mixed = highSpeed; mixed.inputPreference = 2;
    const auto mixedPair = solve(pair, mixed);
    expect(verified(mixedPair) && std::any_of(mixedPair.rows.begin(), mixedPair.rows.end(), [](const auto& row) {
        return row.playerCost > 0 && row.peakLoad > 0;
    }), "mixed physical route uses the same player effort model", mixedPair);

    QString locked = QStringLiteral("(150){16}");
    for (int i = 0; i < 16; ++i) locked += QStringLiteral("2~r,");
    const auto accumulated = solve(locked);
    expect(verified(accumulated) && accumulated.windowCount > 1
        && accumulated.rows.last().peakLoad > accumulated.rows.first().peakLoad,
        "accepted per-hand fatigue survives rolling windows", accumulated);
    auto weak = highSpeed; weak.player.staminaCapacity = 0.5;
    const auto ordinaryStamina = solve(locked, highSpeed), weakStamina = solve(locked, weak);
    expect(verified(weakStamina) && verified(ordinaryStamina)
        && weakStamina.rows.last().playerCost > ordinaryStamina.rows.last().playerCost,
        "lower stamina raises cost for identical locked physical history", weakStamina);
    const auto lowAbilityLocked = solve(locked, lowSpeed);
    expect(verified(lowAbilityLocked) && std::any_of(lowAbilityLocked.rows.begin(), lowAbilityLocked.rows.end(), [](const auto& row) {
        return row.peakTapRate > 2.0 && row.pressureLevel == QStringLiteral("extreme") && row.confidence < 0.4;
    }), "same-key repeated hits expose high player pressure even with nearly zero travel", lowAbilityLocked);
    const auto recovered = solve(locked + QStringLiteral("{1},,,2~r,"));
    if (verified(accumulated) && verified(recovered))
        expect(recovered.rows.last().peakLoad < accumulated.rows.last().peakLoad,
            "a real chart rest restores per-hand capacity before the next DOWN", recovered);
    else expect(false, "rest-recovery chart remains replayable", recovered);

    BasicParameters screen; screen.inputPreference = 1;
    const auto sweep = solve(QStringLiteral("(180){32}1,2,3,4,"), screen);
    expect(verified(sweep) && std::all_of(sweep.rows.begin(), sweep.rows.end(), [](const auto& row) {
        return row.peakTapRate < 1.0;
    }), "continuous recognized screen sweep is not charged as repeated physical DOWNs", sweep);
    const auto rimSweep = solve(QStringLiteral("(190){48}4,,,3,,,2,,,1,,,"));
    expect(verified(rimSweep) && std::all_of(rimSweep.rows.begin(), rimSweep.rows.end(), [](const auto& row) {
        return row.head == HandOrderHand::Right && row.peakTapRate < 1.0;
    }), "equally timed adjacent rim buttons form one wrist sweep across sparse grid notation", rimSweep);

    BasicParameters invalid; invalid.player.comfortableMoveSpeed = std::numeric_limits<double>::quiet_NaN();
    const auto rejected = solve(pair, invalid);
    expect(rejected.error == QStringLiteral("invalid_parameters") && rejected.patches.isEmpty(),
        "invalid player settings cannot produce applicable annotations", rejected);

    BasicParameters postureFree; postureFree.player.postureWeight = 0;
    const QString shortReach = QStringLiteral("(120){4}1h[4:1]~l,");
    const QString longReach = QStringLiteral("(120){4}1h[1:2]~l,");
    const auto shortHeld = solve(shortReach), longHeld = solve(longReach);
    const auto shortNeutral = solve(shortReach, postureFree), longNeutral = solve(longReach, postureFree);
    expect(verified(shortHeld) && verified(longHeld) && verified(shortNeutral) && verified(longNeutral)
        && shortHeld.rows.first().playerCost > shortNeutral.rows.first().playerCost
        && longHeld.rows.first().playerCost - longNeutral.rows.first().playerCost
            > 10 * (shortHeld.rows.first().playerCost - shortNeutral.rows.first().playerCost),
        "holding a contralateral pose costs its required body time rather than only one shallow strike", longHeld);
    const QString centreHold = QStringLiteral("(120){4}Ch[1:2]~l,");
    const auto centreHeld = solve(centreHold), centreNeutral = solve(centreHold, postureFree);
    expect(verified(centreHeld) && verified(centreNeutral)
        && std::abs(centreHeld.rows.first().playerCost - centreNeutral.rows.first().playerCost) < 1e-8,
        "a central DX Touch Hold has no invented contralateral posture charge", centreHeld);

    QString budgetChart = QStringLiteral("(120){4}1w5[4:1],,,,,B8/C/B1,,,,{16}");
    for (int i = 0; i < 24; ++i) budgetChart += i % 2 ? QStringLiteral("6,") : QStringLiteral("2,");
    bool observedLimit = false, observedVerified = false;
    for (qint64 budget : {qint64(1000), qint64(5000), qint64(20000), qint64(80000), qint64(200000), qint64(800000)}) {
        BasicParameters bounded; bounded.maxWorkUnits = budget;
        const auto result = solve(budgetChart, bounded);
        bool consistent = result.error.isEmpty() && result.workUsed >= 0 && result.workUsed <= budget;
        observedLimit |= result.workLimited;
        for (const auto& row : result.rows) {
            observedVerified |= row.fullReplayVerified;
            if (row.status == QStringLiteral("review"))
                consistent &= row.hasCandidate && row.fullReplayVerified
                    && row.conditions.contains(QStringLiteral("full_candidate_scope_replay"));
            if (row.status == QStringLiteral("heuristic"))
                consistent &= !row.fullReplayVerified && row.confidence == 0 && row.conditions.isEmpty();
            if (row.reason == QStringLiteral("unstable_touch_responsibility")) {
                consistent &= row.status == QStringLiteral("undecided");
                consistent &= std::none_of(result.patches.begin(), result.patches.end(), [&](const auto& patch) {
                    return patch.start == row.start;
                });
            }
        }
        for (const auto& patch : result.patches)
            consistent &= std::any_of(result.rows.begin(), result.rows.end(), [&](const auto& row) {
                return row.start == patch.start && (row.status == QStringLiteral("review")
                    || row.status == QStringLiteral("heuristic"));
            });
        expect(consistent,
            "bounded retries retain only replayed reviews or explicitly unverified fallbacks within total work", result);
    }
    expect(observedLimit && observedVerified,
        "budget regression exercises both exhausted searches and retained replayed candidates");
    return ok ? 0 : 1;
}
