#include "core/analysis/ChartStructureAnalysis.h"

#include "common/TaskCancellation.h"
#include "core/chart/ChartScoringEvents.h"

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

int laneDistance(int a, int b)
{
    const int distance = std::abs(std::clamp(a, 1, 8) - std::clamp(b, 1, 8));
    return std::min(distance, 8 - distance);
}

struct Attack {
    double second = 0.0;
    int lane = 0;
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
    QVector<double> holds(frameCount + 1), slides(frameCount + 1);
    const auto frameAt = [&](double second) {
        return static_cast<int>(std::clamp(second / curve.secondsPerFrame, 0.0, static_cast<double>(frameCount - 1)));
    };
    const auto occupy = [&](QVector<double>& delta, double start, double end) {
        if (!std::isfinite(start) || !std::isfinite(end) || end <= start || end <= 0 || start >= curve.durationSeconds) return;
        const int first = frameAt(start);
        const int last = frameAt(end);
        if (first == last) {
            const double share = std::min(1.0, (end - std::max(0.0, start)) / curve.secondsPerFrame);
            delta[first] += share;
            delta[first + 1] -= share;
            return;
        }
        const double firstShare = first + 1 - std::max(0.0, start) / curve.secondsPerFrame;
        const double lastShare = std::min(1.0, end / curve.secondsPerFrame - last);
        delta[first] += firstShare;
        delta[first + 1] += 1.0 - firstShare;
        delta[last] -= 1.0 - lastShare;
        delta[last + 1] -= lastShare;
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
    QSet<QString> slideHeads;
    for (const auto& note : chart.noteMarkers) {
        task::CancellationScope::checkpoint();
        const double second = chart_preview::toMediaSeconds(note.second, firstSeconds);
        const double end = chart_preview::toMediaSeconds(note.endSecond, firstSeconds);
        const bool slide = note.type == QLatin1String("slide") || note.type == QLatin1String("wifi");
        const bool touch = note.type == QLatin1String("touch") || note.type == QLatin1String("touch_hold");
        if (slide) {
            if (!note.trackMine) {
                const double trace = chart_preview::toMediaSeconds(std::max(note.second, note.slideTraceSecond), firstSeconds);
                occupy(slides, trace, end);
            }
            if (!note.hasHeadStar || note.headMine) continue;
            const QString head = QStringLiteral("%1/%2/%3/%4").arg(note.second, 0, 'g', 17)
                .arg(note.sourceLine).arg(note.sourceCol).arg(note.lane);
            if (slideHeads.contains(head)) continue;
            slideHeads.insert(head);
        } else {
            if (note.isMine) continue;
            if (note.type == QLatin1String("hold") || note.type == QLatin1String("touch_hold")) occupy(holds, second, end);
        }
        if (inMedia(second)) attacks.append({second, touch ? 0 : note.lane});
    }

    std::stable_sort(attacks.begin(), attacks.end(), [](const Attack& a, const Attack& b) { return a.second < b.second; });
    double previousSecond = -1.0;
    double previousInterval = 0.0;
    int previousLane = 0;
    for (qsizetype first = 0; first < attacks.size();) {
        task::CancellationScope::checkpoint();
        const auto& attack = attacks.at(first);
        auto& frame = curve.frames[frameAt(attack.second)];
        qsizetype end = first + 1;
        while (end < attacks.size() && attacks.at(end).second - attack.second < 0.0001) ++end;
        frame.simultaneous += end - first - 1;
        if (attack.lane > 0 && previousLane > 0 && attack.second - previousSecond < 0.75)
            frame.movement += laneDistance(attack.lane, previousLane) / 4.0;
        std::array<bool, 9> lanes{};
        for (auto index = first; index < end; ++index) {
            const int lane = attacks.at(index).lane;
            if (lane > 0 && lane <= 8) lanes[lane] = true;
        }
        int span = 0;
        for (int a = 1; a <= 8; ++a) {
            if (!lanes[a]) continue;
            for (int b = a + 1; b <= 8; ++b) {
                if (lanes[b]) span = std::max(span, laneDistance(a, b));
            }
        }
        frame.movement += span / 4.0;
        const double interval = attack.second - previousSecond;
        if (previousSecond >= 0.0 && previousInterval > 0.0 && interval < 1.0)
            frame.rhythm += std::min(1.0, std::abs(std::log2(interval / previousInterval)));
        previousSecond = attack.second;
        previousInterval = interval;
        previousLane = attacks.at(end - 1).lane;
        first = end;
    }

    double held = 0.0;
    double slide = 0.0;
    QVector<double> raw;
    raw.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        auto& frame = curve.frames[i];
        held += holds.at(i);
        slide += slides.at(i);
        frame.density /= curve.secondsPerFrame;
        frame.held = std::max(0.0, held);
        frame.slides = std::max(0.0, slide);
        raw.append(0.30 * std::log1p(frame.density) + 0.33 * std::log1p(frame.simultaneous)
            + 0.20 * frame.held + 0.26 * frame.slides + 0.18 * frame.movement + 0.13 * frame.rhythm);
    }
    const auto sums = prefix(raw);
    QVector<double> smoothed;
    smoothed.reserve(frameCount);
    const int shortRadius = std::max(1, static_cast<int>(0.5 / curve.secondsPerFrame));
    const int longRadius = std::max(1, static_cast<int>(1.5 / curve.secondsPerFrame));
    for (int i = 0; i < frameCount; ++i) {
        smoothed.append(0.58 * raw.at(i) + 0.30 * mean(sums, i - shortRadius, i + shortRadius + 1)
            + 0.12 * mean(sums, i - longRadius, i + longRadius + 1));
    }
    const double scale = std::max(percentile(smoothed, 0.95), *std::max_element(smoothed.cbegin(), smoothed.cend()) * 0.5);
    for (int i = 0; i < frameCount; ++i) curve.frames[i].strength = normalized(smoothed.at(i), scale);
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
        const double score = 0.31 * level + 0.30 * dynamic + 0.18 * lift + 0.08 * repetition + 0.13 * chartActivity;
        const bool confident = dynamic >= 0.45 && lift >= 0.12 && level >= 0.44
            && (peak - baseline) / peak >= 0.24 && score >= 0.42;
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
