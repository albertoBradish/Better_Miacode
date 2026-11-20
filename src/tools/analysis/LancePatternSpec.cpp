#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include <QCoreApplication>
#include <QHash>
#include <QTextStream>
#include <algorithm>

using namespace miacode::hand_order;

namespace {
BasicResult solve(const QString& text)
{
    BasicParameters parameters;
    parameters.counterfactualBudget = 0;
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), 0, text.size(), parameters);
}
QString apply(const QString& text, const BasicResult& result)
{
    QString output = text;
    for (int i = result.patches.size() - 1; i >= 0; --i) {
        const auto& patch = result.patches.at(i);
        output.replace(patch.start, patch.end - patch.start, patch.text);
    }
    return output;
}
bool verified(const BasicResult& result)
{
    return result.error.isEmpty() && !result.rows.isEmpty()
        && std::all_of(result.rows.cbegin(), result.rows.cend(), [](const auto& row) {
            return row.fullReplayVerified;
        });
}
int rotate(int lane, int offset) { return (lane - 1 + offset + 80) % 8 + 1; }
QString winding(int lane, int direction)
{
    QString text = QStringLiteral("(156){16}");
    for (int block = 0; block < 3; ++block) {
        text += QStringLiteral("%1/%2,").arg(lane).arg(rotate(lane, 4));
        for (int step = 1; step <= 3; ++step) text += QString::number(rotate(lane, direction * step)) + ',';
    }
    return text + QStringLiteral("%1b/%2b,").arg(lane).arg(rotate(lane, 4));
}
bool windingHands(const QString& text)
{
    const auto parsed = SimaiParser::validateSyntax(text);
    if (!parsed.errors.isEmpty() || !parsed.annotationErrors.isEmpty() || parsed.noteMarkers.size() != 17) return false;
    QHash<int, HandOrderHand> hands;
    for (const auto& marker : parsed.noteMarkers) hands.insert(marker.parseOrder, marker.handOrderHead);
    for (int block = 0; block < 3; ++block) {
        const int first = 5 * block;
        const auto flow = hands.value(first), other = hands.value(first + 1);
        if ((flow != HandOrderHand::Left && flow != HandOrderHand::Right)
            || other == flow || other == HandOrderHand::Unspecified) return false;
        for (int step = 2; step <= 4; ++step) if (hands.value(first + step) != flow) return false;
        if (hands.value(first + 6) != flow) return false;
    }
    return true;
}
bool oneHand(const BasicResult& result)
{
    if (!verified(result)) return false;
    const auto hand = result.rows.first().head;
    return std::all_of(result.rows.cbegin(), result.rows.cend(), [&](const auto& row) { return row.head == hand; });
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* description, const QString& text = {}, const BasicResult& result = {}) {
        QTextStream out(pass ? stdout : stderr);
        out << (pass ? "PASS: " : "FAIL: ") << description << '\n';
        ok &= pass;
        if (!pass) {
            out << apply(text, result) << "\nerror=" << result.error << " work=" << result.workUsed << '\n';
            for (const auto& row : result.rows)
                out << row.start << ':' << row.status << ':' << row.reason << " replay=" << row.fullReplayVerified << '\n';
        }
    };
    for (const auto& variant : {QPair<int, int>{7, -1}, {2, 1}, {8, -1}, {1, 1}}) {
        const QString text = winding(variant.first, variant.second);
        const auto result = solve(text);
        expect(verified(result) && windingHands(apply(text, result)),
            "anchored triple winding follows one hand to the next anchor under rotation and reflection", text, result);
        const auto layout = miacode::chart_transform::basicHandOrderLayoutInSelection(text, 0, text.size());
        expect(layout.windingLinks.size() == 12 && layout.windingEachHeads.size() == 8
            && windingHands(layout.edit.replacement),
            "layout exposes complete winding topology and coherent fallback hands", text, result);
    }
    const QString locked = "(156){16}7~l/3~r,6~l,5~l,4~l,7~r/3~l,6~r,5~r,4~r,"
        "7~l/3~r,6~l,5~l,4~l,7b~r/3b~l,";
    const auto preserved = solve(locked);
    expect(verified(preserved) && preserved.patches.isEmpty(),
        "intentional winding Each crosses are preserved rather than repaired by isolated-side rules", locked, preserved);
    const QString isolated = "(190){8}8~r/2~l,7~r/3~l,";
    const auto corrected = solve(isolated);
    expect(verified(corrected) && apply(isolated, corrected) == "(190){8}8~l/2~r,7~l/3~r,",
        "isolated unmistakably crossed Each labels retain the existing correction", isolated, corrected);
    const QString broken = "(156){16}7/3,6,5,4,7/3,6,4,5,7/3,";
    const auto noWinding = miacode::chart_transform::basicHandOrderLayoutInSelection(broken, 0, broken.size());
    expect(noWinding.windingLinks.isEmpty() && noWinding.windingEachHeads.isEmpty(),
        "a broken or unrepeated anchored path does not disable ordinary Each protection");

    for (const QString text : {QStringLiteral("(156){16}2,3,4,5,6,7,8,1,"),
            QStringLiteral("(156){16}7,6,5,4,3,2,1,8,")}) {
        const auto result = solve(text);
        expect(oneHand(result), "a complete eight-key circle stays on one hand even at sixteenth notes", text, result);
    }
    for (const QString text : {QStringLiteral("(156){32}1,2,3,4,"), QStringLiteral("(156){32}8,7,6,5,")}) {
        const auto result = solve(text);
        expect(oneHand(result), "a four-key sweep on one side retains strong continuity", text, result);
    }
    const QString crossing = "(156){32}2,3,4,5,";
    const auto split = solve(crossing);
    expect(verified(split) && split.rows.size() == 4 && split.rows[0].head == split.rows[1].head
        && split.rows[2].head == split.rows[3].head && split.rows[0].head != split.rows[2].head,
        "a short cross-side sweep can split into two contiguous two-key chunks", crossing, split);
    const QString boundary = "(156){16}3,2,3,4,5,6,7,8,1,";
    QHashSeed::setDeterministicGlobalSeed();
    const auto deterministic = solve(boundary);
    const auto expected = apply(boundary, deterministic);
    bool reproducible = verified(deterministic);
    for (int trial = 0; trial < 8; ++trial) {
        QHashSeed::resetRandomGlobalSeed();
        const auto repeated = solve(boundary);
        reproducible &= verified(repeated) && apply(boundary, repeated) == expected
            && repeated.workUsed == deterministic.workUsed
            && repeated.maxWindowTasks == deterministic.maxWindowTasks;
    }
    expect(reproducible, "hash seed cannot change a lookahead boundary beside a multi-note hard group");
    return ok ? 0 : 1;
}
