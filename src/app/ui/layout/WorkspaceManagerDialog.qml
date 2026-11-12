import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

AppDialog {
    id: root
    required property var workspace
    property string selectedName: ""
    property string pendingAction: ""
    property string pendingName: ""
    readonly property bool hasSelection: workspace.savedNames.includes(selectedName)
    readonly property string proposedName: nameField.text.trim()
    readonly property bool nameAvailable: proposedName.length > 0
        && !workspace.savedNames.includes(proposedName)

    objectName: "workspaceManagerDialog"
    title: qsTrId("workspace.manage")
    preferredWidth: 720
    preferredHeight: 650
    onOpened: {
        if (!hasSelection) selectedName = workspace.savedNames[0] || ""
        nameField.text = selectedName
    }

    function ask(action) {
        pendingAction = action
        pendingName = selectedName
        confirmation.open()
    }

    body: ColumnLayout {
        spacing: Theme.settingsRowSpacing
        Label {
            Layout.fillWidth: true
            text: qsTrId("workspace.manage_hint")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                model: [
                    { id: "default", title: qsTrId("workspace.classic") },
                    { id: "editing", title: qsTrId("workspace.editing") },
                    { id: "preview", title: qsTrId("workspace.preview") },
                    { id: "analysis", title: qsTrId("workspace.analysis") }
                ]
                delegate: AppButton {
                    required property var modelData
                    text: modelData.title
                    onClicked: { root.workspace.applyBuiltin(modelData.id); root.close() }
                }
            }
        }
        Label { text: qsTrId("workspace.saved_presets"); color: Theme.colors.text.heading }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            clip: true
            ListView {
                id: layouts
                model: root.workspace.savedNames
                spacing: 2
                delegate: ItemDelegate {
                    required property string modelData
                    width: layouts.width
                    text: modelData
                    highlighted: root.selectedName === modelData
                    onClicked: { root.selectedName = modelData; nameField.text = modelData }
                }
            }
        }
        Label {
            visible: root.workspace.savedNames.length === 0
            text: qsTrId("workspace.no_saved_presets")
            color: Theme.colors.text.secondary
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            AppButton {
                text: qsTrId("workspace.apply")
                enabled: root.hasSelection && root.workspace.validSavedLayout(root.selectedName)
                onClicked: { root.workspace.applySaved(root.selectedName); root.close() }
            }
            AppButton {
                text: qsTrId("workspace.move_up")
                enabled: root.workspace.savedNames.indexOf(root.selectedName) > 0
                onClicked: root.workspace.preferences.moveWorkspaceLayout(root.selectedName, -1)
            }
            AppButton {
                text: qsTrId("workspace.move_down")
                enabled: root.hasSelection && root.workspace.savedNames.indexOf(root.selectedName) < root.workspace.savedNames.length - 1
                onClicked: root.workspace.preferences.moveWorkspaceLayout(root.selectedName, 1)
            }
            AppButton {
                text: qsTrId("workspace.update_selected")
                enabled: root.hasSelection
                onClicked: root.ask("update")
            }
            AppButton {
                text: qsTrId("workspace.remove")
                enabled: root.hasSelection
                onClicked: root.ask("remove")
            }
        }
        AppTextField {
            id: nameField
            objectName: "workspacePresetName"
            Layout.fillWidth: true
            maximumLength: 48
            placeholderText: qsTrId("workspace.name")
            Accessible.name: placeholderText
        }
        Label {
            Layout.fillWidth: true
            visible: root.proposedName.length > 0 && !root.nameAvailable
            text: qsTrId("workspace.name_exists")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            AppButton {
                text: qsTrId("workspace.save_new")
                enabled: root.nameAvailable
                onClicked: {
                    root.workspace.preferences.saveWorkspaceLayout(root.proposedName, root.workspace.captureLayout())
                    root.selectedName = root.proposedName
                }
            }
            AppButton {
                text: qsTrId("workspace.rename")
                enabled: root.hasSelection && root.nameAvailable
                onClicked: {
                    if (root.workspace.preferences.renameWorkspaceLayout(root.selectedName, root.proposedName))
                        root.selectedName = root.proposedName
                }
            }
            AppButton {
                text: qsTrId("workspace.duplicate")
                enabled: root.hasSelection && root.nameAvailable
                onClicked: {
                    if (root.workspace.preferences.copyWorkspaceLayout(root.selectedName, root.proposedName))
                        root.selectedName = root.proposedName
                }
            }
        }
    }
    footer: DialogFooter {
        cancelText: qsTrId("action.close")
        onRejected: root.close()
    }
    ChoiceDialog {
        id: confirmation
        title: root.pendingAction === "remove" ? qsTrId("workspace.remove") : qsTrId("workspace.update_selected")
        message: (root.pendingAction === "remove" ? qsTrId("workspace.confirm_remove") : qsTrId("workspace.confirm_update")).arg(root.pendingName)
        dismissChoiceId: "cancel"
        choices: [
            { id: "cancel", label: qsTrId("action.cancel"), default: true },
            { id: "apply", label: qsTrId("workspace.apply"), role: "accept" }
        ]
        onChosen: function(choice) {
            if (choice !== "apply" || !root.workspace.savedNames.includes(root.pendingName)) return
            if (root.pendingAction === "remove") {
                root.workspace.preferences.removeWorkspaceLayout(root.pendingName)
                root.selectedName = root.workspace.savedNames[0] || ""
                nameField.text = root.selectedName
            } else root.workspace.preferences.saveWorkspaceLayout(root.pendingName, root.workspace.captureLayout())
        }
    }
}
