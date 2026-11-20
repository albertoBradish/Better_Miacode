#include "core/analysis/hand_order/HandOrderExperiment.h"
#include "common/TaskCancellation.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace miacode::hand_order::experiment {
namespace {
constexpr double segmentSeconds = 2.0;
void reason(Segment& segment, const QString& key)
{
    if (!segment.reasons.contains(key)) segment.reasons.append(key);
}

QJsonObject profileJson(const PlayerProfile& p)
{
    return {{"tap_rate", p.maxComfortableTapRate}, {"movement_speed", p.comfortableMoveSpeed},
        {"stamina", p.staminaCapacity}, {"recovery_half_life", p.recoveryHalfLife},
        {"technique_preference", p.techniquePreference}, {"posture_weight", p.postureWeight}};
}

double randomSigned(quint32& state)
{
    // Explicit integer PRNG avoids standard-library distribution differences.
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return 2.0 * static_cast<double>(state) / 4294967295.0 - 1.0;
}
}

bool withinInputLimit(const QString& text)
{
    // UTF-8 is never shorter than the UTF-16 code-unit count; reject large
    // strings before allocating the bounded temporary encoding.
    return text.size() <= maximumInputUtf8Bytes && text.toUtf8().size() <= maximumInputUtf8Bytes;
}

bool validParameters(const SimulationParameters& p)
{
    const auto bounded = [](double x, double lo, double hi) { return std::isfinite(x) && x >= lo && x <= hi; };
    return bounded(p.reactionMs, 0, 500) && bounded(p.movementSpeed, 2, 20)
        && bounded(p.stamina, 0.5, 2) && bounded(p.judgementJitterMs, 0, 100)
        && bounded(p.referenceWindowMs, 10, 500) && p.trials >= 1 && p.trials <= 200;
}

