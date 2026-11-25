pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI
import "WorkbenchLayouts.js" as Layouts

AppDialog {
    id: root
    required property var workspace
    property string selectedKey: "default"
    property var currentLayout: null
    property string pendingAction: ""
    property string pendingName: ""
    readonly property var roman: ["I", "II", "III"]
    readonly property bool custom: selectedKey.startsWith("saved:")
    readonly property string selectedName: custom ? selectedKey.slice(6) : ""
    readonly property string proposedName: nameField.text.trim()
    readonly property bool newName: proposedName.length > 0 && !workspace.savedNames.includes(proposedName)
    readonly property bool validEntry: workspace.layoutKeys.includes(selectedKey)
        && (!custom || workspace.validSavedLayout(selectedName))
    readonly property var selectedLayout: custom ? workspace.preferences.savedWorkspaceLayouts[selectedName]
        : { tree: workspace.builtinTree(selectedKey) }

    objectName: "workspaceManagerDialog"
    title: qsTrId("workspace.manage")
    preferredWidth: 760
    preferredHeight: 570
    fillBody: true
    onOpened: {
        currentLayout = workspace.captureLayout()
        select(workspace.layoutKeys.includes(selectedKey) ? selectedKey : "default")
    }

    function select(key) {
        selectedKey = key
        nameField.text = selectedName
        slotPicker.currentIndex = custom ? workspace.slotKeys.indexOf(key) + 1 : 0
    }
    function ask(action) {
        pendingAction = action
        pendingName = selectedName
        confirmation.open()
    }

    component Miniature: ColumnLayout {
        id: miniature
        required property string caption
        required property var snapshot
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        spacing: 4
        Label {
            text: miniature.caption
            color: Theme.colors.text.secondary
        }
        Rectangle {
            id: frame
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(80, Math.min(135, width * 0.55))
            clip: true
            color: Theme.colors.background.panel
            Repeater {
                model: Layouts.previewCells(miniature.snapshot, root.workspace.panelIds)
                delegate: Rectangle {
                    required property var modelData
                    x: frame.width * modelData.x + 1
                    y: frame.height * modelData.y + 1
                    width: Math.max(0, frame.width * modelData.w - 2)
                    height: Math.max(0, frame.height * modelData.h - 2)
                    radius: 3
                    color: Theme.colors.background.elevated
                    border.color: Theme.separatorColor
                    Label {
                        anchors.fill: parent
                        anchors.margins: 2
                        text: root.workspace.panelTitle(parent.modelData.panel)
                        color: Theme.colors.text.secondary
                        font.pixelSize: Theme.secondaryFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        clip: true
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            readonly property var floating: Object.keys(miniature.snapshot?.floating ?? {})
                .concat(miniature.snapshot?.previewDetached ? ["preview"] : [])
            text: floating.length ? qsTrId("workspace.floating_panels").arg(floating.map(id => root.workspace.panelTitle(id)).join(", ")) : ""
            visible: !!text
            color: Theme.colors.text.secondary
            elide: Text.ElideRight
        }
    }

    body: ColumnLayout {
        spacing: Theme.settingsRowSpacing
        ListView {
            id: list
            objectName: "workspaceEntryList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 110
            clip: true
            model: root.workspace.layoutKeys
            ScrollBar.vertical: AppScrollBar {}
            property int dropRow: -1
            delegate: ChromeRow {
                id: entry
                required property int index
                required property string modelData
                readonly property int shortcut: root.workspace.slotKeys.indexOf(modelData)
                width: list.width - 8
                selected: root.selectedKey === modelData
                text: root.workspace.layoutTitle(modelData)
                Accessible.name: text
                onClicked: root.select(modelData)
                Keys.onPressed: event => {
                    if (!(event.modifiers & Qt.ControlModifier)
                            || (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)) return
                    root.workspace.preferences.moveWorkspaceEntry(modelData, index + (event.key === Qt.Key_Up ? -1 : 1))
                    event.accepted = true
                }
                contentItem: RowLayout {
                    Label {
                        Layout.fillWidth: true
                        text: entry.text
                        textFormat: Text.PlainText
                        color: Theme.colors.text.primary
                        elide: Text.ElideRight
                    }
                    Label {
                        text: entry.shortcut < 0 ? "" : root.roman[entry.shortcut]
                        color: Theme.colors.text.secondary
                    }
                    Label {
                        Layout.preferredWidth: 28
                        Layout.fillHeight: true
                        text: "≡"
                        color: Theme.colors.text.secondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        MouseArea {
                            id: handle
                            anchors.fill: parent
                            preventStealing: true
                            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                            function targetRow() {
                                const p = mapToItem(list.contentItem, mouseX, mouseY)
                                return Math.max(0, Math.min(list.count - 1, Math.floor(p.y / entry.height)))
                            }
                            onPressed: {
                                root.select(entry.modelData)
                                list.dropRow = entry.index
                            }
                            onPositionChanged: if (pressed) list.dropRow = targetRow()
                            onReleased: {
                                const destination = targetRow()
                                list.dropRow = -1
                                root.workspace.preferences.moveWorkspaceEntry(entry.modelData, destination)
                            }
                            onCanceled: list.dropRow = -1
                            Timer {
                                interval: 65
                                repeat: true
                                running: handle.pressed
                                onTriggered: {
                                    const y = handle.mapToItem(list, handle.mouseX, handle.mouseY).y
                                    if (y >= 22 && y <= list.height - 22) return
                                    list.contentY = Math.max(0, Math.min(list.contentHeight - list.height,
                                        list.contentY + (y < 22 ? -entry.height : entry.height)))
                                    list.dropRow = handle.targetRow()
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 2
                    color: Theme.colors.text.active
                    visible: list.dropRow === entry.index
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.panelPadding
            Miniature { caption: qsTrId("workspace.current_layout"); snapshot: root.currentLayout }
            Miniature { caption: qsTrId("workspace.selected_layout"); snapshot: root.selectedLayout }
        }
        RowLayout {
            Layout.fillWidth: true
            AppTextField {
                id: nameField
                objectName: "workspacePresetName"
                Layout.minimumWidth: 32
                Layout.fillWidth: true
                maximumLength: 48
                placeholderText: qsTrId("workspace.name")
                Accessible.name: placeholderText
            }
            AppComboBox {
                id: slotPicker
                model: [qsTrId("workspace.unbound")].concat(root.roman)
                Accessible.name: qsTrId("workspace.shortcuts")
            }
            AppButton {
                text: root.custom && root.proposedName === root.selectedName
                    ? qsTrId("workspace.update_selected") : qsTrId("workspace.save_new")
                enabled: root.newName || (root.custom && root.proposedName === root.selectedName)
                onClicked: {
                    if (!root.newName) { root.ask("update"); return }
                    const name = root.proposedName
                    root.workspace.preferences.saveWorkspaceLayout(name, root.currentLayout, slotPicker.currentIndex - 1)
                    root.select("saved:" + name)
                }
            }
            IconButton {
                id: moreButton
                glyph: "⋯"
                tooltip: qsTrId("qml.more")
                enabled: root.custom
                onClicked: moreMenu.openAt(moreButton)
                AppMenu {
                    id: moreMenu
                    AppMenuItem {
                        text: qsTrId("workspace.rename")
                        enabled: root.newName
                        onTriggered: {
                            const name = root.proposedName
                            if (root.workspace.preferences.renameWorkspaceLayout(root.selectedName, name))
                                root.select("saved:" + name)
                        }
                    }
                    AppMenuItem {
                        text: qsTrId("workspace.duplicate")
                        enabled: root.newName
                        onTriggered: {
                            const name = root.proposedName
                            if (root.workspace.preferences.copyWorkspaceLayout(root.selectedName, name))
                                root.select("saved:" + name)
                        }
                    }
                    AppMenuItem { text: qsTrId("workspace.remove"); onTriggered: root.ask("remove") }
                }
            }
        }
    }
    footer: DialogFooter {
        cancelText: qsTrId("action.close")
        acceptText: qsTrId("workspace.apply")
        acceptEnabled: root.validEntry
        onAccepted: { root.workspace.applyEntry(root.selectedKey); root.close() }
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
                root.select(root.workspace.layoutKeys[0])
            } else {
                root.workspace.preferences.saveWorkspaceLayout(root.pendingName, root.currentLayout, slotPicker.currentIndex - 1)
            }
        }
    }
}
