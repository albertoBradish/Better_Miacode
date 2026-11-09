#include "core/analysis/ChartStructureAnalysis.h"

#include "common/TaskCancellation.h"
#include "core/chart/ChartScoringEvents.h"

#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace miacode::chart_structure {
namespace {

constexpr int kFftSize = 512;
constexpr int kBandCount = 6;
constexpr int kMaxFrames = 16000;
using Spectrum = std::array<std::complex<double>, kFftSize>;

void fft(Spectrum& spectrum)
{
    for (int i = 1, j = 0; i < kFftSize; ++i) {
        int bit = kFftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(spectrum[i], spectrum[j]);
    }
    for (int span = 2; span <= kFftSize; span *= 2) {
        const auto step = std::polar(1.0, -2.0 * std::numbers::pi / span);
        for (int first = 0; first < kFftSize; first += span) {
            std::complex<double> phase = 1.0;
            for (int offset = 0; offset < span / 2; ++offset) {
                const auto even = spectrum[first + offset];
                const auto odd = spectrum[first + offset + span / 2] * phase;
                spectrum[first + offset] = even + odd;
                spectrum[first + offset + span / 2] = even - odd;
                phase *= step;
            }
        }
    }
}

double percentile(QVector<double> values, double fraction)
{
    if (values.isEmpty()) return 0.0;
    const auto middle = values.begin() + static_cast<qsizetype>(fraction * (values.size() - 1));
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

double normalized(double value, double scale)
{
    return scale > 1e-9 ? std::clamp(value / scale, 0.0, 1.0) : 0.0;
}

QVector<double> prefix(const QVector<double>& values)
{
    QVector<double> sums{0.0};
    sums.reserve(values.size() + 1);
    for (const double value : values) sums.append(sums.constLast() + value);
    return sums;
}

double mean(const QVector<double>& sums, int start, int end)
{
    start = std::clamp(start, 0, static_cast<int>(sums.size()) - 1);
    end = std::clamp(end, start, static_cast<int>(sums.size()) - 1);
    return end > start ? (sums.at(end) - sums.at(start)) / (end - start) : 0.0;
}

// The parser's touch positions and slide paths use 540-unit chart space.
constexpr double kPlayfieldCenter = 270.0;
constexpr double kButtonRadius = 240.0;
constexpr double kChordWindow = 0.0001;
constexpr double kMovementMemory = 0.65;

QPointF buttonPosition(int lane)
{
    const double angle = (-67.5 + (std::clamp(lane, 1, 8) - 1) * 45.0) * std::numbers::pi / 180.0;
    return {kPlayfieldCenter + kButtonRadius * std::cos(angle), kPlayfieldCenter + kButtonRadius * std::sin(angle)};
}

double reach(const QPointF& a, const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y()) / (2.0 * kButtonRadius);
}

double nearestReach(const QPointF& position, const QVector<QPointF>& other)
{
    double distance = 1.0;
    for (const auto& point : other) distance = std::min(distance, reach(position, point));
    return distance;
}

double pathReach(const QVector<QPointF>& points)
{
    double length = 0.0;
    for (qsizetype i = 1; i < points.size(); ++i) length += reach(points.at(i - 1), points.at(i));
    return length;
}

double slideReach(const TimelineNoteMarker& note)
{
    if (!note.wifiLanePoints.isEmpty()) {
        double length = 0.0;
        for (const auto& lane : note.wifiLanePoints) length = std::max(length, pathReach(lane));
        if (length > 0.0) return length;
    }
    double length = 0.0;
    for (const auto& segment : note.slideSegmentPoints) length += pathReach(segment);
    return length > 0.0 ? length : reach(buttonPosition(note.lane), buttonPosition(note.endLane));
}

struct Attack {
    double second = 0.0;
    QPointF position;
    bool startsHold = false;
};

struct Occupancy {
    double held = 0.0;
    double slides = 0.0;
    double extraWifi = 0.0;
    double travel = 0.0;
};

struct OccupancyEdge {
    double second = 0.0;
    Occupancy load;
};

} // namespace

