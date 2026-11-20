#include "core/chart/transform/HandOrderPatternRules.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/model/HandOrderRhythm.h"
#include "core/chart/transform/HandOrderInteraction.h"
#include "core/chart/transform/HandOrderPatternPriority.h"
#include "core/chart/model/TouchPadAdjacency.h"

#include "common/TaskCancellation.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QSet>
#include <QCryptographicHash>
#include <QLineF>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-7;
using Hand = HandOrderHand;
struct Token {
    int start = 0, end = 0, line = 1, col = 1;
    QString base;
    QVector<TimelineNoteMarker> markers;
    bool selected = false;
    const TimelineNoteMarker& first() const { return markers.first(); }
};
Hand flip(Hand hand) { return hand == Hand::Left ? Hand::Right : Hand::Left; }
Hand natural(int lane) { return lane >= 5 ? Hand::Left : Hand::Right; }
int distance(int a, int b) { const int d = std::abs(a - b); return qMin(d, 8 - d); }
int direction(int a, int b) {
    const int d = (b - a + 8) % 8;
    // A sweep may skip one button while preserving its direction around the ring.
    return d == 1 || d == 2 ? 1 : d == 6 || d == 7 ? -1 : 0;
}
bool point(const Token& token) {
    return token.markers.size() == 1 && (token.first().type == "tap" || token.first().type == "touch")
        && !token.first().isMine;
}
bool buttonStrike(const Token& token) {
    return chart::handOrderIsButtonStrike(token.first());
}
bool sameKeyRebound(const Token& first, const Token& second) {
    return chart::handOrderSameKeyRebound(first.first(), second.first());
}
QString key(const Token& token) {
    return token.first().type == "touch" ? token.first().touchPad : QString::number(token.first().lane);
}
Hand pointHand(const Token& token) {
    return token.first().type == "touch" ? (token.first().touchPoint.x() < 270 ? Hand::Left : Hand::Right)
        : natural(token.first().lane);
}
Hand gestureHand(const QVector<Token>& tokens, const QVector<int>& ids) {
    const Hand existing = tokens.at(ids.first()).first().handOrderHead;
    if (concreteHoldHand(existing) && std::all_of(ids.cbegin(), ids.cend(), [&](int id) {
        return tokens.at(id).first().handOrderHead == existing;
    })) return existing;
    return pointHand(tokens.at(ids.first()));
}
QVector<Token> tokenize(const QString& text, const SimaiParseResult& parsed, int begin, int end) {
    QVector<int> lines{0};
    for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') lines.append(i + 1);
    QHash<int, TimelineNoteMarker> markers;
    for (const auto& marker : parsed.noteMarkers) markers.insert(marker.parseOrder, marker);
    QVector<Token> tokens;
    for (const auto& source : parsed.sourceTokens) {
        if (!source.nativeValid || !source.markerCount) continue;
        Token t; t.line = source.line; t.col = source.col;
        t.start = lines.at(source.line - 1) + source.col - 1; t.end = t.start + source.length;
        t.base = splitToken(text.mid(t.start, source.length)).baseToken;
        for (int i = 0; i < source.markerCount; ++i) t.markers.append(markers.value(source.firstParseOrder + i));
        t.selected = t.start >= begin && t.end <= end;
        tokens.append(t);
    }
    std::stable_sort(tokens.begin(), tokens.end(), [](const auto& a, const auto& b) {
        return a.first().second < b.first().second;
    });
    return tokens;
}
// Each assignment contains one head and one hand per native branch. Rendering
// receives those same native markers; it never re-infers a different solution.
struct Assignment {
    Hand head = Hand::Unspecified;
    QVector<QVector<HandOrderTrackStop>> tracks;
    bool keepTrack = false;
};
using Assignments = QHash<int, Assignment>;
Assignment whole(const Token& token, Hand hand) {
    Assignment a; a.head = hand;
    if (token.first().type == "slide") a.tracks.fill({{0, hand}}, token.markers.size());
    return a;
}
Assignment strikeAssignment(const Token& token, Hand hand) {
    if (!chart::handOrderHasStarStrike(token.first())) return whole(token, hand);
    Assignment a;
    a.head = hand;
    a.keepTrack = true;
    return a;
}
PatternVariant variant(const QString& text, const QVector<Token>& tokens, const QVector<int>& ids,
    const Assignments& assignments, const QString& strategy, int start, int end) {
    PatternVariant v; v.strategy = strategy; v.syntax = text.mid(start, end - start);
    for (int id : ids) {
        const auto& t = tokens.at(id); const auto a = assignments.value(id);
        chart_transform::HandOrderTokenEdit edit; edit.head = a.head;
        edit.hasHead = a.keepTrack || t.first().type != "slide" || t.first().hasHeadStar;
        if (a.keepTrack) {
            for (const auto& marker : t.markers) {
                const bool wifi = marker.type == "wifi";
                auto mode = marker.handOrderWifiMode;
                auto stops = marker.handOrderTrackStops;
                auto sectors = marker.handOrderWifiSectorStops;
                if (mode != HandOrderWifiMode::Split && stops.isEmpty()) {
                    if (wifi) {
                        mode = HandOrderWifiMode::Split;
                        const bool firstOnLeft = marker.wifiLanePoints.size() != 3
                            || marker.wifiLanePoints.first().value(marker.wifiLanePoints.first().size() - 1).x()
                                <= marker.wifiLanePoints.last().value(marker.wifiLanePoints.last().size() - 1).x();
                        sectors = {{{0.0, firstOnLeft ? Hand::Left : Hand::Right}},
                                   {{0.0, firstOnLeft ? Hand::Right : Hand::Left}}};
                    } else {
                        stops = {{0.0, natural(marker.lane)}};
                    }
                }
                edit.wifiBranches.append(wifi);
                edit.wifiModes.append(mode);
                edit.tracks.append(stops);
                edit.wifiSectors.append(sectors);
            }
        }
        for (int branch = 0; branch < a.tracks.size(); ++branch) {
            edit.tracks.append(a.tracks.at(branch));
            edit.wifiBranches.append(false); edit.wifiModes.append(HandOrderWifiMode::Legacy);
            edit.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
        }
        const QString replacement = t.base + (edit.tracks.isEmpty()
            ? (a.head == Hand::Left ? QStringLiteral("~l") : QStringLiteral("~r"))
            : chart_transform::handOrderSuffix(edit));
        v.patches.append({t.start, t.end, replacement});
        for (int branch = 0; branch < t.markers.size(); ++branch) {
            auto marker = t.markers.at(branch); marker.handOrderHead = a.head;
            if (a.keepTrack) {
                marker.handOrderTrackStops = edit.tracks.at(branch);
                marker.handOrderWifiMode = edit.wifiModes.at(branch);
                marker.handOrderWifiSectorStops = edit.wifiSectors.at(branch);
            } else if (marker.type == "slide") marker.handOrderTrackStops = a.tracks.value(branch, {{0, a.head}});
            v.markers.append(std::move(marker));
        }
    }
    std::sort(v.patches.begin(), v.patches.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    for (int i = v.patches.size() - 1; i >= 0; --i) {
        const auto& p = v.patches.at(i); v.syntax.replace(p.start - start, p.end - p.start, p.text);
    }
    return v;
}
PatternSuggestion suggestion(const QVector<Token>& tokens, const QVector<int>& ids,
    const QString& registryId, const QString& name, bool definite) {
    PatternSuggestion s; s.registryId = registryId; s.name = name; s.definite = definite;
    s.solverPriority = handOrderPatternPriority(registryId);
    s.preferred = definite ? 0 : -1; s.start = std::numeric_limits<int>::max();
    s.second = std::numeric_limits<double>::infinity();
    for (int id : ids) {
        const auto& t = tokens.at(id);
        if (t.start < s.start) { s.start = t.start; s.line = t.line; s.col = t.col; }
        s.end = qMax(s.end, t.end); s.second = qMin(s.second, t.first().second);
        for (const auto& marker : t.markers) s.endSecond = qMax(s.endSecond, qMax(marker.second, marker.endSecond));
    }
    int firstOrder = std::numeric_limits<int>::max(), lastOrder = -1;
    for (int id : ids) for (const auto& marker : tokens.at(id).markers) {
        firstOrder = qMin(firstOrder, marker.parseOrder); lastOrder = qMax(lastOrder, marker.parseOrder);
    }
    s.instanceKey = registryId + ':' + QString::number(firstOrder) + ':' + QString::number(lastOrder);
    return s;
}
void addVariant(PatternSuggestion& s, const QString& text, const QVector<Token>& tokens,
    const QVector<int>& ids, const Assignments& assignment, const QString& strategy) {
    QVector<TimelineNoteMarker> holds;
    for (const auto& token : tokens) for (const auto& m : token.markers) if (isLockingHold(m)) holds.append(m);
    auto constrained = assignment; bool adjusted = false;
    // Holds inside this choice use their proposed head hand, including short Holds.
    for (auto& hold : holds) for (int id : ids)
        if (hold.parseOrder == tokens.at(id).first().parseOrder) hold.handOrderHead = assignment.value(id).head;
    for (int id : ids) for (const auto& hold : holds)
        if (holdLocksHead(hold, tokens.at(id).first(), constrained[id].head)) {
            if (s.interactionPhase != InteractionPhaseMode::None && strategy != "natural_sides") return;
            if (s.registryId == "T28" || s.registryId == "T30") return;
            constrained[id].head = flip(constrained[id].head); adjusted = true;
        }
    auto v = variant(text, tokens, ids, constrained, adjusted ? "hold_constrained" : strategy, s.start, s.end);
    for (auto& hold : holds) for (const auto& marker : v.markers)
        if (marker.parseOrder == hold.parseOrder) hold.handOrderHead = marker.handOrderHead;
    if (!respectsHoldLocks(holds, v.markers) || !respectsHoldLocks(v.markers, v.markers)) return;
    for (const auto& old : s.variants) if (old.syntax == v.syntax) return;
    v.choiceKey = strategy;
    s.prioritizesTrack |= std::any_of(constrained.cbegin(), constrained.cend(), [](const auto& a) {
        return !a.keepTrack && !a.tracks.isEmpty();
    });
    s.variants.append(std::move(v));
}
Assignments mirrored(Assignments value) {
    for (auto& a : value) { a.head = flip(a.head); for (auto& stops : a.tracks) for (auto& stop : stops) stop.hand = flip(stop.hand); }
    return value;
}

void resolvePatternClaims(QVector<PatternSuggestion>& candidates)
{
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        const bool defaultA = a.definite && a.preferred >= 0;
        const bool defaultB = b.definite && b.preferred >= 0;
        if (defaultA != defaultB) return defaultA;
        if (a.solverPriority != b.solverPriority) return a.solverPriority < b.solverPriority;
        if (a.variants.first().markers.size() != b.variants.first().markers.size())
            return a.variants.first().markers.size() > b.variants.first().markers.size();
        return a.start < b.start;
    });
    QSet<int> claimed;
    candidates.removeIf([&](const auto& group) {
        const auto& notes = group.variants.first().markers;
        if (group.solverPriority == std::numeric_limits<int>::max()
            || std::any_of(notes.cbegin(), notes.cend(), [&](const auto& note) { return claimed.contains(note.parseOrder); }))
            return true;
        for (const auto& note : notes) claimed.insert(note.parseOrder);
        return false;
    });
}
bool regular(const QVector<Token>& tokens, const QVector<int>& run, int first, int last) {
    if (last - first < 2) return false;
    const double step = tokens.at(run.at(first + 1)).first().second - tokens.at(run.at(first)).first().second;
    if (step < 0.025 || step > chart::handOrderMaximumReboundGapSeconds) return false;
    for (int i = first + 2; i < last; ++i) {
        const double gap = tokens.at(run.at(i)).first().second - tokens.at(run.at(i - 1)).first().second;
        if (std::abs(gap - step) > step * 0.1 + epsilon) return false;
    }
    return true;
}

