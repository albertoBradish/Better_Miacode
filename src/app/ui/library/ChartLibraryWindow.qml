import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ApplicationWindow {
    id: root
    required property var library
    property var editingSession: null
    property bool synchronizing: false
    property bool pendingEdit: false
    title: qsTrId("library.title")
    width: 1180; height: 780
    minimumWidth: 780; minimumHeight: 520
    visible: false
    color: Theme.colors.background.surface
    function open() { show(); raise(); requestActivate(); syncSession() }
    function flush() {
        editTimer.stop()
        if (editingSession && !synchronizing) {
            synchronizing = true
            const accepted = editingSession.replaceText(rawEditor.text)
            synchronizing = false
            if (!accepted) return false
        }
        pendingEdit = false
        return true
    }
    function syncSession() {
        if (synchronizing || (editingSession === library.activeSession && pendingEdit)) return
        if (editingSession !== library.activeSession) {
            if (!flush()) return
            editingSession = library.activeSession
        }
        if (rawEditor.text !== (editingSession ? editingSession.text : "")) {
            synchronizing = true
            rawEditor.text = editingSession ? editingSession.text : ""
            synchronizing = false
        }
    }
    onClosing: close => { if (!flush()) close.accepted = false }
    Shortcut { sequences: [StandardKey.Close]; onActivated: root.close() }
    Timer { id: editTimer; interval: 300; onTriggered: root.flush() }
    Connections { target: root.library; function onChanged() { root.syncSession() } }
    Connections { target: root.editingSession; function onChanged() { if (!editTimer.running) root.syncSession() } }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Theme.dialogPadding; spacing: 8
        RowLayout {
            Layout.fillWidth: true
            AppButton { text: qsTrId("library.choose_root"); enabled: !root.library.busy; onClicked: root.library.chooseRoot() }
            AppButton { text: qsTrId("library.refresh"); enabled: !root.library.busy && root.library.rootPath.length > 0; onClicked: root.library.refresh() }
            AppButton { text: qsTrId("library.cancel"); enabled: root.library.busy; onClicked: root.library.cancel() }
            Label { Layout.fillWidth: true; text: root.library.rootPath; textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.colors.text.secondary }
        }
        Label { Layout.fillWidth: true; text: qsTrId("library.help"); wrapMode: Text.Wrap; color: Theme.colors.text.secondary }
        RowLayout {
            Layout.fillWidth: true
            AppTextField { Layout.fillWidth: true; placeholderText: qsTrId("library.search"); text: root.library.query; onTextChanged: root.library.query = text }
            AppCheckBox { text: qsTrId("library.duplicates"); checked: root.library.duplicatesOnly; onToggled: root.library.duplicatesOnly = checked }
            Label { text: qsTrId("library.count").arg(root.library.totalCount).arg(root.library.selectedCount); color: Theme.colors.text.secondary }
        }
        SplitView {
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                SplitView.preferredWidth: 500; SplitView.minimumWidth: 300
                RowLayout {
                    AppButton { text: qsTrId("library.select_visible"); enabled: !root.library.busy; onClicked: root.library.selectVisible(true) }
                    AppButton { text: qsTrId("library.clear_visible"); enabled: !root.library.busy; onClicked: root.library.selectVisible(false) }
                }
                ListView {
                    id: charts
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; model: root.library; spacing: 4
                    ScrollBar.vertical: AppScrollBar {}
                    delegate: Rectangle {
                        id: entry
                        required property string chartPath
                        required property string chartTitle
                        required property string artist
                        required property string levels
                        required property string chartVersion
                        required property string sha256
                        required property int copies
                        required property bool selected
                        width: charts.width; height: 116
                        color: Theme.colors.background.panel; radius: Theme.controlRadius
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 6; spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                AppCheckBox { enabled: !root.library.busy; checked: entry.selected; onToggled: root.library.selectPath(entry.chartPath, checked) }
                                Label { Layout.fillWidth: true; text: entry.chartTitle || entry.chartPath; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.colors.text.primary }
                                Label { visible: entry.copies > 1; text: "×" + entry.copies; color: Theme.colors.text.secondary }
                            }
                            Label { Layout.fillWidth: true; text: entry.artist + "  ·  " + entry.levels + "  ·  " + entry.chartVersion; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.colors.text.secondary }
                            Label { Layout.fillWidth: true; text: entry.chartPath; textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.colors.text.secondary }
                            RowLayout {
                                AppButton { text: qsTrId("library.edit_buffer"); onClicked: { if (root.flush()) root.library.openBuffer(entry.chartPath) } }
                                AppButton { text: qsTrId("library.open_editor"); onClicked: { if (root.flush()) root.library.openInEditor(entry.chartPath) } }
                                Label { Layout.fillWidth: true; text: "SHA-256 " + entry.sha256.slice(0, 12); color: Theme.colors.text.secondary }
                            }
                        }
                    }
                }
            }
            ColumnLayout {
                SplitView.fillWidth: true; SplitView.minimumWidth: 300
                Flow {
                    Layout.fillWidth: true; spacing: 4
                    Repeater {
                        model: root.library.sessions
                        delegate: AppButton {
                            required property var modelData
                            required property int index
                            text: (modelData.dirty ? "* " : "") + modelData.path.split(/[\\/]/).slice(-2).join("/")
                            emphasized: modelData === root.editingSession
                            onClicked: { if (root.flush()) root.library.activateBuffer(index) }
                        }
                    }
                }
                RowLayout {
                    AppButton { text: qsTrId("library.save"); enabled: !!root.editingSession; onClicked: { if (root.flush()) root.library.saveActive() } }
                    AppButton { text: qsTrId("library.undo"); enabled: !!root.editingSession && root.editingSession.canUndo; onClicked: { if (root.flush()) root.editingSession.undo() } }
                    AppButton { text: qsTrId("library.redo"); enabled: !!root.editingSession && root.editingSession.canRedo; onClicked: { if (root.flush()) root.editingSession.redo() } }
                    AppButton { text: qsTrId("library.close_buffer"); enabled: !!root.editingSession; onClicked: { if (root.flush()) root.library.closeActive() } }
                }
                Label { Layout.fillWidth: true; text: qsTrId("library.buffer_help"); wrapMode: Text.Wrap; color: Theme.colors.text.secondary }
                Label { Layout.fillWidth: true; text: root.editingSession ? root.editingSession.error : ""; visible: text.length > 0; wrapMode: Text.Wrap; color: Theme.colors.text.primary }
                ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    TextArea {
                        id: rawEditor
                        enabled: !!root.editingSession
                        textFormat: TextEdit.PlainText; wrapMode: TextEdit.NoWrap
                        color: Theme.colors.text.primary; font.family: "Consolas"
                        onTextChanged: if (!root.synchronizing && activeFocus) { root.pendingEdit = true; editTimer.restart() }
                        Keys.onPressed: event => {
                            if ((event.modifiers & Qt.ControlModifier) && root.editingSession && (event.key === Qt.Key_Z || event.key === Qt.Key_Y)) {
                                if (!root.flush()) return
                                if (event.key === Qt.Key_Y || (event.modifiers & Qt.ShiftModifier)) root.editingSession.redo()
                                else root.editingSession.undo()
                                event.accepted = true
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            AppButton { text: qsTrId("library.prepare"); enabled: !root.library.busy && root.library.selectedCount > 0; onClicked: { if (root.flush()) root.library.prepareNormalization() } }
            AppButton { text: qsTrId("library.review").arg(root.library.proposals.length); enabled: root.library.proposals.length > 0; onClicked: review.open() }
            AppButton { text: qsTrId("library.undo_batch"); enabled: root.library.canUndoBatch; onClicked: root.library.undoBatch() }
            BusyIndicator { running: root.library.busy; visible: running; Layout.preferredWidth: 30; Layout.preferredHeight: 30 }
        }
        Label { Layout.fillWidth: true; text: root.library.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.colors.text.secondary }
    }
    AppDialog {
        id: review
        parent: Overlay.overlay
        title: qsTrId("library.review_title")
        preferredWidth: 1000; preferredHeight: 650
        body: ColumnLayout {
            Label { Layout.fillWidth: true; text: qsTrId("library.review_help"); wrapMode: Text.Wrap; color: Theme.colors.text.secondary }
            Repeater {
                model: root.library.proposals
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; text: modelData.path; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.colors.text.primary }
                    RowLayout {
                        Layout.fillWidth: true
                        TextArea { Layout.fillWidth: true; Layout.preferredWidth: 1; text: modelData.before; textFormat: TextEdit.PlainText; readOnly: true; wrapMode: TextEdit.Wrap; color: Theme.colors.text.secondary }
                        TextArea { Layout.fillWidth: true; Layout.preferredWidth: 1; text: modelData.after; textFormat: TextEdit.PlainText; readOnly: true; wrapMode: TextEdit.Wrap; color: Theme.colors.text.primary }
                    }
                }
            }
        }
        footer: DialogFooter {
            cancelText: qsTrId("library.cancel")
            acceptText: qsTrId("library.apply")
            onRejected: review.close()
            onAccepted: { review.close(); root.library.applyPrepared() }
        }
    }
}
