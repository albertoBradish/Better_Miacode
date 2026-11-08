#pragma once

#include <QtGlobal>

namespace miacode::editor {
class SpaceInputGuard {
public:
    enum class Action { TogglePlayback, Unlock, Insert, Ignore };
    bool unlocked() const { return unlocked_; }
    void reset() { unlocked_ = false; count_ = 0; first_ = last_ = -1; }
    void interruptSequence() { count_ = 0; first_ = last_ = -1; }
    Action press(qint64 milliseconds, bool autoRepeat)
    {
        if (unlocked_) return Action::Insert;
        if (autoRepeat) return Action::Ignore;
        if (last_ < 0 || milliseconds < last_ || milliseconds - last_ > 300
            || milliseconds - first_ > 600) {
            count_ = 0;
            first_ = milliseconds;
        }
        last_ = milliseconds;
        if (++count_ == 3) {
            unlocked_ = true;
            return Action::Unlock;
        }
        return Action::TogglePlayback;
    }
private:
    bool unlocked_ = false;
    int count_ = 0;
    qint64 first_ = -1;
    qint64 last_ = -1;
};
} // namespace miacode::editor