bool regularExtension(const QVector<Token>& tokens, const QVector<int>& run, int first, int last)
{
    const double step = tokens.at(run.at(first + 1)).first().second - tokens.at(run.at(first)).first().second;
    const double gap = tokens.at(run.at(last)).first().second - tokens.at(run.at(last - 1)).first().second;
    return std::abs(gap - step) <= step * 0.1 + epsilon;
}

int unitStep(int from, int to)
{
    const int delta = (to - from + 8) % 8;
    return delta == 1 ? 1 : delta == 7 ? -1 : 0;
}

int steeringEnd(const QVector<Token>& tokens, const QVector<int>& points, int first, int last)
{
    if (last - first < 8) return first;
    const auto lane = [&](int index) { return tokens.at(points.at(index)).first().lane; };
    const int step = unitStep(lane(first), lane(first + 2));
    if (!step || distance(lane(first), lane(first + 1)) < 3) return first;
    int at = first + 2;
    int pairs = 1;
    for (; at + 1 < last; at += 2) {
        if (distance(lane(at), lane(at + 1)) < 3) break;
        if (unitStep(lane(at - 2), lane(at)) == step && unitStep(lane(at - 1), lane(at + 1)) == step) {
            ++pairs;
        } else if (pairs >= 4 && lane(at) == lane(first) && lane(at + 1) == lane(first + 1)) {
            // Both rails can reset together after a partial turn.
            pairs = 1;
        } else break;
    }
    if (at == last - 1 && unitStep(lane(at - 2), lane(at)) == step) ++at;
    return at - first >= 8 ? at : first;
}

void recognizeRingChanges(const QString& text, const QVector<Token>& tokens, const QVector<int>& run,
    QVector<PatternSuggestion>& result)
{
    const auto noteAt = [&](int i) -> const TimelineNoteMarker& { return tokens.at(run.at(i)).first(); };
    for (int first = 0; first + 4 < run.size();) {
        task::CancellationScope::checkpoint();
        if (!buttonStrike(tokens.at(run.at(first)))) { ++first; continue; }
        const int direction = unitStep(noteAt(first).lane, noteAt(first + 1).lane);
        if (!direction) { ++first; continue; }
        double minGap = std::numeric_limits<double>::infinity(), maxGap = 0;
        int turn = -1, strides = 0, last = first + 1;
        bool retraces = true;
        for (; last < run.size(); ++last) {
            const auto& a = noteAt(last - 1);
            const auto& b = noteAt(last);
            const double gap = b.second - a.second;
            const int step = unitStep(a.lane, b.lane);
            if (!buttonStrike(tokens.at(run.at(last))) || !step || !chart::handOrderRegularInputStep(a, b)) break;
            if (step != direction && turn < 0) {
                if (strides < 2) break;
                turn = last - 1;
            } else if (step == direction && turn >= 0) break;
            if (turn >= 0) {
                const int corresponding = 2 * turn - last;
                retraces &= corresponding >= first && b.lane == noteAt(corresponding).lane;
            }
            ++strides;
            minGap = qMin(minGap, gap);
            maxGap = qMax(maxGap, gap);
        }
        const bool returns = turn >= 0 && last - turn >= 3 && retraces && maxGap <= 0.2 + epsilon;
        const bool changesTempo = turn < 0 && strides >= 7 && maxGap >= 1.45 * minGap && maxGap <= 0.25 + epsilon;
        if (!returns && !changesTempo) { ++first; continue; }
        const auto ids = run.mid(first, last - first);
        const Hand hand = gestureHand(tokens, ids);
        Assignments one, split;
        for (int i = 0; i < ids.size(); ++i) {
            const int id = ids.at(i);
            one[id] = strikeAssignment(tokens.at(id), hand);
            split[id] = strikeAssignment(tokens.at(id), i <= (returns ? turn - first : strides / 2) ? hand : flip(hand));
        }
        auto group = suggestion(tokens, ids, returns ? "T29" : "T31", returns ? "return_sweep" : "variable_rotation", true);
        addVariant(group, text, tokens, ids, one, "same_hand");
        addVariant(group, text, tokens, ids, split, "paginate");
        if (!group.variants.isEmpty()) result.append(std::move(group));
        first = last;
    }
}

double headX(const TimelineNoteMarker& note)
{
    return std::sin((note.lane - 0.5) * 3.14159265358979323846 / 4);
}

