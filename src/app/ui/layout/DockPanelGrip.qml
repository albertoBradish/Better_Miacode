import QtQuick
import QtQuick.Controls
import MiaCode.UI

Item {
    id: root

    required property string panelId
    required property var workspace

    objectName: "workspacePanelGrip_" + panelId
    width: 36
    height: 14
    z: 2
    Accessible.name: workspace.panelTitle(panelId)
    Accessible.description: qsTrId("workspace.drag_panel")
    ToolTip.visible: hover.hovered && !drag.active
    ToolTip.text: Accessible.description

    HoverHandler {
        id: hover
        cursorShape: Qt.OpenHandCursor
    }

    DockPanelDragHandler {
        id: drag
        panelId: root.panelId
        workspace: root.workspace
    }

    Rectangle {
        anchors.centerIn: parent
        width: 22
        height: 10
        radius: height / 2
        visible: hover.hovered || drag.active
        color: Theme.colors.background.panel
        border.color: Theme.separatorColor

        Row {
            anchors.centerIn: parent
            spacing: 2

            Repeater {
                model: 3
                Rectangle {
                    width: 2
                    height: 2
                    radius: 1
                    color: Theme.colors.text.secondary
                }
            }
        }
    }
}
