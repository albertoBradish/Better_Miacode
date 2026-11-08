pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MiaCode.UI

ColumnLayout {
    id: root

    property var structure: null
    property real startSeconds: 0
    property real endSeconds: 0
    property real positionSeconds: 0
    property real handleInset: 0
    property int movingEdge: -1
    property real dragSecond: 0
    readonly property bool hasDrop: structure !== null && structure.dropAvailable
    readonly property real dropStart: movingEdge === 0 ? dragSecond : (hasDrop ? structure.dropStartSeconds : 0)
    readonly property real dropEnd: movingEdge === 1 ? dragSecond : (hasDrop ? structure.dropEndSeconds : 0)
    spacing: 3

    function xAt(second) {
        if (endSeconds <= startSeconds)
            return 0
        return track.width * Math.max(0, Math.min(1, (second - startSeconds) / (endSeconds - startSeconds)))
    }

    function secondAt(x) {
        return startSeconds + Math.max(0, Math.min(1, x / Math.max(1, track.width))) * (endSeconds - startSeconds)
    }

    function formatSeconds(second) {
        return Number(second).toLocaleString(Qt.locale(), 'f', 3)
    }

    function parseSeconds(value) {
        try {
            return Number.fromLocaleString(Qt.locale(), value.trim())
        } catch (_) {
            return NaN
        }
    }

    function fillRange(start, end) {
        dropStartField.text = formatSeconds(start)
        dropEndField.text = formatSeconds(end)
    }

    function openEditor() {
        const duration = structure.mediaDuration
        if (hasDrop)
            fillRange(dropStart, dropEnd)
        else if (structure.candidates.length > 0)
            fillRange(structure.candidates[0].start, structure.candidates[0].end)
        else {
            const start = Math.max(0, Math.min(positionSeconds, Math.max(0, duration - 0.1)))
            fillRange(start, Math.min(duration, start + 8.5))
        }
        editor.open()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.panelPadding
        Text {
            Layout.fillWidth: true
            text: root.structure ? root.structure.status : ""
            elide: Text.ElideRight
            font.family: Theme.uiFont
            font.pixelSize: Theme.secondaryFontSize
            color: Theme.colors.text.secondary
            Accessible.name: text
            HoverHandler { id: statusHover }
            ToolTip.visible: statusHover.hovered && truncated
            ToolTip.text: text
        }
        AppButton {
            id: dropButton
            objectName: "previewDropEditorButton"
            text: root.structure && root.structure.pending ? qsTrId("structure.cancel_analysis") : qsTrId("structure.edit_button")
            onClicked: {
                if (root.structure.pending)
                    root.structure.cancelAnalysis()
                else
                    root.openEditor()
            }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: 19

        Rectangle {
            id: track
            anchors.fill: parent
            anchors.leftMargin: root.handleInset
            anchors.rightMargin: root.handleInset
            color: Theme.overlayColor(Theme.colors.slider.rail)
            radius: 3

            Canvas {
                id: heatmap
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    if (!root.structure || width < 1)
                        return
                    const columns = Math.min(4096, Math.ceil(width * Screen.devicePixelRatio))
                    const samples = root.structure.heatmapSamples(root.startSeconds, root.endSeconds, columns)
                    const columnWidth = width / Math.max(1, samples.length)
                    for (let i = 0; i < samples.length; ++i) {
                        const strength = samples[i]
                        if (strength < 0.015)
                            continue
                        const color = Qt.rgba(0.25 + strength * 0.69, 0.53 - strength * 0.18, 0.85 - strength * 0.62, 0.93)
                        const barHeight = Math.max(1, height * strength)
                        context.fillStyle = color
                        context.fillRect(i * columnWidth, height - barHeight, columnWidth + 0.15, barHeight)
                    }
                }
            }

            Repeater {
                model: root.structure ? root.structure.candidates : []
                delegate: Rectangle {
                    required property var modelData
                    x: root.xAt(modelData.start)
                    width: Math.max(0, root.xAt(modelData.end) - x)
                    height: track.height
                    color: "transparent"
                    border.width: 1
                    border.color: "#80d9bd63"
                }
            }

            Rectangle {
                visible: root.hasDrop
                x: root.xAt(root.dropStart)
                width: Math.max(0, root.xAt(root.dropEnd) - x)
                height: track.height
                color: "#55f7d575"
                border.color: "#f1ce70"
                border.width: 1
                radius: 2
            }

            Rectangle {
                x: root.xAt(root.positionSeconds)
                width: 1
                height: track.height
                color: Theme.colors.text.secondary
            }

            HoverHandler { id: pressureHover }
            ToolTip.visible: pressureHover.hovered && root.movingEdge === -1 && ToolTip.text !== ""
            ToolTip.delay: 400
            ToolTip.text: {
                if (!root.structure || !pressureHover.hovered)
                    return ""
                const second = root.secondAt(pressureHover.point.position.x)
                const p = root.structure.pressureAt(second)
                if (p.density === undefined)
                    return ""
                return qsTrId("structure.pressure_tooltip")
                    .arg(root.formatSeconds(second)).arg(p.density.toFixed(1)).arg(p.simultaneous.toFixed(1))
                    .arg(p.held.toFixed(1)).arg(p.slides.toFixed(1)).arg(p.movement.toFixed(1)).arg(p.rhythm.toFixed(1))
            }

            Repeater {
                model: 2
                delegate: Item {
                    id: handle
                    required property int index
                    visible: root.hasDrop
                    x: root.xAt(index === 0 ? root.dropStart : root.dropEnd) - width / 2
                    width: 14
                    height: track.height
                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: track.height
                        radius: 2
                        color: "#f1ce70"
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.SizeHorCursor
                        onPressed: {
                            root.dragSecond = handle.index === 0 ? root.dropStart : root.dropEnd
                            root.movingEdge = handle.index
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed)
                                return
                            const second = root.secondAt(mapToItem(track, mouse.x, mouse.y).x)
                            const duration = root.structure.mediaDuration
                            root.dragSecond = handle.index === 0 ? Math.max(0, Math.min(root.dropEnd - 0.001, second))
                                : Math.min(duration, Math.max(root.dropStart + 0.001, second))
                        }
                        onReleased: {
                            root.structure.setManualDrop(root.dropStart, root.dropEnd)
                            root.movingEdge = -1
                        }
                        onCanceled: root.movingEdge = -1
                    }
                }
            }
        }
    }

    Connections {
        target: root.structure
        function onChanged() { heatmap.requestPaint() }
    }
    onStartSecondsChanged: heatmap.requestPaint()
    onEndSecondsChanged: heatmap.requestPaint()

    AppDialog {
        id: editor
        title: qsTrId("structure.edit_title")
        preferredWidth: 400
        preferredHeight: 355
        readonly property real start: root.parseSeconds(dropStartField.text)
        readonly property real end: root.parseSeconds(dropEndField.text)

        body: ColumnLayout {
            spacing: Theme.panelPadding
            Text {
                Layout.fillWidth: true
                text: root.structure ? root.structure.status : ""
                wrapMode: Text.WordWrap
                font.family: Theme.uiFont
                font.pixelSize: Theme.secondaryFontSize
                color: Theme.colors.text.secondary
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                Label { text: qsTrId("structure.start_seconds") }
                AppTextField {
                    id: dropStartField
                    Layout.fillWidth: true
                    Accessible.name: qsTrId("structure.start_seconds")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
                Label { text: qsTrId("structure.end_seconds") }
                AppTextField {
                    id: dropEndField
                    Layout.fillWidth: true
                    Accessible.name: qsTrId("structure.end_seconds")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
            }
            Repeater {
                model: root.structure ? root.structure.candidates : []
                delegate: AppButton {
                    required property var modelData
                    Layout.fillWidth: true
                    text: (modelData.confident ? qsTrId("structure.recommendation") : qsTrId("structure.candidate"))
                        .arg(root.formatSeconds(modelData.start)).arg(root.formatSeconds(modelData.end))
                    onClicked: root.fillRange(modelData.start, modelData.end)
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.panelPadding
                AppButton {
                    text: qsTrId("structure.undo")
                    enabled: root.structure !== null && root.structure.canUndo
                    onClicked: { root.structure.undoDrop(); editor.close() }
                }
                AppButton {
                    text: qsTrId("structure.redo")
                    enabled: root.structure !== null && root.structure.canRedo
                    onClicked: { root.structure.redoDrop(); editor.close() }
                }
                AppButton {
                    text: qsTrId("structure.clear")
                    enabled: root.structure !== null
                    onClicked: { root.structure.clearDrop(); editor.close() }
                }
                AppButton {
                    text: qsTrId("structure.analyze_again")
                    enabled: root.structure !== null && !root.structure.pending
                    onClicked: { root.structure.startAnalysis(); editor.close() }
                }
                AppButton {
                    text: qsTrId("structure.save_drop")
                    visible: root.structure !== null && root.structure.unsaved
                    onClicked: {
                        if (root.structure.saveSessionDrop())
                            editor.close()
                    }
                }
            }
        }
        footer: DialogFooter {
            acceptText: qsTrId("structure.apply_manual")
            cancelText: qsTrId("qml.cancel")
            acceptEnabled: root.structure !== null && editor.start >= 0 && editor.end > editor.start
                && editor.end <= root.structure.mediaDuration + 0.001
            onAccepted: {
                if (root.structure.setManualDrop(editor.start, editor.end))
                    editor.close()
            }
            onRejected: editor.close()
        }
    }
}
