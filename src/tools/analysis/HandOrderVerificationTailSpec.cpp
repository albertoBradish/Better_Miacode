#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

using namespace miacode::hand_order;
namespace {
BasicResult fixed(const QString& source, bool legacy = false, qint64 budget = 300000000)
{
    BasicParameters p; p.evaluateCompleteAssignment = true; p.counterfactualBudget = 0;
    p.legacyFixedTargetLookahead = legacy; p.maxWorkUnits = budget;
    return inferBasicHandOrder(source, SimaiParser::validateSyntax(source), 0, source.size(), p);
}
QString apply(const QString& source, const BasicResult& result)
{
    QString output = source;
    for (auto patch = result.patches.crbegin(); patch != result.patches.crend(); ++patch)
        output.replace(patch->start, patch->end - patch->start, patch->text);
    return output;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& message) {
        ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n';
    };
    const QVector<QPair<QString, QString>> cases{
        {"T17", "(270){32}1~r/8~l,1~r/8~l,,,3~r/6~l,3~r/6~l,"},
        {"T18", "(270){32}1~r/5~l,1~r/5~l,,2~r/6~l,2~r/6~l,,3~r/7~l,3~r/7~l,,4~r/8~l,4~r/8~l,"},
        {"T43", "(270){32}2~r/7~l,2~r/7~l,,3~r/8~l,3~r/8~l,,4~r/1~l,4~r/1~l,"}
    };
    for (const auto& item : cases) {
        const auto result = fixed(item.second);
        bool locked = result.patches.isEmpty() && !result.rows.isEmpty();
        for (const auto& row : result.rows) {
            const auto original = parseAnnotation(item.second.mid(row.start, row.end - row.start), false);
            locked &= row.fullReplayVerified && original.valid && row.head == original.head;
        }
        expect(result.evaluation.complete && locked, item.first + " cold reconstruction verifies every original locked hand without rewriting");
        expect(result.workUsed < 37500000 && !result.workLimited && !result.taskLimited,
            item.first + " fits the standard guard reference allowance without raising its budget");
        const auto legacy = fixed(item.second, true, 1000000);
        expect(!result.searchWindows.isEmpty() && !legacy.searchWindows.isEmpty()
            && result.searchWindows.first().protectedTargetImports == 0
            && legacy.searchWindows.first().protectedTargetImports > 0
            && result.searchWindows.first().plannedTokens < legacy.searchWindows.first().plannedTokens,
            item.first + " separates fixed hands from irrelevant protected-target lookahead");
    }
    const auto a = fixed(cases[1].second);
    const auto b = fixed(QString(cases[1].second).replace("(270){32}", "(135){64}"));
    expect(a.evaluation.complete && b.evaluation.complete && std::abs(a.evaluation.cost - b.evaluation.cost) < 1e-6,
        "high-BPM fixed reconstruction retains equal-second score consistency");
    const QString scoped = "(180){16}1~r,3~r,5~l,7~l,";
    BasicParameters p; p.evaluateCompleteAssignment = true; p.counterfactualBudget = 0;
    const int end = scoped.indexOf("5~l");
    const auto local = inferBasicHandOrder(scoped, SimaiParser::validateSyntax(scoped), 0, end, p);
    bool importedContext = false, keptContext = false;
    for (const auto& window : local.searchWindows) importedContext |= window.protectedContextImports > 0;
    for (const auto& row : local.rows) keptContext |= row.context && row.originalAnnotation == "~l" && row.head == HandOrderHand::Left;
    expect(importedContext && keptContext && local.patches.isEmpty(),
        "bounded fixed windows retain real read-only locks outside a selected range");