Analysis analyze(const QString& text, const SimaiParseResult& parsed,
                 const simai::SimaiTimingMetadata& timing, const PlayerProfile& profile)
{
    Analysis out;
    out.profile = profile;
    if (!withinInputLimit(text) || parsed.noteMarkers.size() > 10000) {
        out.error = QStringLiteral("input_limit"); return out;
    }
    out.inputSha256 = QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (!validPlayerProfile(profile) || !parsed.ok || !parsed.annotationErrors.isEmpty()) {
        out.error = QStringLiteral("invalid_chart_or_profile"); return out;
    }
    for (const auto& note : parsed.noteMarkers) {
        if (!std::isfinite(note.second) || std::abs(note.second) > 86400.0) {
            out.error = QStringLiteral("input_limit"); return out;
        }
    }
    BasicParameters p;
    p.player = profile;
    p.beamWidth = 16;
    p.maxTasks = 256;
    p.counterfactualBudget = 16;
    p.maxWorkUnits = 3000000;
    p.overwriteExisting = false;
    p.colorUnresolved = false;
    task::CancellationScope::check();
    // Reuse the existing protected-annotation solver; deliberately discard patches.
    const auto inferred = inferBasicHandOrder(text, parsed, 0, text.size(), p, timing);
    task::CancellationScope::check();
    out.error = inferred.error;
    if (!out.error.isEmpty()) return out;
    out.algorithmVersion = inferred.algorithmVersion;
    out.judgeProfileRevision = inferred.judgeProfileRevision;
    out.workUsed = inferred.workUsed;
    out.limited = inferred.truncated || inferred.evidenceTruncated || inferred.taskLimited || inferred.workLimited;
    QVector<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i) if (text[i] == QLatin1Char('\n')) lineStarts.append(i + 1);
    QMap<int, double> tokenSeconds;
    for (const auto& note : parsed.noteMarkers) {
        const int line = qBound(0, note.sourceLine - 1, static_cast<int>(lineStarts.size()) - 1);
        const int offset = lineStarts[line] + qMax(0, note.sourceCol - 1);
        if (!tokenSeconds.contains(offset) || tokenSeconds[offset] > note.second) tokenSeconds[offset] = note.second;
    }
    QMap<int, Segment> grouped;
    for (const auto& row : inferred.rows) {
        task::CancellationScope::checkpoint();
        if (row.context) continue;
        const auto it = tokenSeconds.lowerBound(row.start);
        if (it == tokenSeconds.cend() || it.key() >= row.end || !std::isfinite(it.value())) {
            ++out.unlocatedRows; continue;
        }
        const double second = it.value();
        const int bucket = static_cast<int>(std::floor(second / segmentSeconds));
        auto& segment = grouped[bucket];
        if (segment.events == 0) {
            segment.startSecond = bucket * segmentSeconds;
            segment.endSecond = segment.startSecond + segmentSeconds;
            segment.sourceStart = row.start; segment.sourceEnd = row.end;
            segment.line = row.line; segment.column = row.col;
        }
        segment.sourceStart = std::min(segment.sourceStart, row.start);
        segment.sourceEnd = std::max(segment.sourceEnd, row.end);
        ++segment.events;
        if (!row.hasCandidate || !row.fullReplayVerified) { ++segment.unresolved; reason(segment, QStringLiteral("unresolved")); }
        if (!row.originalAnnotation.isEmpty()) reason(segment, QStringLiteral("protected_annotation"));
        if (std::isfinite(row.playerCost)) segment.effortCost += std::max(0.0, row.playerCost);
        if (std::isfinite(row.peakSpeed)) segment.peakSpeed = std::max(segment.peakSpeed, row.peakSpeed);
        if (std::isfinite(row.peakTapRate)) segment.peakTapRate = std::max(segment.peakTapRate, row.peakTapRate);
        if (std::isfinite(row.peakLoad)) segment.peakLoad = std::max(segment.peakLoad, row.peakLoad);
        if (std::isfinite(row.confidence)) segment.candidateSupport += std::clamp(row.confidence, 0.0, 1.0);
        // A reused physical contact is a candidate for brushing, not proof that
        // the intended player will produce that input or earn its judgement.
        if (std::any_of(row.causes.cbegin(), row.causes.cend(), [&](const BasicCause& cause) {
            return cause.tokenStart >= 0 && cause.tokenStart != row.start;
        })) {
            ++segment.contactCandidates;
            reason(segment, QStringLiteral("shared_contact"));
        }
        if (!row.fullReplayVerified && row.hasCandidate) reason(segment, QStringLiteral("unverified_replay"));
    }
    for (auto& segment : grouped) {
        segment.candidateSupport /= std::max(1, segment.events);
        if (segment.peakTapRate > profile.maxComfortableTapRate) reason(segment, QStringLiteral("tap_rate"));
        if (segment.peakSpeed > profile.comfortableMoveSpeed) reason(segment, QStringLiteral("movement"));
        if (segment.peakLoad > 1.0) reason(segment, QStringLiteral("fatigue"));
        if (out.limited) reason(segment, QStringLiteral("bounded_search"));
        reason(segment, QStringLiteral("uncalibrated"));
        out.segments.append(segment);
    }
    out.valid = true;
    return out;
}

