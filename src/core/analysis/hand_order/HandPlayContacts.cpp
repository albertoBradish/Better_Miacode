#include "core/analysis/hand_order/HandPlayContacts.h"

#include "common/TaskCancellation.h"
#include "core/analysis/MuriAnalyzerGeometry.h"
#include "core/analysis/hand_order/BasicHandOrderWifi.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include "core/chart/SlideJudgementTiming.h"
#include "core/video/PreviewGameplayConfig.h"

#include <QHash>
#include <QLineF>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace miacode::hand_order {
namespace {
using Hand = HandOrderHand;
constexpr double epsilon = 1e-7;
constexpr double pressSeconds = 0.025;
constexpr double cueLead = 0.18;
constexpr double cueFade = 0.14;
constexpr double approachSeconds = 0.90;
constexpr double postureLookaheadSeconds = 0.32;
constexpr double maxMoveSpeed = 9.0;
constexpr double reachLookaheadSeconds = 0.08;
constexpr int maxMergedContacts = 8;
constexpr qsizetype maximumFrameCues = 80;

int handIndex(Hand hand)
{
    if (hand == Hand::Left) return 0;
    if (hand == Hand::Right) return 1;
    return -1;
}

bool finitePoint(const QPointF& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

QPointF buttonPosition(int lane)
{
    const double angle = (lane - 0.5) * std::numbers::pi / 4.0;
    return {std::sin(angle), -std::cos(angle)};
}

double arrivalLead(double speed, double hs, bool trace)
{
    using namespace preview_gameplay;
    const double effective = qMax(0.0001, std::abs(normalizePreviewTimingFlowSpeed(speed) * hs));
    const double frames = kPreviewTimingBaseFrames + kPreviewTimingFlowFramesNumerator / effective;
    return previewTimingSecondsFromFramesAt120Fps(trace ? frames + 2.0 : frames * 2.0);
}

bool explicitHand(Hand hand) { return hand == Hand::Left || hand == Hand::Right; }

QPointF externalButtonPosition(int lane)
{
    return buttonPosition(lane) * playExternalButtonRadius;
}

ContactAction slidePath(const TimelineNoteMarker& marker)
{
    ContactAction path;
    path.start = marker.slideTraceSecond;
    path.end = marker.endSecond;
    for (int i = 0; i < marker.slideSegmentPoints.size(); ++i) {
        const auto& points = marker.slideSegmentPoints.at(i);
        if (i >= marker.slideSegmentShootSeconds.size() || i >= marker.slideSegmentDurations.size()
            || points.size() < 2) return {};
        const double start = marker.slideSegmentShootSeconds.at(i);
        const double duration = marker.slideSegmentDurations.at(i);
        if (!std::isfinite(start) || !std::isfinite(duration) || duration <= 0.0) return {};
        for (int j = 0; j < points.size(); ++j) {
            if (!finitePoint(points.at(j))) return {};
            const double second = start + duration * j / (points.size() - 1);
            if (!path.times.isEmpty() && second <= path.times.last() + epsilon) continue;
            path.times.append(second);
            path.points.append(points.at(j) / playFieldRadius);
        }
    }
    return path;
}

ContactAction wifiCenterPath(const TimelineNoteMarker& marker)
{
    ContactAction path;
    path.start = marker.slideTraceSecond;
    path.end = marker.endSecond;
    if (marker.wifiLanePoints.size() != 3 || marker.wifiLanePoints.at(1).size() < 2) return path;
    const auto& points = marker.wifiLanePoints.at(1);
    for (int i = 0; i < points.size(); ++i) {
        path.times.append(path.start + (path.end - path.start) * i / (points.size() - 1));
        if (!finitePoint(points.at(i))) return {};
        path.points.append(points.at(i) / playFieldRadius);
    }
    return path;
}

void appendTrace(QVector<PlayContact>& contacts, const ContactAction& path,
                 const QVector<HandOrderTrackStop>& stops, int source, int channel,
                 double appear, double reaction, const QVector<PlayTraceShortcut>& shortcuts = {},
                 double minimumRadius = 0.0)
{
    if (path.points.isEmpty() || !std::isfinite(path.start) || !std::isfinite(path.end) || path.end <= path.start)
        return;
    const auto appendSpan = [&](double from, double to, Hand hand, double observed) {
        if (to <= from + epsilon) return;
        auto clipped = path;
        clipped.start = from;
        clipped.end = to;
        PlayContact contact{PlayContactKind::Trace, hand, std::move(clipped), source, channel};
        contact.appear = appear;
        contact.ready = qMax(appear + reaction, observed);
        contact.nominal = from;
        contact.minimumRadius = minimumRadius;
        contacts.append(std::move(contact));
    };
    const auto append = [&](double from, double to, Hand hand) {
        double lastObservation = appear + reaction;
        for (const auto& skip : shortcuts) {
            if (skip.fromSecond >= to) break;
            if (skip.toSecond <= from) {
                lastObservation = qMax(lastObservation, skip.availableSecond);
                continue;
            }
            appendSpan(from, skip.fromSecond, hand, lastObservation);
            from = skip.toSecond;
            lastObservation = qMax(lastObservation, skip.availableSecond);
        }
        appendSpan(from, to, hand, lastObservation);
    };
    const double length = path.end - path.start;
    double from = path.start;
    Hand hand = Hand::Unspecified;
    for (const auto& stop : stops) {
        if (!std::isfinite(stop.progress)) continue;
        const double at = qBound(from, path.start + length * stop.progress, path.end);
        append(from, at, hand);
        from = at;
        hand = stop.hand;
    }
    append(from, path.end, hand);
}

QPointF positionAtEnd(const PlayContact& contact)
{
    return contactPosition(contact.path, contact.path.end);
}

bool sharingCenter(const QVector<const PlayContact*>& active, double second, double radius, QPointF* center)
{
    if (active.size() > maxMergedContacts) return false;
    QVector<QPointF> targets;
    int button = -1;
    QPointF buttonPoint;
    for (const auto* contact : active) {
        if (contact->button) {
            if (button >= 0 && button != contact->channel) return false;
            button = contact->channel;
            buttonPoint = contactPosition(contact->path, second);
            continue;
        }
        targets.append(contactPosition(contact->path, second));
    }
    if (targets.isEmpty()) return true;
    if (button >= 0) {
        const bool close = targets.size() == 1 && QLineF(buttonPoint, targets.first()).length() <= 0.30;
        if (close) *center = targets.first();
        return close;
    }
    if (targets.size() == 1) { *center = targets.first(); return true; }
    const auto circle = muri::detail::smallestCoveringCircle(targets);
    if (!circle.valid || circle.radius > radius + epsilon) return false;
    *center = circle.center;
    return true;
}

} // namespace

void HandPlayContacts::rebuild(const QVector<TimelineNoteMarker>& markers, double palmRadius, double restY,
    const MuriRenderOptions* settings, const PlayTraceShortcuts& shortcuts)
{
    contacts_.clear();
    tracks_ = {};
    palmRadius_ = palmRadius;
    restY_ = restY;
    adaptivePalm_ = settings && settings->playProfile.adaptivePalm;
    const double reaction = settings ? settings->playProfile.reactionMs / 1000.0 : 0.0;
    const double tapFlow = settings ? settings->playTapFlowSpeed : preview_gameplay::kPreviewTimingDefaultFlowSpeed;
    const double touchFlow = settings ? settings->playTouchFlowSpeed : tapFlow;
    const slide_judge::LaunchBeatIndex launches(markers);
    QVector<int> traceStarts;
    QHash<QString, int> sharedHeads;
    for (const auto& marker : markers) {
        task::CancellationScope::checkpoint();
        if (marker.parseOrder < 0 || !std::isfinite(marker.second) || marker.isMine || marker.lane < 1 || marker.lane > 9) continue;
        const bool slide = marker.type == "slide" || marker.type == "wifi";
        const bool touch = marker.type == "touch" || marker.type == "touch_hold";
        const bool hold = marker.type == "hold" || marker.type == "touch_hold";
        if ((!touch && marker.lane > 8) || (!slide && !touch && !hold && marker.type != "tap")) continue;
        if ((!slide || marker.hasHeadStar) && !marker.headMine) {
            const bool button = !touch && (!settings || settings->playProfile.preferButtons);
            const QPointF point = touch ? (marker.touchPoint - QPointF(270, 270)) / playFieldRadius
                                        : button ? (settings ? externalButtonPosition(marker.lane) : buttonPosition(marker.lane))
                                                 : buttonPosition(marker.lane) * 0.88;
            if (finitePoint(point)) {
                ContactAction path;
                path.start = marker.second;
                path.end = hold && std::isfinite(marker.endSecond)
                    ? qMax(marker.second + pressSeconds, marker.endSecond) : marker.second + pressSeconds;
                path.points = {point};
                PlayContact head{hold ? PlayContactKind::Hold : PlayContactKind::Strike,
                    marker.handOrderHead, std::move(path), marker.parseOrder, -1, marker.lane, button, true};
                head.appear = marker.second - arrivalLead(touch ? touchFlow : tapFlow, marker.hsMultiplier, false);
                head.ready = head.appear + reaction;
                head.nominal = marker.second;
                if (slide) {
                    const QString key = QStringLiteral("%1/%2/%3/%4/%5")
                        .arg(marker.sourceLine).arg(marker.sourceCol).arg(marker.lane)
                        .arg(marker.eachGroupId).arg(marker.second, 0, 'g', 16);
                    const auto it = sharedHeads.constFind(key);
                    if (it == sharedHeads.cend()) {
                        sharedHeads.insert(key, contacts_.size());
                        contacts_.append(std::move(head));
                    } else {
                        auto& first = contacts_[it.value()].assignment;
                        if (first != head.assignment) first = Hand::Unspecified;
                    }
                } else {
                    contacts_.append(std::move(head));
                }
            }
        }
        if (!slide || marker.trackMine || !std::isfinite(marker.slideTraceSecond)
            || !std::isfinite(marker.endSecond) || marker.endSecond <= marker.slideTraceSecond) continue;
        const double appear = marker.second - arrivalLead(tapFlow, marker.hsMultiplier, true);
        if (marker.type == "slide") {
            const int first = contacts_.size();
            appendTrace(contacts_, slidePath(marker), marker.handOrderTrackStops, marker.parseOrder, 0,
                appear, reaction, shortcuts.value(marker.parseOrder));
            if (settings && contacts_.size() > first && !shortcuts.contains(marker.parseOrder))
                traceStarts.append(first);
        } else if (marker.handOrderWifiMode == HandOrderWifiMode::Legacy) {
            appendTrace(contacts_, wifiCenterPath(marker), marker.handOrderTrackStops, marker.parseOrder, 0,
                appear, reaction, shortcuts.value(marker.parseOrder));
        } else {
            QVector<ContactAction> sectors, single;
            if (!buildWifiHandOrderActions(marker, &sectors, &single)) {
                appendTrace(contacts_, wifiCenterPath(marker), {}, marker.parseOrder, 0, appear, reaction);
            } else if (marker.handOrderWifiMode == HandOrderWifiMode::Single) {
                const bool left = marker.handOrderTrackStops.value(0).hand == Hand::Left;
                const auto& path = single.at(left ? 1 : 0);
                appendTrace(contacts_, path, marker.handOrderTrackStops, marker.parseOrder, 0, appear, reaction,
                    shortcuts.value(marker.parseOrder), adaptivePalm_ ? path.radius : 0.0);
            } else {
                for (int sector = 0; sector < sectors.size(); ++sector) {
                    const auto& path = sectors.at(sector);
                    appendTrace(contacts_, path, marker.handOrderWifiSectorStops.value(sector), marker.parseOrder, sector, appear,
                        reaction, shortcuts.value(marker.parseOrder), adaptivePalm_ ? path.radius : 0.0);
                }
            }
        }
    }
    if (settings) {
        QHash<int, const TimelineNoteMarker*> bySource;
        for (const auto& marker : markers) bySource.insert(marker.parseOrder, &marker);
        std::array<QVector<muri::detail::MuriTimeInterval>, 2> busy;
        for (int i = 0; i < contacts_.size(); ++i) {
            const auto& contact = contacts_.at(i);
            const int hand = handIndex(contact.assignment);
            if (hand >= 0) busy[hand].append({contact.path.start, contact.path.end, i});
            else if (contact.assignment == Hand::Either)
                for (auto& intervals : busy) intervals.append({contact.path.start, contact.path.end, i});
        }
        const std::array<muri::detail::MuriTimeIndex, 2> index{
            muri::detail::MuriTimeIndex(std::move(busy[0])), muri::detail::MuriTimeIndex(std::move(busy[1]))};
        std::array<QVector<QPair<double, double>>, 2> moved;
        for (int i : traceStarts) {
            auto& contact = contacts_[i];
            const auto* marker = bySource.value(contact.source, nullptr);
            if (!marker || marker->slideEach || !explicitHand(contact.assignment) || marker->handOrderTrackStops.size() != 1) continue;
            const auto timing = slide_judge::timingForMarker(*marker, &launches);
            if (!timing.valid || timing.usesLaunchBeat || std::abs(contact.path.start - timing.traceSecond) > epsilon) continue;
            const double earliest = qMax(qMax(contact.ready, marker->second + 0.035),
                timing.traceSecond - 0.20);
            // Preserve the same Perfect reference as Slide judgement timing.
            const double start = qMax(earliest, timing.traceSecond + timing.perfectStartSecond - timing.judgementSecond + 0.025);
            const double shift = start - timing.traceSecond;
            if (shift >= -0.01) continue;
            const int hand = handIndex(contact.assignment);
            const double approach = qMax(contact.ready, start - 0.12);
            const auto busyAtStart = index[hand].intersecting(approach, contact.path.start - epsilon);
            if (std::any_of(busyAtStart.cbegin(), busyAtStart.cend(), [&](int j) {
                    const auto& prior = contacts_.at(j);
                    if (j == i || prior.path.end <= approach) return false;
                    if (prior.kind == PlayContactKind::Trace && prior.path.start >= start
                        && prior.path.start <= contact.path.start + epsilon) return true;
                    const double travel = QLineF(positionAtEnd(prior), contactPosition(contact.path, contact.path.start)).length();
                    return prior.path.end + travel / maxMoveSpeed > start + epsilon;
                })) continue;
            if (std::any_of(moved[hand].cbegin(), moved[hand].cend(), [&](const auto& interval) {
                    return interval.first < start + pressSeconds && interval.second > approach;
                })) continue;
            for (double& stamp : contact.path.times) stamp += shift;
            contact.path.start += shift;
            contact.path.end += shift;
            moved[hand].append({start, contact.path.end});
        }
        for (auto& contact : contacts_) {
            if (!contact.strikes) continue;
            // A delayed hit may still fall within the existing judge window.
            const double end = contact.nominal + FullReplayProfile::tapAvailableSeconds + pressSeconds;
            const double delayedEnd = qMax(contact.nominal, contact.ready) + pressSeconds;
            if (delayedEnd <= end) contact.path.end = qMax(contact.path.end, delayedEnd);
        }
        if (adaptivePalm_) {
            for (int i = 0; i < contacts_.size(); ++i) {
                auto& contact = contacts_[i];
                if (contact.kind != PlayContactKind::Trace) continue;
                const int hand = handIndex(contact.assignment);
                if (hand < 0) continue;
                if (contact.minimumRadius > 0.0) continue;
                // Slide groups can brush the opposite hand's live path. Use
                // a fingertip when the tracks approach within a palm width.
                const auto neighbours = index[1 - hand].intersecting(contact.path.start, contact.path.end);
                for (int j : neighbours) {
                    const auto& peer = contacts_.at(j);
                    if (peer.kind != PlayContactKind::Trace || peer.source == contact.source) continue;
                    const double start = qMax(contact.path.start, peer.path.start);
                    const double end = qMin(contact.path.end, peer.path.end);
                    if (end <= start + epsilon) continue;
                    for (int sample = 0; sample < 7; ++sample) {
                        const double at = start + (end - start) * sample / 6.0;
                        if (QLineF(contactPosition(contact.path, at), contactPosition(peer.path, at)).length()
                            >= qMax(0.14, palmRadius_ * 2.2)) continue;
                        contact.fingertip = true;
                        break;
                    }
                    if (contact.fingertip) break;
                }
            }
        }
    }
    std::stable_sort(contacts_.begin(), contacts_.end(), [](const auto& a, const auto& b) {
        if (a.path.start != b.path.start) return a.path.start < b.path.start;
        return a.source < b.source;
    });

    // Resolve only explicit "either" labels. Each choice accounts for nearby
    // fixed contacts and keeps the assigned hand for the entire interval.
    std::array<QVector<muri::detail::MuriTimeInterval>, 2> intervals;
    for (int i = 0; i < contacts_.size(); ++i) {
        const auto& contact = contacts_.at(i);
        const int hand = handIndex(contact.assignment);
        if (hand >= 0) intervals[hand].append({contact.path.start, contact.path.end, i});
    }
    const std::array<muri::detail::MuriTimeIndex, 2> fixed{
        muri::detail::MuriTimeIndex(intervals[0]), muri::detail::MuriTimeIndex(intervals[1])};
    std::array<int, 2> previous{-1, -1};
    for (int i = 0; i < contacts_.size(); ++i) {
        task::CancellationScope::checkpoint();
        auto& contact = contacts_[i];
        int hand = handIndex(contact.assignment);
        if (contact.assignment == Hand::Either) {
            std::array<double, 2> costs{};
            const auto target = contactPosition(contact.path, contact.path.start);
            for (int candidate = 0; candidate < 2; ++candidate) {
                const auto origin = previous[candidate] >= 0
                    ? positionAtEnd(contacts_.at(previous[candidate])) : chart::handOrderNaturalRestPoint(candidate) + QPointF(0, restY_);
                costs[candidate] = QLineF(origin, target).length() + 0.25 * qMax(0.0, candidate == 0 ? target.x() : -target.x());
                if (previous[candidate] >= 0 && contacts_.at(previous[candidate]).path.end > contact.path.start + epsilon)
                    costs[candidate] += 3.0;
                const auto neighbours = fixed[candidate].intersecting(contact.path.start, contact.path.end + 0.16);
                for (int j : neighbours) {
                    const auto& other = contacts_.at(j);
                    const double gap = other.path.start - contact.path.end;
                    if (gap > 0) {
                        costs[candidate] += 0.4 * QLineF(positionAtEnd(contact), contactPosition(other.path, other.path.start)).length();
                        break;
                    }
                    const double through = qMin(contact.path.end, other.path.end);
                    const double from = qMax(contact.path.start, other.path.start);
                    if (through <= from + epsilon) continue;
                    const bool differentButtons = contact.button && other.button && contact.channel != other.channel;
                    const auto separation = [&](double time) {
                        return QLineF(contactPosition(contact.path, time), contactPosition(other.path, time)).length();
                    };
                    if (differentButtons || qMax(separation(from), separation(through)) > 2 * palmRadius_) {
                        costs[candidate] += 4.0;
                    }
                }
            }
            hand = costs[0] <= costs[1] ? 0 : 1;
            contact.assignment = hand == 0 ? Hand::Left : Hand::Right;
            intervals[hand].append({contact.path.start, contact.path.end, i});
        }
        if (hand >= 0 && (previous[hand] < 0 || contact.path.end >= contacts_.at(previous[hand]).path.end)) previous[hand] = i;
    }
    QVector<muri::detail::MuriTimeInterval> cues;
    for (int i = 0; i < contacts_.size(); ++i) {
        const auto& contact = contacts_.at(i);
        const int hand = handIndex(contact.assignment);
        if (hand >= 0) tracks_[hand].starts.append(i);
        cues.append({qMax(contact.appear, contact.path.start - cueLead), contact.path.end + cueFade, i});
    }
    for (int hand = 0; hand < 2; ++hand) {
        auto& track = tracks_[hand];
        track.ends = track.starts;
        std::stable_sort(track.ends.begin(), track.ends.end(), [&](int a, int b) {
            return contacts_.at(a).path.end < contacts_.at(b).path.end;
        });
        track.index = muri::detail::MuriTimeIndex(std::move(intervals[hand]));
    }
    cues_ = muri::detail::MuriTimeIndex(std::move(cues));
}

QPointF HandPlayContacts::positionAt(int hand, double second, PlayHandMotion* motion,
                                    QVector<PlayContactCue>* cues) const
{
    const auto& track = tracks_[hand];
    const auto active = track.index.intersecting(second, second);
    QVector<const PlayContact*> contacts;
    for (int index : active) {
        const auto& contact = contacts_.at(index);
        if (second < contact.path.end) contacts.append(&contact);
    }
    const auto priorAt = [&](double through) -> const PlayContact* {
        const auto ended = std::upper_bound(track.ends.cbegin(), track.ends.cend(), through,
            [&](double time, int index) { return time < contacts_.at(index).path.end; });
        return ended == track.ends.cbegin() ? nullptr : &contacts_.at(*(ended - 1));
    };
    const auto upcoming = std::upper_bound(track.starts.cbegin(), track.starts.cend(), second,
        [&](double time, int index) { return time < contacts_.at(index).path.start; });
    const auto* next = upcoming == track.starts.cend() ? nullptr : &contacts_.at(*upcoming);
    if (next && motion && second >= next->ready - epsilon) {
        motion->nextTarget = contactPosition(next->path, next->path.start);
        motion->targetSecond = next->path.start;
        if (next->path.start - second <= postureLookaheadSeconds) motion->source = next->source;
    }
    const auto home = chart::handOrderNaturalRestPoint(hand) + QPointF(0, restY_);
    const auto rested = [&](const PlayContact* prior, double time) {
        if (!prior) return home;
        const auto end = positionAtEnd(*prior);
        const double distance = QLineF(end, home).length();
        if (distance <= epsilon) return home;
        const double moved = qMax(0.0, time - prior->path.end - chart::handOrderReturnDelaySeconds) * chart::handOrderDefaultReturnSpeed;
        return end + (home - end) * qMin(1.0, moved / distance);
    };
    const auto departure = [&](const PlayContact* prior, const PlayContact& target) {
        const double start = qMax(target.path.start - approachSeconds, target.ready);
        return prior ? qMax(prior->path.end, start) : start;
    };

    if (!contacts.isEmpty()) {
        const auto priority = [](const auto* contact) {
            return contact->kind == PlayContactKind::Hold ? 0 : contact->kind == PlayContactKind::Trace ? 1 : 2;
        };
        std::stable_sort(contacts.begin(), contacts.end(), [&](const auto* a, const auto* b) {
            return priority(a) < priority(b);
        });
        const auto& anchor = *contacts.first();
        const auto* prior = priorAt(anchor.path.start);
        QPointF centre = contactPosition(anchor.path, second);
        const double smallPalm = qMax(0.015, palmRadius_ * 0.40);
        const bool twoTraces = std::count_if(contacts.cbegin(), contacts.cend(), [](const auto* contact) {
            return contact->kind == PlayContactKind::Trace;
        }) >= 2;
        double radius = adaptivePalm_ && anchor.fingertip ? smallPalm : palmRadius_;
        radius = qMax(radius, anchor.minimumRadius);
        bool shares = false;
        if (adaptivePalm_ && twoTraces && sharingCenter(contacts, second, FullReplayProfile::maximumPalmRadius, &centre)) {
            radius = FullReplayProfile::maximumPalmRadius;
            shares = true;
        } else {
            shares = sharingCenter(contacts, second, radius, &centre);
        }
        bool late = false;
        if (anchor.kind == PlayContactKind::Strike || second - anchor.path.start <= reachLookaheadSeconds
            || second < anchor.ready + pressSeconds) {
            // Keep the palm on the reachable portion of the movement while the
            // beat cue still identifies the requested position.
            const double start = departure(prior, anchor);
            const auto origin = rested(prior, start);
            const double allowed = maxMoveSpeed * qMax(0.0, second - start);
            const double distance = QLineF(origin, centre).length();
            late = second + epsilon < anchor.ready || distance > allowed + qMax(radius, 0.065);
            if (late && distance > epsilon) centre = origin + (centre - origin) * qMin(1.0, allowed / distance);
        }
        const bool blocked = !shares || late;
        if (motion) {
            motion->touching = !late;
            motion->blocked = blocked;
            motion->source = anchor.source;
            motion->kind = anchor.kind;
            motion->palmRadius = radius;
            if (!late) for (const auto* contact : contacts) {
                if (!shares && contact != &anchor) continue;
                if (contact->button) motion->buttonLane = contact->channel;
                else motion->screen = true;
            }
        }
        if (blocked && cues) for (auto& cue : *cues) {
            if (handIndex(cue.hand) != hand || cue.timeToContact > epsilon) continue;
            const auto& demand = contacts_.at(cue.contactIndex);
            if (second >= demand.path.end) continue;
            if (late) cue.block = PlayBlock::TooFast;
            else if (!shares && &demand != &anchor) cue.block = PlayBlock::Occupied;
        }
        return centre;
    }

    const auto* prior = priorAt(second);
    if (!next) return rested(prior, second);
    const auto target = contactPosition(next->path, next->path.start);
    const double start = departure(prior, *next);
    const auto origin = rested(prior, start);
    if (second < start) return rested(prior, second);
    const double t = qBound(0.0, (second - start) / qMax(epsilon, next->path.start - start), 1.0);
    double eased = t * t * (3 - 2 * t);
    const double distance = QLineF(origin, target).length();
    if (distance > epsilon) eased = qMin(eased, maxMoveSpeed * (second - start) / distance);
    return origin + (target - origin) * eased;
}

PlayContactFrame HandPlayContacts::sample(double second, bool withTrail) const
{
    PlayContactFrame frame;
    if (isEmpty() || !std::isfinite(second)) return frame;
    QSet<quint64> activeTracks;
    const auto trackKey = [](const PlayContact& contact) {
        return (quint64(quint32(contact.source)) << 32) | quint32(contact.part);
    };
    auto visible = cues_.intersecting(second, second);
    for (int index : visible) {
        const auto& contact = contacts_.at(index);
        if (contact.kind == PlayContactKind::Trace && second >= contact.path.start && second < contact.path.end)
            activeTracks.insert(trackKey(contact));
    }
    if (visible.size() > maximumFrameCues) {
        std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) {
            const auto distance = [&](int index) {
                const auto& path = contacts_.at(index).path;
                return qMax(0.0, qMax(path.start - second, second - path.end));
            };
            return distance(a) < distance(b);
        });
    }
    for (int index : visible) {
        if (frame.cues.size() == maximumFrameCues) break;
        const auto& contact = contacts_.at(index);
        if (contact.kind == PlayContactKind::Trace && second >= contact.path.end) continue;
        if (contact.kind == PlayContactKind::Trace && second < contact.path.start && activeTracks.contains(trackKey(contact))) continue;
        if (contact.kind != PlayContactKind::Trace && second > contact.path.end + cueFade) continue;
        const double elapsed = second - contact.path.start;
        const double opacity = elapsed < 0.0 ? 1.0 + 0.6 * elapsed / cueLead
            : 1.0 - qBound(0.0, (second - contact.path.end) / cueFade, 1.0);
        frame.cues.append({contactPosition(contact.path, second < contact.path.start ? contact.path.start : qMin(second, contact.path.end)),
            contact.kind, contact.assignment, -elapsed, opacity, index, contact.source, contact.strikes});
        frame.cues.last().buttonLane = contact.button ? contact.channel : 0;
    }
    for (int hand = 0; hand < 2; ++hand) {
        auto& motion = frame.hands[hand];
        motion.contact = positionAt(hand, second, &motion, &frame.cues);
        if (!withTrail || tracks_[hand].starts.isEmpty()) continue;
        for (int i = 5; i >= 1; --i) motion.trail.append(positionAt(hand, second - i * 0.026, nullptr, nullptr));
        motion.trail.append(motion.contact);
    }
    return frame;
}

