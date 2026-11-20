import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MiaCode.UI

ColumnLayout {
    id: root
    required property var library
    property var song: ({})
    property var edits: ({})
    property int slot: 0
    property int notes: -1
    property bool audioSuspended: false
    readonly property string path: song.path ?? ""
    readonly property bool pending: Object.keys(edits).length > 0
    readonly property var buffer: root.library.sessions.find(s => s.path === root.path) ?? null
    readonly property bool dirty: pending || !!buffer?.dirty
    signal editSourceRequested(string path)
    signal backRequested()
    property bool showBack: false
    spacing: Theme.settingsRowSpacing

    function value(key) { return edits[key] ?? song.fields?.[key] ?? "" }
    function change(key, text) {
        if (value(key) === text) return
        edits = Object.assign({}, edits, { [key]: text })
        draftTimer.restart()
    }
    function stage() {
        draftTimer.stop()
        if (!pending) return true
        const stamp = library.stageSongFields(path, song.stamp, edits)
        if (!stamp) return false
        song = Object.assign({}, song, { fields: Object.assign({}, song.fields, edits), stamp: stamp })
        edits = ({})
        return true
    }
    function load(path, force) {
        if (!force && root.path === path) return true
        if (!stage()) return false
        const next = path ? library.songDetails(path) : ({})
        if (path && !next.path) return false
        if (root.path === path && root.song.stamp === next.stamp
                && root.song.songId === next.songId && root.song.kind === next.kind && root.song.pack === next.pack
                && String(root.song.track) === String(next.track) && String(root.song.jacket) === String(next.jacket))
            return true
        player.stop()
        audioSuspended = false
        const previousSlot = root.path === path ? slot : 0
        song = next
        const slots = next.difficulties ?? []
        slot = slots.find(d => d.id === previousSlot)?.id ?? slots.find(d => d.id === 5)?.id ?? slots[0]?.id ?? 0
        refreshCount()
        viewport.contentItem.contentY = 0
        return true
    }
    function refreshCount() {
        notes = -1
        countTimer.restart()
    }
    function clock(ms) {
        const t = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(t / 60) + ":" + String(t % 60).padStart(2, "0")
    }
    Timer { id: countTimer; interval: 100; onTriggered: if (root.path && root.slot) root.library.requestNoteCount(root.path, root.slot) }
    Timer { id: draftTimer; interval: 500; onTriggered: root.stage() }
    Connections {
        target: root.library
        function onSongAudioReady(path) {
            if (path !== root.path) return
            root.audioSuspended = false
            if (!root.pending) root.load(path, true)
        }
        function onNoteCountReady(path, difficulty, notes) {
            if (root.path === path && root.slot === difficulty) root.notes = notes
        }
    }
    MediaPlayer {
        id: player
        audioOutput: AudioOutput {}
        source: root.visible && root.Window.window?.visible && !root.audioSuspended && root.path ? (root.song.track ?? "") : ""
    }
    component FormField: ColumnLayout {
        id: field
        required property string caption
        required property string fieldKey
        property string hint: ""
        property var options: []
        Layout.fillWidth: true
        enabled: !root.library.busy
        spacing: 2
        Label { text: field.caption; color: Theme.colors.text.secondary }
        RowLayout {
            Layout.fillWidth: true
            AppTextField {
                Layout.fillWidth: true
                Accessible.name: field.caption
                outlined: true
                text: root.value(field.fieldKey)
                placeholderText: field.hint
                onTextEdited: root.change(field.fieldKey, text)
            }
            AppComboBox {
                visible: field.options.length > 0
                Layout.preferredWidth: Math.min(130, field.width / 3)
                Accessible.name: field.caption
                model: [qsTrId("library.choose_existing")].concat(Array.from(field.options))
                currentIndex: 0
                onActivated: index => { if (index) root.change(field.fieldKey, model[index]); currentIndex = 0 }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        AppButton { visible: root.showBack; text: qsTrId("library.browse"); onClicked: root.backRequested() }
        Label {
            Layout.fillWidth: true
            text: root.song.fields?.title || qsTrId("library.untitled")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.bold: true
            font.pixelSize: Theme.uiFontSize + 3
            color: Theme.colors.text.primary
        }
        AppButton {
            visible: root.dirty
            text: qsTrId("library.discard_edits")
            onClicked: discardDialog.open()
        }
        AppButton {
            text: qsTrId("action.save")
            emphasized: root.dirty
            enabled: root.dirty && !root.library.busy
            onClicked: if (root.stage() && root.library.saveSong(root.path)) root.load(root.path, true)
        }
    }
    ScrollView {
        id: viewport
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: AppScrollBar {}
        ColumnLayout {
            width: viewport.availableWidth
            spacing: Theme.settingsRowSpacing + 4
            Label {
                Layout.fillWidth: true
                text: [root.song.pack, root.song.kind === "UTAGE" ? qsTrId("library.utage") : root.song.kind,
                       root.song.songId ? "ID " + root.song.songId : ""].filter(Boolean).join("  ·  ")
                color: Theme.colors.text.secondary
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }
            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    Layout.fillWidth: true
                    FormField { caption: qsTrId("net.ui.song_title"); fieldKey: "title" }
                    FormField { caption: qsTrId("net.ui.artist"); fieldKey: "artist" }
                }
                Rectangle {
                    Layout.preferredWidth: Math.min(128, viewport.width / 4)
                    Layout.preferredHeight: width
                    color: Theme.colors.background.panel
                    radius: Theme.controlRadius
                    Image { id: jacket; anchors.fill: parent; source: root.song.jacket ?? ""; sourceSize: Qt.size(256,256); asynchronous: true; fillMode: Image.PreserveAspectFit }
                    Label { anchors.centerIn: parent; visible: jacket.status !== Image.Ready; text: "♪"; font.pixelSize: 30; color: Theme.colors.text.secondary }
                }
            }
            RowLayout {
                FormField { caption: "BPM"; fieldKey: "wholebpm" }
                FormField { caption: qsTrId("library.source_id"); fieldKey: "shortid" }
            }
            FormField { caption: qsTrId("metadata.advanced.version"); fieldKey: "version" }
            RowLayout {
                FormField { caption: qsTrId("library.genre"); fieldKey: "genre"; options: root.library.genres }
                FormField { caption: qsTrId("library.version_category"); fieldKey: "miacode_version_category"; options: root.library.versions }
            }
            RowLayout {
                Layout.fillWidth: true
                readonly property bool hasTrack: String(root.song.track ?? "").length > 0
                IconButton {
                    enabled: parent.hasTrack && !root.audioSuspended
                    iconSource: Qt.resolvedUrl(player.playbackState === MediaPlayer.PlayingState ? "icons/pause.svg" : "icons/play.svg")
                    tooltip: qsTrId("library.audition")
                    onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
                }
                Label { visible: parent.hasTrack; text: root.clock(player.position) + " / " + root.clock(player.duration); color: Theme.colors.text.secondary }
                AppSlider {
                    Layout.fillWidth: true
                    from: 0; to: Math.max(1, player.duration)
                    value: player.position
                    Accessible.name: qsTrId("library.audition")
                    enabled: parent.hasTrack && player.seekable
                    onMoved: player.position = value
                }
                AppButton {
                    text: parent.hasTrack ? qsTrId("library.replace_track") : qsTrId("library.add_track")
                    enabled: !root.library.busy && !root.audioSuspended
                    onClicked: {
                        player.stop()
                        root.audioSuspended = true
                        root.library.chooseSongAudio(root.path)
                    }
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 5
                Repeater {
                    model: root.song.difficulties ?? []
                    delegate: AppChoiceButton {
                        required property var modelData
                        text: modelData.name
                        difficultyId: modelData.id
                        checked: root.slot === modelData.id
                        onClicked: { root.slot = modelData.id; root.refreshCount(); checked = Qt.binding(() => root.slot === modelData.id) }
                    }
                }
            }
            ColumnLayout {
                visible: root.slot > 0
                Layout.fillWidth: true
                FormField {
                    caption: qsTrId("net.designer")
                    fieldKey: "des_" + root.slot
                    hint: root.song.designer ?? ""
                }
                RowLayout {
                    FormField { caption: qsTrId("qml.level"); fieldKey: "lv_" + root.slot }
                    FormField { caption: qsTrId("metadata.advanced.constant"); fieldKey: "level_constant_" + root.slot }
                }
                Label { text: qsTrId("library.note_count").arg(root.notes >= 0 ? root.notes : "—"); color: Theme.colors.text.secondary }
            }
            Flow {
                Layout.fillWidth: true; spacing: 6
                AppButton {
                    text: qsTrId("library.open_difficulty")
                    enabled: root.slot > 0 && !root.library.busy
                    onClicked: if (root.stage()) root.library.openInEditor(root.path, root.slot)
                }
                AppButton { text: qsTrId("library.edit_buffer"); onClicked: if (root.stage()) root.editSourceRequested(root.path) }
                AppButton { text: qsTrId("library.remove_song"); enabled: !root.dirty && !root.library.busy; onClicked: root.library.removeSong(root.path) }
            }
        }
    }
    AppDialog {
        id: discardDialog
        title: qsTrId("library.discard_edits")
        preferredHeight: implicitHeight
        body: Label { text: qsTrId("library.discard_song_prompt").arg(root.value("title")); textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.colors.text.primary }
        footer: DialogFooter {
            cancelText: qsTrId("action.cancel")
            acceptText: qsTrId("library.discard_edits")
            onRejected: discardDialog.close()
            onAccepted: {
                discardDialog.close()
                draftTimer.stop()
                root.edits = ({})
                root.library.discardSongEdits(root.path)
                root.load(root.library.hasSong(root.path) ? root.path : "", true)
            }
        }
    }
}
