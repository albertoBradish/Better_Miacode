pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root

    required property var slider
    property var structure: null
    property real positionSeconds: 0
    property int movingEdge: -1
    property real dragSecond: 0
    property real initialStart: 0
    property real initialEnd: 0
    readonly property bool available: structure !== null && structure.available
    readonly property bool hasDrop: available && structure.dropAvailable
    readonly property real dropStart: movingEdge === 0 ? dragSecond : (hasDrop ? structure.dropStartSeconds : 0)
    readonly property real dropEnd: movingEdge === 1 ? dragSecond : (hasDrop ? structure.dropEndSeconds : 0)
    readonly property real startX: xAt(dropStart)
    readonly property real endX: xAt(dropEnd)
    readonly property real railCenter: slider.topPadding + slider.availableHeight / 2
    readonly property color markerColor: Theme.darkTheme ? "#ead28c" : "#8d752e"

    function xAt(second) {
        return slider.handle ? slider.leftPadding + slider.positionForValue(second) : 0
    }

    function secondAt(x) {
        const start = slider.leftPadding + slider.handle.width / 2
        const travel = Math.max(1, slider.availableWidth - slider.handle.width)
        const fraction = Math.max(0, Math.min(1, (x - start) / travel))
        return slider.from + fraction * (slider.to - slider.from)
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
        if (!available || editor.opened)
            return
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

    function closeEditor() { editor.close() }

    MouseArea {
        anchors.fill: parent
        enabled: root.available
        acceptedButtons: Qt.RightButton
        onClicked: root.openEditor()
    }
    HoverHandler {
        id: sliderHover
        enabled: root.available
    }
    ToolTip.visible: sliderHover.hovered && root.movingEdge === -1 && !root.slider.pressed && !editor.opened
        && !(root.hasDrop && sliderHover.point.position.y >= root.railCenter - 9
            && sliderHover.point.position.y <= root.railCenter - 2
            && Math.min(Math.abs(sliderHover.point.position.x - root.startX),
                Math.abs(sliderHover.point.position.x - root.endX)) <= 6)
    ToolTip.delay: 650
    ToolTip.text: root.available ? root.structure.status + "\n" + qsTrId("structure.edit_title") + " · Shift+F10" : ""

    Shortcut {
        sequences: ["Shift+F10", "Menu"]
        enabled: root.available && root.slider.activeFocus && !editor.opened
        onActivated: root.openEditor()
    }

    Repeater {
        model: root.available && !root.hasDrop ? root.structure.candidates : []
        delegate: Item {
            id: candidate
            required property var modelData
            readonly property real start: root.xAt(modelData.start)
            readonly property real end: root.xAt(modelData.end)
            Rectangle {
                x: candidate.start
                y: root.railCenter - 6
                width: 1
                height: 3
                color: root.markerColor
                opacity: 0.52
            }
            Rectangle {
                x: candidate.end
                y: root.railCenter - 6
                width: 1
                height: 3
                color: root.markerColor
                opacity: 0.52
            }
        }
    }

    Rectangle {
        visible: root.hasDrop
        x: root.startX
        y: root.railCenter - 4
        width: Math.max(0, root.endX - root.startX)
        height: 1
        color: root.markerColor
        opacity: 0.75
    }

    Repeater {
        model: 2
        delegate: Item {
            id: edge
            required property int index
            readonly property real second: index === 0 ? root.dropStart : root.dropEnd
            visible: root.hasDrop
            x: root.xAt(second) - width / 2
            y: root.railCenter - 9
            width: Math.min(12, Math.max(4, root.endX - root.startX))
            height: 7
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: 2
                height: 5
                radius: 1
                color: root.markerColor
            }
            MouseArea {
                id: grip
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.SizeHorCursor
                hoverEnabled: true
                preventStealing: true
                property real pressX: 0
                onPressed: function(mouse) {
                    if (mouse.button !== Qt.LeftButton)
                        return
                    pressX = mapToItem(root, mouse.x, mouse.y).x
                    root.initialStart = root.structure.dropStartSeconds
                    root.initialEnd = root.structure.dropEndSeconds
                }
                onPositionChanged: function(mouse) {
                    if (!(pressedButtons & Qt.LeftButton))
                        return
                    const x = mapToItem(root, mouse.x, mouse.y).x
                    if (root.movingEdge === -1 && Math.abs(x - pressX) < 3)
                        return
                    const original = edge.index === 0 ? root.initialStart : root.initialEnd
                    const second = root.secondAt(root.xAt(original) + x - pressX)
                    const limit = root.structure.mediaDuration
                    root.dragSecond = edge.index === 0 ? Math.max(0, Math.min(root.initialEnd - 0.001, second))
                        : Math.min(limit, Math.max(root.initialStart + 0.001, second))
                    root.movingEdge = edge.index
                }
                onReleased: function(mouse) {
                    if (mouse.button !== Qt.LeftButton || root.movingEdge !== edge.index)
                        return
                    if (root.structure.dropStartSeconds === root.initialStart && root.structure.dropEndSeconds === root.initialEnd)
                        root.structure.setManualDrop(root.dropStart, root.dropEnd)
                    root.movingEdge = -1
                }
                onCanceled: root.movingEdge = -1
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton)
                        root.openEditor()
                }
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton)
                        root.openEditor()
                }
                ToolTip.visible: containsMouse && root.movingEdge === -1 && !editor.opened
                ToolTip.delay: 400
                ToolTip.text: (edge.index === 0 ? qsTrId("structure.start_seconds") : qsTrId("structure.end_seconds"))
                    + " · " + root.formatSeconds(edge.second) + "\n" + qsTrId("structure.edit_title")
            }
        }
    }

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
                    text: root.structure && root.structure.pending
                        ? qsTrId("structure.cancel_analysis") : qsTrId("structure.analyze_again")
                    enabled: root.structure !== null
                    onClicked: {
                        if (root.structure.pending)
                            root.structure.cancelAnalysis()
                        else
                            root.structure.startAnalysis()
                    }
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
            cancelText: qsTrId("action.cancel")
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
