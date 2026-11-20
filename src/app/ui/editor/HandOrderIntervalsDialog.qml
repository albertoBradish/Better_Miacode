import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

AppDialog {
    id: root
    required property var controller
    required property var applyEdit
    signal editingFinished()
    objectName: "handOrderIntervals"
    title: qsTrId("hand_order.intervals")
    preferredWidth: 640
    preferredHeight: 480
    onClosed: root.editingFinished()
    body: Column {
        spacing: Theme.panelPadding
        Label {
            width: parent.width
            text: qsTrId("hand_order.draft_notice")
            color: Theme.colors.text.secondary
            wrapMode: Text.Wrap
        }
        Label {
            width: parent.width
            text: qsTrId("hand_order.wifi_mode_notice")
            color: Theme.colors.text.secondary
            wrapMode: Text.Wrap
        }
        Repeater {
            model: root.controller.handOrderRows
            delegate: RowLayout {
                id: intervalRow
                required property var modelData
                required property int index
                width: parent.width
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.primary
                    text: modelData.branch < 0
                        ? qsTrId("hand_order.head_row").arg(modelData.line).arg(modelData.col)
                        : modelData.wifiMode === 2
                        ? qsTrId("hand_order.wifi_single_row").arg(modelData.line).arg(modelData.col).arg(modelData.branch + 1)
                        : modelData.sector >= 0
                        ? qsTrId("hand_order.wifi_sector_row").arg(modelData.line).arg(modelData.col)
                            .arg(modelData.branch + 1).arg(modelData.sector + 1)
                            .arg((modelData.begin * 100).toFixed(2)).arg((modelData.end * 100).toFixed(2))
                        : qsTrId("hand_order.track_row").arg(modelData.line).arg(modelData.col)
                            .arg(modelData.branch + 1).arg((modelData.begin * 100).toFixed(2))
                            .arg((modelData.end * 100).toFixed(2))
                }
                AppComboBox {
                    visible: modelData.wifi && modelData.stop === 0 && modelData.sector <= 0
                    Layout.preferredWidth: 140
                    objectName: "handOrderWifiMode_" + intervalRow.index
                    model: [qsTrId("hand_order.wifi_legacy"), qsTrId("hand_order.wifi_split"), qsTrId("hand_order.wifi_single")]
                    currentIndex: modelData.wifiMode
                    onActivated: selected => root.controller.setHandOrderWifiMode(intervalRow.index, selected)
                }
                AppComboBox {
                    objectName: "handOrderIntervalHand_" + intervalRow.index
                    Layout.preferredWidth: 150
                    model: [qsTrId("hand_order.unspecified"), qsTrId("hand_order.left"),
                        qsTrId("hand_order.right"), qsTrId("hand_order.either")]
                    currentIndex: modelData.hand
                    onActivated: selected => root.controller.setHandOrderInterval(intervalRow.index, selected)
                }
            }
        }
    }
    footer: DialogFooter {
        acceptText: qsTrId("hand_order.apply_intervals")
        cancelText: qsTrId("action.close")
        acceptEnabled: root.controller.handOrderCanApply
        onAccepted: {
            if (root.applyEdit()) root.close()
        }
        onRejected: root.close()
    }
}
