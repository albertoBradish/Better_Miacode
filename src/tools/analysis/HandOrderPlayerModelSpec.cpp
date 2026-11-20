#include "core/analysis/hand_order/HandOrderPlayerModel.h"
#include <iostream>

using namespace miacode::hand_order;

int main()
{
    bool ok = true;
    const auto expect = [&](bool pass, const char* description) {
        (pass ? std::cout : std::cerr) << (pass ? "PASS: " : "FAIL: ") << description << '\n';
        ok &= pass;
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const PlayerProfile profile;
    expect(validPlayerProfile(profile), "default profile is valid");
    auto invalid = profile;
    invalid.staminaCapacity = std::numeric_limits<double>::quiet_NaN();
    expect(!validPlayerProfile(invalid), "NaN capacity is rejected");
    invalid = profile; invalid.maxComfortableTapRate = 12.01;
    expect(!validPlayerProfile(invalid), "out-of-range tap rate is rejected");
    invalid = profile; invalid.techniquePreference = -1.01;
    expect(!validPlayerProfile(invalid), "out-of-range technique preference is rejected");

    PlayerEffortState repeated;
    const auto initial = playerStrike(repeated, 0.0, profile);
    const auto rapid = playerStrike(repeated, 0.1, profile);
    expect(initial.valid && rapid.valid && rapid.cost > initial.cost && close(rapid.peakTapRate, 10.0),
        "stationary repeated DOWNs have hand-speed pressure without any movement");
    auto quicker = profile; quicker.maxComfortableTapRate = 10.0;
    PlayerEffortState capable;
    playerStrike(capable, 0.0, quicker);
    expect(playerStrike(capable, 0.1, quicker).cost < rapid.cost,
        "a higher configured hand-speed reference lowers the same rapid pattern's cost");
    const double oldLoad = repeated.load;
    const double oldStrike = repeated.lastStrike;
    expect(recoverPlayerEffort(repeated, repeated.lastUpdate + profile.recoveryHalfLife, profile)
        && close(repeated.load, oldLoad / 2.0) && repeated.lastStrike == oldStrike,
        "one recovery half-life halves load and preserves the true DOWN time");
    const auto saved = repeated;
    const auto backward = playerStrike(repeated, -1.0, profile);
    expect(!backward.valid && repeated.load == saved.load && repeated.lastUpdate == saved.lastUpdate,
        "out-of-order events fail without discarding accumulated load");

    PlayerEffortState whole, split;
    playerStrike(whole, 0.0, profile); split = whole;
    const auto full = playerMovement(whole, 0.0, 2.0, 15.0, profile, true);
    double splitCost = 0.0, splitPeak = 0.0;
    bool splitValid = true;
    for (int i = 0; i < 200; ++i) {
        const auto part = playerMovement(split, i * 0.01, (i + 1) * 0.01, 0.075, profile, true);
        splitValid &= part.valid;
        splitCost += part.cost; splitPeak = std::max(splitPeak, part.peakLoad);
    }
    expect(full.valid && splitValid && close(whole.load, split.load)
        && close(full.cost, splitCost) && close(full.peakLoad, splitPeak),
        "constant-speed path subdivision preserves load, cost and peak pressure");
    auto mobile = profile; mobile.comfortableMoveSpeed = 12.0;
    PlayerEffortState mobileState;
    playerStrike(mobileState, 0.0, mobile);
    expect(playerMovement(mobileState, 0.0, 2.0, 15.0, mobile, true).cost < full.cost,
        "a higher configured travel-speed reference lowers the same motion's cost");

    auto weak = profile; weak.staminaCapacity = 0.5;
    PlayerEffortState usual, tired;
    double usualCost = 0.0, weakCost = 0.0;
    for (int i = 0; i < 32; ++i) {
        usualCost += playerStrike(usual, i * 0.2, profile).cost;
        weakCost += playerStrike(tired, i * 0.2, weak).cost;
    }
    expect(weakCost > usualCost && close(tired.load, usual.load),
        "lower stamina increases repeated-hit cost without inventing extra physical work");

    PlayerEffortState burst;
    double firstBurst = 0.0, sustainedBurst = 0.0;
    for (int i = 0; i < 32; ++i) {
        const auto strike = playerStrike(burst, i * 0.08, profile);
        if (i == 1) firstBurst = strike.cost;
        if (i == 31) sustainedBurst = strike.cost;
    }
    expect(sustainedBurst > firstBurst * 2.0,
        "brief speed bursts cost less than sustained overload at the identical strike rate");

    auto decomposition = profile, bracing = profile;
    decomposition.techniquePreference = -1.0; bracing.techniquePreference = 1.0;
    PlayerEffortState a, b;
    playerStrike(a, 0.0, decomposition); playerStrike(b, 0.0, bracing);
    const double splitStrike = playerStrike(a, 0.1, decomposition).cost;
    const double braceStrike = playerStrike(b, 0.1, bracing).cost;
    a = {}; b = {};
    const double splitMove = playerMovement(a, 0.0, 0.2, 2.0, decomposition).cost;
    const double braceMove = playerMovement(b, 0.0, 0.2, 2.0, bracing).cost;
    expect(splitStrike > braceStrike && splitMove < braceMove,
        "technique preference trades repeated strikes against travel in the intended direction");

    PlayerEffortState holding, idle;
    const auto held = playerMovement(holding, 0.0, 5.0, 0.0, profile, true);
    const auto rested = playerMovement(idle, 0.0, 5.0, 0.0, profile);
    expect(held.valid && rested.valid && held.cost > rested.cost && holding.load > idle.load,
        "a stationary sustained hold adds occupancy load while idle time does not");
    a = {};
    playerStrike(a, 0.0, profile);
    const auto simultaneous = playerStrike(a, 0.0, profile);
    expect(!simultaneous.valid, "simultaneous contacts must be coalesced into one physical DOWN");
    return ok ? 0 : 1;
}
