#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/parser/SimaiParser.h"
#include "timeline/TimelineNoteAssets.h"
#include "timeline/TimelineQuickModel.h"
#include "timeline/TimelineSceneStateBuilder.h"
#include "timeline/quick/TimelineQuickItem.h"
#include "timeline/quick/TimelineQuickStateBridge.h"

#include <QApplication>
#include <QDir>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickGraphicsDevice>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <memory>

namespace {
bool sameStops(const QVector<HandOrderTrackStop>& a, const QVector<HandOrderTrackStop>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).hand != b.at(i).hand || qAbs(a.at(i).progress - b.at(i).progress) > 1e-9)
            return false;
    return true;
}

bool sameHands(const TimelineRenderNote& quick, const TimelineNoteMarker& marker)
{
    if (quick.slideBranchIndex != marker.slideBranchIndex || quick.handOrderHead != marker.handOrderHead
        || quick.handOrderWifiMode != marker.handOrderWifiMode
        || !sameStops(quick.handOrderTrackStops, marker.handOrderTrackStops)
        || quick.handOrderWifiSectorStops.size() != marker.handOrderWifiSectorStops.size()) return false;
    for (int i = 0; i < quick.handOrderWifiSectorStops.size(); ++i)
        if (!sameStops(quick.handOrderWifiSectorStops.at(i), marker.handOrderWifiSectorStops.at(i))) return false;
    return true;
}

QVector<TimelineRenderNote> notes(const TimelineRenderSnapshot& snapshot)
{
    QVector<TimelineRenderNote> result;
    for (const auto& line : snapshot.lines) result += line.notes;
    return result;
}

bool hasType(const QVector<miacode::timeline::TimelineSceneSprite>& sprites, const QString& prefix)
{
    return std::any_of(sprites.cbegin(), sprites.cend(),
        [&](const auto& sprite) { return sprite.spriteType.startsWith(prefix); });
}

// Exercise the real timeline QSG item and texture cache without a desktop window.
class OffscreenTimeline {
public:
    bool initialize(TimelineQuickStateBridge* bridge)
    {
        const auto fail = [](const char* reason) {
            QTextStream(stderr) << "Offscreen initialization: " << reason << '\n';
            return false;
        };
        surface.create();
        if (!surface.isValid()) return fail("surface");
        context.setFormat(surface.format());
        if (!context.create() || !context.makeCurrent(&surface)) return fail("OpenGL context");
        control = std::make_unique<QQuickRenderControl>();
        window = std::make_unique<QQuickWindow>(control.get());
        window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(&context));
        window->setColor(QColor(18, 20, 22));
        window->setGeometry(0, 0, 1400, 320);
        if (!control->initialize()) return fail("render control");
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setInternalTextureFormat(GL_RGBA8);
        framebuffer = std::make_unique<QOpenGLFramebufferObject>(QSize(1400, 320), format);
        if (!framebuffer->isValid()) return fail("framebuffer");
        window->setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(framebuffer->texture(), framebuffer->size()));
        item = new TimelineQuickItem(window->contentItem());
        item->setSize(QSizeF(1400, 320));
        item->setStateBridge(bridge);
        return true;
    }

    QImage frame()
    {
        QCoreApplication::processEvents();
        if (!context.makeCurrent(&surface)) return {};
        control->polishItems();
        control->beginFrame();
        control->sync();
        control->render();
        control->endFrame();
        context.functions()->glFinish();
        return framebuffer->toImage();
    }

    ~OffscreenTimeline()
    {
        if (context.isValid()) context.makeCurrent(&surface);
        if (control) control->invalidate();
        window.reset();
        framebuffer.reset();
        control.reset();
        context.doneCurrent();
    }
private:
    QOffscreenSurface surface;
    QOpenGLContext context;
    std::unique_ptr<QQuickRenderControl> control;
    std::unique_ptr<QQuickWindow> window;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer;
    TimelineQuickItem* item = nullptr;
};
}