void recognizeEachPatterns(const QString& text, const QVector<Token>& tokens,
    const QVector<QVector<int>>& moments, QVector<PatternSuggestion>& result)
{
    const auto buttonPair = [&](const QVector<int>& beat) {
        if (beat.size() != 2) return false;
        for (int id : beat)
            if (!tokens.at(id).selected || !buttonStrike(tokens.at(id)) || tokens.at(id).markers.size() != 1)
                return false;
        return tokens.at(beat.first()).first().lane != tokens.at(beat.last()).first().lane;
    };
    const auto pairKey = [&](const QVector<int>& beat) {
        const int a = tokens.at(beat.first()).first().lane, b = tokens.at(beat.last()).first().lane;
        return qMakePair(qMin(a, b), qMax(a, b));
    };
    const auto time = [&](int beat) { return tokens.at(moments.at(beat).first()).first().second; };
    const auto twoSides = [&](const QVector<int>& beat, Assignments& out) {
        const int a = beat.first(), b = beat.last();
        const Hand first = headX(tokens.at(a).first()) < headX(tokens.at(b).first()) ? Hand::Left : Hand::Right;
        out[a] = strikeAssignment(tokens.at(a), first);
        out[b] = strikeAssignment(tokens.at(b), flip(first));
    };
    const auto sameSidePairs = [&](int first, int last, const QString& id, const QString& name) {
        QVector<int> ids;
        Assignments out, sharedLane;
        for (int at = first; at < last; ++at) {
            const auto& beat = moments.at(at);
            ids += beat;
            twoSides(beat, out);
            for (int token : beat) sharedLane[token] = out.value(token);
            if (at == first || id != "T24") continue;
            for (int token : beat) for (int previous : moments.at(at - 1))
                if (tokens.at(token).first().lane == tokens.at(previous).first().lane) {
                    sharedLane[token].head = sharedLane.value(previous).head;
                    const int other = token == beat.first() ? beat.last() : beat.first();
                    sharedLane[other].head = flip(sharedLane[token].head);
                }
        }
        auto group = suggestion(tokens, ids, id, name, true);
        if (id == "T24") addVariant(group, text, tokens, ids, sharedLane, "same_key_return");
        addVariant(group, text, tokens, ids, out, "natural_sides");
        if (out.size() > 2) addVariant(group, text, tokens, ids, mirrored(out), "mirror");
        if (!group.variants.isEmpty()) result.append(std::move(group));
    };
    for (int first = 0; first < moments.size();) {
        task::CancellationScope::checkpoint();
        if (!buttonPair(moments.at(first))) { ++first; continue; }
        int last = first + 1;
        while (last < moments.size() && buttonPair(moments.at(last))
            && time(last) - time(last - 1) <= 0.50 + epsilon) ++last;
        if (last - first == 1) {
            sameSidePairs(first, last, "T19", "each");
            first = last;
            continue;
        }
        QSet<QPair<int,int>> keys;
        for (int i = first; i < last; ++i) keys.insert(pairKey(moments.at(i)));
        int fixed = 0;
        const auto firstKey = pairKey(moments.at(first));
        for (int lane : {firstKey.first, firstKey.second}) {
            bool matches = true;
            for (int i = first + 1; i < last; ++i) {
                const auto current = pairKey(moments.at(i));
                matches &= lane == current.first || lane == current.second;
            }
            if (matches) fixed = lane;
        }
        bool rim = keys.size() >= 4;
        int rimDirection = 0;
        for (int i = first + 1; rim && i < last; ++i) {
            const auto previous = pairKey(moments.at(i - 1));
            const auto current = pairKey(moments.at(i));
            const auto rotates = [&](int step) {
                const int a = (previous.first + step + 7) % 8 + 1;
                const int b = (previous.second + step + 7) % 8 + 1;
                return current == qMakePair(qMin(a, b), qMax(a, b));
            };
            const int step = rotates(1) ? 1 : rotates(-1) ? -1 : 0;
            rim = step != 0 && (rimDirection == 0 || step == rimDirection);
            rimDirection = step;
        }
        int burstLast = first + 1;
        for (; burstLast < last; ++burstLast) {
            const auto prior = pairKey(moments.at(burstLast - 1));
            const auto current = pairKey(moments.at(burstLast));
            if ((burstLast - first) % 2) {
                if (current != prior || time(burstLast) - time(burstLast - 1) > 0.10 + epsilon) break;
            } else {
                const auto rotated = [&](int direction) {
                    const int a = (prior.first + direction + 7) % 8 + 1;
                    const int b = (prior.second + direction + 7) % 8 + 1;
                    return current == qMakePair(qMin(a, b), qMax(a, b));
                };
                if ((!rotated(1) && !rotated(-1)) || time(burstLast) - time(burstLast - 1) > 0.25) break;
            }
        }
        if (burstLast - first >= 6) {
            const int limit = burstLast - (burstLast - first) % 2;
            bool rotatesOppositePairs = true;
            for (int at = first; at < limit; at += 2) {
                const auto [a, b] = pairKey(moments.at(at));
                rotatesOppositePairs &= distance(a, b) == 4;
            }
            sameSidePairs(first, limit, rotatesOppositePairs ? "T18" : "T43",
                rotatesOppositePairs ? "rotating_burst_pairs" : "moving_burst_pairs");
            first = limit;
            continue;
        } else if (rim) {
            QVector<int> ids;
            Assignments out;
            twoSides(moments.at(first), out);
            ids = moments.at(first);
            for (int i = first + 1; i < last; ++i) {
                for (int id : moments.at(i)) {
                    const int lane = tokens.at(id).first().lane;
                    const int previous = (lane - rimDirection + 7) % 8 + 1;
                    for (int prior : moments.at(i - 1))
                        if (tokens.at(prior).first().lane == previous)
                            out[id] = strikeAssignment(tokens.at(id), out.value(prior).head);
                    ids.append(id);
                }
            }
            auto group = suggestion(tokens, ids, "T32", "each_rotation", true);
            addVariant(group, text, tokens, ids, out, "follow_rails");
            Assignments sides;
            for (int i = first; i < last; ++i) twoSides(moments.at(i), sides);
            addVariant(group, text, tokens, ids, sides, "natural_sides");
            if (!group.variants.isEmpty()) result.append(std::move(group));
        } else if (fixed && keys.size() >= 3) {
            QVector<int> ids;
            Assignments out;
            const Hand fixedHand = natural(fixed);
            for (int at = first; at < last; ++at) for (int id : moments.at(at)) {
                ids.append(id);
                out[id] = strikeAssignment(tokens.at(id), tokens.at(id).first().lane == fixed ? fixedHand : flip(fixedHand));
            }
            auto group = suggestion(tokens, ids, "T25", "each_axis", true);
            addVariant(group, text, tokens, ids, out, "anchor_axis");
            addVariant(group, text, tokens, ids, mirrored(out), "mirror");
            if (!group.variants.isEmpty()) result.append(std::move(group));
        } else if (keys.size() == 1 && last - first >= 3
            && time(first + 1) - time(first) <= chart::handOrderFastRepeatGapSeconds + epsilon) {
            sameSidePairs(first, last, "T16", "double_jacks");
        } else if (last - first >= 3) {
            bool shared = true;
            for (int i = first + 1; i < last; ++i) {
                const auto a = pairKey(moments.at(i - 1)), b = pairKey(moments.at(i));
                shared &= a.first == b.first || a.first == b.second || a.second == b.first || a.second == b.second;
            }
            if (shared && keys.size() >= 2) sameSidePairs(first, last, "T24", "each_overlap");
            else if (keys.size() >= 4 && last - first >= 5) sameSidePairs(first, last, "T21", "each_array");
            else sameSidePairs(first, last, "T20", "each_run");
        } else {
            for (int i = first; i < last; ++i) sameSidePairs(i, i + 1, "T19", "each");
        }
        first = last;
    }
    const auto single = [&](const QVector<int>& beat) {
        return beat.size() == 1 && tokens.at(beat.first()).selected && buttonStrike(tokens.at(beat.first()));
    };
    for (int first = 0; first + 3 < moments.size();) {
        task::CancellationScope::checkpoint();
        if (!single(moments.at(first)) || !buttonPair(moments.at(first + 1))) { ++first; continue; }
        int last = first + 2;
        const double gap = time(first + 1) - time(first);
        for (; last < moments.size(); ++last) {
            const auto& beat = moments.at(last);
            if (gap < 0.025 || gap > 0.43 || std::abs(time(last) - time(last - 1) - gap) > 0.1 * gap + epsilon
                || ((last - first) % 2 == 0 ? !single(beat) : !buttonPair(beat))) break;
        }
        if (last - first < 4) { ++first; continue; }
        QVector<int> ids;
        Assignments out;
        const Hand axis = pointHand(tokens.at(moments.at(first).first()));
        bool disjoint = true;
        for (int i = first; i < last; ++i) {
            const auto& beat = moments.at(i);
            ids += beat;
            if (beat.size() == 1) out[beat.first()] = strikeAssignment(tokens.at(beat.first()), axis);
            else {
                twoSides(beat, out);
                const int lane = tokens.at(moments.at(i - 1).first()).first().lane;
                for (int id : beat) disjoint &= tokens.at(id).first().lane != lane;
            }
        }
        auto group = suggestion(tokens, ids, disjoint ? "T23" : "T22", disjoint ? "cut_keys" : "single_each", true);
        addVariant(group, text, tokens, ids, out, "anchor_axis");
        addVariant(group, text, tokens, ids, mirrored(out), "mirror");
        if (!group.variants.isEmpty()) result.append(std::move(group));
        first = last;
    }
}

void recognizeTouchSweeps(const QString& text, const QVector<Token>& tokens, QVector<PatternSuggestion>& result)
{
    for (int first = 0; first < tokens.size();) {
        task::CancellationScope::checkpoint();
        int last = first + 1;
        while (last < tokens.size()) {
            const auto& a = tokens.at(last - 1); const auto& b = tokens.at(last);
            const double gap = b.first().second - a.first().second;
            if (!a.selected || !b.selected || !point(a) || !point(b) || a.first().type != "touch" || b.first().type != "touch"
                || gap < 0.025 - epsilon || gap > 0.18 + epsilon || a.first().touchPad == b.first().touchPad
                || !chart::touchPadsCanSweep(a.first().touchPad, b.first().touchPad)) break;
            ++last;
        }
        if (last - first >= 3) {
            QVector<int> ids;
            for (int i = first; i < last; ++i) ids.append(i);
            const Hand hand = gestureHand(tokens, ids);
            Assignments out;
            for (int id : ids) out[id] = whole(tokens.at(id), hand);
            auto group = suggestion(tokens, ids, "U02", "touch_sweep", true);
            addVariant(group, text, tokens, ids, out, "same_hand");
            addVariant(group, text, tokens, ids, mirrored(out), "mirror");
            if (!group.variants.isEmpty()) result.append(std::move(group));
        }
        first = last;
    }
}

