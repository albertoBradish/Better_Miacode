#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>
#include <cmath>

using namespace miacode::hand_order;

namespace {
QVector<ChartInputAction> inputsFor(const SimaiParseResult& parsed, double shift)
{
    QVector<ChartInputAction> result;
    for (const auto& marker : parsed.noteMarkers) {
        if (marker.type != "slide" || marker.hasHeadStar) {
            ChartInputAction input;
            input.button = true; input.lane = marker.lane;
            auto& action = input.contact;
            action.kind = "button"; action.tokenStart = marker.parseOrder;
            action.hand = marker.handOrderHead == HandOrderHand::Left ? 0 : 1;
            action.start = marker.second; action.end = marker.type == "hold" ? marker.endSecond : marker.second + 0.025;
            const double angle = (marker.lane - 0.5) * 3.14159265358979323846 / 4.0;
            action.points = {{std::sin(angle), -std::cos(angle)}}; action.times = {action.start};
            result.append(input);
        }
        if (marker.type != "slide") continue;
        ChartInputAction input;
        auto& action = input.contact;
        action.kind = "slide"; action.tokenStart = marker.parseOrder;
        action.hand = !marker.handOrderTrackStops.isEmpty() && marker.handOrderTrackStops.first().hand == HandOrderHand::Left ? 0 : 1;
        action.start = action.motionStart = marker.slideTraceSecond + shift;
        action.end = action.motionEnd = marker.endSecond + shift;
        for (int segment = 0; segment < marker.slideSegmentPoints.size(); ++segment) {
            const auto& points = marker.slideSegmentPoints.at(segment);
            for (int point = 0; point < points.size(); ++point) {
                const double time = marker.slideSegmentShootSeconds.at(segment)
                    + marker.slideSegmentDurations.at(segment) * point / (points.size() - 1) + shift;
                if (!action.times.isEmpty() && time <= action.times.last() + 1e-8) continue;
                action.points.append(points.at(point) / 240.0); action.times.append(time);
            }
        }
        result.append(input);
    }
    return result;
}

BasicResult infer(const QString& text)
{
    BasicParameters parameters;
    parameters.counterfactualBudget = 0;
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), 0, text.size(), parameters);
}
bool verified(const BasicResult& result)
{
    return result.error.isEmpty() && !result.rows.isEmpty()
        && std::all_of(result.rows.cbegin(), result.rows.cend(), [](const auto& row) { return row.fullReplayVerified; });
}
QString timingChart(int rotation)
{
    const auto lane = [&](int value) { return QString::number((value - 1 + rotation) % 8 + 1); };
    const QString left = rotation == 0 ? QStringLiteral("l") : QStringLiteral("r");
    const QString right = rotation == 0 ? QStringLiteral("r") : QStringLiteral("l");
    const auto tap = [&](int value, const QString& hand) { return lane(value) + '~' + hand; };
    QString text = "(156){32}" + tap(8, left) + '/' + lane(4) + '-' + lane(7)
        + "[8:1]~[head=" + right + ";track=0:" + left + "],";
    for (int value : {7, 6, 5}) text += tap(value, left) + ',';
    text += "{8}" + tap(4, left) + ",{32}";
    for (int value : {1, 2, 3, 4}) text += tap(value, right) + ',';
    return text;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTextStream out(stdout);
    if (application.arguments().contains(QStringLiteral("--probe"))) {
    const QString text = QTextStream(stdin).readAll();
    const auto parsed = SimaiParser::validateSyntax(text);
    for (double shift : {-0.05, -3.0 / 180.0, -2.0 / 180.0, -1.0 / 180.0, 0.0,
            1.0 / 180.0, 3.0 / 180.0, 0.05, 0.1, 0.15}) {
        const auto replay = replayHandOrderChart(parsed.noteMarkers, inputsFor(parsed, shift));
        out << "shift=" << shift << " valid=" << replay.valid << " target=" << replay.meetsTarget << " error=" << replay.error << '\n';
        for (const auto& judgement : replay.judgements) if (!judgement.meetsTarget) {
            out << "  order=" << judgement.parseOrder << " kind=" << judgement.kind << " reason=" << judgement.reason
                << " at=" << judgement.second << " delta=" << judgement.hitDelta << " channel=" << judgement.inputChannel << '\n';
            for (const auto& cause : judgement.causes)
                out << "    cause=" << cause.kind << " source=" << cause.tokenStart << " pad=" << cause.pad << " at=" << cause.second << '\n';
        }
    }
    return 0;
    }
    bool ok = true;
    const auto expect = [&](bool pass, const char* description) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << description << '\n';
        ok &= pass;
    };
    for (int rotation : {0, 4}) {
        const QString text = timingChart(rotation);
        const auto parsed = SimaiParser::validateSyntax(text);
        const auto nominal = replayHandOrderChart(parsed.noteMarkers, inputsFor(parsed, 0.0));
        bool stolen = false;
        for (const auto& judgement : nominal.judgements)
            if (judgement.parseOrder == 9 && judgement.reason == "outside_target_window"
                && judgement.hitDelta < -0.12 && judgement.hitDelta > -0.15)
                for (const auto& cause : judgement.causes)
                    stolen |= cause.kind == "slide" && cause.tokenStart == 1;
        expect(nominal.valid && !nominal.meetsTarget && stolen,
            "nominal slide entry consumes the later tap about 140 ms early under the unchanged judge");
        const auto oneTick = replayHandOrderChart(parsed.noteMarkers, inputsFor(parsed, -1.0 / FullReplayProfile::ticksPerSecond));
        const auto twoTicks = replayHandOrderChart(parsed.noteMarkers, inputsFor(parsed, -2.0 / FullReplayProfile::ticksPerSecond));
        expect(!oneTick.meetsTarget && twoTicks.valid && twoTicks.meetsTarget,
            "two ticks of actual gesture advance avoid premature consumption without shifting any native marker");
        const auto solved = infer(text);
        expect(verified(solved) && solved.patches.isEmpty() && !solved.workLimited,
            "inference keeps the original locked hands and validates the timing-adjusted physical candidate");
        expect(std::any_of(solved.rows.cbegin(), solved.rows.cend(), [](const auto& row) {
            return row.conditions.contains("adjusted_slide_input_timing");
        }), "timing adjustment is exposed as an explicit candidate condition");
        const QString unannotated = stripAnnotations(text);
        const auto automatic = infer(unannotated);
        QString annotated = unannotated;
        for (int i = automatic.patches.size() - 1; i >= 0; --i) {
            const auto& patch = automatic.patches.at(i);
            annotated.replace(patch.start, patch.end - patch.start, patch.text);
        }
        expect(verified(automatic) && !automatic.workLimited && stripAnnotations(annotated) == unannotated
            && std::any_of(automatic.rows.cbegin(), automatic.rows.cend(), [](const auto& row) {
                return row.conditions.contains("adjusted_slide_input_timing");
            }), "an unannotated trajectory is solved with its future native tap consumer before committing input timing");
    }
    const QString plain = "(120){4}1-5[4:1]~r,";
    const auto ordinary = infer(plain);
    expect(verified(ordinary) && std::none_of(ordinary.rows.cbegin(), ordinary.rows.cend(), [](const auto& row) {
        return row.conditions.contains("adjusted_slide_input_timing");
    }), "a valid nominal route does not need a timing expansion");

    const QString chain = "(156){8}8-5[8:1]~l,4~r,8-4[8:1]~l,3~r,8-3[8:1]~l,2~r,"
        "{4}8~l/1-6[8:1]~[head=r;track=0:r],{8}1~r/5~l,4~l,3~r,4~r,"
        "5-1[8:1]~l,4~r,3~r/5~l,2~r,";
    const auto inherited = infer(chain);
    expect(verified(inherited) && inherited.patches.isEmpty() && !inherited.workLimited,
        "rolling windows preserve all generations of actual input causes for inherited star heads");
    const auto chainParsed = SimaiParser::validateSyntax(chain);
    auto inheritedInputs = inputsFor(chainParsed, 0.0);
    inheritedInputs.removeIf([](const auto& input) {
        return input.button && (input.contact.tokenStart == 2 || input.contact.tokenStart == 4);
    });
    const auto carried = replayHandOrderChart(chainParsed.noteMarkers, inheritedInputs);
    expect(carried.valid && carried.meetsTarget,
        "two successive star heads can genuinely be supplied by their preceding slide inputs");
    inheritedInputs.removeIf([](const auto& input) { return input.contact.tokenStart == 0; });
    auto incompleteMarkers = chainParsed.noteMarkers;
    incompleteMarkers.removeIf([](const auto& marker) { return marker.parseOrder == 0; });
    const auto missingAncestor = replayHandOrderChart(incompleteMarkers, inheritedInputs);
    expect(!missingAncestor.meetsTarget && std::any_of(missingAncestor.judgements.cbegin(), missingAncestor.judgements.cend(), [](const auto& judgement) {
        return judgement.parseOrder == 2 && !judgement.headHighQuality;
    }), "dropping a grandparent input really invalidates the retained historical star under the same judge");
    QString ordinaryText = "(120){16}";
    for (int i = 0; i < 200; ++i) ordinaryText += i % 2 ? QStringLiteral("6,") : QStringLiteral("2,");
    const auto boundedOrdinary = infer(ordinaryText);
    expect(verified(boundedOrdinary) && boundedOrdinary.rows.size() == 200
        && boundedOrdinary.windowCount >= 190 && boundedOrdinary.maxWindowTasks <= 12,
        "two hundred ordinary notes remain small rolling windows without recursive trajectory lookahead");
    return ok ? 0 : 1;
}
