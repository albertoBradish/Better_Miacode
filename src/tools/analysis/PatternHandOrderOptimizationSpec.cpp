#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/TouchPadAdjacency.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <cmath>

using namespace miacode::hand_order;
namespace {
QString apply(const QString& text, const BasicResult& result)
{
    QString updated = text;
    for (auto it = result.patches.crbegin(); it != result.patches.crend(); ++it)
        updated.replace(it->start, it->end - it->start, it->text);
    return updated;
}
BasicResult optimize(const QString& text, int begin = 0, int end = 0, PatternOptimizationParameters options = {})
{
    BasicParameters parameters; parameters.counterfactualBudget = 0;
    return optimizeHandOrderPatterns(text, SimaiParser::validateSyntax(text), begin, end, parameters, options);
}
BasicResult evaluate(const QString& text, int beam = 64)
{
    BasicParameters parameters; parameters.counterfactualBudget = 0;
    parameters.evaluateCompleteAssignment = true; parameters.beamWidth = beam;
    return inferBasicHandOrder(text, SimaiParser::validateSyntax(text), 0, text.size(), parameters);
}
QString read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}
QString chartFromDocument(const QString& source)
{
    const auto start = source.indexOf("&inote_5=");
    return start < 0 ? QString{} : source.mid(start + 9);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }
