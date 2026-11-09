import QtQuick
import QtQuick.Controls
import MiaCode.UI

AppTabButton {
    id: root

    required property string panelId
    required property var workspace
    readonly property var issues: workspace.issueCounts[panelId]
    readonly property int badgeCount: issues ? issues.errors || issues.warnings : 0

    objectName: "workspacePanelTab_" + panelId
    text: workspace.panelTitle(panelId)
    ToolTip.visible: hovered && !drag.active
    ToolTip.text: qsTrId("workspace.drag_panel")
    Accessible.description: badgeCount > 0 ? String(badgeCount) : ToolTip.text
    onClicked: workspace.activate(panelId)
    accessory: badgeCount > 0 ? badge : null

    DockPanelDragHandler {
        id: drag
        panelId: root.panelId
        workspace: root.workspace
    }

    Component {
        id: badge
        Rectangle {
            implicitHeight: badgeLabel.font.pixelSize + 5
            implicitWidth: Math.max(implicitHeight, Math.ceil(badgeLabel.implicitWidth) + 8)
            radius: height / 2
            color: root.issues?.errors > 0 ? Theme.colors.danger.primary : Theme.colors.accent.badge

            Text {
                id: badgeLabel
                anchors.fill: parent
                text: String(root.badgeCount)
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize - 2
                font.preferTypoLineMetrics: true
                color: Theme.colors.text.onAccent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
