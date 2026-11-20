import QtQuick
import QtQuick.Window
import QtQml.Models
import QtQuick.Controls
import MiaCode.UI

Item {
    id: root
    required property var comparisons
    required property var workspaceWindow
    required property var dock
    required property Item primaryPane
    property bool comparisonsVisible: true
    property real canvasRatio: 1
    property bool freeAspect: false
    function screenPan(globalX, screen) {
        if (!screen) return 0
        const screens = Qt.application.screens
        let left = screen.virtualX, right = left + screen.width
        for (const s of screens) {
            left = Math.min(left, s.virtualX)
            right = Math.max(right, s.virtualX + s.width)
        }
        return Math.max(-1, Math.min(1, 2 * (globalX - left) / Math.max(1, right - left) - 1))
    }
    Binding {
        target: root.comparisons
        property: "presentationActive"
        value: !!root.workspaceWindow?.visible && root.comparisonsVisible
        when: !!root.comparisons
    }
    HoverHandler {
        parent: root.primaryPane
        onHoveredChanged: if (hovered) root.comparisons?.focusMainAudio()
    }
    Timer {
        interval: 100
        running: !!root.comparisons?.mixAudio && root.comparisons.count > 0 && root.comparisonsVisible
        repeat: true; triggeredOnStart: true
        onTriggered: root.comparisons.setMainPan(root.screenPan(root.primaryPane.mapToGlobal(root.primaryPane.width / 2, 0).x, root.primaryPane.Window.window?.screen))
    }
    Instantiator {
        model: root.comparisons
        delegate: ComparisonPreviewTile {
            id: tile
            objectName: "comparisonPreviewPanel"
            parent: comparison ? root.dock.hostFor(comparison.panelId) : null
            anchors.fill: parent
            visible: root.comparisonsVisible
            surfaceActive: visible && !!parent?.visible
            syncAvailable: root.comparisons?.synchronize ?? false
            canvasRatio: root.canvasRatio
            freeAspect: root.freeAspect
            HoverHandler { onHoveredChanged: tile.comparison?.focusAudio(hovered) }
            Timer {
                interval: 100; repeat: true; triggeredOnStart: true
                running: tile.visible && !!root.comparisons?.mixAudio
                onTriggered: tile.comparison?.setScreenPan(root.screenPan(tile.mapToGlobal(tile.width / 2, 0).x, tile.Window.window?.screen))
            }
        }
    }
}
