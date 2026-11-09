import QtQuick
import QtQuick.Controls
import QtQuick.Window
import MiaCode.UI
import "qrc:/preview/runtime/qml" as Preview

Rectangle {
    id: root

    required property var previewSession
    required property var preferences
    required property var exportSession
    property bool documentAvailable: true
    property bool latencyActive: false
    // See PreviewTransport: the canvas menu hides on the export page.
    property bool exportPageActive: false
    // Suspend the scene subscription while moving the pane between windows.
    property bool surfaceActive: true
    property bool detached: false
    // Mirrors the transport row when the preview sits left of the editor; set
    // by the workspace layout, kept separate from `detached` so the detached
    // window always renders the default order.
    property bool transportMirrored: false
    readonly property real minimumHeight: heading.implicitHeight + transport.implicitHeight
                                          + statistics.implicitHeight + 64
    readonly property real minimumWidth: transport.minimumWidth
    signal detachRequested()
    signal dockRequested()
    readonly property alias cornerSourceItem: heading
    readonly property Item transportItem: transport
    readonly property Item renderModeItem: renderModeButton
    // The 预览 → 渲染模式 menu entry stands in for a click on the button, so it
    // is available exactly when the button can be clicked.
    readonly property bool renderModeMenuAvailable: renderModeButton.visible && renderModeButton.enabled

    // Export page still uses the backend ratio. Edit mode defaults to 1:1;
    // free aspect sizes the surface to the live stage geometry.
    readonly property bool freeAspectActive:
        !root.exportPageActive && root.preferences && root.preferences.previewCanvasFreeAspect
    readonly property real canvasAspectRatio: {
        const ratio = root.previewSession && root.previewSession.canvasAspectRatio !== undefined
                      ? root.previewSession.canvasAspectRatio
                      : 1.0
        return Math.max(1.0, (ratio > 0 && isFinite(ratio)) ? ratio : 1.0)
    }

    color: Theme.surfaceColor(Theme.colors.background.panel)
    clip: true

    function toggleRenderModeMenu() {
        if (renderModeMenu.active) {
            renderModeMenu.close()
            return
        }
        renderModeMenu.openAt(renderModeButton)
    }

    function closeMenus() {
        renderModeMenu.close()
        transport.closeMenus()
    }

    function fittedFrameWidth(hostWidth, hostHeight) {
        const safeWidth = Math.max(1, hostWidth)
        const safeHeight = Math.max(1, hostHeight)
        return Math.max(1, Math.min(safeWidth, safeHeight * root.canvasAspectRatio))
    }

    function fittedFrameHeight(hostWidth, hostHeight) {
        const frameWidth = fittedFrameWidth(hostWidth, hostHeight)
        return Math.max(1, Math.min(hostHeight, frameWidth / root.canvasAspectRatio))
    }

    PanelHeader {
        id: heading
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        title: qsTrId("dialog.render_settings.preview_group")
        sidebarTitle: true

        ChromeRow {
            id: renderModeButton
            enabled: root.documentAvailable
            implicitWidth: renderModeLabelText.implicitWidth + leftPadding + rightPadding
            selected: renderModeMenu.active
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTrId("qml.preview_rendering_mode")
            Accessible.description: root.previewSession.renderModeLabel
            onClicked: root.toggleRenderModeMenu()

            contentItem: Text {
                id: renderModeLabelText
                text: root.previewSession.renderModeLabel
                color: !renderModeButton.enabled ? Theme.colors.text.disabled
                     : (renderModeButton.selected || renderModeButton.hovered || renderModeButton.visualFocus) ? Theme.colors.text.active
                     : Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            Tooltip {
                visible: renderModeButton.hovered
                text: qsTrId("qml.preview_rendering_mode")
            }

        }
    }

    PreviewRenderModeMenu {
        id: renderModeMenu
        previewSession: root.previewSession
    }

    Item {
        id: stageArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: heading.bottom
        anchors.bottom: transport.top
        clip: true

        Loader {
            id: previewSurfaceLoader
            anchors.centerIn: parent
            width: root.freeAspectActive
                   ? parent.width
                   : root.fittedFrameWidth(parent.width, parent.height)
            height: root.freeAspectActive
                    ? parent.height
                    : root.fittedFrameHeight(parent.width, parent.height)
            // Do not construct an invisible scene root: it still subscribes to
            // PreviewRuntime::frameStateChanged even when QSG skips painting it.
            active: root.surfaceActive && root.visible && width >= 64 && height >= 64

            sourceComponent: Preview.PreviewSurface {
                anchors.fill: parent
                runtime: root.previewSession.runtime
                mediaHost: root.previewSession.mediaHost
                logger: root.previewSession
                surfaceRole: root.detached ? "detached" : "workspace"
                backgroundColor: "transparent"
                hudTextColor: Theme.colors.previewHud.text
                hudShadowColor: Theme.colors.previewHud.shadow
            }
        }

        Item {
            id: pressureOverlay
            objectName: "previewPressureOverlay"
            anchors.left: previewSurfaceLoader.left
            anchors.right: previewSurfaceLoader.right
            anchors.bottom: previewSurfaceLoader.bottom
            height: Math.min(36, previewSurfaceLoader.height * 0.075)
            visible: previewSurfaceLoader.active && root.documentAvailable && !root.latencyActive
                && !root.exportPageActive && !root.exportSession.pageSessionActive
                && !root.exportSession.introAuditionPlaying && !introActive
                && structure !== null && structure.pressureAvailable && endSecond > 0
            readonly property var structure: root.previewSession.structure || null
            readonly property bool introActive: root.previewSession.runtime
                ? root.previewSession.runtime.introOverlayActive : false
            readonly property real endSecond: root.previewSession.durationSeconds

            function secondAt(x) {
                return Math.max(0, Math.min(1, x / Math.max(1, width))) * endSecond
            }

            onEndSecondChanged: pressureShape.requestPaint()
            onVisibleChanged: if (visible) pressureShape.requestPaint()
            Canvas {
                id: pressureShape
                anchors.fill: parent
                opacity: pressureScrub.containsMouse || pressureScrub.pressed ? 0.28 : 0.16
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    if (!pressureOverlay.visible || width < 1)
                        return
                    const spans = pressureOverlay.structure.heatmapSpans(0, pressureOverlay.endSecond)
                    if (spans.length === 0)
                        return
                    const pixelsPerSecond = width / pressureOverlay.endSecond
                    let previousEnd = -1
                    context.beginPath()
                    for (let i = 0; i < spans.length; i += 3) {
                        const left = spans[i] * pixelsPerSecond
                        const right = spans[i + 1] * pixelsPerSecond
                        const y = height - spans[i + 2] * (height - 1)
                        if (spans[i] > previousEnd) {
                            if (previousEnd >= 0) {
                                context.lineTo(previousEnd * pixelsPerSecond, height)
                                context.closePath()
                            }
                            context.moveTo(left, height)
                            context.lineTo(left, y)
                        } else {
                            // Round the edge only after the change, inside the occupied span.
                            const ramp = Math.min(1.5, (right - left) * 0.25)
                            context.quadraticCurveTo(left, y, left + ramp, y)
                        }
                        context.lineTo(right, y)
                        previousEnd = spans[i + 1]
                    }
                    context.lineTo(previousEnd * pixelsPerSecond, height)
                    context.closePath()
                    context.fillStyle = Qt.rgba(0.87, 0.87, 0.87, 1)
                    context.fill()
                }
            }

            Connections {
                target: pressureOverlay.structure
                function onChanged() {
                    if (pressureOverlay.visible)
                        pressureShape.requestPaint()
                }
            }

            MouseArea {
                id: pressureScrub
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                preventStealing: true
                scrollGestureEnabled: false
                cursorShape: pressed ? Qt.SizeHorCursor : Qt.PointingHandCursor
                property bool resumePlaying: false

                function scrubTo(x) {
                    root.previewSession.updateScrub(pressureOverlay.secondAt(x))
                }

                onPressed: function(mouse) {
                    resumePlaying = root.previewSession.playing
                    root.previewSession.beginScrub()
                    scrubTo(mouse.x)
                }
                onPositionChanged: function(mouse) {
                    if (pressed)
                        scrubTo(mouse.x)
                }
                onReleased: function(mouse) {
                    const second = pressureOverlay.secondAt(mouse.x)
                    root.previewSession.endScrub(second)
                    if (resumePlaying && second < pressureOverlay.endSecond - 0.001) {
                        // The paused transport state is published on the event queue.
                        Qt.callLater(function() {
                            if (pressureOverlay.visible && !pressureScrub.pressed)
                                root.previewSession.playing = true
                        })
                    }
                    resumePlaying = false
                }
                onCanceled: {
                    root.previewSession.endScrub(root.previewSession.positionSeconds)
                    resumePlaying = false
                }
                onWheel: function(wheel) { wheel.accepted = false }
            }
            ToolTip.visible: pressureScrub.containsMouse && !pressureScrub.pressed && ToolTip.text.length > 0
            ToolTip.delay: 500
            ToolTip.text: {
                if (!pressureOverlay.visible || !pressureScrub.containsMouse || pressureScrub.pressed)
                    return ""
                const second = secondAt(pressureScrub.mouseX)
                const p = structure.pressureAt(second)
                if (p.density === undefined)
                    return ""
                return qsTrId("structure.pressure_tooltip")
                    .arg(second.toLocaleString(Qt.locale(), 'f', 3)).arg(Math.round(p.strength * 100))
                    .arg(p.density.toFixed(1)).arg(p.simultaneous.toFixed(1)).arg(p.movement.toFixed(1))
                    .arg(p.rhythm.toFixed(1)).arg(p.endurance.toFixed(1)).arg(p.held.toFixed(1))
                    .arg(p.slides.toFixed(1)).arg(p.hybrid.toFixed(1))
            }
        }

    }

    PreviewTransport {
        id: transport
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: statistics.top
        previewSession: root.previewSession
        preferences: root.preferences
        exportSession: root.exportSession
        exportPageActive: root.exportPageActive
        detached: root.detached
        transportMirrored: root.transportMirrored
        dataAvailable: root.documentAvailable
        enabled: root.documentAvailable
        onDetachRequested: root.detachRequested()
        onDockRequested: root.dockRequested()
    }

    NoteStatistics {
        id: statistics
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        efficientMode: root.preferences.efficientSidebarEnabled
        statistics: root.previewSession.statistics
        dataAvailable: !root.latencyActive && root.previewSession.statisticsAvailable
        enabled: root.documentAvailable
    }
}
