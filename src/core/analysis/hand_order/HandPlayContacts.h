#pragma once

#include "core/analysis/MuriTimeIndex.h"
#include "core/analysis/MuriRenderOptions.h"
#include "core/analysis/hand_order/HandOrderJudge.h"

#include <array>
#include <limits>

namespace miacode::hand_order {

inline constexpr double playFieldRadius = 240.0;

inline double playPalmRadius(const MuriRenderOptions& options)
{
    return qMax(0.015, muri::handRadiusForOptions(options) / playFieldRadius);
}

enum class PlayContactKind { Strike, Hold, Trace };
enum class PlayBlock { None, Occupied, TooFast };

struct PlayTraceShortcut {
    double fromSecond = 0.0;
    double toSecond = 0.0;
    double availableSecond = 0.0;
};
using PlayTraceShortcuts = QHash<int, QVector<PlayTraceShortcut>>;

struct PlayContact {
    PlayContactKind kind = PlayContactKind::Strike;
    HandOrderHand assignment = HandOrderHand::Unspecified;
    ContactAction path;
    int source = -1;
    int part = -1;
    int channel = -1;
    bool button = false;
    bool strikes = false;
    double appear = 0.0;
    double ready = 0.0;
    double nominal = 0.0;
    double minimumRadius = 0.0;
    bool fingertip = false;
};

// Time and positions are in chart seconds and field radii. A contact cue may
// remain visible even when the assigned hand cannot reach it.
struct PlayContactCue {
    QPointF target;
    PlayContactKind kind = PlayContactKind::Strike;
    HandOrderHand hand = HandOrderHand::Unspecified;
    double timeToContact = 0.0;
    double opacity = 1.0;
    int contactIndex = -1;
    int source = -1;
    bool strikes = false;
    PlayBlock block = PlayBlock::None;
    int buttonLane = 0;
};

struct PlayHandMotion {
    QPointF contact;
    QPointF nextTarget;
    QVector<QPointF> trail;
    double targetSecond = std::numeric_limits<double>::infinity();
    int source = -1;
    int buttonLane = 0;
    PlayContactKind kind = PlayContactKind::Strike;
    bool screen = false;
    double palmRadius = 0.0;
    bool touching = false;
    bool blocked = false;
};

struct PlayContactFrame {
    std::array<PlayHandMotion, 2> hands;
    QVector<PlayContactCue> cues;
};

class HandPlayContacts
{
public:
    void rebuild(const QVector<TimelineNoteMarker>& markers, double palmRadius, double restY = 0.0,
        const MuriRenderOptions* settings = nullptr, const PlayTraceShortcuts& shortcuts = {});
    bool isEmpty() const { return contacts_.isEmpty(); }
    PlayContactFrame sample(double second, bool withTrail = true) const;
    std::array<PlayHandMotion, 2> handsAt(double second) const;
    QVector<double> reviewTimes() const;
    const QVector<PlayContact>& plannedContacts() const { return contacts_; }

private:
    struct HandTrack {
        QVector<int> starts;
        QVector<int> ends;
        muri::detail::MuriTimeIndex index;
    };
    QPointF positionAt(int hand, double second, PlayHandMotion* motion,
                       QVector<PlayContactCue>* cues) const;

    QVector<PlayContact> contacts_;
    std::array<HandTrack, 2> tracks_;
    muri::detail::MuriTimeIndex cues_;
    double palmRadius_ = 0.075;
    double restY_ = 0.0;
    bool adaptivePalm_ = false;
};

} // namespace miacode::hand_order