bool streamPoint(const Token& token) {
    const auto& m = token.first();
    return buttonStrike(token) || (token.markers.size() == 1 && !m.isMine && m.type == "hold"
        && m.endSecond - m.second <= qMin(0.4, 2 * chart::handOrderGridSeconds(m)) + epsilon);
}
void addInteractionVariants(PatternSuggestion& group, const QString& text, const QVector<Token>& tokens,
    const QVector<int>& ids, const Assignments& alternating, const Assignments& naturalSides)
{
    addVariant(group, text, tokens, ids, alternating, "alternate");
    addVariant(group, text, tokens, ids, mirrored(alternating), "mirror");
    const bool playablePhase = !group.variants.isEmpty();
    addVariant(group, text, tokens, ids, naturalSides, "natural_sides");
    if (!playablePhase) group.preferred = -1;
}
bool launchAt(const TimelineNoteMarker& slide, const Token& token) {
    const auto& m = token.first();
    if (std::abs(m.second - slide.slideTraceSecond) > epsilon || m.isMine || m.headMine) return false;
    if (m.type == "touch" || m.type == "touch_hold") return m.touchPad == "A" + QString::number(slide.lane);
    return (buttonStrike(token) || (m.type == "hold" && !m.isMine)) && m.lane == slide.lane;
}
Assignment existing(const Token& token) {
    Assignment a; a.head = concreteHoldHand(token.first().handOrderHead) ? token.first().handOrderHead : pointHand(token);
    if (token.first().type == "slide") for (const auto& m : token.markers)
        a.tracks.append(m.handOrderTrackStops.isEmpty() ? QVector<HandOrderTrackStop>{{0, a.head}} : m.handOrderTrackStops);
    return a;
}
QPointF pathPosition(const TimelineNoteMarker& marker, double second) {
    for (int segment = 0; segment < marker.slideSegmentPoints.size(); ++segment) {
        const auto& points = marker.slideSegmentPoints[segment];
        if (points.isEmpty()) continue;
        const double start = marker.slideSegmentShootSeconds.value(segment, marker.slideTraceSecond);
        const double duration = marker.slideSegmentDurations.value(segment, marker.endSecond - start);
        if (second > start + duration + epsilon && segment + 1 < marker.slideSegmentPoints.size()) continue;
        const double index = qBound(0.0, duration > 0 ? (second - start) / duration : 0.0, 1.0) * (points.size() - 1);
        const int before = int(std::floor(index)), after = qMin(before + 1, int(points.size()) - 1);
        return points[before] * (1 - (index - before)) + points[after] * (index - before);
    }
    return {};
}
bool sharedTrajectory(const Token& token) {
    if (token.first().type != "slide") return false;
    const auto& first = token.first();
    for (const auto& marker : token.markers) {
        if (marker.type != "slide" || marker.trackMine || marker.headMine
            || marker.slideSegmentPoints.size() != 1 || marker.slideSegmentPoints.first().isEmpty()
            || marker.handOrderTrackStops.size() > 1
            || std::abs(marker.slideTraceSecond - first.slideTraceSecond) > epsilon) return false;
        const double overlap = qMin(first.endSecond, marker.endSecond) - first.slideTraceSecond;
        if (overlap <= epsilon) return false;
        for (int sample = 0; sample <= 8; ++sample) {
            const double second = first.slideTraceSecond + overlap * sample / 8;
            if (QLineF(pathPosition(first, second), pathPosition(marker, second)).length() > 6.0) return false;
        }
    }
    return true;
}
QString similarKey(const QVector<Token>& tokens, const QVector<int>& ids, const PatternSuggestion& group) {
    // Canonical native geometry under the eight rotations and their reflections.
    // Relative timing and durations remain part of the shape, annotations do not.
    QString best;
    for (int mirror : {1, -1}) for (int rotation = 0; rotation < 8; ++rotation) {
        const double angle = rotation * 3.14159265358979323846 / 4;
        const auto pointKey = [&](QPointF p) {
            p = (p - QPointF(270,270)) / 240; p.setX(p.x() * mirror);
            return QString::number(qRound((p.x()*std::cos(angle)-p.y()*std::sin(angle))*1000)) + ':'
                + QString::number(qRound((p.x()*std::sin(angle)+p.y()*std::cos(angle))*1000));
        };
        QStringList objects;
        for (int id : ids) for (const auto& m : tokens.at(id).markers) {
            const double a = (m.lane-0.5) * 3.14159265358979323846 / 4;
            QString part = QString::number(qRound64((m.second-group.second)*100000)) + '/' + m.type + '/'
                + QString::number(qRound64((m.type == "hold" || m.type == "touch_hold" || m.type == "slide" ? m.endSecond-m.second : 0)*100000)) + '/';
            part += pointKey(m.type.startsWith("touch") ? m.touchPoint : QPointF(270+240*std::sin(a),270-240*std::cos(a)));
            part += '/' + QString::number(qRound64((m.type == "slide" ? m.slideTraceSecond-m.second : 0)*100000));
            for (const auto& segment : m.slideSegmentPoints) {
                part += '[';
                for (int sample = 0; sample <= 8 && !segment.isEmpty(); ++sample)
                    part += pointKey(segment.at(qRound(sample*(segment.size()-1)/8.0))) + ';';
                part += ']';
            }
            objects.append(part);
        }
        std::sort(objects.begin(), objects.end());
        const QString value = objects.join('|'); if (best.isEmpty() || value < best) best = value;
    }
    return group.registryId + ':' + QString::fromLatin1(QCryptographicHash::hash(best.toUtf8(), QCryptographicHash::Sha256).toHex());
}
} // namespace

