#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <cmath>

using namespace miacode::hand_order;
namespace {
QString apply(const QString& text, const BasicResult& result)
{
    QString output = text;
    for (auto patch = result.patches.crbegin(); patch != result.patches.crend(); ++patch)
        output.replace(patch->start, patch->end - patch->start, patch->text);
    return output;
}
QString layout(const QString& raw)
{
    return miacode::chart_transform::basicHandOrderLayoutInSelection(raw, 0, 0).edit.replacement;
}
BasicAssignmentEvaluation evaluate(const QString& text)
{
    BasicParameters p; p.evaluateCompleteAssignment = true; p.counterfactualBudget = 0;
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), 0, text.size(), p).evaluation;
}
QJsonArray annotations(const QString& text)
{
    QJsonArray result;
    for (const auto& marker : SimaiParser::validateSyntax(text).noteMarkers) {
        QJsonArray tracks, sectors;
        for (const auto& stop : marker.handOrderTrackStops) tracks.append(QJsonArray{stop.progress, int(stop.hand)});
        for (const auto& sector : marker.handOrderWifiSectorStops) {
            QJsonArray sequence;
            for (const auto& stop : sector) sequence.append(QJsonArray{stop.progress, int(stop.hand)});
            sectors.append(sequence);
        }
        result.append(QJsonArray{int(marker.handOrderHead), tracks, int(marker.handOrderWifiMode), sectors});
    }
    return result;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& message) {
        ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message << '\n';
    };
    const auto check = [&](const QString& name, const QString& raw) {
        const QString source = layout(raw);
        BasicParameters p; p.overwriteExisting = true;
        const auto result = inferBasicHandOrder(source, SimaiParser::validateSyntax(source), 0, source.size(), p);
        const auto before = evaluate(source), after = evaluate(apply(source, result));
        expect(before.complete && after.complete && after.cost <= before.cost + 1e-6
            && after.risk <= before.risk + 1e-8 && after.ergonomicBreaks <= before.ergonomicBreaks,
            name + QStringLiteral(" preserves an independently reconstructed good baseline (%1 -> %2)").arg(before.cost).arg(after.cost));
        expect(!result.contextGuardOutcome.isEmpty() && result.contextBaselineEvaluation.complete
            && result.workUsed == result.contextBaselineWorkUsed + result.contextSearchWorkUsed + result.contextSerializedWorkUsed
            && result.workUsed <= p.maxWorkUnits && result.contextBaselineWorkUsed <= p.maxWorkUnits / 8
            && result.contextSerializedWorkUsed <= p.maxWorkUnits / 8,
            name + " accounts for reference/search/serialized verification inside one budget");
        expect(stripAnnotations(apply(source, result)) == stripAnnotations(source), name + " preserves native input");
        return apply(source, result);
    };
    check("T38_90", "(90){8},1,,5,,2,,6,,3,");
    check("T39_270", "(270){16}4,4,,,4,4,,,4,4,,,");
    check("S31_150", "(150){4}1>3[4:1],3>5[4:1],5>7[4:1],7>1[4:1],,,");
    check("N03_150", "(150){4}2-6[4:1]~[head=r;track=0:l],6-2[4:1]~[head=l;track=0:r],3-7[4:1]~[head=r;track=0:l],,,");
    const QVector<QPair<QString, QString>> equalBodies{
        {"{8}2-6[2:1],,,4,5,4,,,,", "{16}2-6[0.4##0.8],,,4,5,4,,,,"},
        {"{8}1-5[2:1],,1,1,1,,,,", "{16}1-5[0.4##0.8],,1,1,1,,,,"},
        {"{8}7h[1:1],2,3,2,3,2,", "{16}7h[150#1:1],2,3,2,3,2,"},
        {"{8}6h[2:1],1,2,1,", "{16}6h[150#2:1],1,2,1,"},
        {"{8}1-5[2:1]~[head=r;track=0:l],2~r,3~r,2~r,,,,", "{16}1-5[0.4##0.8]~[head=r;track=0:l],2~r,3~r,2~r,,,,"},
        {"{8}1,5,{64}2,3,4,5,6,7,{8}3,7,", "{16}1,5,{128}2,3,4,5,6,7,{16}3,7,"}
    };
    const QStringList ids{"S33", "S36", "H01", "H02", "A09", "G25"};
    for (int i = 0; i < equalBodies.size(); ++i) {
        QString a = layout("(150)" + equalBodies[i].first), b = layout("(75)" + equalBodies[i].second);
        for (int stage = 1; stage <= 3; ++stage) {
            const auto x = evaluate(a), y = evaluate(b);
            const bool completeness = x.complete == y.complete && (stage == 1 || x.complete);
            const bool scores = !x.complete || (std::abs(x.cost - y.cost) < 1e-6
                && std::abs(x.risk - y.risk) < 1e-6 && x.ergonomicBreaks == y.ergonomicBreaks);
            const auto scoreText = x.complete && y.complete
                ? QStringLiteral("%1 / %2").arg(x.cost).arg(y.cost) : QStringLiteral("unverified / unverified");
            expect(annotations(a) == annotations(b) && completeness && scores,
                ids[i] + QStringLiteral(" equal seconds preserve stage %1 annotations, completeness and available score (%2)").arg(stage).arg(scoreText));
            if (stage == 1) {
                BasicParameters p; p.overwriteExisting = true;
                a = apply(a, inferBasicHandOrder(a, SimaiParser::validateSyntax(a), 0, a.size(), p));
                b = apply(b, inferBasicHandOrder(b, SimaiParser::validateSyntax(b), 0, b.size(), p));
            } else if (stage == 2) {
                a = apply(a, optimizeHandOrderPatterns(a, SimaiParser::validateSyntax(a), 0, a.size()));
                b = apply(b, optimizeHandOrderPatterns(b, SimaiParser::validateSyntax(b), 0, b.size()));
            }
        }
    }
    BasicParameters p; p.overwriteExisting = true;
    const QString unlabelled = "(150){16}1,5,1,5,";
    const auto raw = inferBasicHandOrder(unlabelled, SimaiParser::validateSyntax(unlabelled), 0, unlabelled.size(), p);
    expect(raw.contextGuardOutcome.isEmpty() && !raw.patches.isEmpty(), "unlabelled input is still inferred without inventing a fixed baseline");
    const QString selected = layout("(150){16}1,5,1,5,3,7,");
    const int begin = selected.indexOf("1~"), end = selected.indexOf(',') + 1;
    const auto local = inferBasicHandOrder(selected, SimaiParser::validateSyntax(selected), begin, end, p);
    bool scoped = local.contextGuardOutcome.isEmpty();
    for (const auto& patch : local.patches) scoped &= patch.start >= begin && patch.end <= end;
    expect(scoped, "partial selections retain their own context and patch boundaries");
    p.overwriteExisting = false;
    const auto locked = inferBasicHandOrder(selected, SimaiParser::validateSyntax(selected), 0, selected.size(), p);
    expect(locked.contextGuardOutcome.isEmpty() && locked.patches.isEmpty(), "explicit manual-label protection is unchanged");
    p.overwriteExisting = true; p.maxWorkUnits = 100000;
    const QString impossible = "(150){4}1~l/3~l/5~l,";
    const auto uncertain = inferBasicHandOrder(impossible, SimaiParser::validateSyntax(impossible), 0, impossible.size(), p);
    expect(uncertain.contextGuardOutcome == "baseline_unverified" && uncertain.workUsed <= p.maxWorkUnits,
        "an unverifiable baseline does not silently block repair search or multiply its work budget");
    const QString crossed = "(150){8}8~r/2~l,7~r/3~l,";
    const auto invalidFixed = evaluate(crossed);
    expect(!invalidFixed.complete, "a fixed audit cannot silently repair crossed original labels and call them verified");
    const QString slowA = layout("(80){16}3,4,3,4,3,");
    const QString slowB = layout("(40){32}3,4,3,4,3,");
    const auto slowScoreA = evaluate(slowA), slowScoreB = evaluate(slowB);
    expect(annotations(slowA) == annotations(slowB) && slowScoreA.complete && slowScoreB.complete
        && std::abs(slowScoreA.cost - slowScoreB.cost) < 1e-6,
        "80 BPM sixteenths and 40 BPM finer-grid inputs share the same 187.5ms alternation prior");
    return ok ? 0 : 1;
}