Simulation simulate(const Analysis& analysis, const SimulationParameters& p)
{
    Simulation out; out.parameters = p;
    if (!analysis.valid || !validParameters(p)) { out.error = QStringLiteral("invalid_analysis_or_parameters"); return out; }
    if (!validPlayerProfile(analysis.profile) || analysis.segments.size() > 10000) {
        out.error = QStringLiteral("invalid_analysis_or_parameters"); return out;
    }
    qint64 eventsTotal = 0;
    for (const auto& segment : analysis.segments) {
        if (segment.events < 0 || segment.unresolved < 0 || segment.unresolved > segment.events
            || !std::isfinite(segment.peakSpeed) || segment.peakSpeed < 0
            || !std::isfinite(segment.peakLoad) || segment.peakLoad < 0) {
            out.error = QStringLiteral("invalid_analysis_or_parameters"); return out;
        }
        eventsTotal += segment.events;
    }
    if (eventsTotal > 10000) { out.error = QStringLiteral("input_limit"); return out; }
    quint32 state = p.seed ? p.seed : 0x9e3779b9u;
    for (int index = 0; index < analysis.segments.size(); ++index) {
        const auto& segment = analysis.segments[index];
        SimulationSegment row; row.index = index;
        QVector<double> offsets;
        // Unresolved assignments are excluded rather than silently assigned a hand.
        const int events = std::max(0, segment.events - segment.unresolved);
        const double movementOverload = std::max(0.0, segment.peakSpeed / p.movementSpeed - 1.0);
        const double fatigueOverload = std::max(0.0,
            segment.peakLoad * analysis.profile.staminaCapacity / p.stamina - 1.0);
        const double meanDelay = p.reactionMs * (1.0 + movementOverload + 0.5 * fatigueOverload);
        if (!std::isfinite(movementOverload) || !std::isfinite(fatigueOverload) || !std::isfinite(meanDelay)) {
            out.error = QStringLiteral("invalid_analysis_or_parameters"); return out;
        }
        offsets.reserve(events * p.trials);
        for (int trial = 0; trial < p.trials; ++trial) for (int event = 0; event < events; ++event) {
            task::CancellationScope::checkpoint();
            const double offset = std::abs(meanDelay + randomSigned(state) * p.judgementJitterMs);
            const double accumulated = row.meanAbsOffsetMs + offset;
            if (!std::isfinite(offset) || !std::isfinite(accumulated)) {
                out.error = QStringLiteral("invalid_analysis_or_parameters"); return out;
            }
            offsets.append(offset);
            row.meanAbsOffsetMs = accumulated;
            if (offset > p.referenceWindowMs) ++row.outsideWindow;
        }
        row.samples = offsets.size();
        if (!offsets.isEmpty()) {
            row.meanAbsOffsetMs /= offsets.size();
            std::sort(offsets.begin(), offsets.end());
            row.p95AbsOffsetMs = offsets[std::min(offsets.size() - 1, static_cast<qsizetype>(std::ceil(offsets.size() * 0.95) - 1))];
        }
        out.samples += row.samples; out.outsideWindow += row.outsideWindow;
        out.segments.append(row);
    }
    out.valid = true;
    return out;
}

QJsonObject toJson(const Analysis& a)
{
    QJsonArray segments;
    for (const auto& s : a.segments) segments.append(QJsonObject{
        {"start_seconds", s.startSecond}, {"end_seconds", s.endSecond}, {"source_start", s.sourceStart},
        {"source_end", s.sourceEnd}, {"line", s.line}, {"column", s.column}, {"events", s.events},
        {"unresolved", s.unresolved}, {"shared_contact_candidates", s.contactCandidates},
        {"effort_cost", s.effortCost}, {"peak_speed", s.peakSpeed}, {"peak_tap_rate", s.peakTapRate},
        {"peak_load", s.peakLoad}, {"candidate_support_not_probability", s.candidateSupport},
        {"reasons", QJsonArray::fromStringList(s.reasons)}});
    return {{"experiment_version", version}, {"algorithm_version", a.algorithmVersion},
        {"judge_profile_revision", a.judgeProfileRevision}, {"valid", a.valid}, {"limited", a.limited},
        {"calibrated_against_human_players", false}, {"input_sha256", a.inputSha256}, {"error", a.error},
        {"work_used", a.workUsed}, {"unlocated_rows", a.unlocatedRows}, {"beam_width", 16}, {"max_work_units", 3000000}, {"input_preference", "buttons_preferred"},
        {"profile", profileJson(a.profile)}, {"segments", segments}};
}

QJsonObject toJson(const Simulation& s)
{
    QJsonArray segments;
    for (const auto& row : s.segments) segments.append(QJsonObject{{"segment_index", row.index},
        {"samples", row.samples}, {"outside_reference_window", row.outsideWindow},
        {"mean_absolute_offset_ms", row.meanAbsOffsetMs}, {"p95_absolute_offset_ms", row.p95AbsOffsetMs}});
    return {{"experiment_version", version}, {"kind", "synthetic_timing_scenario"},
        {"calibrated_against_human_players", false}, {"official_score_prediction", false},
        {"valid", s.valid}, {"error", s.error}, {"seed", static_cast<qint64>(s.parameters.seed)},
        {"reaction_ms", s.parameters.reactionMs}, {"movement_speed", s.parameters.movementSpeed},
        {"stamina", s.parameters.stamina}, {"judgement_jitter_ms", s.parameters.judgementJitterMs},
        {"reference_window_ms", s.parameters.referenceWindowMs}, {"trials", s.parameters.trials},
        {"samples", s.samples}, {"outside_reference_window", s.outsideWindow}, {"segments", segments}};
}
} // namespace miacode::hand_order::experiment
