#include "core/analysis/hand_order/HandOrderHoldCandidates.h"
#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include <QCoreApplication>
#include <QTextStream>

using namespace miacode::hand_order;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << message << '\n';
        ok &= pass;
    };
    const QString locked = "(150){8},2~r,3~r,2h[2:1]~r,,1~r,8~l,1~r,";
    BasicParameters strict; strict.overwriteExisting = true; strict.counterfactualBudget = 0;
    for (int input : {0,1,2}) for (int begin : {0, int(locked.indexOf("1~r"))}) {
        strict.inputPreference = input;
        const auto plan = inferBasicHandOrder(locked, SimaiParser::validateSyntax(locked), begin, locked.size(), strict);
        QString output = locked;
        for (int i = plan.patches.size() - 1; i >= 0; --i) {
            const auto& patch = plan.patches.at(i); output.replace(patch.start, patch.end - patch.start, patch.text);
        }
        const auto parsed = SimaiParser::validateSyntax(output); int leftOnes = 0; bool rightHold = false;
        for (const auto& m : parsed.noteMarkers) {
            if (m.type == "tap" && m.lane == 1) leftOnes += m.handOrderHead == HandOrderHand::Left;
            if (m.type == "hold") rightHold = m.handOrderHead == HandOrderHand::Right;
        }
        if (leftOnes != 2 || !rightHold) QTextStream(stderr) << "LOCKED_OUTPUT " << output << '\n';
        expect(leftOnes == 2 && rightHold, "a right-hand Hold forces both lane-1 taps left, including its exact native tail");
    }
    const auto hold = SimaiParser::validateSyntax("(150){8}2h[2:1]~r,,1~r,8~l,1~r,");
    expect(!respectsHoldLocks(hold.noteMarkers, hold.noteMarkers), "the hard Hold check rejects a conflicting fixed annotation");
    const QString carriedShort = "(150){8}2-6[4:1]~r,,2h[16:1]~r,1~l,";
    for (int input : {0,1,2}) {
        BasicParameters fixed; fixed.evaluateCompleteAssignment = true; fixed.counterfactualBudget = 0; fixed.inputPreference = input;
        fixed.diagnosticSlideInputOffsetSeconds = 0.0;
        const auto rejected = inferBasicHandOrder(carriedShort, SimaiParser::validateSyntax(carriedShort),0,carriedShort.size(),fixed);
        expect(!rejected.evaluation.complete, "with a fixed moving Slide, a short Hold cannot inherit its head contact and escape full-duration locking");
    }
    for (const auto& text : {QStringLiteral("(120){4}1h[2:1],"), QStringLiteral("(156){8}1h[8:1],"),
                             QStringLiteral("(120){4}Ch[2:1],")}) {
        const auto parsed = SimaiParser::validateSyntax(text);
        expect(parsed.ok && parsed.noteMarkers.size() == 1, "hold fixture parses as one native object");
        if (!parsed.ok || parsed.noteMarkers.size() != 1) continue;
        const auto marker = parsed.noteMarkers.first();
        const bool button = marker.type == "hold";
        ContactAction full;
        full.kind = button ? QStringLiteral("button") : QStringLiteral("touch_hold");
        full.hand = 1; full.start = marker.second; full.end = marker.endSecond;
        full.points = {button ? QPointF(0.3826834324, -0.9238795325) : touchPadPoint(marker.touchPad)};
        full.times = {full.start};
        const auto choices = holdReleaseCandidates(full, marker, 0.025);
        expect(!choices.isEmpty() && choices.first().end == full.end,
               "the full-duration physical hold remains the first option");
        for (const auto& action : choices) {
            const auto replay = replayHandOrderChart({marker}, {{action, button, marker.lane}});
            expect(replay.valid && replay.meetsTarget && replay.judgements.first().highQuality,
                   "every full-duration default independently satisfies the unchanged full replay");
        }
        if (marker.endSecond - marker.second > 0.9) {
            expect(choices.size() == 1, "long stationary hold keeps only the full-duration default");
            auto unsafe = full; unsafe.end = full.start + 0.7;
            const auto replay = replayHandOrderChart({marker}, {{unsafe, button, marker.lane}});
            expect(!replay.meetsTarget, "releasing inside the required hold body still fails the target");
        }
    }
    const QString chain = QStringLiteral("(130){8}6,5,4,3,{16}2h[16:3],,,3h[16:3],,,4,,");
    for (bool touch : {false, true}) for (bool longBody : {false, true})
        for (bool ex : {false, true}) for (bool breaking : {false, true}) {
            const QString text = touch ? QStringLiteral("(120){4}Ch[2:1],") : QStringLiteral("(120){4}1h[2:1],");
            auto marker = SimaiParser::validateSyntax(text).noteMarkers.first();
            marker.isEx = marker.headEx = ex;
            marker.isBreak = marker.headBreak = breaking && !touch;
            marker.endSecond = marker.second + (touch ? 0.45 : 0.3) + (longBody ? 0.08 : -0.01);
            ContactAction full;
            full.hand = 1; full.kind = touch ? QStringLiteral("touch_hold") : QStringLiteral("button");
            full.start = marker.second; full.end = marker.endSecond;
            full.points = {touch ? touchPadPoint(marker.touchPad) : QPointF(0.3826834324, -0.9238795325)};
            full.times = {full.start};
            const auto choices = holdReleaseCandidates(full, marker, 0.025);
            const bool tapRelease = std::any_of(choices.cbegin(), choices.cend(), [&](const auto& action) {
                return std::abs(action.end - action.start - 0.025) < 1e-8;
            });
            expect(!tapRelease,
                "short and long Hold defaults never use tap-release, including EX and Break");
            auto instant = full; instant.end = instant.start + 0.025;
            const auto replay = replayHandOrderChart({marker}, {{instant, !touch, marker.lane}});
            expect(replay.meetsTarget != longBody,
                "EX and Break do not remove the required sustained Hold body in complete replay");
        }
    BasicParameters parameters; parameters.counterfactualBudget = 0;
    const auto result = inferBasicHandOrder(chain, SimaiParser::validateSyntax(chain), 0, chain.size(), parameters);
    expect(!result.rows.isEmpty() && std::all_of(result.rows.cbegin(), result.rows.cend(), [](const auto& row) {
        return row.fullReplayVerified;
    }) && result.rows.last().peakSpeed < 8.0,
        "successive holds retain a comfortable verified approach to the following adjacent tap");
    return ok ? 0 : 1;
}