AudioFeatures analyzeAudio(const QVector<float>& samples, int sampleRate, const std::atomic_bool* cancelled)
{
    AudioFeatures result;
    if (samples.isEmpty() || sampleRate <= 0) return result;
    result.durationSeconds = static_cast<double>(samples.size()) / sampleRate;
    result.secondsPerFrame = std::max(0.5, result.durationSeconds / (kMaxFrames - 1));
    result.frames.resize(static_cast<qsizetype>(std::ceil(result.durationSeconds / result.secondsPerFrame)));
    QVector<int> counts(result.frames.size());
    constexpr std::array<double, kBandCount - 1> limits{120, 250, 500, 1000, 2000};
    std::array<int, kFftSize / 2> bands{};
    std::array<double, kFftSize> hann{};
    for (int i = 0; i < kFftSize; ++i) {
        hann[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (kFftSize - 1));
        if (i < kFftSize / 2) {
            const double hz = static_cast<double>(i) * sampleRate / kFftSize;
            bands[i] = static_cast<int>(std::upper_bound(limits.begin(), limits.end(), hz) - limits.begin());
        }
    }
    std::array<double, kBandCount> previous{};
    for (qsizetype offset = 0; offset < samples.size(); offset += kFftSize) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return {};
        Spectrum spectrum{};
        const auto count = std::min<qsizetype>(kFftSize, samples.size() - offset);
        double squareSum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double sample = samples.at(offset + i);
            const double value = std::isfinite(sample) ? sample : 0.0;
            squareSum += value * value;
            spectrum[i] = value * hann[i];
        }
        fft(spectrum);
        std::array<double, kBandCount> powers{};
        for (int i = 1; i < kFftSize / 2; ++i) powers[bands[i]] += std::norm(spectrum[i]);
        const auto index = std::min(result.frames.size() - 1,
            static_cast<qsizetype>(offset / (sampleRate * result.secondsPerFrame)));
        auto& frame = result.frames[index];
        frame.energy += std::sqrt(squareSum / count);
        frame.lowBand += std::sqrt(powers[0] + powers[1]) / kFftSize;
        frame.highBand += std::sqrt(powers[4] + powers[5]) / kFftSize;
        for (int band = 0; band < kBandCount; ++band) {
            const double current = std::log1p(20.0 * std::sqrt(powers[band]) / kFftSize);
            frame.flux += std::max(0.0, current - previous[band]);
            previous[band] = current;
        }
        ++counts[index];
    }
    for (qsizetype i = 0; i < result.frames.size(); ++i) {
        auto& frame = result.frames[i];
        const int count = std::max(1, counts.at(i));
        frame.energy /= count;
        frame.flux /= count;
        frame.lowBand /= count;
        frame.highBand /= count;
    }
    return result;
}