QVector<PatternSuggestion> recognizeHandOrderPatterns(const QString& text,
    const SimaiParseResult& parsed, int begin, int end, PatternRecognitionMode mode, int sweepThreshold)
{
    QVector<PatternSuggestion> result;
    if (!parsed.ok || !parsed.annotationErrors.isEmpty()) return result;
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    if (begin == end) { begin = 0; end = text.size(); }
    const auto tokens = tokenize(text, parsed, begin, end);
    const auto sustained = sustainedInteractionContext(parsed.noteMarkers);
    const bool prioritized = mode == PatternRecognitionMode::Prioritized;
    QSet<int> claimed;
    const auto claim = [&](const QVector<int>& ids) {
        if (!prioritized) for (int id : ids) claimed.insert(id);
    };
    if (prioritized) {
        QVector<QVector<int>> moments;
        for (int i = 0; i < tokens.size(); ++i) {
            if (moments.isEmpty() || std::abs(tokens.at(i).first().second
                - tokens.at(moments.last().first()).first().second) > epsilon)
                moments.append(QVector<int>{});
            moments.last().append(i);
        }
        recognizeEachPatterns(text, tokens, moments, result);
        recognizeTouchSweeps(text, tokens, result);
    }
    // S11: simultaneous branches from one star, including slash spelling.
    for (int i = 0; i < tokens.size(); ++i) {
        task::CancellationScope::check();
        if (claimed.contains(i) || !tokens.at(i).selected || tokens.at(i).first().type != "slide") continue;
        QVector<int> ids{i};
        for (int j = i + 1; j < tokens.size() && std::abs(tokens.at(j).first().second - tokens.at(i).first().second) < epsilon; ++j)
            if (tokens.at(j).selected && tokens.at(j).first().type == "slide"
                && tokens.at(j).first().lane == tokens.at(i).first().lane) ids.append(j);
        int branches = 0; double trace = tokens.at(i).first().slideTraceSecond;
        QVector<const TimelineNoteMarker*> paths;
        double left = 2, right = -2; bool valid = true;
        for (int id : ids) for (const auto& m : tokens.at(id).markers) {
            ++branches; valid &= m.hasHeadStar && !m.trackMine && !m.headMine
                && std::abs(m.slideTraceSecond - trace) < epsilon && m.slideSegmentPoints.size() == 1;
            // The track's native end position defines the two roles, independent
            // of source branch order and the shared head's position.
            const auto points = m.slideSegmentPoints.value(0);
            if (points.isEmpty()) { valid = false; continue; }
            left = qMin(left, points.last().x()); right = qMax(right, points.last().x());
            paths.append(&m);
        }
        if (!valid || branches != 2) continue;
        bool lateDivergence = false;
        // Equal endpoints do not imply equal paths: a ring and an inner curve
        // can approach the common tail from opposite sides.
        if (paths[0]->endLane == paths[1]->endLane
            && std::abs(paths[0]->endSecond - paths[1]->endSecond) < epsilon) {
            const double second = trace + 0.75 * (paths[0]->endSecond - trace);
            const double x0 = pathPosition(*paths[0], second).x(), x1 = pathPosition(*paths[1], second).x();
            left = qMin(x0, x1); right = qMax(x0, x1);
            lateDivergence = left < -24 && right > 24;
            if (!lateDivergence) continue;
        }
        if (right - left < 0.35 * 240) continue;
        Assignments a;
        for (int id : ids) {
            auto& assignment = a[id]; assignment.head = natural(tokens.at(id).first().lane);
            for (const auto& m : tokens.at(id).markers) {
                const double x = lateDivergence ? pathPosition(m, trace + 0.75 * (m.endSecond - trace)).x()
                    : m.slideSegmentPoints.first().last().x();
                assignment.tracks.append({{0, x < (left + right) / 2 ? Hand::Left : Hand::Right}});
            }
        }
        auto s = suggestion(tokens, ids, "S11", "forked_stars", left < -0.1 && right > 0.1);
        addVariant(s, text, tokens, ids, a, "track_sides");
        addVariant(s, text, tokens, ids, mirrored(a), "mirror");
        result.append(s); claim(ids);
    }
    // N01/U10: head -> off-lane tap -> same-lane launch beat. The head hand
    // continues the trace; the next triplet belongs to the other hand.
    struct Triplet { QVector<int> ids; double start = 0, step = 0; };
    QVector<Triplet> triplets;
    for (int i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens.at(i); const auto& m = t.first();
        if (!t.selected || claimed.contains(i) || t.markers.size() != 1 || m.type != "slide"
            || !m.hasHeadStar || m.headMine || m.trackMine) continue;
        const double step = (m.slideTraceSecond - m.second) / 2;
        if (step < 0.05 || step > 0.4) continue;
        int middle = -1, launch = -1;
        for (int j = i + 1; j < tokens.size() && tokens.at(j).first().second <= m.slideTraceSecond + epsilon; ++j) {
            const auto& p = tokens.at(j);
            if (!p.selected || claimed.contains(j)) continue;
            if (std::abs(p.first().second - m.second - step) < epsilon
                && (point(p) || buttonStrike(p)) && (p.first().type == "touch" || distance(m.lane, p.first().lane) >= 1))
                middle = middle == -1 ? j : -2;
            if (launchAt(m, p)) launch = launch == -1 ? j : -2;
        }
        if (middle >= 0 && launch >= 0) triplets.append({{i, middle, launch}, m.second, step});
    }
    for (int i = 0; i < triplets.size();) {
        int last = i + 1;
        while (last < triplets.size()) {
            const double gap = triplets.at(last).start - triplets.at(last - 1).start;
            const double step = triplets.at(last - 1).step;
            if ((std::abs(gap - 2 * step) > epsilon && std::abs(gap - 3 * step) > epsilon)
                || std::abs(triplets.at(last).step - step) > epsilon) break;
            ++last;
        }
        if (last - i < 2) { ++i; continue; }
        QVector<int> ids; Assignments a; bool touch = false, disjoint = true, fastSeparated = false;
        const Hand firstHand = natural(tokens.at(triplets.at(i).ids.first()).first().lane);
        for (int n = i; n < last; ++n) for (int id : triplets.at(n).ids) {
            disjoint &= !ids.contains(id); ids.append(id);
            const Hand hand = (n - i) % 2 ? flip(firstHand) : firstHand;
            a[id] = id == triplets.at(n).ids.first() ? whole(tokens.at(id), hand)
                : strikeAssignment(tokens.at(id), hand);
            touch |= tokens.at(id).first().type == "touch";
        }
        if (!disjoint) { ++i; continue; }
        auto unsplit = a;
        for (int n = i; n < last; ++n) {
            const auto& triplet = triplets.at(n);
            const bool separated = (n > i && std::abs(triplet.start-triplets.at(n-1).start-3*triplet.step) < epsilon)
                || (n+1 < last && std::abs(triplets.at(n+1).start-triplet.start-3*triplet.step) < epsilon);
            const int middle = triplet.ids.at(1);
            if (separated && tokens.at(triplet.ids.first()).first().tempoBpm > 170) {
                a[middle].head = flip(a[middle].head); fastSeparated = true;
            }
            const int head = triplet.ids.first(), launch = triplet.ids.last();
            if (isLockingHold(tokens.at(launch).first())) {
                // Holding the start pad cannot also follow the moving trace.
                a[head].tracks = {{{0, flip(a[launch].head)}}};
                unsplit[head].tracks = {{{0, flip(unsplit[launch].head)}}};
            } else if (chart::handOrderHasStarStrike(tokens.at(launch).first())) {
                a[launch] = strikeAssignment(tokens.at(launch), a[head].tracks.first().first().hand);
                unsplit[launch] = a[launch];
            }
        }
        auto s = suggestion(tokens, ids, touch ? "U10" : "N01", "umiyuri", true);
        addVariant(s, text, tokens, ids, a, fastSeparated ? "umiyuri_share" : "umiyuri_triplets");
        addVariant(s, text, tokens, ids, mirrored(a), "mirror");
        if (fastSeparated) addVariant(s, text, tokens, ids, unsplit, "umiyuri_triplets");
        result.append(s); claim(ids); i = last;
    }
    // Repeated key after a mixed Touch+Tap Each retains the key hand.
    for (int i = 0; i + 2 < tokens.size(); ++i) {
        const auto& a = tokens[i]; const auto& b = tokens[i+1]; const auto& next = tokens[i+2];
        if (!a.selected || !b.selected || !next.selected || claimed.contains(i) || claimed.contains(i+1) || claimed.contains(i+2)
            || !point(a) || !point(b) || !point(next) || next.first().type != "tap"
            || (a.first().type != "touch" && b.first().type != "touch")
            || std::abs(a.first().second-b.first().second) > epsilon) continue;
        const int source = a.first().type == "tap" && a.first().lane == next.first().lane ? i
            : b.first().type == "tap" && b.first().lane == next.first().lane ? i+1 : -1;
        const double gap = next.first().second-a.first().second;
        if (source < 0 || gap < 0.12 || std::abs(gap-30.0/a.first().tempoBpm) > epsilon) continue;
        const int peer = source == i ? i+1 : i;
        QVector<int> ids{i,i+1,i+2}; Assignments a0;
        const Hand hand = pointHand(tokens[source]);
        a0[source] = whole(tokens[source],hand); a0[peer] = whole(tokens[peer],flip(hand)); a0[i+2] = whole(next,hand);
        auto group = suggestion(tokens,ids,"T24","each_return",true);
        addVariant(group,text,tokens,ids,a0,"same_key_return");
        addVariant(group,text,tokens,ids,mirrored(a0),"mirror");
        a0[i+2].head = flip(hand); addVariant(group,text,tokens,ids,a0,"alternate_return");
        result.append(group); claim(ids);
    }
    // Burst bullets: AA B, CC B. The two taps of each burst stay together,
    // while its last note and the next burst share the other hand.
    for (int i = 0; i + 5 < tokens.size(); ++i) {
        QVector<int> ids; bool valid = true;
        for (int n = 0; n < 6; ++n) {
            ids.append(i+n); valid &= tokens[i+n].selected && !claimed.contains(i+n)
                && buttonStrike(tokens[i+n]);
        }
        if (!valid || key(tokens[i]) != key(tokens[i+1]) || key(tokens[i+3]) != key(tokens[i+4])
            || key(tokens[i+2]) != key(tokens[i+5]) || key(tokens[i]) == key(tokens[i+2])
            || key(tokens[i+3]) == key(tokens[i+2]) || key(tokens[i]) == key(tokens[i+3])) continue;
        const double step = tokens[i+1].first().second-tokens[i].first().second;
        if (step < 0.025 || step > 0.13) continue;
        const int ticks[] = {0,1,2,4,5,6};
        for (int n = 0; n < 6; ++n) valid &= std::abs(tokens[i+n].first().second-tokens[i].first().second-ticks[n]*step) < epsilon;
        if (!valid) continue;
        Assignments a; const Hand hand = pointHand(tokens[i]);
        for (int n = 0; n < 6; ++n)
            a[i+n] = strikeAssignment(tokens[i+n], n == 0 || n == 1 || n == 5 ? hand : flip(hand));
        auto group = suggestion(tokens,ids,"T17","burst_bullets",true);
        addVariant(group,text,tokens,ids,a,"burst_groups"); addVariant(group,text,tokens,ids,mirrored(a),"mirror");
        result.append(group); claim(ids);
    }
    // Unique endpoint-to-start connections may contain a brief dwell. Star
    // strikes remain independent; only the moving tracks form the stroke.
    QVector<int> previous(tokens.size(),-1), successors(tokens.size(),0);
    const auto slide = [&](int id) { return tokens[id].selected && !claimed.contains(id)
        && tokens[id].markers.size() == 1 && tokens[id].first().type == "slide"
        && !tokens[id].first().trackMine && !tokens[id].first().headMine
        && tokens[id].first().handOrderTrackStops.size() <= 1; };
    for (int next = 0; next < tokens.size(); ++next) if (slide(next)) {
        int found = -1;
        for (int before = 0; before < next; ++before) if (slide(before)) {
            const auto& a = tokens[before].first(); const auto& b = tokens[next].first();
            const double gap = b.slideTraceSecond-a.endSecond;
            const double limit = qMin(0.6,60.0/qMax(a.tempoBpm,b.tempoBpm));
            if (a.endLane != b.lane || a.slideTraceSecond >= b.slideTraceSecond-epsilon || gap < -0.01 || gap > limit+epsilon) continue;
            if (found >= 0) { found = -2; break; } found = before;
        }
        if (found >= 0) { previous[next] = found; ++successors[found]; }
    }
    for (int first = 0; first < tokens.size(); ++first) if (slide(first) && previous[first] < 0) {
        QVector<int> chain{first};
        for (int at = first; successors[at] == 1;) {
            int next = previous.indexOf(at); if (next < 0 || !slide(next)) break;
            chain.append(next); at = next;
        }
        if (chain.size() < 2) continue;
        QVector<int> ids = chain; Assignments coherent, separate;
        const auto initial = existing(tokens[first]);
        const Hand hand = initial.tracks.first().first().hand;
        for (int id : chain) { coherent[id] = existing(tokens[id]); coherent[id].tracks = {{{0,hand}}}; separate[id] = existing(tokens[id]); }
        // During a continuous stroke, an off-path star belongs to the free
        // hand. A star on the moving path still shares its launch gesture.
        for (int id : chain) {
            const auto& head = tokens[id].first();
            if (!head.hasHeadStar) continue;
            const auto& points = head.slideSegmentPoints.first();
            if (points.isEmpty()) continue;
            bool occupied = false, onPath = false;
            for (int active : chain) {
                const auto& track = tokens[active].first();
                if (active == id || head.second < track.slideTraceSecond - epsilon || head.second > track.endSecond + epsilon) continue;
                occupied = true;
                onPath |= QLineF(points.first(), pathPosition(track, head.second)).length() <= 0.35 * 240;
            }
            if (occupied) coherent[id].head = onPath ? hand : flip(hand);
        }
        for (int id : chain) for (int launch = id+1; launch < tokens.size(); ++launch) {
            if (tokens[launch].first().second > tokens[id].first().slideTraceSecond+epsilon) break;
            if (!tokens[launch].selected || (claimed.contains(launch) && !ids.contains(launch)) || !launchAt(tokens[id].first(),tokens[launch])) continue;
            if (!ids.contains(launch)) {
                ids.append(launch);
                const auto& token = tokens[launch];
                coherent[launch] = separate[launch] = chart::handOrderHasStarStrike(token.first())
                    ? strikeAssignment(token, pointHand(token)) : existing(token);
            }
            coherent[launch].head = isLockingHold(tokens[launch].first()) ? flip(hand) : hand;
            separate[launch].head = isLockingHold(tokens[launch].first()) ? flip(separate[id].tracks.first().first().hand) : separate[id].tracks.first().first().hand;
        }
        auto group = suggestion(tokens,ids,"S07","one_stroke",true);
        addVariant(group,text,tokens,ids,coherent,"continuous_tracks");
        addVariant(group,text,tokens,ids,mirrored(coherent),"mirror");
        addVariant(group,text,tokens,ids,separate,"separate_tracks");
        result.append(group); claim(ids);
    }
    // A launch beat can itself be a star, Hold or matching A-zone Touch.
    for (int id = 0; id < tokens.size(); ++id) if (slide(id)) {
        bool repeatedLane = false;
        for (int next = id+1; next < tokens.size(); ++next) {
            const auto& m = tokens[next].first();
            if (m.second > tokens[id].first().endSecond+epsilon) break;
            repeatedLane |= m.second > tokens[id].first().slideTraceSecond+epsilon
                && point(tokens[next]) && m.type == "tap" && m.lane == tokens[id].first().lane;
        }
        if (repeatedLane) continue;
        QVector<int> ids{id}; Assignments a; a[id] = existing(tokens[id]);
        for (int launch = id+1; launch < tokens.size(); ++launch) {
            if (tokens[launch].first().second > tokens[id].first().slideTraceSecond+epsilon) break;
            if (!tokens[launch].selected || claimed.contains(launch) || !launchAt(tokens[id].first(),tokens[launch])) continue;
            ids.append(launch);
            a[launch] = tokens[launch].first().type == "wifi"
                ? strikeAssignment(tokens[launch], pointHand(tokens[launch])) : existing(tokens[launch]);
            if (isLockingHold(tokens[launch].first()))
                a[id].tracks = {{{0,flip(a[launch].head)}}};
            else {
                const Hand hand = a[id].tracks.first().first().hand;
                a[launch].head = hand;
                const auto& current = tokens[id].first(); const auto& next = tokens[launch].first();
                // A shared-path fan is one moving gesture. If the preceding
                // stroke is still away from its entry at launch, use the free
                // hand instead of copying the shared star's striking hand.
                if (sharedTrajectory(tokens[launch]) && next.slideTraceSecond <= current.endSecond + epsilon
                    && QLineF(pathPosition(next, next.slideTraceSecond), pathPosition(current, next.slideTraceSecond)).length() > 0.35 * 240)
                    a[launch].tracks.fill({{0,flip(hand)}}, tokens[launch].markers.size());
            }
        }
        if (ids.size() < 2) continue;
        Assignments handoff;
        if (ids.size() == 2 && !isLockingHold(tokens[ids[1]].first())) {
            const int launch = ids[1];
            for (int peer = 0; peer < tokens.size(); ++peer) {
                if (peer == launch || !tokens[peer].selected || claimed.contains(peer) || !point(tokens[peer])
                    || std::abs(tokens[peer].first().second-tokens[launch].first().second) > epsilon) continue;
                const auto x = [&](const Token& token) { return token.first().type == "touch" ? (token.first().touchPoint.x()-270)/240
                    : std::sin((token.first().lane-0.5)*3.14159265358979323846/4); };
                const Hand launchHand = x(tokens[launch]) < x(tokens[peer]) ? Hand::Left : Hand::Right;
                ids.append(peer); a[peer] = whole(tokens[peer],flip(launchHand));
                a[id].head = launchHand;
                a[launch].head = launchHand; a[id].tracks = {{{0,launchHand}}};
                handoff = a;
                handoff[id].tracks = {{{0,flip(launchHand)}}};
                handoff[launch].head = flip(launchHand);
                handoff[peer].head = launchHand;
                break;
            }
        }
        bool alreadyConsistent = true;
        for (int token : ids) {
            alreadyConsistent &= tokens[token].first().handOrderHead == a[token].head;
            for (int branch = 0; branch < a[token].tracks.size(); ++branch) {
                const auto& old = tokens[token].markers[branch].handOrderTrackStops;
                const auto& proposed = a[token].tracks[branch];
                alreadyConsistent &= old.size() == proposed.size() && std::equal(old.cbegin(),old.cend(),proposed.cbegin(),
                    [](const auto& x,const auto& y) { return x.hand == y.hand && x.progress == y.progress; });
            }
        }
        if (alreadyConsistent && handoff.isEmpty()) continue;
        auto group = suggestion(tokens,ids,"S01","launch_beat",true);
        addVariant(group,text,tokens,ids,a,"launch_track");
        if (!handoff.isEmpty()) addVariant(group,text,tokens,ids,handoff,"handoff");
        addVariant(group,text,tokens,ids,mirrored(a),"mirror");
        result.append(group); claim(ids);
    }
    if (prioritized) for (int id = 0; id < tokens.size(); ++id) {
        task::CancellationScope::checkpoint();
        const auto& token = tokens.at(id);
        const auto& slide = token.first();
        if (!token.selected || token.markers.size() != 1 || slide.type != "slide"
            || !chart::handOrderHasStarStrike(slide) || slide.trackMine) continue;
        const auto end = std::upper_bound(tokens.cbegin() + id + 1, tokens.cend(), slide.endSecond + epsilon,
            [](double second, const auto& item) { return second < item.first().second; });
        int early = -1, launch = -1;
        for (auto it = tokens.cbegin() + id + 1; it != end; ++it) {
            const auto& marker = it->first();
            if (!it->selected || it->markers.size() != 1 || !buttonStrike(*it) || marker.lane != slide.lane) continue;
            if (launchAt(slide, *it)) { launch = int(it - tokens.cbegin()); break; }
            if (early < 0 && marker.second > slide.second + epsilon && marker.second < slide.slideTraceSecond - epsilon)
                early = int(it - tokens.cbegin());
        }
        if (early < 0) continue;
        const QVector<int> ids = launch >= 0 ? QVector<int>{id, early, launch} : QVector<int>{id, early};
        if (tokens.at(early).first().second - slide.second < 0.07) continue;
        Assignments out;
        const Hand hand = pointHand(token);
        out[id] = existing(token);
        out[id].head = hand;
        out[early] = strikeAssignment(tokens.at(early), hand);
        if (launch >= 0) out[launch] = strikeAssignment(tokens.at(launch), out[id].tracks.first().first().hand);
        auto group = suggestion(tokens, ids, "S02", "early_launch", true);
        addVariant(group, text, tokens, ids, out, "same_key_return");
        addVariant(group, text, tokens, ids, mirrored(out), "mirror");
        if (!group.variants.isEmpty()) result.append(std::move(group));
    }
    QVector<const TimelineNoteMarker*> triangleStream(tokens.size(), nullptr);
    QHash<int, int> tokenByOrder;
    for (int i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens.at(i);
        const auto& note = token.first();
        const bool simultaneous = (i && std::abs(note.second - tokens.at(i - 1).first().second) < epsilon)
            || (i + 1 < tokens.size() && std::abs(note.second - tokens.at(i + 1).first().second) < epsilon);
        if (!token.selected || (!buttonStrike(token) && (token.markers.size() != 1 || note.isMine || note.type != "hold"))
            || simultaneous || claimed.contains(i)) continue;
        triangleStream[i] = &note;
        tokenByOrder.insert(note.parseOrder, i);
    }
    const auto addPhasedGroup = [&](const InteractionSteps& steps, const QString& id, const QString& name, InteractionPhaseMode mode) {
        QVector<int> ids;
        Assignments alternating, sides;
        const Hand firstHand = natural(steps.first().note->lane);
        for (const auto& step : steps) {
            const int id = tokenByOrder.value(step.note->parseOrder);
            ids.append(id);
            alternating[id] = strikeAssignment(tokens.at(id), interactionHand(firstHand, step.opposite));
            sides[id] = strikeAssignment(tokens.at(id), pointHand(tokens.at(id)));
        }
        auto group = suggestion(tokens, ids, id, name, true);
        group.interactionPhase = mode;
        addInteractionVariants(group, text, tokens, ids, alternating, sides);
        if (group.variants.isEmpty()) return;
        result.append(std::move(group));
        claim(ids);
    };
    for (const auto& steps : findTriangleInteractions(triangleStream))
        addPhasedGroup(steps, "T06", "triangle", InteractionPhaseMode::Triangle);

    for (auto& head : triangleStream) if (head && (claimed.contains(tokenByOrder.value(head->parseOrder))
        || !streamPoint(tokens.at(tokenByOrder.value(head->parseOrder))))) head = nullptr;
    for (const auto& steps : findTrillInteractions(triangleStream)) {
        const bool longTrill = prioritized && steps.size() >= 16;
        const bool separated = prioritized && steps.size() >= 6 && distance(steps.first().note->lane, steps.at(1).note->lane) >= 3;
        addPhasedGroup(steps, longTrill ? "T02" : separated ? "T03" : "T01",
            longTrill ? "long_trill" : separated ? "fixed_rails" : "trill", InteractionPhaseMode::Alternating);
    }
    if (prioritized) {
        // The four-point triangle stroke may revisit its middle lane before
        // starting the next cycle: A-B-C-B, then A-B-C-B again.
        for (int first = 0; first + 7 < triangleStream.size();) {
            task::CancellationScope::checkpoint();
            const auto* a = triangleStream.at(first), *b = triangleStream.at(first + 1),
                *c = triangleStream.at(first + 2), *returning = triangleStream.at(first + 3);
            if (!a || !b || !c || !returning || b->lane != returning->lane
                || a->lane == b->lane || a->lane == c->lane || b->lane == c->lane) { ++first; continue; }
            const double gap = b->second - a->second;
            if (gap < 0.025 || gap > chart::handOrderMaximumInteractionGapSeconds) { ++first; continue; }
            const auto* incoming = triangleStream.value(first - 1);
            if (incoming && incoming->lane == b->lane
                && std::abs(a->second - incoming->second - gap) <= 0.1 * gap + epsilon) { ++first; continue; }
            const std::array<int, 4> lanes{a->lane, b->lane, c->lane, b->lane};
            int last = first + 1;
            for (; last < triangleStream.size(); ++last) {
                const auto* note = triangleStream.at(last);
                if (!note || note->lane != lanes.at((last - first) % 4)
                    || std::abs(note->second - triangleStream.at(last - 1)->second - gap) > 0.1 * gap + epsilon) break;
            }
            if (last - first < 8) { ++first; continue; }
            InteractionSteps steps;
            for (int i = first; i < last; ++i) steps.append({triangleStream.at(i), (i - first) % 2 != 0});
            addPhasedGroup(steps, "T06", "triangle", InteractionPhaseMode::Triangle);
            first = last;
        }
    }

    // Keep simultaneous events and sustained objects as boundaries. Point runs
    // never silently absorb one member of an Each or cross a selected boundary.
    QVector<QVector<int>> runs; QVector<int> run;
    const auto flush = [&] { if (!run.isEmpty()) runs.append(run); run.clear(); };
    for (int i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens.at(i);
        const bool simultaneous = (i && std::abs(t.first().second - tokens.at(i - 1).first().second) < epsilon)
            || (i + 1 < tokens.size() && std::abs(t.first().second - tokens.at(i + 1).first().second) < epsilon);
        if (!t.selected || !streamPoint(t) || simultaneous || claimed.contains(i)) { flush(); continue; }
        if (!run.isEmpty()) {
            const auto& previous = tokens.at(run.last());
            if (t.first().second - previous.first().second > 0.4 + epsilon && !sameKeyRebound(previous, t)) flush();
        }
        run.append(i);
    }
    flush();
    if (prioritized) for (const auto& points : runs) recognizeRingChanges(text, tokens, points, result);
    for (const auto& points : runs) for (int first = 0; first < points.size();) {
        task::CancellationScope::check();
        int last = first + 1;
        const auto continuesKey = [&](int at) {
            return at < points.size() && key(tokens.at(points.at(at))) == key(tokens.at(points.at(first)))
                && (at == first + 1 || regularExtension(tokens, points, first, at));
        };
        while (continuesKey(last)) ++last;
        if (last - first == 2 && last < points.size()
            && chart::handOrderHasStarStrike(tokens.at(points.at(first)).first())) {
            // A relaxed star can be the entrance to a faster same-key run.
            const auto& a = tokens.at(points.at(last - 1)).first();
            const auto& b = tokens.at(points.at(last)).first();
            if (a.lane == b.lane) { ++first; continue; }
        }
        if (last - first >= 2 && regular(tokens, points, first, last)) {
            const auto ids = points.mid(first, last - first); Assignments a, alternate;
            bool rebound = true;
            for (int n = 1; n < ids.size(); ++n) {
                const auto& prior = tokens.at(ids[n - 1]);
                const auto& hit = tokens.at(ids[n]);
                rebound &= sameKeyRebound(prior, hit) && (ids.size() == 2
                    || !chart::handOrderLongitudinalStep(prior.first(), hit.first()));
            }
            if (ids.size() == 2 && !rebound) { first = last; continue; }
            const Hand hand = gestureHand(tokens, ids);
            for (int n = 0; n < ids.size(); ++n) {
                a[ids[n]] = strikeAssignment(tokens.at(ids[n]), hand);
                alternate[ids[n]] = strikeAssignment(tokens.at(ids[n]), n % 2 ? flip(hand) : hand);
            }
            auto s = suggestion(tokens, ids, ids.size() >= 5 ? "T15" : "T14", ids.size() >= 5 ? "long_jacks" : "short_jacks",
                rebound && ids.size() <= 4);
            bool concrete = true, mixed = false, activeSlide = false;
            const Hand originalHand = tokens.at(ids.first()).first().handOrderHead;
            for (int id : ids) {
                const auto existing = tokens.at(id).first().handOrderHead;
                concrete &= concreteHoldHand(existing); mixed |= existing != originalHand;
            }
            const double firstSecond = tokens.at(ids.first()).first().second, lastSecond = tokens.at(ids.last()).first().second;
            for (const auto& context : tokens) for (const auto& marker : context.markers)
                activeSlide |= (marker.type == "slide" || marker.type == "wifi")
                    && marker.slideTraceSecond <= lastSecond + epsilon && marker.endSecond >= firstSecond - epsilon;
            const bool keepExistingOption = concrete && mixed && activeSlide;
            if (ids.size() == 2 && !prioritized && (!concrete || !mixed)) { first = last; continue; }
            if (keepExistingOption) {
                Assignments current;
                for (int id : ids) current[id] = strikeAssignment(tokens.at(id), tokens.at(id).first().handOrderHead);
                addVariant(s, text, tokens, ids, current, "existing");
                s.definite = false; s.preferred = -1;
            }
            InteractionSteps hits;
            for (int id : ids) hits.append({&tokens.at(id).first(), false});
            if (!s.definite || interactionPhaseFits(hits, hand, sustained))
                addVariant(s, text, tokens, ids, a, "same_hand");
            if (!s.definite || interactionPhaseFits(hits, flip(hand), sustained))
                addVariant(s, text, tokens, ids, mirrored(a), "mirror");
            if (s.variants.isEmpty()) s.preferred = -1;
            if (!keepExistingOption) addVariant(s, text, tokens, ids, alternate, "alternate");
            result.append(s); first = last; continue;
        }
        // Classify a maximal regular, non-repeating point stream before trying
        // sweeps. Periods and fixed axes survive rotations and mirror variants.
        last = first + 1;
        while (last < points.size() && key(tokens.at(points.at(last))) != key(tokens.at(points.at(last - 1)))) {
            if (last > first + 1 && !regularExtension(tokens, points, first, last)) break;
            ++last;
        }
        if (last - first < 3 || !regular(tokens, points, first, last)) { ++first; continue; }
        // Extract a local trill or moving two-hand rail from a regular
        // corridor, even when a different configuration starts on the next beat.
        int localEnd = first + 2;
        for (; localEnd < last; ++localEnd) {
            const int lane = tokens.at(points.at(localEnd)).first().lane;
            const int previous = tokens.at(points.at(localEnd - 2)).first().lane;
            if (distance(lane, previous) > 1) break;
        }
        const int steeringLast = prioritized ? steeringEnd(tokens, points, first, last) : first;
        if (steeringLast > first) last = steeringLast;
        else if (localEnd >= first + 6 && localEnd < last) {
            int sign = direction(tokens.at(points.at(first)).first().lane, tokens.at(points.at(first + 1)).first().lane);
            bool localSweep = sign != 0;
            for (int n = first + 2; localSweep && n < localEnd; ++n)
                localSweep &= direction(tokens.at(points.at(n - 1)).first().lane, tokens.at(points.at(n)).first().lane) == sign;
            if (!localSweep) last = localEnd;
        }
        auto ids = points.mid(first, last - first); QSet<QString> keys;
        for (int id : ids) keys.insert(key(tokens.at(id)));
        int period = 0;
        for (int p : {2, 3, 4}) {
            if (ids.size() < 2 * p || keys.size() != p) continue;
            bool repeat = true;
            for (int n = p; n < ids.size(); ++n) repeat &= key(tokens.at(ids[n])) == key(tokens.at(ids[n % p]));
            if (repeat) { period = p; break; }
        }
        bool axis = false;
        for (int phase = 0; phase < 2 && ids.size() >= 5; ++phase) {
            bool fixed = true; const QString anchor = key(tokens.at(ids[phase]));
            for (int n = phase; n < ids.size(); n += 2) fixed &= key(tokens.at(ids[n])) == anchor;
            axis |= fixed && keys.size() >= 3;
        }
        int step = direction(tokens.at(ids[0]).first().lane, tokens.at(ids[1]).first().lane);
        bool sweep = step != 0;
        for (int n = 0; n < ids.size(); ++n) {
            sweep &= buttonStrike(tokens.at(ids[n]));
            if (n) sweep &= direction(tokens.at(ids[n - 1]).first().lane, tokens.at(ids[n]).first().lane) == step;
        }
        bool repeatedSweep = (period == 3 || period == 4) && step != 0;
        for (int n = 0; repeatedSweep && n < period; ++n) {
            repeatedSweep &= buttonStrike(tokens.at(ids[n]));
            if (n) repeatedSweep &= direction(tokens.at(ids[n - 1]).first().lane, tokens.at(ids[n]).first().lane) == step;
        }
        bool movingRails = ids.size() >= 6 && keys.size() >= 3 && !axis;
        for (int n = 2; movingRails && n < ids.size(); ++n)
            movingRails &= distance(tokens.at(ids[n]).first().lane, tokens.at(ids[n-2]).first().lane) <= 1;
        movingRails &= !sweep && !repeatedSweep;
        const bool fullCircle = sweep && ids.size() >= 8;
        const double cadence = tokens.at(ids.at(1)).first().second - tokens.at(ids.first()).first().second;
        const auto laneAt = [&](int index) { return tokens.at(ids.at(index)).first().lane; };
        bool rimSweep = sweep && !repeatedSweep;
        int travelled = 0;
        for (int i = 1; rimSweep && i < ids.size(); ++i) {
            rimSweep = unitStep(laneAt(i - 1), laneAt(i)) == step;
            ++travelled;
        }
        const bool steering = steeringLast > first;
        sweep |= repeatedSweep;
        if (!sweep && period == 3) period = 0;
        QString id = movingRails && !axis ? "T04" : axis ? (keys.size() == 3 ? "T08" : "T07") : period == 2 ? "T01"
            : sweep ? (fullCircle ? "T30" : "T28") : period == 4 ? "T10" : "T11";
        QString name = movingRails && !axis ? "moving_interaction" : axis ? "axis" : period == 2 ? "trill" : sweep ? "sweep" : period == 4 ? "four_cycle" : "point_run";
        if (steering) { id = "T09"; name = "steering_interaction"; }
        if (prioritized && rimSweep && travelled >= 16 && cadence <= chart::handOrderSweepGapSeconds(sweepThreshold) + epsilon) {
            id = "T33"; name = "multiple_rotations";
        } else if (prioritized && rimSweep && !fullCircle && cadence > chart::handOrderSweepGapSeconds(sweepThreshold) + epsilon) {
            bool crosses = false;
            for (int i = 1; i < ids.size(); ++i)
                crosses |= natural(laneAt(i - 1)) != natural(laneAt(i));
            if (crosses) {
                id = "T05"; name = "staircase_interaction";
                sweep = false;
            } else {
                id = "T26"; name = "staircase";
            }
        }
        const bool definite = sweep || period || axis || movingRails || steering || id == "T05";
        if (!definite) { ++first; continue; }
        QVector<int> terminal;
        if (!sweep && (axis || period == 2 || movingRails) && ids.size() >= 4) {
            const int tail = ids.last(); const double stepSeconds = tokens[tail].first().second-tokens[ids[ids.size()-2]].first().second;
            for (int j = tail+1; j < tokens.size(); ++j) {
                const double gap = tokens[j].first().second-tokens[tail].first().second;
                if (gap > stepSeconds+epsilon) break;
                if (tokens[j].selected && !claimed.contains(j) && buttonStrike(tokens[j])
                    && std::abs(gap-stepSeconds) < epsilon) terminal.append(j);
            }
            if (terminal.size() != 2) terminal.clear();
        }
        const int streamSize = ids.size(); ids += terminal;
        auto s = suggestion(tokens, ids, id, name, definite);
        Assignments a, sides;
        const Hand firstHand = pointHand(tokens.at(ids.first()));
        for (int n = 0; n < ids.size(); ++n) {
            a[ids[n]] = strikeAssignment(tokens.at(ids[n]), n % 2 ? flip(firstHand) : firstHand);
            sides[ids[n]] = strikeAssignment(tokens.at(ids[n]), pointHand(tokens.at(ids[n])));
        }
        if (!terminal.isEmpty()) {
            // Continue the two rails into the Each using the shortest role travel.
            const int x = terminal[0], y = terminal[1];
            const int lastA = ids[streamSize-2], lastB = ids[streamSize-1];
            const int straight = distance(tokens[lastA].first().lane,tokens[x].first().lane)+distance(tokens[lastB].first().lane,tokens[y].first().lane);
            const int crossed = distance(tokens[lastA].first().lane,tokens[y].first().lane)+distance(tokens[lastB].first().lane,tokens[x].first().lane);
            const Hand firstRole = a[lastA].head;
            a[x] = strikeAssignment(tokens[x],straight <= crossed ? firstRole : flip(firstRole));
            a[y] = strikeAssignment(tokens[y],flip(a[x].head));
        }
        if (sweep) {
            const bool mixedMaterial = std::any_of(ids.cbegin(), ids.cend(), [&](int id) {
                return chart::handOrderHasStarStrike(tokens.at(id).first());
            });
            // A relaxed first/last star can be followed by a launch or an
            // independent track. Keep such variants available for review.
            const double gap = tokens.at(ids.at(1)).first().second - tokens.at(ids.first()).first().second;
            if (mixedMaterial && gap > 0.13 + epsilon) { s.definite = false; s.preferred = -1; }
            const Hand preferredSweepHand = gestureHand(tokens, ids);
            Hand sweepingHand = preferredSweepHand;
            for (int token : ids) for (const auto& context : tokens)
                for (const auto& hold : context.markers)
                    if (holdLocksHead(hold, tokens.at(token).first(), preferredSweepHand)) sweepingHand = flip(preferredSweepHand);
            Assignments one; for (int token : ids) one[token] = strikeAssignment(tokens.at(token), sweepingHand);
            addVariant(s, text, tokens, ids, one, "same_hand");
            if (s.variants.isEmpty()) { s.definite = false; s.preferred = -1; }
            bool split = false;
            for (int n = 1; n < ids.size(); ++n) split |= sides[ids[n]].head != sides[ids[0]].head;
            if (repeatedSweep) for (int n = 0; n < ids.size(); ++n)
                sides[ids[n]].head = (n / period) % 2 ? flip(firstHand) : firstHand;
            else if (!split) for (int n = 0; n < ids.size(); ++n)
                sides[ids[n]].head = n < (ids.size() + 1) / 2 ? firstHand : flip(firstHand);
            addVariant(s, text, tokens, ids, sides, "paginate");
        } else {
            s.interactionPhase = InteractionPhaseMode::Alternating;
            addInteractionVariants(s, text, tokens, ids, a, sides);
            if (id == "T05") {
                Assignments singleHand;
                const Hand hand = gestureHand(tokens, ids);
                for (int token : ids) singleHand[token] = strikeAssignment(tokens.at(token), hand);
                addVariant(s, text, tokens, ids, singleHand, "same_hand");
            }
        }
        result.append(s);
        claim(terminal);
        first = last;
    }
    result.removeIf([](const auto& s) { return s.variants.isEmpty(); });
    if (prioritized) resolvePatternClaims(result);
    for (auto& group : result) {
        QVector<int> ids;
        if (!group.variants.isEmpty()) for (int i = 0; i < tokens.size(); ++i)
            for (const auto& m : group.variants.first().markers)
                if (tokens.at(i).first().parseOrder == m.parseOrder) { ids.append(i); break; }
        group.similarityKey = similarKey(tokens, ids, group);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return result;
}
} // namespace miacode::hand_order
