#include "app/services/JobProgressService.h"
#include <iterator>

#include <algorithm>

namespace miacode {

JobProgressService::JobProgressService(QObject* parent)
    : QObject(parent)
{
}

bool JobProgressService::chartExport() const
{
    return taskType_ == TaskType::ChartExport;
}

quint64 JobProgressService::begin(const LocalizedText& title,
                                  const LocalizedText& label,
                                  bool cancellable,
                                  TaskType taskType)
{
    active_ = true;
    ++token_;
    indeterminate_ = false;
    pausable_ = paused_ = false;
    cancellable_ = cancellable;
    taskType_ = taskType;
    // A new job always starts uncancelled: a stale flag from the previous job
    // would abort this one at its first checkpoint.
    cancelRequested_ = false;
    percent_ = 0;
    stages_.clear();
    remainingSeconds_ = -1;
    title_ = title;
    label_ = label;
    emit changed();
    return token_;
}

void JobProgressService::report(int percent, const LocalizedText& label)
{
    if (!active_) {
        return;
    }
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped == percent_ && label.text() == label_.text() && !indeterminate_) {
        return;
    }
    indeterminate_ = false;
    percent_ = clamped;
    label_ = label;
    emit changed();
}

void JobProgressService::reportIndeterminate(const LocalizedText& label)
{
    if (!active_) {
        return;
    }
    if (indeterminate_ && label.text() == label_.text()) {
        return;
    }
    indeterminate_ = true;
    label_ = label;
    emit changed();
}

QVariantList JobProgressService::stages() const
{
    static constexpr const char* names[]{
        "hand_order.workflow.stage_normalize", "hand_order.workflow.stage_patterns",
        "hand_order.workflow.stage_context", "hand_order.workflow.stage_verify", "hand_order.workflow.stage_conflicts",
    };
    QVariantList result;
    for (int i = 0; i < stages_.size() && i < int(std::size(names)); ++i)
        result.append(QVariantMap{{"name", qtTrId(names[i])}, {"percent", stages_.at(i)}});
    return result;
}

void JobProgressService::reportStages(int overall, const QVector<int>& percentages, qint64 remainingSeconds,
    const LocalizedText& label)
{
    if (!active_) return;
    stages_ = percentages;
    remainingSeconds_ = remainingSeconds;
    percent_ = std::clamp(overall, 0, 100);
    indeterminate_ = false;
    label_ = label;
    emit changed();
}

void JobProgressService::retranslate()
{
    if (active_) emit changed();
}
void JobProgressService::setPausable(bool enabled)
{
    if (!active_ || pausable_ == enabled) return;
    pausable_ = enabled; if (!enabled) paused_ = false; emit changed();
}
void JobProgressService::setPaused(bool paused)
{
    if (!active_ || !pausable_ || paused_ == paused) return;
    paused_ = paused; emit changed();
}
void JobProgressService::requestPauseToggle()
{
    if (!active_ || !pausable_ || cancelRequested_) return;
    const auto owner = token_;
    const bool paused = !paused_;
    setPaused(paused);
    if (active_ && token_ == owner) emit pauseRequested(owner, paused);
}

void JobProgressService::end()
{
    if (!active_) {
        return;
    }
    active_ = false;
    cancellable_ = false;
    cancelRequested_ = false;
    indeterminate_ = false;
    percent_ = 0;
    stages_.clear();
    remainingSeconds_ = -1;
    taskType_ = TaskType::Generic;
    pausable_ = paused_ = false;
    title_.clear();
    label_.clear();
    emit changed();
}

void JobProgressService::requestCancel()
{
    if (!active_ || !cancellable_ || cancelRequested_) {
        return;
    }
    cancelRequested_ = true;
    emit changed();
    emit cancellationRequested(token_);
}

}  // namespace miacode
