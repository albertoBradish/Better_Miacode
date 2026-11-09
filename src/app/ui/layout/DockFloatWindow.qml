import QtQuick
import QtQuick.Controls
import MiaCode.UI

ApplicationWindow {
    id: root

    required property string panelId
    required property var workspace
    readonly property alias panelHost: contentHost
    readonly property Item backdropSource: floatScene
    property bool ready: false
    property bool sourceEditorFocused: false
    property bool sourceEditorOverlayHeld: false

    title: workspace.panelTitle(panelId)
    flags: Qt.Tool | Qt.WindowTitleHint | Qt.WindowSystemMenuHint | Qt.WindowCloseButtonHint
    transientParent: workspace.Window.window
    visible: !!transientParent?.visible && workspace.isFloating(panelId)
        && workspace.availablePanels.includes(panelId)
    minimumWidth: 240
    minimumHeight: 180
    width: 460
    height: 400

    function restoreGeometry() {
        const geometry = workspace.config.floating[panelId]
        const rect = workspace.preferences.boundedWorkspaceWindowGeometry(Qt.rect(geometry.x, geometry.y,
            Math.max(minimumWidth, geometry.width || 460), Math.max(minimumHeight, geometry.height || 400)))
        x = rect.x
        y = rect.y
        width = rect.width
        height = rect.height
    }

    function geometryChanged() {
        if (ready && visible)
            saveGeometry.restart()
    }

    onVisibleChanged: {
        ready = false
        saveGeometry.stop()
        if (visible) {
            restoreGeometry()
            ready = true
        }
    }
    onXChanged: geometryChanged()
    onYChanged: geometryChanged()
    onWidthChanged: geometryChanged()
    onHeightChanged: geometryChanged()
    onClosing: workspace.closeFloating(panelId)

    Connections {
        target: root.workspace
        function onSavedLayoutApplied() {
            if (root.ready && root.visible) {
                root.ready = false
                root.restoreGeometry()
                root.ready = true
            }
        }
    }

    Timer {
        id: saveGeometry
        interval: 350
        onTriggered: {
            if (root.ready && root.visible)
                root.workspace.updateFloatingGeometry(root.panelId,
                    { x: root.x, y: root.y, width: root.width, height: root.height })
        }
    }

    UiScaleSurface {
        id: floatScene

        WindowBackground { anchors.fill: parent }

        Loader {
            active: root.visible && root.panelId === "editor"
            sourceComponent: root.workspace.editorShortcuts
        }

        DockPanelTab {
            id: tab
            workspace: root.workspace
            panelId: root.panelId
            checked: true
        }

        Item {
            id: contentHost
            anchors.top: tab.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
        }
    }
}
