#pragma once

#include "core/analysis/hand_order/HandOrderProgress.h"
#include <algorithm>
#include <cmath>
#include <QtGlobal>
#include <QVector>
#include <array>

namespace miacode {
// Worker-local estimate with an injected monotonic clock for deterministic QA.
// Reports only the current measurable stage, never unknown later candidates.
class HandOrderProgressEstimate {
public:
    struct Reading { int percent = -1; qint64 remainingSeconds = -1; };
    Reading update(const hand_order::HandOrderProgress& progress, qint64 elapsedMs)
    {
        // Window completions interleave with decision/replay stages. Preserve
        // that window-series clock while each search attempt has its own clock.
        return (progress.phase == hand_order::HandOrderProgress::Windows ? windows_ : stage_)
            .update(progress, elapsedMs);
    }
private:
    struct Stage {
        Reading update(const hand_order::HandOrderProgress& progress, qint64 elapsedMs)
        {
            const bool changed = !known_ || progress.phase != previous_.phase
                || progress.candidate != previous_.candidate || progress.window != previous_.window
                || progress.attempt != previous_.attempt || progress.total != previous_.total;
            if (changed) {
                startMs_ = elapsedMs; completed_ = 0; smoothedMs_ = -1;
                originCompleted_ = std::clamp(progress.completed, 0, std::max(0, progress.total));
                previous_ = progress; known_ = true;
            }
            if (progress.total <= 0) return {};
            completed_ = std::max(completed_, std::clamp(progress.completed, 0, progress.total));
            Reading reading;
            reading.percent = int(100LL * completed_ / progress.total);
            const qint64 duration = std::max(qint64(0), elapsedMs - startMs_);
            const int measured = completed_ - originCompleted_;
            if (measured >= 3 && completed_ < progress.total && duration >= 1500) {
                const double estimate = double(duration) * (progress.total - completed_) / measured;
                smoothedMs_ = smoothedMs_ < 0 ? estimate : (2 * smoothedMs_ + estimate) / 3;
                reading.remainingSeconds = qint64(std::ceil(smoothedMs_ / 1000));
            } else if (completed_ == progress.total) reading.remainingSeconds = 0;
            return reading;
        }
        hand_order::HandOrderProgress previous_;
        bool known_ = false;
        int completed_ = 0;
        int originCompleted_ = 0;
        qint64 startMs_ = 0;
        double smoothedMs_ = -1;
    };
    Stage windows_, stage_;
};

class HandOrderWorkflowProgress {
public:
    struct Reading {
        QVector<int> stages;
        int percent = 0;
        qint64 remainingSeconds = -1;
    };
    static constexpr int stageCount = 5;

    void resetClock(qint64 elapsedMs)
    {
        sampleTime_ = elapsedMs;
        sampleUnits_ = units();
        millisecondsPerUnit_ = -1;
    }

    Reading update(const hand_order::HandOrderProgress& update, qint64 elapsedMs)
    {
        using Work = hand_order::HandOrderProgress;
        const int measured = update.total > 0 ? qBound(0, int(100LL * update.completed / update.total), 100) : 0;
        if (update.pass == Work::ContextSearch || update.pass == Work::VerifyAssignments) {
            const int stage = update.pass == Work::ContextSearch ? 2 : 3;
            if (update.pass == Work::VerifyAssignments && update.candidate > 0) {
                setStage(2, 100);
                setStage(3, 50 + passPercent(update, measured) / 2);
            } else if (update.pass == Work::VerifyAssignments && update.candidate == 0) {
                setStage(3, passPercent(update, measured) / 2);
            } else if (update.pass == Work::VerifyAssignments) {
                setStage(3, 90);
            } else {
                setStage(stage, passPercent(update, measured));
            }
        } else switch (update.phase) {
        case Work::Normalizing: setStage(0, measured); break;
        case Work::Patterns: setStage(1, measured); break;
        case Work::ConflictReview:
            setStage(2, 100);
            setStage(3, 100);
            setStage(4, measured);
            break;
        case Work::Finished:
            stages_.fill(100);
            break;
        default: break;
        }
        const int done = units();
        if (sampleTime_ < 0) resetClock(elapsedMs);
        if (elapsedMs - sampleTime_ >= 1000 && done - sampleUnits_ >= 100) {
            const double rate = double(elapsedMs - sampleTime_) / (done - sampleUnits_);
            millisecondsPerUnit_ = millisecondsPerUnit_ < 0 ? rate : (2 * millisecondsPerUnit_ + rate) / 3;
            sampleTime_ = elapsedMs;
            sampleUnits_ = done;
        }
        Reading result;
        for (int percent : stages_) result.stages.append(percent);
        result.percent = done / 100;
        if (done == 10000) result.remainingSeconds = 0;
        else if (millisecondsPerUnit_ > 0)
            result.remainingSeconds = qint64(std::ceil(millisecondsPerUnit_ * (10000 - done) / 1000));
        return result;
    }

private:
    static int passPercent(const hand_order::HandOrderProgress& update, int measured)
    {
        using Work = hand_order::HandOrderProgress;
        if (update.phase == Work::Finished) return 100;
        if (update.phase == Work::Replay && update.window == 0) return 96;
        if (update.phase == Work::Windows) return 95 * measured / 100;
        if (update.phase == Work::Decisions && update.windows > 0)
            return qBound(0, int(95LL * ((update.window - 1) * 100LL + measured) / (update.windows * 100LL)), 95);
        return 0;
    }
    void setStage(int stage, int percent) { stages_[stage] = qMax(stages_.at(stage), percent); }
    int units() const
    {
        constexpr std::array<int, stageCount> weights{4, 8, 64, 18, 6};
        int sum = 0;
        for (int i = 0; i < stageCount; ++i) sum += weights.at(i) * stages_.at(i);
        return sum;
    }
    std::array<int, stageCount> stages_{};
    qint64 sampleTime_ = -1;
    int sampleUnits_ = 0;
    double millisecondsPerUnit_ = -1;
};
} // namespace miacode
