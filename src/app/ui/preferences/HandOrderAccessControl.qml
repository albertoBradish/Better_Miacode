import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    required property var preferencesModel
    property real minimumRowHeight: Theme.controlMinHeight
    property string handOrderStorageError: ""
    AppDialog {
        id: unlockDialog
        objectName: "handOrderPasswordDialog"
        title: qsTrId("hand_order.features.enable")
        preferredHeight: 245
        property string errorText: ""
        function submit() {
            if (!passwordField.text.length) return
            errorText = root.preferencesModel.unlockHandOrderFeatures(passwordField.text)
            if (!errorText.length) close()
            else {
                passwordField.selectAll()
                passwordField.forceActiveFocus()
            }
        }
        onOpened: passwordField.forceActiveFocus()
        onAboutToHide: {
            passwordField.clear()
            errorText = ""
        }
        body: ColumnLayout {
            spacing: Theme.panelPadding
            Label {
                Layout.fillWidth: true
                text: qsTrId("hand_order.features.password_notice")
                wrapMode: Text.Wrap
                color: Theme.colors.text.primary
            }
            AppTextField {
                id: passwordField
                objectName: "handOrderPasswordField"
                Layout.fillWidth: true
                placeholderText: qsTrId("net.ui.password")
                Accessible.name: placeholderText
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onTextEdited: unlockDialog.errorText = ""
                onAccepted: unlockDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: unlockDialog.errorText.length > 0
                text: unlockDialog.errorText
                color: Theme.colors.danger.primary
                wrapMode: Text.Wrap
            }
        }
        footer: DialogFooter {
            acceptText: qsTrId("action.ok")
            cancelText: qsTrId("action.cancel")
            acceptEnabled: passwordField.text.length > 0
            onAccepted: unlockDialog.submit()
            onRejected: unlockDialog.close()
        }
    }

    AppSwitch {
        Layout.minimumHeight: root.minimumRowHeight
        objectName: "preferencesHandOrderFeaturesEnabled"
        text: qsTrId("hand_order.features.enable")
        checked: root.preferencesModel.handOrderFeaturesEnabled
        onToggled: {
            const enabling = checked
            checked = Qt.binding(() => root.preferencesModel.handOrderFeaturesEnabled)
            root.handOrderStorageError = ""
            if (enabling) unlockDialog.open()
            else if (!root.preferencesModel.disableHandOrderFeatures())
                root.handOrderStorageError = qsTrId("hand_order.features.storage_error")
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.handOrderStorageError.length > 0
        text: root.handOrderStorageError
        color: Theme.colors.danger.primary
        wrapMode: Text.Wrap
    }
}
