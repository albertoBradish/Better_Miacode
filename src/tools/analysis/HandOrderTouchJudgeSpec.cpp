#include "core/analysis/hand_order/HandOrderJudge.h"
#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QTextStream>
#include <limits>
#include <cmath>
using namespace miacode::hand_order;
namespace {
ContactAction press(const QString& pad, double start, double end, int hand, int source = 1, QString kind = "touch") {
    ContactAction action; action.tokenStart = source; action.hand = hand; action.kind = kind;
    action.start = start; action.end = end; action.motionStart = start; action.motionEnd = end;
    action.points = {touchPadPoint(pad)}; action.times = {start}; return action;
}
TouchTarget target(const QString& pad, int order, int each = 1, double second = 0) {
    return {order, order, each, pad, second, false, -1};
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message) { QTextStream(pass ? stdout : stderr)
        << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass; };
    expect(touchPadsAdjacent("b1", "c") && touchPadsAdjacent("B1", "B2")
        && !touchPadsAdjacent("A1", "A2") && !touchPadsAdjacent("B1", "B1"),
        "profile reuses native sensor adjacency with normalized pad names");
    for (int n = 2; n <= 5; ++n) {
        QVector<TouchTarget> targets; QVector<ContactAction> actions;
        for (int i = 0; i < n; ++i) targets.append(target(QStringLiteral("B%1").arg(i + 1), i));
        const int threshold = int(std::ceil(0.51 * n));
        for (int i = 0; i < threshold - 1; ++i) actions.append(press(targets.at(i).pad, i * 0.02, i * 0.02 + 0.01, 0, i));
        const auto partial = replayHandOrderTouches(targets, actions, 0.1);
        int partialCount = 0; for (const auto& judged : partial.judgements) partialCount += judged.highQuality;
        expect(partialCount == threshold - 1, "a connected group remains partial below ceil(0.51 N)");
        actions.append(press(targets.at(threshold - 1).pad, 0.08, 0.1, 0, threshold - 1));
        const auto complete = replayHandOrderTouches(targets, actions, 0.1);
        int completed = 0; for (const auto& judged : complete.judgements) completed += judged.highQuality;
        expect(completed == n && complete.groups.size() == 1, "N=2..5 thresholds are 2,2,3,3 and only then carry");
        if (n > threshold) expect(complete.judgements.last().kind == "group_carry"
            && complete.judgements.last().causes.size() == threshold && complete.judgements.last().handMask == 1,
            "single-hand carry retains the full threshold cause set");
    }
    QVector<TouchTarget> mixed{target("B1", 0), target("B2", 1), target("B3", 2)};
    const auto carried = replayHandOrderTouches(mixed, {press("B1", 0, 0.03, 0, 10, "slide"), press("B3", 0, 0.03, 1, 20, "slide")}, 0.1);
    expect(carried.judgements.at(1).kind == "group_carry" && carried.judgements.at(1).handMask == 3
        && carried.judgements.at(1).causes.size() == 2,
        "same-frame mixed-hand threshold keeps both contributors instead of the last event hand");
    mixed.append(target("B2", 3, 2));
    const auto noFeedback = replayHandOrderTouches(mixed, {press("B1", 0, 0.03, 0), press("B3", 0, 0.03, 1)}, 0.1);
    expect(!noFeedback.judgements.last().judged && noFeedback.groups.size() == 2,
        "carry does not create a contact or recursively judge another Each group");
    QVector<TouchTarget> repeated{target("B1", 0, 1, 0), target("B1", 1, 2, 0.1)};
    const auto held = replayHandOrderTouches(repeated, {press("B1", 0, 0.3, 0)}, 0.2);
    expect(held.judgements.first().highQuality && !held.judgements.last().judged,
        "continuous pad ON cannot supply a second DOWN");
    const auto reentry = replayHandOrderTouches(repeated, {press("B1", 0, 0.03, 0), press("B1", 0.1, 0.13, 1)}, 0.2);
    expect(reentry.judgements.first().handMask == 1 && reentry.judgements.last().handMask == 2,
        "OFF then reentry supplies a distinct event and its actual hand");
    const auto unionOn = replayHandOrderTouches(repeated, {press("B1", 0, 0.12, 0), press("B1", 0.1, 0.3, 1)}, 0.2);
    expect(!unionOn.judgements.last().judged, "changing contributors while the sensor stays ON is not a new event");
    const auto competing = replayHandOrderTouches({target("B1", 0), target("B1", 1)}, {press("B1", 0, 0.03, 0)}, 0.1);
    expect(competing.judgements.first().highQuality && !competing.judgements.last().judged,
        "one same-frame pad DOWN is consumed once in explicit parse order");
    TouchTarget hold = target("B2", 4); hold.hold = true;
    const auto withoutHold = buildHandOrderTouchGroups({target("B1", 0), target("B3", 1), hold});
    expect(withoutHold.size() == 2 && withoutHold.first().size() == 1,
        "Touch Hold cannot bridge or participate in a normal Touch component");
    auto slice = press("C", 1.5, 1.6, 1, 77, "slide"); slice.motionStart = 1; slice.motionEnd = 3;
    const auto inherited = replayHandOrderTouches({target("C", 0, 1, 1.5)}, {slice}, 1.55);
    expect(inherited.judgements.first().handMask == 2 && inherited.judgements.first().kind == "slide_cover"
        && std::abs(inherited.judgements.first().causes.first().progress - 0.25) < 1e-8,
        "a sliced track reports original full-motion progress and trigger-time hand");
    const auto tooEarly = replayHandOrderTouches({target("B1", 0, 1, 1)}, {press("B1", 0, 2, 0)}, 1.2);
    expect(!tooEarly.judgements.first().highQuality, "contact established before the window cannot retrigger on its boundary");
    qint64 work = 10000;
    const auto longTrack = replayHandOrderTouches({target("B1", 0, 1, 1000)}, {press("B1", 0, 1e9, 0)}, 1000.2, &work);
    expect(!longTrack.exhausted && !longTrack.judgements.first().highQuality && work > 9000,
        "very long contact scans only relevant windows and restores prior ON state");
    const auto overflow = replayHandOrderTouches({target("B1", 0, 1, 1e100)}, {});
    expect(overflow.exhausted, "unsafe time magnitudes return explicit resource failure before tick conversion");
    qint64 tiny = 1;
    expect(replayHandOrderTouches({target("B1", 0)}, {press("B1", 0, 1, 0)}, 0.1, &tiny).exhausted,
        "the shared replay work budget is enforced");
    QVector<ContactAction> inactive;
    for (int i = 0; i < 20; ++i) inactive.append(press("B1", 100 + i, 100.1 + i, i % 2));
    qint64 inactiveBudget = 500;
    expect(replayHandOrderTouches({target("C", 0)}, inactive, 0.1, &inactiveBudget).exhausted,
        "inactive contact scans consume work rather than bypassing the total replay budget");
    QVector<TouchTarget> manyWindows;
    for (int i = 0; i < 20; ++i) manyWindows.append(target("C", i, i, i * 2));
    qint64 targetBudget = 1500;
    expect(replayHandOrderTouches(manyWindows, {}, 100, &targetBudget).exhausted,
        "target and group scans across disjoint windows consume the shared replay budget");
    bool cancelled = false; auto flag = std::make_shared<std::atomic_bool>(true);
    try { miacode::task::CancellationScope scope(flag); replayHandOrderTouches(mixed, {}); }
    catch (const miacode::task::Cancelled&) { cancelled = true; }
    expect(cancelled, "contact replay responds to cancellation");
    return ok ? 0 : 1;
}