PressureCurve estimatePressure(const SimaiParseResult& chart, double firstSeconds, double mediaDuration)
{
    PressureCurve curve;
    const double chartEnd = chart_preview::toMediaSeconds(std::max(chart.durationSeconds, chart.endSecond), firstSeconds);
    curve.durationSeconds = std::max({0.0, chartEnd, mediaDuration});
    if (!std::isfinite(curve.durationSeconds) || curve.durationSeconds <= 0.0) return {};
    curve.secondsPerFrame = std::max(0.25, curve.durationSeconds / (kMaxFrames - 1));
    const int frameCount = static_cast<int>(std::ceil(curve.durationSeconds / curve.secondsPerFrame));
    curve.frames.resize(frameCount);
    const auto frameAt = [&](double second) {
        return static_cast<int>(std::clamp(second / curve.secondsPerFrame, 0.0, static_cast<double>(frameCount - 1)));
    };
    const auto inMedia = [&](double second) {
        return std::isfinite(second) && second >= 0.0 && second <= curve.durationSeconds;
    };
    for (const auto& event : miacode::chart::buildScoringEvents(chart.noteMarkers).events) {
        task::CancellationScope::checkpoint();
        const double second = chart_preview::toMediaSeconds(event.second, firstSeconds);
        if (inMedia(second)) ++curve.frames[frameAt(second)].density;
    }

    QVector<Attack> attacks;
    QVector<OccupancyEdge> edges;
    const auto occupy = [&](double start, double end, Occupancy load) {
        if (!std::isfinite(start) || !std::isfinite(end)) return false;
        start = std::max(0.0, start);
        end = std::min(end, curve.durationSeconds);
        if (end <= start) return false;
        edges.append(OccupancyEdge{start, load});
        edges.append(OccupancyEdge{end, {-load.held, -load.slides, -load.extraWifi, -load.travel}});
        return true;
    };
    QSet<QString> slideHeads;
    for (const auto& note : chart.noteMarkers) {
        task::CancellationScope::checkpoint();
        const double second = chart_preview::toMediaSeconds(note.second, firstSeconds);
        const double end = chart_preview::toMediaSeconds(note.endSecond, firstSeconds);
        const bool wifi = note.type == QLatin1String("wifi");
        const bool slide = note.type == QLatin1String("slide") || wifi;
        const bool touch = note.type == QLatin1String("touch") || note.type == QLatin1String("touch_hold");
        bool startsHold = false;
        if (slide) {
            if (!note.trackMine) {
                const double trace = chart_preview::toMediaSeconds(std::max(note.second, note.slideTraceSecond), firstSeconds);
                if (std::isfinite(trace) && std::isfinite(end) && end > trace && end > 0.0
                    && trace < curve.durationSeconds) {
                    const double speed = std::min(4.0, slideReach(note) / std::max(0.06, end - trace));
                    occupy(trace, end, {0.0, 1.0, wifi ? 0.65 : 0.0, speed});
                }
            }
            if (!note.hasHeadStar || note.headMine) continue;
            const QString head = QStringLiteral("%1/%2/%3/%4").arg(note.second, 0, 'g', 17)
                .arg(note.sourceLine).arg(note.sourceCol).arg(note.lane);
            if (slideHeads.contains(head)) continue;
            slideHeads.insert(head);
        } else {
            if (note.isMine) continue;
            if (note.type == QLatin1String("hold") || note.type == QLatin1String("touch_hold"))
                startsHold = occupy(second, end, {1.0, 0.0, 0.0, 0.0});
        }
        if (inMedia(second))
            attacks.append(Attack{second, touch ? note.touchPoint : buttonPosition(note.lane), startsHold});
    }

    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::sort(attacks.begin(), attacks.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    // Integrate actual overlaps. Occupations in separate parts of the same
    // frame do not compete with one another or with a strike outside them.
    Occupancy active;
    double lastIntegrated = 0.0;
    const auto integrateTo = [&](double end) {
        const double load = active.held + active.slides + active.extraWifi;
        if (end > lastIntegrated && load > 0.0) {
            const double overlap = 0.36 * std::max(0.0, load - 1.0)
                + 0.22 * std::min(active.held, active.slides);
            for (int i = frameAt(lastIntegrated); i <= frameAt(end); ++i) {
                const double startInFrame = std::max(lastIntegrated, i * curve.secondsPerFrame);
                const double endInFrame = std::min(end, (i + 1) * curve.secondsPerFrame);
                const double share = std::max(0.0, endInFrame - startInFrame) / curve.secondsPerFrame;
                auto& frame = curve.frames[i];
                frame.held += share * active.held;
                frame.slides += share * active.slides;
                frame.movement += share * (0.32 * active.travel + 0.24 * active.extraWifi);
                frame.hybrid += share * overlap;
            }
        }
        lastIntegrated = end;
    };
    qsizetype edgeIndex = 0;
    const auto advanceOccupancy = [&](double second) {
        while (edgeIndex < edges.size() && edges.at(edgeIndex).second <= second) {
            task::CancellationScope::checkpoint();
            const auto& edge = edges.at(edgeIndex++);
            integrateTo(edge.second);
            active.held += edge.load.held;
            active.slides += edge.load.slides;
            active.extraWifi += edge.load.extraWifi;
            active.travel += edge.load.travel;
            if (active.slides == 0.0) {
                active.extraWifi = 0.0;
                active.travel = 0.0;
            }
        }
        integrateTo(second);
    };

    QVector<double> strikeCounts(frameCount);
    const double strikeScale = 0.25 / curve.secondsPerFrame;
    QVector<QPointF> previousPositions;
    QVector<QPointF> twoBackPositions;
    double previousSecond = -1.0;
    double previousInterval = 0.0;
    for (qsizetype first = 0; first < attacks.size();) {
        task::CancellationScope::checkpoint();
        const auto& attack = attacks.at(first);
        advanceOccupancy(attack.second);
        const int frameIndex = frameAt(attack.second);
        auto& frame = curve.frames[frameIndex];
        QVector<QPointF> positions;
        double startingHolds = 0.0;
        qsizetype end = first;
        while (end < attacks.size() && attacks.at(end).second - attack.second < kChordWindow) {
            const auto& contact = attacks.at(end++);
            if (!positions.contains(contact.position)) positions.append(contact.position);
            if (contact.startsHold && contact.second == attack.second) ++startingHolds;
        }
        const double strikes = end - first;
        strikeCounts[frameIndex] += strikes;
        frame.simultaneous += (strikes - 1) * strikeScale;
        const double occupied = std::max(0.0, active.held - startingHolds) * 0.40
            + active.slides * 0.58 + active.extraWifi * 0.24;
        frame.hybrid += strikeScale * occupied * std::sqrt(strikes);

        double span = 0.0;
        for (qsizetype a = 0; a < positions.size(); ++a) {
            for (qsizetype b = a + 1; b < positions.size(); ++b)
                span = std::max(span, reach(positions.at(a), positions.at(b)));
        }
        frame.movement += strikeScale * 0.72 * span;

        const double interval = attack.second - previousSecond;
        if (!previousPositions.isEmpty() && interval < kMovementMemory) {
            // Returning to a recent contact makes alternating patterns less
            // costly than a sequence of equally long, continuously new reaches.
            const bool hasReturn = !twoBackPositions.isEmpty() && previousInterval + interval < kMovementMemory;
            const double returnWeight = hasReturn ? 0.44 / (1.0 + occupied) : 0.0;
            const double urgency = 0.22 / std::max(0.06, interval) * (1.0 - interval / kMovementMemory);
            for (const auto& point : positions) {
                const double direct = nearestReach(point, previousPositions);
                const double returning = hasReturn ? std::min(direct, nearestReach(point, twoBackPositions)) : direct;
                frame.movement += strikeScale * urgency * (direct + returnWeight * (returning - direct));
                frame.rhythm += strikeScale * 0.22 * urgency * std::exp(-12.0 * direct);
            }
            if (previousInterval > 0.0 && previousInterval < kMovementMemory) {
                const double change = std::min(2.0, std::abs(std::log2(interval / previousInterval)));
                frame.rhythm += strikeScale * 0.32 * change;
            }
        }
        previousSecond = attack.second;
        previousInterval = interval;
        twoBackPositions = std::move(previousPositions);
        previousPositions = std::move(positions);
        first = end;
    }
    advanceOccupancy(curve.durationSeconds);

    double fatigue = 0.0;
    QVector<double> raw;
    raw.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        auto& frame = curve.frames[i];
        frame.density /= curve.secondsPerFrame;
        const double pressRate = strikeCounts.at(i) / curve.secondsPerFrame;
        const double exertion = 0.19 * std::log1p(pressRate)
            + 0.42 * std::log1p(frame.simultaneous) + 0.23 * frame.held + 0.25 * frame.slides
            + 0.78 * frame.movement + 0.52 * frame.rhythm + 0.56 * frame.hybrid;
        const double sustained = std::max(0.0, exertion - 0.22);
        const double timeConstant = sustained > fatigue ? 4.0 : 2.3;
        fatigue += (sustained - fatigue) * (1.0 - std::exp(-curve.secondsPerFrame / timeConstant));
        frame.endurance = fatigue;
        raw.append(exertion + 0.62 * fatigue);
    }
    const auto sums = prefix(raw);
    const int shortRadius = std::max(1, static_cast<int>(0.5 / curve.secondsPerFrame));
    for (int i = 0; i < frameCount; ++i) {
        const double effort = 0.66 * raw.at(i) + 0.34 * mean(sums, i - shortRadius, i + shortRadius + 1);
        curve.frames[i].strength = 1.0 - std::exp(-effort / 1.8);
    }
    return curve;
}

