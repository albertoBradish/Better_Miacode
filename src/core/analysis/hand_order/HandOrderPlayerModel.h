#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::hand_order {

// Adjustable ergonomic heuristics, not measurements of human physiology.
struct PlayerProfile {
    static constexpr double minTapRate = 2.0, maxTapRate = 12.0;
    static constexpr double minMoveSpeed = 2.0, maxMoveSpeed = 20.0;
    static constexpr double minStamina = 0.5, maxStamina = 2.0;
    static constexpr double minRecoveryHalfLife = 0.5, maxRecoveryHalfLife = 10.0;
    static constexpr double minTechniquePreference = -1.0, maxTechniquePreference = 1.0;
    static constexpr double minPostureWeight = 0.0, maxPostureWeight = 8.0;
    double maxComfortableTapRate = 7.5;
    double comfortableMoveSpeed = 7.5;
    double staminaCapacity = 1.2;
    double recoveryHalfLife = 2.5;
    // Negative favours movement/decomposition; positive favours repeated strikes.
    double techniquePreference = 0.2;
    double postureWeight = 6.0;
};

inline bool validPlayerProfile(const PlayerProfile& profile)
{
    const auto between = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    return between(profile.maxComfortableTapRate, PlayerProfile::minTapRate, PlayerProfile::maxTapRate)
        && between(profile.comfortableMoveSpeed, PlayerProfile::minMoveSpeed, PlayerProfile::maxMoveSpeed)
        && between(profile.staminaCapacity, PlayerProfile::minStamina, PlayerProfile::maxStamina)
        && between(profile.recoveryHalfLife, PlayerProfile::minRecoveryHalfLife, PlayerProfile::maxRecoveryHalfLife)
        && between(profile.techniquePreference, PlayerProfile::minTechniquePreference, PlayerProfile::maxTechniquePreference)
        && between(profile.postureWeight, PlayerProfile::minPostureWeight, PlayerProfile::maxPostureWeight);
}

struct PlayerEffortState {
    double lastUpdate = -std::numeric_limits<double>::infinity();
    double lastStrike = -std::numeric_limits<double>::infinity();
    double load = 0.0;
};

struct PlayerEffortDelta {
    double cost = 0.0;
    double peakTapRate = 0.0;
    double peakLoad = 0.0;
    bool valid = true;
};

inline PlayerEffortDelta invalidPlayerEffort()
{
    return {std::numeric_limits<double>::infinity(), 0.0, 0.0, false};
}

inline bool validPlayerEffortState(const PlayerEffortState& state)
{
    const auto time = [](double value) {
        return std::isfinite(value) || value == -std::numeric_limits<double>::infinity();
    };
    return time(state.lastUpdate) && time(state.lastStrike)
        && state.lastStrike <= state.lastUpdate
        && std::isfinite(state.load) && state.load >= 0.0;
}

// Idle time restores capacity without changing the previous DOWN timestamp.
inline bool recoverPlayerEffort(PlayerEffortState& state, double second, const PlayerProfile& profile)
{
    if (!validPlayerProfile(profile) || !validPlayerEffortState(state)
        || !std::isfinite(second) || second < state.lastUpdate - 1e-8) return false;
    if (std::isfinite(state.lastUpdate))
        state.load *= std::exp2(-std::max(0.0, second - state.lastUpdate) / profile.recoveryHalfLife);
    state.lastUpdate = std::max(second, state.lastUpdate);
    return true;
}

// Call once for each independent hand strike, coalescing simultaneous contacts.
// A continuous sweep has movement effort even when it triggers several button
// DOWNs; the judgement replay still retains every distinct input transition.
inline PlayerEffortDelta playerStrike(PlayerEffortState& state, double second,
                                      const PlayerProfile& profile, double strikeWeight = 1.0)
{
    if (!std::isfinite(strikeWeight) || strikeWeight <= 0.0 || strikeWeight > 4.0)
        return invalidPlayerEffort();
    auto next = state;
    if (!recoverPlayerEffort(next, second, profile)) return invalidPlayerEffort();
    const double interval = second - next.lastStrike;
    // Distinct DOWNs at the same instant are not a valid sequential input.
    if (std::isfinite(next.lastStrike) && interval <= 1e-8) return invalidPlayerEffort();
    const double rate = std::isfinite(next.lastStrike) ? 1.0 / interval : 0.0;
    const double excess = std::max(0.0, rate / profile.maxComfortableTapRate - 1.0);
    const double before = next.load;
    next.load += 0.08 * strikeWeight * (1.0 + 0.25 * excess);
    next.lastStrike = second;
    const double fatigue = 0.04 * strikeWeight * (before * before + next.load * next.load)
        / (profile.staminaCapacity * profile.staminaCapacity);
    const double technique = 1.0 - 0.35 * profile.techniquePreference;
    // A fresh hand can spend a short burst above its sustained reference.
    // The discount disappears as real committed effort accumulates.
    const double burstCharge = 0.2 + 0.8 * std::min(1.0, before / profile.staminaCapacity);
    const double cost = (0.03 * strikeWeight + 0.9 * burstCharge * excess * excess) * technique + fatigue;
    if (!std::isfinite(next.load) || !std::isfinite(cost)) return invalidPlayerEffort();
    state = next;
    return {cost, rate, next.load / profile.staminaCapacity, true};
}

// Advance a non-overlapping, constant-speed physical path segment. The exact
// load integral makes constant-speed subdivision independent of frame rate.
// A sustained hold adds a small occupancy load, including at zero distance.
inline PlayerEffortDelta playerMovement(PlayerEffortState& state, double begin, double end,
                                        double distance, const PlayerProfile& profile, bool hold = false)
{
    if (!std::isfinite(begin) || !std::isfinite(end) || end <= begin
        || !std::isfinite(distance) || distance < 0.0) return invalidPlayerEffort();
    auto next = state;
    if (!recoverPlayerEffort(next, begin, profile)) return invalidPlayerEffort();
    const double duration = end - begin;
    const double speed = distance / duration;
    const double relativeSpeed = speed / profile.comfortableMoveSpeed;
    const double input = 0.1 * relativeSpeed * relativeSpeed + (hold ? 0.025 : 0.0);
    const double decay = std::log(2.0) / profile.recoveryHalfLife;
    const double equilibrium = input / decay;
    const double offset = next.load - equilibrium;
    const double oneMinusDecay = -std::expm1(-decay * duration);
    const double integral = equilibrium * equilibrium * duration
        + 2.0 * equilibrium * offset * oneMinusDecay / decay
        + offset * offset * (-std::expm1(-2.0 * decay * duration)) / (2.0 * decay);
    const double before = next.load;
    next.load += (equilibrium - next.load) * oneMinusDecay;
    next.lastUpdate = end;
    const double excess = std::max(0.0, relativeSpeed - 1.0);
    const double technique = 1.0 + 0.35 * profile.techniquePreference;
    const double cost = technique * (0.08 * distance + 0.6 * excess * excess * distance)
        + 0.08 * input * std::max(0.0, integral) / (profile.staminaCapacity * profile.staminaCapacity);
    if (!std::isfinite(next.load) || !std::isfinite(cost)) return invalidPlayerEffort();
    state = next;
    return {cost, 0.0, std::max(before, next.load) / profile.staminaCapacity, true};
}

} // namespace miacode::hand_order