std::array<PlayHandMotion, 2> HandPlayContacts::handsAt(double second) const
{
    std::array<PlayHandMotion, 2> hands;
    for (int hand = 0; hand < 2; ++hand)
        hands[hand].contact = positionAt(hand, second, &hands[hand], nullptr);
    return hands;
}

QVector<double> HandPlayContacts::reviewTimes() const
{
    QVector<QPair<double, double>> spans;
    QVector<double> times;
    for (const auto& contact : contacts_) {
        task::CancellationScope::checkpoint();
        if (handIndex(contact.assignment) < 0) continue;
        const double first = qMin(contact.path.start, qMax(contact.path.start - approachSeconds, contact.ready)) - 0.03;
        const double last = contact.path.end + 0.36;
        if (spans.isEmpty() || spans.last().second < first) spans.append({first, last});
        else spans.last().second = qMax(spans.last().second, last);
        times.append(contact.path.start);
        if (contact.ready > contact.path.start && contact.ready < contact.path.end) times.append(contact.ready);
        times.append(qMax(contact.path.start, contact.path.end - 0.0001));
    }
    double duration = 0.0;
    for (const auto& span : spans) duration += span.second - span.first;
    const double step = qMax(1.0 / 120.0, duration / 60000.0);
    for (const auto& span : spans) {
        const int count = qCeil((span.second - span.first) / step);
        for (int i = 0; i < count; ++i) times.append(span.first + i * step);
        times.append(span.second);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

} // namespace miacode::hand_order