bool sameTimes(const SimaiParseResult& a, const SimaiParseResult& b)
{
    if (a.noteMarkers.size() != b.noteMarkers.size()) return false;
    for (int i = 0; i < a.noteMarkers.size(); ++i) {
        const auto& x = a.noteMarkers.at(i); const auto& y = b.noteMarkers.at(i);
        if (x.type != y.type || x.lane != y.lane || x.endLane != y.endLane || x.touchPad != y.touchPad
            || !near(x.second, y.second) || !near(x.endSecond, y.endSecond)
            || !near(x.slideTraceSecond, y.slideTraceSecond)
            || x.slideSegmentKeys != y.slideSegmentKeys) return false;
    }
    return true;
}
// These assertions test actual parsed timing and geometry, not fixture names.
// Other IDs retain an explicit structure-review category in the coverage map.
QString semanticFailure(const QString& id, const SimaiParseResult& parsed)
{
    const auto& notes = parsed.noteMarkers;
    QVector<TimelineNoteMarker> slides, holds, taps, touches;
    for (const auto& note : notes) {
        if (note.type == "slide" || note.type == "wifi") slides.append(note);
        if (note.type == "hold" || note.type == "touch_hold") holds.append(note);
        if (note.type == "tap") taps.append(note);
        if (note.type == "touch") touches.append(note);
    }
    const auto overlap = [](const auto& a, const auto& b) {
        return qMax(a.second, b.slideTraceSecond) < qMin(a.endSecond, b.endSecond) - 1e-8;
    };
    if (id == "S07" || id == "S08" || id == "S31" || id == "N19") {
        if (slides.size() < 2) return "missing chain";
        for (int i = 1; i < slides.size(); ++i)
            if (slides[i - 1].endLane != slides[i].lane || !near(slides[i - 1].endSecond, slides[i].slideTraceSecond))
                return "one-stroke endpoint/time mismatch";
    }
    if (id == "S01") {
        bool found = false;
        for (const auto& slide : slides) for (const auto& tap : taps)
            found |= tap.lane == slide.lane && near(tap.second, slide.slideTraceSecond);
        if (!found) return "no same-origin tap at trace start";
    }
    if (id == "S02") {
        bool waiting = false, start = false;
        for (const auto& slide : slides) for (const auto& tap : taps) if (tap.lane == slide.lane) {
            waiting |= tap.second > slide.second && tap.second < slide.slideTraceSecond;
            start |= near(tap.second, slide.slideTraceSecond);
        }
        if (!waiting || !start) return "missing wait-phase tap / trace-start tap";
    }
    if (id == "S04" || id == "S05")
        for (int i = 1; i < slides.size(); ++i)
            if (!near(slides[i].second, slides[i - 1].slideTraceSecond)) return "head/trace phase mismatch";
    if (id == "S09" && (slides.isEmpty() || slides.first().slideSegmentKeys.size() < 2)) return "not a native connected slide";
    if (id == "S13" || id == "S15") {
        if (slides.size() != 2) return "expected paired slides";
        if (id == "S13" && near(slides[0].slideTraceSecond, slides[1].slideTraceSecond)) return "waits are equal";
        if (id == "S15" && near(slides[0].endSecond - slides[0].slideTraceSecond,
                slides[1].endSecond - slides[1].slideTraceSecond)) return "durations are equal";
    }
    if (id == "S24" && (slides.size() != 1 || slides.first().type != "wifi"
            || slides.first().wifiLanePoints.size() != 3)) return "missing three native Wifi lanes";
    if (id == "S32" || id == "H03" || id == "U11" || id == "N19") {
        bool found = false;
        for (const auto& hold : holds) for (const auto& slide : slides) found |= overlap(hold, slide);
        if (!found) return "hold does not occupy a slide-motion window";
    }
    if (id == "S33" || id == "S34" || id == "S36" || id == "S37") {
        bool found = false;
        const auto points = id == "S34" ? touches : taps;
        for (const auto& point : points) for (const auto& slide : slides) {
            if (id == "S37") found |= point.second > slide.second && point.second < slide.slideTraceSecond;
            else found |= point.second >= slide.slideTraceSecond && point.second < slide.endSecond
                && (id != "S36" || point.lane == slide.lane);
        }
        if (!found) return "point is outside required slide phase";
    }
    if (id == "T26" || id == "T27" || id == "T28" || id == "T30" || id == "T33") {
        int direction = 0;
        for (int i = 1; i < taps.size(); ++i) {
            const int step = (taps[i].lane - taps[i - 1].lane + 8) % 8;
            const int next = step == 1 ? 1 : step == 7 ? -1 : 0;
            if (!next || (direction && next != direction)) return "not a directed adjacent key run";
            direction = next;
        }
    }
    if (id == "U02" || id == "U03")
        for (int i = 1; i < touches.size(); ++i)
            if (!miacode::chart::touchPadsCanSweep(touches[i - 1].touchPad, touches[i].touchPad)) return "nonadjacent Touch sweep";
    if (id == "G09") {
        bool changed = false;
        for (int i = 1; i < notes.size(); ++i) changed |= !near(notes[i].tempoBpm, notes[0].tempoBpm);
        if (!changed) return "missing tempo boundary";
    }
    return {};
}
int corpus(const QString& manifestPath)
{
    const QDir root(QFileInfo(manifestPath).absolutePath());
    const auto manifest = QJsonDocument::fromJson(read(manifestPath).toUtf8()).object();
    QJsonArray details;
    int count = 0, failures = 0, pending = 0;
    const QString overview = chartFromDocument(read(root.filePath("overview/maidata.txt")));
    const auto overviewParse = SimaiParser::validateSyntax(overview);
    if (!overviewParse.ok || !overviewParse.annotationErrors.isEmpty()) {
        ++failures; QTextStream(stderr) << "FAIL overview parse\n";
    }
    for (const auto& entryValue : manifest.value("entries").toArray()) {
        const auto entry = entryValue.toObject(); const QString id = entry.value("ID").toString();
        const auto variants = entry.value("variants").toArray();
        if (variants.isEmpty()) { ++pending; continue; }
        SimaiParseResult reference;
        for (const auto& variantValue : variants) {
            ++count;
            const auto variant = variantValue.toObject();
            const auto name = variant.value("name").toString();
            const QString chart = chartFromDocument(read(root.filePath(variant.value("path").toString())));
            const auto parsed = SimaiParser::validateSyntax(chart);
            QString failure;
            if (!parsed.ok || parsed.noteMarkers.isEmpty() || !parsed.annotationErrors.isEmpty()) {
                failure = "native/annotation parse failed";
                for (const auto& error : parsed.errors) failure += "; " + error.message;
                for (const auto& error : parsed.annotationErrors) failure += "; " + error.message;
            } else if (name != "tempo_change") failure = semanticFailure(id, parsed);
            if (name == "bpm_150") reference = parsed;
            if (name == "equal_seconds_75" && !sameTimes(reference, parsed)) failure += "; equal-second variant differs";
            details.append(QJsonObject{{"id", id}, {"variant", name}, {"markers", parsed.noteMarkers.size()},
                {"passed", failure.isEmpty()}, {"failure", failure}});
            if (!failure.isEmpty()) {
                ++failures; QTextStream(stderr) << "FAIL " << id << '/' << name << ": " << failure << '\n';
            }
        }
        const auto map = entry.value("overview").toObject();
        const int begin = map.value("start_utf16").toInt(), end = map.value("end_utf16").toInt();
        const QString isolated = chartFromDocument(read(root.filePath(variants.at(1).toObject().value("path").toString())));
        if (begin < 0 || end <= begin || !isolated.startsWith(overview.mid(begin, end - begin))) {
            ++failures; QTextStream(stderr) << "FAIL " << id << " overview source map\n";
        }
    }
    QJsonArray semanticIds;
    for (const auto& id : {"S07", "S08", "S31", "N19", "S01", "S02", "S04", "S05", "S09", "S13", "S15", "S24",
            "S32", "H03", "U11", "S33", "S34", "S36", "S37", "T26", "T27", "T28", "T30", "T33", "U02", "U03", "G09"})
        semanticIds.append(QString::fromLatin1(id));
    double lastSecond = 0;
    for (const auto& marker : overviewParse.noteMarkers) lastSecond = qMax(lastSecond, qMax(marker.second, marker.endSecond));
    QJsonObject report{{"fixtures", count}, {"pending_definitions", pending}, {"failures", failures},
        {"overview_markers", overviewParse.noteMarkers.size()}, {"overview_last_note_second", lastSecond},
        {"semantic_assertion_ids", semanticIds},
        {"details", details}, {"scope", "native syntax, selected explicit structure assertions, all equal-second pairs and overview offsets; not named-strategy or cabinet validation"}};
    QFile output(root.filePath("validation.json"));
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(report).toJson()) < 0) return 2;
    QTextStream(stdout) << "CORPUS fixtures=" << count << " pending=" << pending << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--corpus") return corpus(QString::fromLocal8Bit(argv[2]));
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--probe") {
        const QString text = QTextStream(stdin).readAll();
        const auto result = optimize(text);
        QTextStream(stdout) << "outcome=" << result.patternOutcome << " baseline=" << result.originalEvaluation.complete
            << " before=" << result.originalEvaluation.cost << " after=" << result.evaluation.cost
            << " candidates=" << result.patternCandidates << " accepted=" << result.patternAccepted
            << " work=" << result.workUsed << '\n' << apply(text, result) << '\n';
        return result.error.isEmpty() ? 0 : 1;
    }
    bool ok = true;
    const auto expect = [&](bool pass, const char* message, const BasicResult& result = {}) {
        ok &= pass;
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << message
            << " outcome=" << result.patternOutcome << " before=" << result.originalEvaluation.cost
            << " after=" << result.evaluation.cost << " segments=" << result.patternSegments
            << " candidates=" << result.patternCandidates << " accepted=" << result.patternAccepted << '\n';
    };
    const QString baseline = "(150){16}1~l,5~r,1~l,5~r,1~l,5~r,1~l,5~r,";
    const auto improved = optimize(baseline);
    expect(improved.patternAccepted > 0 && !improved.patches.isEmpty()
        && !improved.evaluation.complete && improved.workUsed == 0,
        "named trill defaults replace opposite labels without a whole-chart baseline search", improved);
    const QString updated = apply(baseline, improved);
    expect(stripAnnotations(updated) == stripAnnotations(baseline)
        && sameTimes(SimaiParser::validateSyntax(updated), SimaiParser::validateSyntax(baseline)),
        "optimization preserves native tokens, timing and geometry", improved);
    const auto repeat = optimize(updated);
    expect(repeat.patches.isEmpty(), "accepted fixture reaches a stable second run", repeat);
    const auto fixedA = evaluate(updated, 8), fixedB = evaluate(updated, 64);
    expect(fixedA.evaluation.complete && fixedB.evaluation.complete
        && near(fixedA.evaluation.cost, fixedB.evaluation.cost) && fixedA.evaluation.cost > 0,
        "fixed labels are charged equally across search beam sizes", fixedA);
    const QString equivalent = "(75){32}1~r,5~l,1~r,5~l,1~r,5~l,1~r,5~l,";
    const auto equal = evaluate(equivalent);
    expect(equal.evaluation.complete && near(equal.evaluation.cost, fixedA.evaluation.cost),
        "equal absolute time at different BPM/subdivision has equal complete score", equal);
    PatternOptimizationParameters locked; locked.preserveExisting = true;
    const auto preserved = optimize(baseline, 0, 0, locked);
    expect(preserved.patches.isEmpty() && preserved.patternOutcome == "locked", "explicit existing-label lock is honored", preserved);
    const auto missing = optimize("(150){16}1,5,1,5,");
    expect(!missing.patches.isEmpty() && missing.patternOutcome == "recognized", "unlabelled charts can receive named structural defaults", missing);
    const auto either = optimize("(150){16}1~m,5~l,1~r,5~l,");
    expect(!either.patches.isEmpty() && either.patternOutcome == "recognized", "Either labels do not block structural recommendations", either);
    const auto three = optimize("(150){16}1~r,3~r,6~l,");
    expect(three.patternSegments == 0 && three.patches.isEmpty(),
        "three unrelated points do not become a speculative triangle", three);
    const QString selected = baseline + "{1},,,4~r,";
    const auto scoped = optimize(selected, baseline.indexOf("1~l"), baseline.size());
    const QString scopedText = apply(selected, scoped);
    expect(scoped.patternAccepted > 0 && scopedText.endsWith("{1},,,4~r,")
        && std::all_of(scoped.patches.begin(), scoped.patches.end(), [&](const auto& patch) {
            return patch.start >= baseline.indexOf("1~l") && patch.end <= baseline.size();
        }), "selection changes only complete selected tokens", scoped);
    const QString nested = "(150){16}6h[1:1]~l,1~r,2~r,3~r,4~r,";
    const auto nestedResult = optimize(nested);
    expect(nestedResult.patternSegments >= 1, "point-run candidates survive adjacent Hold coupling", nestedResult);
    const QString variable = "(120){8}1-5[4:1]~[head=r;track=0:r],(240)6~l,,,,,,,,";
    const auto variableResult = optimize(variable);
    expect(stripAnnotations(apply(variable, variableResult)) == stripAnnotations(variable)
        && sameTimes(SimaiParser::validateSyntax(variable), SimaiParser::validateSyntax(apply(variable, variableResult))),
        "tempo change across a waiting slide never changes its native timing", variableResult);
    QString medium = "(150){4}";
    for (int i = 0; i < 520; ++i) medium += "1~r,";
    const auto mediumResult = optimize(medium, 9, 35);
    expect(mediumResult.patternOutcome != "size_limit", "ordinary difficulties above 512 markers are accepted", mediumResult);
    return ok ? 0 : 1;
}