QVector<DropCandidate> recommendDrops(const AudioFeatures& audio, const PressureCurve& pressure)
{
    if (audio.frames.isEmpty() || !std::isfinite(audio.secondsPerFrame) || audio.secondsPerFrame <= 0.0
        || audio.durationSeconds < 4.0) return {};
    const int count = static_cast<int>(audio.frames.size());
    const int minimum = std::max(2, static_cast<int>(std::ceil(4.0 / audio.secondsPerFrame)));
    if (count < minimum) return {};
    QVector<double> energy, flux, low, high;
    for (const auto& frame : audio.frames) {
        energy.append(frame.energy);
        flux.append(frame.flux);
        low.append(frame.lowBand);
        high.append(frame.highBand);
    }
    const double peak = percentile(energy, 0.95);
    if (peak < 0.003) return {};
    const double baseline = percentile(energy, 0.45);
    const double fluxScale = std::max(0.015, percentile(flux, 0.90));
    const double lowScale = std::max(0.001, percentile(low, 0.90));
    const double highScale = std::max(0.001, percentile(high, 0.90));
    QVector<double> activity, contrast;
    for (int i = 0; i < count; ++i) {
        const double level = normalized(energy.at(i), peak);
        const double bass = normalized(low.at(i), lowScale);
        const double treble = normalized(high.at(i), highScale);
        const double onset = normalized(flux.at(i), fluxScale);
        activity.append(0.60 * level + 0.18 * onset + 0.14 * bass + 0.08 * treble);
        contrast.append(normalized(energy.at(i) - baseline, std::max(peak - baseline, peak * 0.25)));
    }
    const auto activitySum = prefix(activity);
    const auto contrastSum = prefix(contrast);
    QVector<double> pressureValues;
    for (const auto& frame : pressure.frames) pressureValues.append(frame.strength);
    const auto pressureSum = prefix(pressureValues);
    QVector<DropCandidate> ranked;
    const int window = std::max(minimum, static_cast<int>(std::ceil(8.0 / audio.secondsPerFrame)));
    const int lead = std::max(1, static_cast<int>(std::ceil(3.0 / audio.secondsPerFrame)));
    for (int start = 0; start + minimum <= count; ++start) {
        task::CancellationScope::checkpoint();
        const int end = std::min(start + window, count);
        const double level = mean(activitySum, start, end);
        const double lift = start >= lead ? std::max(0.0, level - mean(activitySum, start - lead, start)) : 0.0;
        const double dynamic = mean(contrastSum, start, end);
        if (level < 0.18) continue;
        double repetition = 0.0;
        for (const int gapSeconds : {8, 16, 32}) {
            const int gap = std::max(1, static_cast<int>(gapSeconds / audio.secondsPerFrame));
            for (const int direction : {-1, 1}) {
                const int match = start + gap * direction;
                if (match < 0 || match + (end - start) > count) continue;
                double difference = 0.0;
                for (int offset = 0; offset < end - start; ++offset) {
                    const int a = start + offset;
                    const int b = match + offset;
                    difference += std::abs(activity.at(a) - activity.at(b))
                        + 0.25 * std::abs(normalized(low.at(a), lowScale) - normalized(low.at(b), lowScale))
                        + 0.25 * std::abs(normalized(high.at(a), highScale) - normalized(high.at(b), highScale));
                }
                repetition = std::max(repetition, std::max(0.0, 1.0 - difference / (end - start))
                    * mean(contrastSum, match, match + (end - start)));
            }
        }
        const double startSeconds = start * audio.secondsPerFrame;
        const double endSeconds = std::min(audio.durationSeconds, end * audio.secondsPerFrame);
        const double chartActivity = pressure.frames.isEmpty() ? 0.0 : mean(pressureSum,
            static_cast<int>(startSeconds / pressure.secondsPerFrame), static_cast<int>(std::ceil(endSeconds / pressure.secondsPerFrame)));
        const double audioScore = 0.31 * level + 0.30 * dynamic + 0.18 * lift + 0.08 * repetition;
        const double score = audioScore + 0.13 * chartActivity;
        const bool confident = dynamic >= 0.45 && lift >= 0.12 && level >= 0.44
            && (peak - baseline) / peak >= 0.24 && audioScore >= 0.42;
        ranked.append({{startSeconds, endSeconds}, score, confident});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const DropCandidate& a, const DropCandidate& b) {
        if (a.confident != b.confident) return a.confident;
        return a.score > b.score;
    });
    QVector<DropCandidate> selected;
    for (const auto& candidate : ranked) {
        const auto overlaps = [&](const DropCandidate& other) {
            return candidate.range.startSeconds < other.range.endSeconds && candidate.range.endSeconds > other.range.startSeconds;
        };
        if (std::none_of(selected.cbegin(), selected.cend(), overlaps)) selected.append(candidate);
        if (selected.size() == 3) break;
    }
    return selected;
}

} // namespace miacode::chart_structure
