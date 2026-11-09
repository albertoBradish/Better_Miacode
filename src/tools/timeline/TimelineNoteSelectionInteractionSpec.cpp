#include "timeline/quick/TimelineQuickItem.h"
#include "timeline/quick/TimelineQuickStateBridge.h"
#include "timeline/TimelineQuickModel.h"
#include "timeline/TimelineSceneStateBuilder.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>
#include <QTextStream>
#include <QWheelEvent>

#include <array>

int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    qRegisterMetaType<QSet<quint64>>();
    bool ok = true;
    const auto expect = [&](bool pass, const char* label) {
        ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << label << '\n';
    };
    TimelineQuickModel model;
    model.rebuildFromText(QStringLiteral("(120){4}1,5,2h[4:1],6,E"), 0);
    QQuickWindow window;
    window.resize(1000, 400);
    TimelineQuickStateBridge bridge;
    TimelineQuickItem item(window.contentItem());
    item.setSize(QSizeF(1000,400));
    item.setStateBridge(&bridge);
    bridge.setFollowProgressEnabled(false);
    bridge.setTimelineData(model.snapshot());
    window.show(); window.requestActivate();
    QTest::qWait(100);
    miacode::timeline::TimelineSceneBuildRequest request;
    request.snapshot = bridge.renderSnapshot(); request.viewportSize = QSize(1000,400);
    request.zoomScale = bridge.zoomScale(); request.contentScale = bridge.contentScale();
    request.horizontalScrollValue = bridge.horizontalScrollValue(); request.fitViewportHeight = true;
    const auto state = miacode::timeline::TimelineSceneStateBuilder::build(request);
    expect(!state.noteHitAreas.isEmpty(), "synthetic timeline exposes native note hit geometry");
    if (state.noteHitAreas.isEmpty()) return 1;
    const auto hit = state.noteHitAreas.first();
    const QPoint center = (hit.rect.center() - QPointF(bridge.horizontalScrollValue(),0)).toPoint();
    const QPoint from = center - QPoint(7,7), to = center + QPoint(7,7);
    QSignalSpy selections(&bridge,&TimelineQuickStateBridge::noteSelectionRequested);
    const auto press = [&](const QPoint& p, Qt::KeyboardModifiers modifiers) { QTest::mousePress(&window,Qt::LeftButton,modifiers,p); };
    const auto release = [&](const QPoint& p) { QTest::mouseRelease(&window,Qt::LeftButton,Qt::ShiftModifier,p); QCoreApplication::processEvents(); };
    press(from,Qt::ShiftModifier); QTest::mouseMove(&window,to); release(to);
    expect(selections.size() == 1 && selections.last()[0].value<QSet<quint64>>().contains(hit.locationId)
        && selections.last()[1].toBool(), "Shift rectangle selects timeline notes with toggle semantics");
    press(to,Qt::ShiftModifier); QTest::mouseMove(&window,from); release(from);
    expect(selections.size() == 2 && selections.last()[0].value<QSet<quint64>>().contains(hit.locationId),
        "reversed rectangle uses the same geometry");
    press(from,Qt::ShiftModifier); QTest::keyClick(&window,Qt::Key_Escape); release(to);
    expect(selections.size() == 2 && !item.keepMouseGrab(), "Escape cancels timeline selection");
    press(from,Qt::ShiftModifier); item.ungrabMouse(); release(to);
    expect(selections.size() == 2, "lost pointer grab cancels selection");
    press(from,Qt::ShiftModifier); bridge.clear(); release(to);
    expect(selections.size() == 2, "document replacement discards captured note geometry");
    bridge.setTimelineData(model.snapshot());
    press(center,Qt::ControlModifier); QTest::mouseRelease(&window,Qt::LeftButton,Qt::ControlModifier,center);
    expect(selections.size() == 2, "Ctrl click never enters note selection");
    using namespace miacode::timeline;
    const auto buildState = [&] {
        request.axisMode = timelineAxisModeForToken(bridge.axisMode());
        request.viewportSize = TimelineAxisProjection{request.axisMode}.axisSize(window.size());
        request.laneOrder = timelineLaneOrderForToken(bridge.laneOrder());
        request.zoomScale = bridge.zoomScale();
        request.noteScale = bridge.noteScale();
        request.playheadSeconds = bridge.playheadSeconds();
        request.horizontalScrollValue = bridge.horizontalScrollValue();
        request.playheadUpperLimitSeconds = bridge.playheadUpperLimitSeconds();
        return TimelineSceneStateBuilder::build(request);
    };
    const std::array orders{
        QStringLiteral("natural"), QStringLiteral("rotated"),
        QStringLiteral("reversed"), QStringLiteral("rotated_reversed"),
    };
    const std::array<std::array<int, 8>, 4> labels{{
        {1, 2, 3, 4, 5, 6, 7, 8},
        {5, 6, 7, 8, 1, 2, 3, 4},
        {8, 7, 6, 5, 4, 3, 2, 1},
        {4, 3, 2, 1, 8, 7, 6, 5},
    }};
    const auto normalSize = buildState().noteHitAreas.first().rect.size();
    bridge.setNoteScale(3.0);
    const auto largeSize = buildState().noteHitAreas.first().rect.size();
    expect(largeSize.width() > normalSize.width() && largeSize.height() > normalSize.height(),
        "note scale increases rendered hit geometry independently of time zoom");
    const auto largeZoom = bridge.zoomScale();
    bridge.setZoomScale(largeZoom * 2.0);
    expect(buildState().noteHitAreas.first().rect.size() == largeSize,
        "time zoom leaves glyph dimensions unchanged");
    bridge.setZoomScale(largeZoom);

    QSignalSpy seeks(&item, &TimelineQuickItem::headerNavigateRequested);
    for (const auto& mode : {QStringLiteral("horizontal"), QStringLiteral("falling"), QStringLiteral("rising")}) {
        bridge.setAxisMode(mode);
        bridge.setPlayheadSeconds(0.1, true);
        const auto anchoredScene = buildState();
        // The overlay removes time scroll from scene coordinates before projecting to the screen.
        const auto viewportPosition = anchoredScene.playheadLine.start - QPointF(anchoredScene.horizontalScrollValue, 0);
        const auto screenPosition = anchoredScene.axisProjection().toScreenTransform().map(viewportPosition);
        const double expectedTimePosition = mode == QLatin1String("falling") ? window.height() * 0.88
            : mode == QLatin1String("rising") ? window.height() * 0.12 : window.width() * 0.5;
        const double actualTimePosition = mode == QLatin1String("horizontal") ? screenPosition.x() : screenPosition.y();
        expect(anchoredScene.hasPlayheadLine && qAbs(expectedTimePosition - actualTimePosition) <= 0.5,
            "playhead follows the viewport centre or the corresponding vertical judgement edge");
        for (std::size_t index = 0; index < orders.size(); ++index) {
            bridge.setLaneOrder(orders.at(index));
            const auto scene = buildState();
            bool labelOrderMatches = scene.laneLabels.size() >= 8;
            for (int lane = 0; labelOrderMatches && lane < 8; ++lane) {
                labelOrderMatches = scene.laneLabels.at(lane).text.toInt() == labels.at(index).at(lane);
            }
            expect(labelOrderMatches, "projected timeline shows the selected lane order");

            const auto axis = scene.axisProjection();
            const auto transform = axis.toScreenTransform();
            const QPointF later = transform.map(QPointF(100, 80));
            const QPointF earlier = transform.map(QPointF(80, 80));
            expect(axis.toAxis(later) == QPointF(100, 80)
                && (mode == QLatin1String("falling") ? later.y() < earlier.y()
                    : mode == QLatin1String("rising") ? later.y() > earlier.y() : later.x() > earlier.x()),
                "timeline direction and inverse hit projection agree");

            if (scene.noteHitAreas.isEmpty()) {
                expect(false, "projected timeline has note hits");
                continue;
            }
            const auto native = scene.noteHitAreas.first();
            const QPointF position = native.rect.center() - QPointF(scene.horizontalScrollValue, 0);
            const QPoint point = transform.map(position).toPoint();
            const auto count = selections.size();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::ShiftModifier, point);
            expect(selections.size() == count + 1
                && selections.last()[0].value<QSet<quint64>>().contains(hit.locationId),
                "projected hit preserves the original note identity");
            const QPoint ruler = transform.map(QPointF(position.x(), scene.timelineTop / 2.0)).toPoint();
            const auto seekCount = seeks.size();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, ruler);
            const double rulerSecond = TimelineSceneStateBuilder::sceneXToSecond(scene, axis.toAxis(ruler).x());
            expect(seeks.size() == seekCount + 1 && qAbs(seeks.last()[0].toDouble() - rulerSecond) < 1e-6,
                "projected ruler click seeks to the corresponding second");
        }
    }

    bridge.setPlayheadUpperLimitSeconds(30.0);
    bridge.setPlayheadSeconds(8.0, true);
    const QPointF anchor(240, 170);
    const auto secondAtAnchor = [&] {
        const auto scene = buildState();
        return TimelineSceneStateBuilder::sceneXToSecond(scene, scene.axisProjection().toAxis(anchor).x());
    };
    const auto wheel = [&](const QPoint& pixels, const QPoint& angle) {
        QWheelEvent event(anchor, window.mapToGlobal(anchor.toPoint()), pixels, angle,
            Qt::NoButton, Qt::AltModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &event);
    };
    const double initialZoom = bridge.zoomScale();
    const double initialAnchor = secondAtAnchor();
    wheel(QPoint(0, 15), {});
    expect(bridge.zoomScale() > initialZoom && bridge.zoomScale() < 0.75
        && qAbs(secondAtAnchor() - initialAnchor) < 1e-6,
        "a fractional Alt-wheel step preserves the time under the pointer");
    bridge.stepZoomIn();
    expect(qFuzzyCompare(bridge.zoomScale(), 0.75), "preset shortcuts step from a fractional scale");
    bridge.setZoomWheelShortcuts({QStringLiteral("Alt+WheelDown")}, {QStringLiteral("Alt+WheelUp")});
    wheel({}, QPoint(0, 120));
    expect(qFuzzyCompare(bridge.zoomScale(), 0.5), "custom Alt-wheel direction keeps its priority");
    bridge.setZoomScale(4.0);
    expect(qFuzzyCompare(bridge.zoomScale(), 3.0), "zoom is clamped at 300 percent");
    bridge.setZoomScale(0.1);
    expect(qFuzzyCompare(bridge.zoomScale(), 0.25), "zoom is clamped at 25 percent");

    window.hide();
    return ok ? 0 : 1;
}