    for (int bpm : {90, 150, 210, 270}) {
        const QString raw = QStringLiteral("(%1){4}B1/B5,B2/B4/C,").arg(bpm);
        const QString laidOut = miacode::chart_transform::basicHandOrderLayoutInSelection(raw, 0, 0).edit.replacement;
        BasicParameters free; free.overwriteExisting = true;
        const auto plan = inferBasicHandOrder(laidOut, SimaiParser::validateSyntax(laidOut), 0, laidOut.size(), free);
        const QString output = apply(laidOut, plan);
        const auto parsed = SimaiParser::validateSyntax(output);
        bool concrete = parsed.ok && parsed.annotationErrors.isEmpty();
        for (const auto& note : parsed.noteMarkers) concrete &= note.handOrderHead == HandOrderHand::Left || note.handOrderHead == HandOrderHand::Right;
        const auto reconstructed = fixed(output);
        expect(concrete && reconstructed.evaluation.complete && stripAnnotations(output) == stripAnnotations(raw),
            QStringLiteral("U08 %1 BPM emits a genuinely reconstructible complete assignment").arg(bpm));
        bool directCentre = false;
        for (const auto& row : plan.rows) if (laidOut.mid(row.start, row.end - row.start) == "C")
            directCentre = row.fullReplayVerified && row.judgementKind == "direct_touch"
                && row.judgementQuality == "critical" && row.head != HandOrderHand::Either
                && row.head != HandOrderHand::Unspecified && row.judgementMargin >= -1e-8;
        expect(directCentre, QStringLiteral("U08 %1 BPM centre has real single-hand direct critical evidence").arg(bpm));
    }
    // The original mixed carry remains a valid multi-contributor observation;
    // a solver must find another real stream, never relabel that observation.
    const QString mixed = "(150){4}B1~r/B5~l,B2~r/B4~l/C~m,";
    const auto nonconcrete = SimaiParser::validateSyntax(mixed);
    bool hasEitherCentre = false;
    for (const auto& marker : nonconcrete.noteMarkers) hasEitherCentre |= marker.touchPad == "C" && marker.handOrderHead == HandOrderHand::Either;
    expect(nonconcrete.ok && hasEitherCentre,
        "Either syntax stays distinct from proof of a single responsible hand");
    const QString chain = "(%1){4}1>3[4:1]~[head=r;track=0:r],3>5[4:1]~[head=l;track=0:r],5>7[4:1]~[head=l;track=0:r],7>1[4:1]~[head=l;track=0:r],,,";
    for (int bpm : {210, 270}) {
        const auto result = fixed(chain.arg(bpm));
        expect(result.evaluation.complete && result.patches.isEmpty() && !result.workLimited
            && result.workUsed < 37500000,
            QStringLiteral("S31 %1 BPM cold locked reconstruction finds an existing coherent input-time chain").arg(bpm));
    }
    const QString fan = "(210){8}1-4[4:1],1-5[4:1],1-6[4:1],,,,,";
    const QString layout = miacode::chart_transform::basicHandOrderLayoutInSelection(fan, 0, 0).edit.replacement;
    BasicParameters context; context.overwriteExisting = true;
    const auto fanResult = inferBasicHandOrder(layout, SimaiParser::validateSyntax(layout), 0, layout.size(), context);
    const auto fanFixed = fixed(apply(layout, fanResult));
    bool limited = false, honestConfidence = true;
    for (const auto& window : fanResult.searchWindows) if (window.confidenceIterationLimited) {
        limited = true;
        for (const auto& row : fanResult.rows) if (row.start >= window.start && row.start < window.end)
            honestConfidence &= row.confidence < 0.4;
    }
    expect(limited && honestConfidence && fanFixed.evaluation.complete,
        "S10 retains an actually replayed stream after confidence iteration limit, with insufficient confidence and cold proof");
    BasicParameters uncached = context; uncached.reuseWindowSearch = false;
    const auto fanUncached = inferBasicHandOrder(layout, SimaiParser::validateSyntax(layout), 0, layout.size(), uncached);
    int hits = 0; qint64 reused = 0; bool counted = true;
    for (const auto& window : fanResult.searchWindows) {
        hits += window.searchCacheHits; reused += window.reusedSearchWork;
        counted &= window.searchRequests == window.searchAttempts + window.searchCacheHits;
    }
    bool sameRows = fanUncached.rows.size() == fanResult.rows.size();
    for (int i = 0; sameRows && i < fanResult.rows.size(); ++i) {
        const auto& a = fanResult.rows[i]; const auto& b = fanUncached.rows[i];
        sameRows &= a.start == b.start && a.end == b.end && a.status == b.status && a.reason == b.reason
            && a.head == b.head && a.judgementKind == b.judgementKind && a.judgementQuality == b.judgementQuality
            && a.fullReplayVerified == b.fullReplayVerified && a.confidence == b.confidence
            && a.judgementSecond == b.judgementSecond && a.judgementMargin == b.judgementMargin;
    }
    expect(hits > 0 && counted && fanUncached.workUsed - fanResult.workUsed == reused
        && sameRows && apply(layout, fanResult) == apply(layout, fanUncached)
        && fixed(apply(layout, fanUncached)).evaluation.complete,
        "exact window attempt reuse saves only actual work and preserves caller-modified rows, patches and cold proof");
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    bool stopped = false, started = false;
    try {
        miacode::task::CancellationScope scope(cancelled);
        inferBasicHandOrder(layout, SimaiParser::validateSyntax(layout), 0, layout.size(), context, {},
            [&](const auto& update) {
                if (update.phase == HandOrderProgress::Decisions && update.completed > 0) {
                    started = true; cancelled->store(true);
                }
            });
    } catch (const miacode::task::Cancelled&) { stopped = true; }
    expect(started && stopped, "a duplicate-search workload still cooperatively cancels after real decision progress");
    return ok ? 0 : 1;
}
