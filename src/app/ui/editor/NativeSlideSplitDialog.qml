import QtQuick
import QtQuick.Controls
import MiaCode.UI

AppDialog {
    id: root
    required property var controller
    required property var applyEdit
    signal editingFinished()
    objectName: "nativeSlideSplitDialog"
    title: qsTrId("hand_order.split_native_slide")
    preferredWidth: 820
    preferredHeight: 640
    onClosed: {
        controller.cancelNativeSlideSplit()
        editingFinished()
    }
    body: Column {
        spacing: Theme.panelPadding
        Label {
            width: parent.width
            text: qsTrId("hand_order.native.notice")
            color: Theme.colors.text.secondary
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "nativeSlideSplitSummary"
            width: parent.width
            text: root.controller.nativeSlideSplitMessage
            color: Theme.colors.text.primary
            wrapMode: Text.Wrap
        }
        Repeater {
            model: root.controller.nativeSlideSplitRows
            delegate: Column {
                required property var modelData
                required property int index
                width: parent.width
                spacing: Theme.panelPadding / 2
                Label {
                    width: parent.width
                    text: modelData.line + ":" + modelData.col + " · " + modelData.statusText
                    color: Theme.colors.text.primary
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    visible: modelData.reason.length > 0
                    text: modelData.reason
                    color: Theme.colors.text.secondary
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    visible: modelData.status === "ready"
                    text: modelData.counts
                    color: Theme.colors.text.secondary
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    text: qsTrId("hand_order.native.before")
                    color: Theme.colors.text.secondary
                }
                AppTextArea {
                    objectName: "nativeSlideSplitBefore_" + index
                    width: parent.width
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: modelData.before
                    selectByMouse: true
                }
                Label {
                    width: parent.width
                    visible: modelData.status === "ready" && modelData.after.length > 0
                    text: qsTrId("hand_order.native.after")
                    color: Theme.colors.text.secondary
                }
                AppTextArea {
                    objectName: "nativeSlideSplitAfter_" + index
                    width: parent.width
                    visible: modelData.status === "ready" && modelData.after.length > 0
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: modelData.after
                    selectByMouse: true
                }
                Label {
                    objectName: "nativeSlideSplitChanges_" + index
                    width: parent.width
                    visible: modelData.changes.length > 0
                    text: modelData.changes
                    color: Theme.colors.text.secondary
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    footer: DialogFooter {
        acceptText: qsTrId("hand_order.native.apply")
        cancelText: qsTrId("action.cancel")
        acceptEnabled: root.controller.nativeSlideSplitCanApply
        onAccepted: if (root.applyEdit()) root.close()
        onRejected: root.close()
    }
}
