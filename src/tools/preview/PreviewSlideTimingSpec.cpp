#include "core/chart/SlideJudgementTiming.h"
#include "core/chart/SlideReferenceData.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/model/TimelineMarkerOffset.h"
#include "core/scene/PreviewSlideTiming.h"
#include "core/scene/PreviewTrackLayerState.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTextStream>
#include <cmath>
#include <limits>

using namespace miacode::preview::scene;
namespace {
bool require(bool value, const char* message)
{
    if (!value) QTextStream(stderr) << "FAIL: " << message << Qt::endl;
    return value;
}
bool near(double a, double b, double epsilon = 1e-6) { return std::abs(a-b) < epsilon; }
TimelineNoteMarker parse(const QString& token)
{
    const auto result = SimaiParser::parseForTimeline(QStringLiteral("(120){8}") + token + QStringLiteral(",\nE"));
    for (const auto& note : result.noteMarkers)
        if (note.type == QLatin1String("slide") || note.type == QLatin1String("wifi")) return note;
    return {};
}
bool timing()
{
    bool ok = true;
    auto marker = parse(QStringLiteral("1-6-4[2:1]"));
    const auto value = miacode::slide_judge::timingForMarker(marker);
    ok &= require(value.valid && near(value.judgementSecond, 1.416779654361566)
        && near(value.lastAreaDuration, .0832203456384342)
        && near(value.perfectStartSecond, 1.1334463210282324)
        && near(value.perfectEndSecond, 1.6709180741045077),
        "fes tail uses only the final segment duration and one final judgement");
    ok &= require(near(miacode::slide_judge::brightnessProgress(value, value.headSecond), 0)
        && near(miacode::slide_judge::brightnessProgress(value, value.judgementSecond/2), .5)
        && near(miacode::slide_judge::brightnessProgress(value, value.judgementSecond), 1)
        && miacode::slide_judge::brightnessProgress(value, .25) > 0,
        "linear brightness includes the initial waiting time and finishes at reference, not earliest Perfect");
    auto zeroWait = parse(QStringLiteral("1-5[0##0.125]"));
    const auto fast = miacode::slide_judge::timingForMarker(zeroWait);
    ok &= require(fast.valid && near(fast.headSecond, fast.traceSecond)
        && near(fast.judgementSecond, .10464120177141166)
        && near(fast.judgementSecond-fast.perfectStartSecond, 17.0/60.0)
        && fast.perfectEndSecond-fast.judgementSecond < 17.0/60.0,
        "zero-wait fast slide has continuous progress and an asymmetric Perfect window");
    const auto slow = miacode::slide_judge::timingForMarker(parse(QStringLiteral("1-5[0##20]")));
    ok &= require(slow.valid && near(slow.judgementSecond-slow.perfectStartSecond, .6)
        && near(slow.perfectEndSecond-slow.judgementSecond, .6), "long slide Perfect window saturates at 36 frames");
    const auto wifi = miacode::slide_judge::timingForMarker(parse(QStringLiteral("1w5[4:1]")));
    ok &= require(wifi.valid && wifi.wifiReference
        && near(wifi.judgementSecond-wifi.perfectStartSecond, wifi.perfectEndSecond-wifi.judgementSecond),
        "WiFi symmetric reference is explicit, separate from normal-slide extra fast allowance");
    marker.slideSegmentCriticalProportions.last() = std::numeric_limits<double>::quiet_NaN();
    ok &= require(!miacode::slide_judge::timingForMarker(marker).valid, "non-finite reference data is rejected");
    marker = parse(QStringLiteral("1-5[4:1]"));
    const auto shifted = miacode::timeline::offset::shiftedNoteMarkers({marker}, -2.5,
        miacode::timeline::offset::NonFiniteHandling::PassThrough);
    const auto shiftedTiming = miacode::slide_judge::timingForMarker(shifted[0]);
    const auto unshiftedTiming = miacode::slide_judge::timingForMarker(marker);
    ok &= require(shiftedTiming.valid && near(shiftedTiming.judgementSecond,unshiftedTiming.judgementSecond-2.5)
        && near(miacode::slide_judge::brightnessProgress(shiftedTiming,-2.25),miacode::slide_judge::brightnessProgress(unshiftedTiming,.25)),
        "negative chart offset shifts all timestamps once and keeps animation phase");
    marker.slideSegmentCriticalProportions.last() = 1.0;
    ok &= require(!miacode::slide_judge::timingForMarker(marker).valid, "parser unknown-proportion sentinel does not fabricate zero tail");
    marker = parse(QStringLiteral("1-5[4:1]"));
    marker.slideSegmentDurations.clear();
    ok &= require(!miacode::slide_judge::timingForMarker(marker).valid, "missing final-segment timing does not invent a judgement");
    return ok;
}
bool shapes()
{
    bool ok = true;
    int count = 0;
    const auto root = miacode::slide_reference::root();
    for (const auto& section : {QStringLiteral("slides"), QStringLiteral("wifi")}) {
        const auto entries = root.value(section).toObject();
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            const auto marker = parse(it.key()+QStringLiteral("[4:1]"));
            const auto geometry = buildPreviewSlideTimingGeometry(marker);
            if (!geometry.isValid()) {
                QTextStream(stderr) << "invalid geometry: " << it.key() << Qt::endl;
                ok = false;
            }
            for (const auto& segment : geometry.slideArrows)
                for (const auto& area : segment)
                    for (const auto& arrow : area)
                        ok &= require(std::isfinite(arrow.distance) && arrow.distance >= 0
                            && arrow.distance <= geometry.totalLength + 1e-6,
                            "arrow projection stays on its own path");
            ++count;
        }
    }
    ok &= require(count == 616, "all bundled 608 slide and 8 WiFi shapes checked");
    const auto backtrack = buildPreviewSlideTimingGeometry(parse(QStringLiteral("1v1[4:1]")));
    const auto first = backtrack.slideArrows.first().first().first();
    const auto last = backtrack.slideArrows.last().last().last();
    ok &= require(first.distance < backtrack.totalLength/2 && last.distance > backtrack.totalLength/2
        && QPointF::dotProduct(first.tangent,last.tangent) < -.9,
        "out-and-back arrows use orientation to distinguish coincident geometry");
    return ok;
}
bool rendering()
{
    bool ok = true;
    PreviewFrameState state;
    state.noteMarkers = {parse(QStringLiteral("1-6-4[2:1]"))};
    state.skin.slideTrackImage = QImage(30, 18, QImage::Format_ARGB32_Premultiplied);
    state.skin.slideTrackImage.fill(QColor(40, 160, 220, 180));
    const qint64 imageKey = state.skin.slideTrackImage.cacheKey();
    auto sample = [&](double second) {
        state.playheadSeconds = second;
        return buildPreviewTrackLayerState(state, PreviewActiveMarkerView(state.noteMarkers), QRectF(0,0,540,540));
    };
    const auto native = sample(.25);
    ok &= require(!native.sprites.isEmpty() && near(native.sprites.first().brightness,1), "default mode keeps native brightness");
    state.render.showSlideJudgementTiming = true;
    const auto head = sample(0);
    ok &= require(!head.sprites.isEmpty() && near(head.sprites.first().brightness,.25), "whole track starts low at the head beat");
    const auto wait = sample(.25);
    bool reveal = false;
    for (const auto& sprite : wait.sprites) reveal |= sprite.unrevealedBrightness < 1 && !sprite.revealDirection.isNull();
    ok &= require(reveal && wait.sprites.size() == head.sprites.size(), "pixel reveal starts while guide star is waiting, without erasing arrows");
    const auto reference = miacode::slide_judge::timingForMarker(state.noteMarkers[0]);
    const auto end = sample(reference.judgementSecond);
    bool bright = end.sprites.size() == head.sprites.size();
    for (const auto& sprite : end.sprites) bright &= near(sprite.brightness,1) && near(sprite.unrevealedBrightness,1);
    ok &= require(bright, "entire native material is normal at exact reference instant");
    const auto replay = sample(.25);
    ok &= require(replay.sprites.size() == wait.sprites.size()
        && near(replay.sprites[0].revealOffset,wait.sprites[0].revealOffset)
        && state.skin.slideTrackImage.cacheKey() == imageKey, "backward seek is deterministic and never mutates skin images");
    ok &= require(sample(state.noteMarkers[0].endSecond).sprites.isEmpty(), "track retires at existing geometric end");
    PreviewPreparedSceneCache cache;
    ok &= require(cache.sync(state), "timing prepared cache builds");
    const auto* stored = cache.slideLikeLayer().entries[0].slideTimingGeometry.get();
    ok &= require(stored && stored->isValid() && !cache.sync(state)
        && cache.slideLikeLayer().entries[0].slideTimingGeometry.get() == stored, "arc-length projections are cached across frame changes");
    state.render.showSlideJudgementTiming = false;
    ok &= require(cache.sync(state) && !cache.slideLikeLayer().entries[0].slideTimingGeometry, "toggle invalidates prepared cache and releases timing geometry");
    state.render.showSlideJudgementTiming = true;
    state.noteMarkers[0].slideSegmentCriticalProportions.clear();
    const auto invalid = sample(.25);
    ok &= require(invalid.sprites.isEmpty() && invalid.missingSlideTimingCount == 1, "invalid timing is skipped with a diagnostic count");
    return ok;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    const bool ok = timing() & shapes() & rendering();
    if (ok) QTextStream(stdout) << "preview_slide_timing_spec ok" << Qt::endl;
    return ok ? 0 : 1;
}
