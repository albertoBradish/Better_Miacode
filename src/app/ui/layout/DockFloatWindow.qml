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
    flags: Qt.Tool | Qt.WindowTitleHint | Qt.WindowSystemMenuHint | Qt.WindowCloseButtonHint | Qt.WindowMaximizeButtonHint
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
        if (ready && visible && visibility === Window.Windowed)
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
    onActiveChanged: if (active && visible) workspace.activePanel = panelId
    onClosing: {
        if (visibility === Window.Windowed)
            workspace.updateFloatingGeometry(panelId, { x: x, y: y, width: width, height: height })
        workspace.setPanelShown(panelId, false)
    }

    Shortcut {
        sequence: "`"
        autoRepeat: false
        enabled: root.visible && root.workspace.keyboardAllowed(root)
        onActivated: root.workspace.togglePanelMaximization(root.panelId, root)
    }

    Connections {
        target: root.workspace
        function onSavedLayoutApplied() {
            if (root.ready && root.visible) {
                root.ready = false
                if (root.visibility !== Window.Windowed) root.showNormal()
                root.restoreGeometry()
                root.ready = true
            }
        }
    }

    Timer {
        id: saveGeometry
        interval: 350
        onTriggered: {
            if (root.ready && root.visible && root.visibility === Window.Windowed)
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

        DockPanelGrip {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            workspace: root.workspace
            panelId: root.panelId
        }

        Item {
            id: contentHost
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
        }
    }
}
