import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Rectangle {
    id: root

    property string documentName: ""
    property int cursorLine: 1
    property int cursorColumn: 1
    property string selectionBeatText: ""
    property string selectionBeatTooltip: ""
    property string handOrderBackgroundText: ""
    property string handOrderBackgroundDetail: ""
    property var handOrderService: null
    property bool metadataActive: false
    property bool difficultyActive: false
    property bool updateAvailable: false
    property string updateVersion: ""

    signal updateActivated()

    implicitHeight: 23
    color: Theme.surfaceColor(Theme.colors.background.statusBar)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1 / (Screen.devicePixelRatio * Theme.uiScale)
        color: Theme.separatorColor
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 14

        StatusText {
            Layout.fillWidth: true
            text: root.documentName
            visible: text.length > 0
            elide: Text.ElideMiddle
        }
        Item { Layout.fillWidth: root.documentName.length === 0 }

        StatusText {
            id: updateStatus
            objectName: "statusBarUpdateBadge"
            visible: root.updateAvailable
            text: qsTrId("status.update_available").arg(root.updateVersion)
            color: Theme.colors.text.status
            Layout.preferredWidth: implicitWidth

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.updateActivated()
            }
        }

        StatusText {
            objectName: "statusBarHandOrderBackground"
            visible: root.difficultyActive && text.length > 0
            text: root.handOrderBackgroundText
            tooltipText: root.handOrderBackgroundDetail
            elide: Text.ElideRight
            Layout.minimumWidth: 0
            Layout.preferredWidth: Math.min(implicitWidth, Math.max(0, root.width
                - (root.handOrderService && root.handOrderService.backgroundActive ? 168 : 100)
                - cursorStatus.implicitWidth - beatStatus.implicitWidth - formatStatus.implicitWidth
                - (updateStatus.visible ? updateStatus.implicitWidth : 0)
                - (metadataStatus.visible ? metadataStatus.implicitWidth : 0)))
            Layout.maximumWidth: 190
        }
        IconButton {
            objectName: "statusBarHandOrderPause"
            visible: root.difficultyActive && root.handOrderService && root.handOrderService.backgroundActive
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            compact: true
            iconSource: Qt.resolvedUrl(root.handOrderService && root.handOrderService.paused ? "icons/play.svg" : "icons/pause.svg")
            tooltip: root.handOrderService && root.handOrderService.paused
                ? qsTrId("hand_order.compute.resume") : qsTrId("hand_order.compute.pause")
            onClicked: {
                if (root.handOrderService.paused) root.handOrderService.resume()
                else root.handOrderService.pause()
            }
        }
        IconButton {
            objectName: "statusBarHandOrderStop"
            visible: root.difficultyActive && root.handOrderService && root.handOrderService.backgroundActive
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            compact: true
            iconSource: Qt.resolvedUrl("icons/stop.svg")
            tooltip: qsTrId("hand_order.compute.exit")
            onClicked: root.handOrderService.cancel()
        }
        StatusText {
            id: metadataStatus
            text: qsTrId("dialog.unsaved_field_changes.field.metadata")
            visible: root.metadataActive
            Layout.preferredWidth: implicitWidth
        }
        StatusText {
            id: cursorStatus
            objectName: "statusBarCursor"
            text: qsTrId("qml.line_1_column_2").arg(root.cursorLine).arg(root.cursorColumn)
            visible: root.difficultyActive
            Layout.preferredWidth: implicitWidth
        }
        StatusText {
            id: beatStatus
            objectName: "statusBarSelectionBeats"
            text: root.selectionBeatText
            tooltipText: root.selectionBeatTooltip
            visible: root.difficultyActive && text.length > 0
            Layout.preferredWidth: implicitWidth
        }
        StatusText {
            id: formatStatus
            text: "simai"
            visible: root.difficultyActive
            Layout.preferredWidth: implicitWidth
        }
    }

    component StatusText: Text {
        id: statusText
        property string tooltipText: ""
        color: enabled ? Theme.colors.text.status : Theme.colors.text.disabled
        font.family: Theme.uiFont
        font.pixelSize: Theme.secondaryFontSize

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            visible: statusText.tooltipText.length > 0
            ToolTip.visible: containsMouse
            ToolTip.text: statusText.tooltipText
        }
    }
}
