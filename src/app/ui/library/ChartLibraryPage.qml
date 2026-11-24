import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root
    required property var library
    property bool normalizeZipDefault: false
    property var editingSession: null
    property bool synchronizing: false
    property bool pendingEdit: false
    property bool viewingSource: false
    readonly property bool editing: viewingSource && !!editingSession
    readonly property Item cornerSourceItem: songs.cornerSourceItem
    signal editorRequested()
    signal renderRequested(var charts)
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
        function onRenderQueuePrepared(charts) { root.renderRequested(charts) }
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
                AppDropDownButton {
                    id: selectedActions
                    visible: root.library.selectedCount > 0
                    text: qsTrId("library.selected_actions").arg(root.library.selectedCount)
                    enabled: !root.library.busy
                    expanded: batchMenu.active
                    onClicked: batchMenu.active ? batchMenu.close() : batchMenu.openAt(selectedActions)
                    AppMenu {
                        id: batchMenu
                        AppMenuItem { text: qsTrId("library.batch_normalize"); onTriggered: if (root.flush()) root.library.prepareNormalization() }
                        AppMenuItem { text: qsTrId("library.batch_cleanup_metadata"); onTriggered: if (root.flush()) root.library.prepareMetadataCleanup() }
                        AppMenuItem { text: qsTrId("library.batch_zip"); onTriggered: if (root.flush()) zipOptions.open() }
                        AppMenuItem { text: qsTrId("library.batch_queue"); onTriggered: if (root.flush()) render.open() }
                        AppMenuItem { text: qsTrId("library.batch_unlink"); onTriggered: if (root.flush()) unlink.open() }
                    }
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
    ChartZipOptionsDialog {
        id: zipOptions
        parent: Overlay.overlay
        normalizeDefault: root.normalizeZipDefault
        title: qsTrId("library.batch_zip")
        onExportRequested: options => root.library.chooseSelectedZipOutput(options)
    }
    AppDialog {
        id: render
        parent: Overlay.overlay
        title: qsTrId("library.batch_queue")
        preferredWidth: 430
        preferredHeight: implicitHeight
        property var difficulties: []
        onAboutToShow: difficulties = []
        function select(slot, checked) {
            difficulties = checked ? difficulties.concat([slot]) : difficulties.filter(id => id !== slot)
        }
        body: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: qsTrId("library.batch_queue_help")
                color: Theme.colors.text.secondary
                wrapMode: Text.Wrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: ["Easy", "Basic", "Advanced", "Expert", "Master", "Re:Master", "UTAGE"]
                    delegate: AppCheckBox {
                        required property int index
                        required property string modelData
                        text: modelData
                        checked: render.difficulties.includes(index + 1)
                        onToggled: render.select(index + 1, checked)
                    }
                }
            }
        }
        footer: DialogFooter {
            cancelText: qsTrId("action.cancel")
            acceptText: qsTrId("library.batch_queue")
            acceptEnabled: render.difficulties.length > 0 && !root.library.busy
            onRejected: render.close()
            onAccepted: { render.close(); root.library.queueSelectedForRender(render.difficulties) }
        }
    }
    AppDialog {
        id: unlink
        parent: Overlay.overlay
        title: qsTrId("library.batch_unlink")
        preferredHeight: implicitHeight
        body: Label {
            text: qsTrId("library.batch_unlink_prompt").arg(root.library.selectedCount)
            color: Theme.colors.text.primary
            wrapMode: Text.Wrap
        }
        footer: DialogFooter {
            cancelText: qsTrId("action.cancel")
            acceptText: qsTrId("library.batch_unlink")
            acceptEnabled: root.library.selectedCount > 0 && !root.library.busy
            onRejected: unlink.close()
            onAccepted: { unlink.close(); root.library.removeSelectedSongs() }
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
