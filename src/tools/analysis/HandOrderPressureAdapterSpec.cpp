#include "core/analysis/HandOrderPressureAdapter.h"
#include "core/analysis/ChartStructureAnalysis.h"

#include <QTextStream>
#include <cmath>

using miacode::chart_structure::estimateHandOrderPressure;
using miacode::chart_structure::estimatePressure;

namespace {

bool expect(bool result, const char* message)
{
    if (!result) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return result;
}

bool equal(double first, double second)
{
    return std::abs(first - second) < 1e-8;
}

TimelineNoteMarker note(const char* type, double second, HandOrderHand hand)
{
    TimelineNoteMarker marker;
    marker.type = QString::fromLatin1(type);
    marker.second = second;
    marker.handOrderHead = hand;
    return marker;
}

bool measureTracksAndOccupancy()
{
    auto hold = note("hold", 1.0, HandOrderHand::Left);
    hold.endSecond = 4.0;
    const auto tap = note("tap", 2.0, HandOrderHand::Right);
    auto track = note("slide", 3.25, HandOrderHand::Unspecified);
    track.slideTraceSecond = 3.75;
    track.endSecond = 5.75;
    track.endLane = 5;
    track.handOrderTrackStops = {{0.0, HandOrderHand::Left}, {0.5, HandOrderHand::Right}};
    auto wifi = note("wifi", 6.25, HandOrderHand::Unspecified);
    wifi.slideTraceSecond = 6.25;
    wifi.endSecond = 7.25;
    wifi.handOrderWifiMode = HandOrderWifiMode::Split;
    wifi.handOrderWifiSectorStops = {{{0.0, HandOrderHand::Left}}, {{0.0, HandOrderHand::Right}}};

    const auto curve = estimateHandOrderPressure({hold, tap, track, wifi}, -1.25, 7.0, 0.25, 28);
    if (!expect(curve.frames.size() == 28, "sample counts match the basic curve")) return false;
    bool ok = expect(equal(curve.frames.at(0).leftOccupancy, 1.0),
        "the hold already active at media zero remains occupied");
    ok &= expect(equal(curve.frames.at(3).rightOccupancy, 0.34), "right tap occupancy is fractional");
    ok &= expect(equal(curve.frames.at(13).leftOccupancy, 1.0) && curve.frames.at(13).leftMovement > 0.0,
        "the left hand owns movement before the handoff");
    ok &= expect(equal(curve.frames.at(14).leftOccupancy, 0.0) && equal(curve.frames.at(14).rightOccupancy, 1.0)
        && equal(curve.frames.at(14).alternation, 1.0), "the boundary frame belongs to the incoming hand");
    ok &= expect(equal(curve.frames.at(20).leftOccupancy, 1.0) && equal(curve.frames.at(20).rightOccupancy, 1.0),
        "the two native Wifi sectors contribute independent hand occupancy");
    return ok;
}

bool measureHeadsAndFallback()
{
    auto first = note("tap", 0.1, HandOrderHand::Left);
    auto second = note("tap", 0.3, HandOrderHand::Left);
    first.lane = 1;
    second.lane = 5;
    auto head = note("slide", 1.0, HandOrderHand::Right);
    head.sourceCol = 19;
    auto otherBranch = head;
    otherBranch.slideBranchIndex = 1;
    const auto curve = estimateHandOrderPressure({first, second, head, otherBranch}, 0.0, 2.0, 0.25, 8);
    if (!expect(curve.frames.size() == 8, "explicit head annotations are measured")) return false;
    bool ok = expect(equal(curve.frames.at(1).leftMovement, 1.0), "same-hand movement uses the distance between inputs");
    ok &= expect(equal(curve.frames.at(4).rightOccupancy, 0.34), "a shared slide head only occupies a hand once");
    first.handOrderHead = HandOrderHand::Either;
    second.isMine = true;
    head.headMine = true;
    ok &= expect(estimateHandOrderPressure({first, second, head}, 0.0, 2.0, 0.25, 8).frames.isEmpty(),
        "unknown assignments and mine notes fall back to base pressure");
    return ok;
}

bool explicitContextReplacesAnonymousMovement()
{
    SimaiParseResult chart;
    chart.durationSeconds = 8.0;
    for (int i = 0; i < 32; ++i) {
        auto tap = note("tap", i * 0.125, HandOrderHand::Unspecified);
        tap.lane = i % 2 == 0 ? 1 : 5;
        chart.noteMarkers.append(tap);
    }
    const auto anonymous = estimatePressure(chart, 0.0, 0.0);
    for (qsizetype i = 0; i < chart.noteMarkers.size(); ++i)
        chart.noteMarkers[i].handOrderHead = i % 2 == 0 ? HandOrderHand::Left : HandOrderHand::Right;
    const auto alternating = estimatePressure(chart, 0.0, 0.0);
    for (auto& tap : chart.noteMarkers) tap.handOrderHead = HandOrderHand::Left;
    const auto singleHand = estimatePressure(chart, 0.0, 0.0);
    for (qsizetype i = 1; i < chart.noteMarkers.size(); i += 2)
        chart.noteMarkers[i].handOrderHead = HandOrderHand::Either;
    const auto mixed = estimatePressure(chart, 0.0, 0.0);

    const auto& base = anonymous.frames.at(10);
    const auto& twoHands = alternating.frames.at(10);
    const auto& oneHand = singleHand.frames.at(10);
    bool ok = expect(equal(base.density, twoHands.density) && equal(base.density, oneHand.density),
        "applied annotations leave scoring TPS intact");
    ok &= expect(equal(twoHands.movement, 0.0) && twoHands.strength < base.strength
        && oneHand.movement > base.movement && oneHand.strength > base.strength,
        "explicit per-hand history replaces the anonymous reach with either smaller or larger effort");
    ok &= expect(equal(mixed.frames.at(10).movement, base.movement)
        && equal(mixed.frames.at(10).endurance, base.endurance),
        "unknown assignments break exact contact history and retain the anonymous calculation");

    auto trace = note("slide", 1.0, HandOrderHand::Unspecified);
    trace.hasHeadStar = false;
    trace.slideTraceSecond = 1.25;
    trace.endSecond = 3.0;
    trace.endLane = 5;
    chart.noteMarkers = {trace};
    const auto anonymousTrack = estimatePressure(chart, 0.0, 0.0);
    chart.noteMarkers[0].handOrderTrackStops = {{0.0, HandOrderHand::Left}, {0.5, HandOrderHand::Right}};
    const auto annotatedTrack = estimatePressure(chart, 0.0, 0.0);
    ok &= expect(equal(anonymousTrack.frames.at(7).strength, annotatedTrack.frames.at(7).strength),
        "a track handoff does not count the same physical trace twice");
    return ok;
}

} // namespace

int main()
{
    const bool track = measureTracksAndOccupancy();
    const bool heads = measureHeadsAndFallback();
    const bool explicitContext = explicitContextReplacesAnonymousMovement();
    return track && heads && explicitContext ? 0 : 1;
}
