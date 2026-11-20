#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include <QCoreApplication>
#include <QTextStream>

using namespace miacode::hand_order;
QString apply(QString text, const QVector<miacode::chart_transform::HandOrderEditPlan::Patch>& patches) {
    for (int i = patches.size() - 1; i >= 0; --i) { const auto& p = patches[i]; text.replace(p.start, p.end - p.start, p.text); }
    return text;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); bool ok = true;
    const auto expect = [&](bool pass, const char* message) { ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n'; };
    const auto scan = [&](const QString& text) { return optimizeHandOrderPatterns(text, SimaiParser::validateSyntax(text), 0, text.size()); };
    for (int rotation = 0; rotation < 8; ++rotation) for (int mirror : {1, -1}) {
        QString text = "(150){16}";
        for (int n = 0; n < 9; ++n) text += QString::number((rotation + mirror * (QVector<int>{3,4,1}.at(n % 3) - 1) + 16) % 8 + 1) + ',';
        const auto r = scan(text);
        expect(r.patternSuggestions.size() == 1 && r.patternSuggestions.first().registryId == "T06"
            && r.patternSuggestions.first().definite, "triangle survives rotation and mirror");
        const auto parsed = SimaiParser::validateSyntax(apply(text, r.patches)); bool alternating = true;
        for (int i = 1; i < parsed.noteMarkers.size(); ++i) alternating &= parsed.noteMarkers[i].handOrderHead != parsed.noteMarkers[i-1].handOrderHead;
        expect(alternating && stripAnnotations(apply(text,r.patches)) == text, "triangle stays in one alternating phase without native edits");
    }
    for (const auto& ordinary : {QString("(150){8}1,1,4,4,6,3,5,"),
        QString("(120){4}2,2,2,"), QString("(150){16}1,4,6,"),
        QString("(150){16}1,3,6,2,7,4,")})
        expect(scan(ordinary).patternSuggestions.isEmpty(), "ordinary pairs, slow repeated notes and unrelated points are not named patterns");
    for (int bpm : {150, 185, 240}) for (int rotation = 0; rotation < 8; ++rotation)
        for (int mirror : {1, -1}) for (const QString& modifier : {QString(), QString("x"), QString("b")}) {
            QString text = QString("(%1){16}").arg(bpm);
            for (int lane : {2, 1, 3})
                text += QString::number((rotation + mirror * (lane - 1) + 16) % 8 + 1) + modifier + ',';
            const auto result = scan(text);
            bool folded = result.patternSuggestions.size() == 1
                && result.patternSuggestions.first().registryId == "T06";
            const auto assigned = SimaiParser::validateSyntax(apply(text, result.patches));
            const auto layout = miacode::chart_transform::basicHandOrderLayoutInSelection(text, 0, 0);
            const auto laidOut = SimaiParser::validateSyntax(layout.edit.replacement);
            for (int i = 1; folded && i < 3; ++i)
                folded &= assigned.noteMarkers[i].handOrderHead != assigned.noteMarkers[i-1].handOrderHead
                    && laidOut.noteMarkers[i].handOrderHead != laidOut.noteMarkers[i-1].handOrderHead;
            expect(folded && stripAnnotations(apply(text,result.patches)) == text
                && stripAnnotations(layout.edit.replacement) == text,
                "fast folded triples alternate in rules and basic layout across geometry and modifiers");
        }
    const QString folds = "(185){16}3/7,,2,1,3,,7,8,6,,";
    const auto foldLayout = miacode::chart_transform::basicHandOrderLayoutInSelection(folds, 0, 0);
    expect(foldLayout.edit.replacement == "(185){16}3~r/7~l,,2~r,1~l,3~r,,7~l,8~r,6~l,,",
        "two short folded triples continue their own alternating roles after an Each");
    expect(scan("(185){8}2,1,3,").patternSuggestions.isEmpty(),
        "a relaxed three-point turn does not force a fast triangle default");
    const QString staleFold = "(185){16}2~l,1~l,3~l,";
    expect(apply(staleFold,scan(staleFold).patches) == "(185){16}2~r,1~l,3~r,",
        "fold inference does not use old annotation hands as an answer prior");
    const QString mixedTrill = "(185){16}1~r,2-5[1##0.45]~[head=r;track=0:r|0.5:l],"
        "1$~l,2~l,1b~l,2~l,1~l,2~l,3~r,7~l,E";
    const auto trill = scan(mixedTrill);
    const QString trillResult = apply(mixedTrill, trill.patches);
    const auto mixedNotes = SimaiParser::validateSyntax(trillResult);
    bool followsPhase = mixedNotes.noteMarkers.size() == 10;
    for (int i = 1; followsPhase && i < 8; ++i)
        followsPhase &= mixedNotes.noteMarkers.at(i - 1).handOrderHead != mixedNotes.noteMarkers.at(i).handOrderHead;
    expect(trill.patternSuggestions.size() >= 1 && trill.patternSuggestions.first().registryId == "T01"
        && followsPhase && trillResult.contains("track=0:r|0.5:l") && stripAnnotations(trillResult) == stripAnnotations(mixedTrill),
        "a mixed Tap and star trill keeps one phase through its last strike and preserves the handoff");
    const QString eighthPair = "(240){8}5~l,5~r,E";
    expect(apply(eighthPair, scan(eighthPair).patches) == "(240){8}5~l,5~l,E",
        "a split same-key eighth-note pair is repaired by pattern optimization");
    const QString continuousMirror = "(150){12}7,5,8,2,4,1,7,3,5,4/6,";
    const auto mirroredRun = scan(continuousMirror);
    expect(mirroredRun.patternSuggestions.size() == 1 && mirroredRun.patternSuggestions.first().registryId == "T06"
        && apply(continuousMirror, mirroredRun.patches).contains("7~l,5~r,8~l,2~r,4~l,1~r,"),
        "adjacent reflected triples are identified inside a longer regular stream");
    const QString foldIntoHold = "(118){16}2h[16:7]~r,,,,E4~l,,,4,3,5,5h[8:3],,E5~r,";
    const auto joined = scan(foldIntoHold);
    expect(apply(foldIntoHold, joined.patches) == "(118){16}2h[16:7]~r,,,,E4~l,,,4~l,3~r,5~l,5h[8:3]~l,,E5~r,",
        "triangle phase uses the entering Hold and retains its final hand on the following same-key Hold");
    const QString activeHold = "(150){16}3h[4:1]~r,4,3,5,";
    const auto lockedTriangle = scan(activeHold);
    expect(apply(activeHold, lockedTriangle.patches) == "(150){16}3h[4:1]~r,4~l,3~r,5~l,"
        && lockedTriangle.patternSuggestions.size() == 1
        && lockedTriangle.patternSuggestions.first().variants.at(lockedTriangle.patternSuggestions.first().preferred).choiceKey == "mirror",
        "one occupied head flips the entire triangle instead of changing one label");
    const QString noCoherentPhase = "(150){16}1h[2:1]~r,4,3,5,";
    const auto blockedTriangle = scan(noCoherentPhase);
    expect(blockedTriangle.patternSuggestions.size() == 1 && blockedTriangle.patternSuggestions.first().preferred < 0
        && blockedTriangle.patches.isEmpty(), "a sustained conflict on both phases leaves the triangle for explicit review");
    const QString moving = "(130){16}2h[8:1]~r/5~l,,,,1,3,8,4,7,5,6,,2~r/4~l,2~r,8~l,";
    const auto movingPlan = scan(moving);
    expect(apply(moving, movingPlan.patches).contains("1~l,3~r,8~l,4~r,7~l,5~r,6~l,,2~r/4~l,2~r,8~l"),
        "moving interaction uses both the entering Hold and the following configuration");
    const QString paginated = "(130){16}4~r,5~l,6~l,";
    const auto pagination = scan(paginated);
    expect(pagination.patternSuggestions.size() == 1 && pagination.patternSuggestions.first().registryId == "T28"
        && pagination.patternSuggestions.first().preferred < 0 && pagination.patches.isEmpty(),
        "the final review retains a coherent two-hand split of a short moderate sweep");
    const QString umi = "(150){8}1-5[4:1],3,1/5-1[4:1],7,5/1-5[4:1],3,1,,,,";
    const auto u = scan(umi);
    expect(u.patternSuggestions.size() == 1 && u.patternSuggestions.first().registryId == "N01", "head/off-lane/launch triplets identify Umiyuri");
    if (!u.patternSuggestions.isEmpty()) {
        const auto& variants = u.patternSuggestions.first().variants;
        expect(variants.size() == 2, "Umiyuri retains its start-hand mirror");
        const auto parsed = SimaiParser::validateSyntax(apply(umi, variants.first().patches));
        bool same = true; QVector<HandOrderHand> stars;
        for (const auto& m : parsed.noteMarkers) if (m.type == "slide") {
            stars.append(m.handOrderHead); same &= m.handOrderHead == m.handOrderTrackStops.first().hand;
            for (const auto& tap : parsed.noteMarkers) if (tap.type == "tap"
                && tap.second > m.second && tap.second <= m.slideTraceSecond + 1e-7
                && (tap.lane == m.lane || std::abs(tap.second - (m.second + m.slideTraceSecond)/2) < 1e-7))
                same &= tap.handOrderHead == m.handOrderHead;
        }
        expect(same && stars.size() == 3 && stars[0] != stars[1] && stars[0] == stars[2], "each Umiyuri hand strikes all three heads and follows its own slide; groups alternate");
    }
    for (const QString& stars : {QString("(150){4}1-5[4:1]/1-3[4:1],"), QString("(150){4}1-3[4:1]*-5[4:1],")}) {
        const auto s = scan(stars); const auto parsed = SimaiParser::validateSyntax(apply(stars,s.patches));
        bool correct = parsed.noteMarkers.size() == 2;
        for (const auto& m : parsed.noteMarkers) correct &= !m.handOrderTrackStops.isEmpty()
            && m.handOrderTrackStops.first().hand == (m.endLane == 5 ? HandOrderHand::Left : HandOrderHand::Right);
        expect(s.patternSuggestions.size() == 1 && correct, "forked stars assign 1-5 left and 1-3 right independently of branch spelling/order");
    }
    const QString simple = "(150){8}4,4,5,5,6,3,5,";
    for (const QString& sweeping : {QString("(150){16}1,2,3,"), QString("(150){16}1,2,3,1,2,3,"),
            QString("(150){16}1,3,5,"), QString("(150){16}1,3,5,1,3,5,"),
            QString("(150){16}1,2,3,4,1,2,3,4,"), QString("(150){16}1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8,"),
            QString("(150){16}1b,,,2x,,,3x,,,4x,,,5x,,,6x,,,7x,,,8x,,,1x,,,2x,,,3x,,,4x,,,5x,,,6x,,,7x,,,8x,,,")}) {
        const auto sweep = scan(sweeping);
        bool sweepChoices = sweep.patternSuggestions.size() == 1;
        if (sweepChoices) {
            const auto& group = sweep.patternSuggestions.first();
            sweepChoices &= group.preferred == 0 && group.variants.size() == 2
                && group.variants.first().strategy == "same_hand" && group.variants.last().strategy == "paginate";
            const auto defaults = group.variants.first().markers;
            for (const auto& marker : defaults) sweepChoices &= marker.handOrderHead == defaults.first().handOrderHead;
            const auto pages = group.variants.last().markers; int switches = 0;
            for (int n = 1; n < pages.size(); ++n) switches += pages[n].handOrderHead != pages[n-1].handOrderHead;
            sweepChoices &= switches > 0 && switches < pages.size() - 1;
        }
        expect(sweepChoices, "sweeps default to a whole-hand gesture and offer contiguous pagination without per-note alternation");
    }
    const QString anchored = "(150){8}2h[2:1]~r,,1,8,1,8,";
    const QString slideJacks = "(150){8}1-5[2:1]~r,,1~r,1~l,1~l,,,,";
    const auto slideChoices = scan(slideJacks);
    expect(slideChoices.patches.isEmpty() && !slideChoices.patternSuggestions.isEmpty()
        && slideChoices.patternSuggestions.first().preferred == -1,
        "a short repeated-key group with existing mixed roles under a Slide remains an explicit choice");
    const auto anchoredPlan = scan(anchored); bool holdSafe = !anchoredPlan.patternSuggestions.isEmpty();
    const auto anchorParsed = SimaiParser::validateSyntax(anchored);
    for (const auto& group : anchoredPlan.patternSuggestions) for (const auto& choice : group.variants)
        holdSafe &= respectsHoldLocks(anchorParsed.noteMarkers, choice.markers)
            && SimaiParser::validateSyntax(apply(anchored,choice.patches)).annotationErrors.isEmpty();
    expect(holdSafe, "every offered pattern choice honors an external Hold through its native tail");
    BasicParameters p; p.overwriteExisting = true; p.counterfactualBudget = 0;
    const auto inferred = inferBasicHandOrder(simple, SimaiParser::validateSyntax(simple), 0, simple.size(), p);
    const auto parsed = SimaiParser::validateSyntax(apply(simple,inferred.patches));
    const QVector<HandOrderHand> expected{HandOrderHand::Right,HandOrderHand::Right,HandOrderHand::Left,HandOrderHand::Left,HandOrderHand::Left,HandOrderHand::Right,HandOrderHand::Left};
    bool correct = parsed.noteMarkers.size() == expected.size();
    for (int i = 0; correct && i < expected.size(); ++i) correct &= parsed.noteMarkers[i].handOrderHead == expected[i];
    if (!correct) QTextStream(stderr) << apply(simple, inferred.patches) << '\n';
    expect(correct, "relaxed repeated pairs remain rrll rather than unnecessary alternation");
    const QString partial = "(150){16}3~l,4~l,1~l,3~l,4~l,1~l,";
    const auto selected = optimizeHandOrderPatterns(partial, SimaiParser::validateSyntax(partial), partial.indexOf("4~l") + 1, partial.size() - 2);
    expect(std::all_of(selected.patches.cbegin(), selected.patches.cend(), [&](const auto& patch) {
        return patch.start >= partial.indexOf("4~l") + 1 && patch.end <= partial.size() - 2;
    }), "a triangle default never changes a partially selected token");
    BasicParameters tiny; tiny.maxWorkUnits = 1000;
    const auto fast = optimizeHandOrderPatterns(umi, SimaiParser::validateSyntax(umi), 0, umi.size(), tiny);
    expect(fast.patternOutcome == "recognized" && fast.workUsed == 0 && !fast.workLimited, "structural defaults are independent of the solver work budget");
    return ok ? 0 : 1;
}