int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& label) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << label << '\n';
        ok &= pass;
    };
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) expect(QDir().mkpath(output), QStringLiteral("evidence directory exists"));

    const QStringList tokens{
        "1~l", "2bx~r", "3m~m", "4h[4:1]~l", "5hx[8:1]~r", "6$$bx~m",
        "A1~l", "B2m~r", "Cf~m", "A4h[8:3]~l", "Ch[4:1]~r", "C1h~m",
        "1@bx-4[0.5##8:1]~[head=l;track=0:l|0.5:r]", "2!-6[4:1]~m",
        "1-5[4:1]*-7[4:2]~[head=r;branch0=0:l|0.5:m;branch1=0:r]",
        "5v5v2[8:1]~[track=0:l|0.75:r]",
        "3w7[4:1]~[head=m;track=split:0:l|0.5:r&0:r]",
        "4w8[4:1]~[track=single:0:m|0.5:m]"
    };
    for (const QString& token : tokens) {
        const QString chart = "(120){4}" + token + ",\nE";
        const auto parsed = SimaiParser::validateSyntax(chart);
        TimelineQuickModel model;
        model.rebuildFromText(chart, 0.0);
        const auto quick = notes(model.snapshot());
        bool matches = parsed.ok && parsed.annotationErrors.isEmpty() && quick.size() == parsed.noteMarkers.size();
        for (int i = 0; matches && i < quick.size(); ++i)
            matches = sameHands(quick.at(i), parsed.noteMarkers.at(i));
        expect(matches, "quick timeline preserves authoritative hand assignments: " + token);
    }
    for (const QString& token : {QStringLiteral("1~[head=l]"), QStringLiteral("1~bad"),
             QStringLiteral("1-5[4:1]~[track=split:0:l&0:r]")}) {
        TimelineQuickModel model;
        model.rebuildFromText("(120){4}" + token + ",2~r,", 0.0);
        const auto quick = notes(model.snapshot());
        expect(quick.size() == 2 && quick.first().handOrderHead == HandOrderHand::Unspecified
            && quick.first().handOrderTrackStops.isEmpty()
            && quick.last().handOrderHead == HandOrderHand::Right,
            "invalid annotations retain native notes without assigning a false hand: " + token);
    }

    TimelineQuickModel incremental;
    const QString before = "(120){4}1~l,2h[4:1]~r,3-7[4:1]~[head=m;track=0:l|0.5:r],";
    const int editAt = before.indexOf("~l") + 1;
    QString after = before;
    after[editAt] = 'r';
    incremental.rebuildFromText(before, 0.0);
    incremental.applyTextChange(after, editAt, 1, 1, 0.0);
    expect(notes(incremental.snapshot()).first().handOrderHead == HandOrderHand::Right,
        "a suffix-only edit updates the incremental hand assignment");
    const int boundaryAt = after.indexOf("0.5:r");
    after.replace(boundaryAt, 5, "0.75:l");
    incremental.applyTextChange(after, boundaryAt, 5, 6, 0.0);
    const auto editedNotes = notes(incremental.snapshot());
    const auto editedParse = SimaiParser::validateSyntax(after);
    expect(editedNotes.size() == editedParse.noteMarkers.size()
        && sameHands(editedNotes.last(), editedParse.noteMarkers.last())
        && editedNotes.last().handOrderTrackStops.last().hand == HandOrderHand::Left,
        "an incremental handoff-boundary edit replaces cached track assignments");

    const QString temporaryRoot = QDir(QStringLiteral(MIACODE_SOURCE_ROOT)).filePath("../runtime/temp");
    expect(QDir().mkpath(temporaryRoot), "custom skin fixture stays inside the workspace");
    QTemporaryDir custom(QDir(temporaryRoot).filePath("timeline_skin_XXXXXX"));
    expect(custom.isValid(), "custom skin fixture is available");
    QImage customTap(16, 16, QImage::Format_ARGB32);
    customTap.fill(QColor(200, 180, 30, 160));
    customTap.setPixelColor(0, 0, Qt::transparent);
    expect(customTap.save(QDir(custom.path()).filePath("tap.png")), "custom native artwork saved");
    auto fallbackAssets = miacode::timeline::loadTimelineNoteAssets(custom.path());
    const QImage fallbackLeft = fallbackAssets.noteIcons.value("hand_order/l/tap").toImage();
    const QImage fallbackRight = fallbackAssets.noteIcons.value("hand_order/r/tap").toImage();
    expect(!fallbackLeft.isNull() && !fallbackRight.isNull() && fallbackLeft != fallbackRight
        && fallbackLeft.pixelColor(3, 3).alpha() == 160 && fallbackLeft.pixelColor(0, 0).alpha() == 0,
        "a custom skin without hand artwork gets distinct cached colors with original alpha");
    expect(fallbackAssets.noteIcons.value("tap").toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied)
        == customTap.convertToFormat(QImage::Format_ARGB32_Premultiplied),
        "custom skin fallback preserves the original-mode pixels");
    const QString leftFolder = QDir(custom.path()).filePath("hand_order/l");
    expect(QDir().mkpath(leftFolder), "partial custom hand artwork directory exists");
    QImage supplied = customTap;
    supplied.fill(QColor(30, 80, 240, 160));
    expect(supplied.save(QDir(leftFolder).filePath("tap.png")), "partial custom artwork saved");
    fallbackAssets = miacode::timeline::loadTimelineNoteAssets(custom.path());
    expect(fallbackAssets.noteIcons.value("hand_order/l/tap").toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied)
        == supplied.convertToFormat(QImage::Format_ARGB32_Premultiplied)
        && fallbackAssets.noteIcons.value("hand_order/r/tap").toImage() == fallbackRight,
        "partial custom art is preferred while missing hands still use fallback");

    const QString chart = "(120){4}1~l/8~r,2~r,3~m,4h[2:1]~l,"
        "A1~l/B2~r/Cf~m,Ch[2:1]~r,,1-5[2:1]~[head=l;track=0:l|0.5:r],"
        "5w1[4:1]~[head=r;track=split:0:l&0:r],4w8[4:1]~[track=single:0:m],,E";
    TimelineQuickModel model;
    model.rebuildFromText(chart, 0.0);
    TimelineQuickStateBridge bridge;
    bridge.setTimelineData(model.snapshot());
    const auto revision = bridge.notesRevision();
    bridge.setShowHandOrderAnnotations(false);
    expect(bridge.notesRevision() > revision && !bridge.showHandOrderAnnotations(),
        "changing hand-order visibility invalidates the note layer");
    const auto unchanged = bridge.notesRevision();
    bridge.setShowHandOrderAnnotations(false);
    expect(bridge.notesRevision() == unchanged, "unchanged visibility does not rebuild the note layer");

    OffscreenTimeline renderer;
    expect(renderer.initialize(&bridge), "real offscreen timeline QSG initializes");
    if (!ok) return 1;
    bridge.setQuickViewportSize(QSize(1400, 320));
    bridge.setZoomScale(1.5);
    bridge.setHorizontalScrollValue(600.0);
    for (const QString& skin : {QStringLiteral("skinSD"), QStringLiteral("skinDX")}) {
        const QString directory = QDir(QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin")).filePath(skin);
        const auto assets = miacode::timeline::loadTimelineNoteAssets(directory);
        for (const QString& hand : {QStringLiteral("l"), QStringLiteral("r"), QStringLiteral("m")}) {
            const QString key = "hand_order/" + hand + "/tap";
            const QImage expected(QDir(directory).filePath("hand_order/" + hand + "/tap.png"));
            expect(!expected.isNull() && assets.noteIcons.contains(key)
                && assets.noteIcons.value(key).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied)
                    == expected.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                skin + " uses the exact prebuilt artwork: " + key);
            for (const QString& type : {QStringLiteral("tap_each_ex"), QStringLiteral("hold_ex"),
                     QStringLiteral("touch"), QStringLiteral("touch_hold_border_only"), QStringLiteral("slide_track")})
                expect(assets.noteIcons.contains("hand_order/" + hand + '/' + type),
                    skin + " preserves native material variants for " + hand + '/' + type);
        }
        miacode::timeline::TimelineSceneBuildRequest request;
        request.snapshot = model.snapshot();
        request.viewportSize = QSize(1400, 320);
        request.skinDirectory = directory;
        request.showHandOrderAnnotations = true;
        const auto scene = miacode::timeline::TimelineSceneStateBuilder::build(request);
        expect(hasType(scene.noteSprites, "hand_order/l/") && hasType(scene.noteSprites, "hand_order/r/")
            && hasType(scene.noteSprites, "hand_order/m/") && hasType(scene.trackSprites, "wifi_single_badge"),
            skin + " emits left, right, either and explicit-single head materials");
        expect(hasType(scene.trackSprites, "hand_order/l/") && hasType(scene.trackSprites, "hand_order/r/")
            && !scene.holdSpans.isEmpty() && scene.holdSpans.first().spriteType.startsWith("hand_order/l/"),
            skin + " emits annotated tracks and hold bodies");

        for (bool wifi : {false, true}) {
            TimelineQuickModel trackModel;
            trackModel.rebuildFromText(wifi
                ? "(120){4}1w5[2:1]~[track=split:0:l|0.5:r&0:r|0.75:l],E"
                : "(120){4}1!-5[2:1]~[track=0:l|0.5:r],E", 0.0);
            request.snapshot = trackModel.snapshot();
            const auto trackScene = miacode::timeline::TimelineSceneStateBuilder::build(request);
            bool correct = trackScene.trackSprites.size() > 4;
            bool sawFirst = false, sawSecond = false, sawLast = false;
            if (correct) {
                const qreal first = trackScene.trackSprites.first().center.x();
                const qreal span = trackScene.trackSprites.last().center.x() - first;
                correct = span > 0;
                for (const auto& sprite : trackScene.trackSprites) {
                    const qreal progress = (sprite.center.x() - first) / span;
                    const QString expected = wifi
                        ? (progress < 0.5 ? "hand_order/split/l_r/" : progress < 0.75
                            ? "hand_order/split/r_r/" : "hand_order/split/r_l/")
                        : (progress < 0.5 ? "hand_order/l/" : "hand_order/r/");
                    correct &= sprite.spriteType.startsWith(expected);
                    sawFirst |= progress < 0.5;
                    sawSecond |= progress >= 0.5;
                    sawLast |= progress >= 0.75;
                }
            }
            expect(correct && sawFirst && sawSecond && sawLast,
                skin + (wifi ? " independently changes both Wifi sectors at their boundaries"
                             : " changes Slide track artwork at the movement-progress boundary"));
        }

        bridge.setSkinDirectory(directory);
        bridge.setShowHandOrderAnnotations(false);
        const QImage original = renderer.frame();
        bridge.setShowHandOrderAnnotations(true);
        const QImage colored = renderer.frame();
        bridge.setShowHandOrderAnnotations(false);
        const QImage restored = renderer.frame();
        expect(!original.isNull() && !colored.isNull() && original != colored,
            skin + " changes actual timeline pixels when hand-order visibility is enabled");
        expect(restored == original, skin + " returns exactly to the original raster after disabling annotations");
        TimelineQuickModel plainModel;
        plainModel.rebuildFromText(miacode::hand_order::stripAnnotations(chart), 0.0);
        bridge.setTimelineData(plainModel.snapshot());
        bridge.setShowHandOrderAnnotations(true);
        const QImage unannotated = renderer.frame();
        expect(unannotated == original, skin + " leaves unannotated notes identical to original-mode rendering");
        bridge.setTimelineData(model.snapshot());
        bridge.setShowHandOrderAnnotations(false);
        int changedBlue = 0, changedRed = 0;
        for (int y = 0; y < colored.height(); ++y) for (int x = 0; x < colored.width(); ++x) {
            const QColor pixel = colored.pixelColor(x, y);
            if (pixel == original.pixelColor(x, y)) continue;
            changedBlue += pixel.blue() > pixel.red() + 30;
            changedRed += pixel.red() > pixel.blue() + 30;
        }
        expect(changedBlue > 10 && changedRed > 10,
            QStringLiteral("%1 raster has distinct left/right pixels (%2 blue, %3 red)").arg(skin).arg(changedBlue).arg(changedRed));
        if (!output.isEmpty()) {
            expect(original.save(QDir(output).filePath(skin + "_original.png")), "original raster saved");
            expect(colored.save(QDir(output).filePath(skin + "_sticking.png")), "sticking raster saved");
        }
    }
    return ok ? 0 : 1;
}
