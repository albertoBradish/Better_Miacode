import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root
    required property var library
    property var editingSession: null
    property bool synchronizing: false
    property bool pendingEdit: false
    property bool viewingSource: false
    readonly property bool editing: viewingSource && !!editingSession
    readonly property Item cornerSourceItem: songs.cornerSourceItem
    signal editorRequested()
    clip: true
    function flush() { return flushRaw() && songs.stage() }
    function flushRaw() {
        editTimer.stop()
        if (pendingEdit && editingSession && !synchronizing) {
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
            if (!flushRaw()) return
            editingSession = library.activeSession
        }
        if (rawEditor.text !== (editingSession ? editingSession.text : "")) {
            synchronizing = true
            rawEditor.text = editingSession ? editingSession.text : ""
            synchronizing = false
        }
    }
    Timer { id: editTimer; interval: 300; onTriggered: root.flush() }
    Connections {
        target: root.library
        function onChanged() { root.syncSession() }
        function onEditorOpened() { root.editorRequested() }
    }
    Connections { target: root.editingSession; function onChanged() { if (!editTimer.running) root.syncSession() } }
    Item {
        anchors.fill: parent
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.panelPadding
            spacing: Theme.settingsRowSpacing
            RowLayout {
                visible: !!root.editingSession
                AppButton {
                    text: qsTrId("library.browse")
                    selected: !root.editing
                    onClicked: if (root.flush()) { root.viewingSource = false; songs.resume() }
                }
                AppButton {
                    text: qsTrId("library.source_tabs").arg(root.library.sessions.length)
                    selected: root.editing
                    onClicked: root.viewingSource = true
                }
            }
            ChartLibrarySongManager {
                id: songs
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !root.editing
                library: root.library
                onEditSourceRequested: path => {
                    if (!root.flush()) return
                    root.library.openBuffer(path)
                    root.viewingSource = root.library.activeSession?.path === path
                }
            }
            ColumnLayout {
                visible: root.editing
                Layout.fillWidth: true
                Layout.fillHeight: true
                Flow {
                    id: sessionTabs
                    Layout.fillWidth: true; spacing: 4
                    Repeater {
                        model: root.library.sessions
                        delegate: AppButton {
                            required property var modelData
                            required property int index
                            text: {
                                root.library.totalCount
                                return (modelData.dirty ? "* " : "") + root.library.chartCaption(modelData.path)
                            }
                            width: Math.min(implicitWidth, sessionTabs.width)
                            selected: modelData === root.editingSession
                            onClicked: { if (root.flush()) root.library.activateBuffer(index) }
                        }
                    }
                }
                RowLayout {
                    AppButton { text: qsTrId("library.save"); enabled: root.editingSession && (root.editingSession.dirty || root.pendingEdit) && !root.library.busy; onClicked: { if (root.flush()) root.library.saveActive() } }
                    AppButton { text: qsTrId("library.undo"); enabled: !!root.editingSession && root.editingSession.canUndo; onClicked: { if (root.flush()) root.editingSession.undo() } }
                    AppButton { text: qsTrId("library.redo"); enabled: !!root.editingSession && root.editingSession.canRedo; onClicked: { if (root.flush()) root.editingSession.redo() } }
                    AppButton { text: qsTrId("library.close_buffer"); enabled: !!root.editingSession; onClicked: { if (root.flush()) root.library.closeActive() } }
                }
                Label { Layout.fillWidth: true; text: root.editingSession ? root.editingSession.error : ""; visible: text.length > 0; wrapMode: Text.Wrap; color: Theme.colors.text.primary }
                ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    TextArea {
                        id: rawEditor
                        enabled: !!root.editingSession
                        textFormat: TextEdit.PlainText; wrapMode: TextEdit.NoWrap
                        color: Theme.colors.text.primary; font.family: "Consolas"
                        onActiveFocusChanged: if (!activeFocus) root.flush()
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
            Flow {
                Layout.fillWidth: true
                spacing: 8
                visible: !root.editing && songs.batchMode && (root.library.selectedCount > 0 || root.library.canUndoBatch)
                AppButton {
                    visible: root.library.selectedCount > 0
                    text: qsTrId("library.prepare")
                    enabled: !root.library.busy
                    onClicked: if (root.flush()) root.library.prepareNormalization()
                }
                AppButton {
                    visible: root.library.proposals.length > 0
                    text: qsTrId("library.review").arg(root.library.proposals.length)
                    onClicked: review.open()
                }
                AppButton {
                    visible: root.library.canUndoBatch
                    text: qsTrId("library.undo_batch")
                    onClicked: root.library.undoBatch()
                }
            }
            RowLayout {
                visible: root.library.busy
                Layout.fillWidth: true
                ProgressBar {
                    Layout.fillWidth: true
                    indeterminate: root.library.progress.indeterminate
                    from: 0; to: 100; value: root.library.progress.percent
                }
                Label {
                    visible: !root.library.progress.indeterminate
                    text: root.library.progress.percent + "%"
                    color: Theme.colors.text.secondary
                }
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.library.status
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.colors.text.secondary
            }
        }
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
