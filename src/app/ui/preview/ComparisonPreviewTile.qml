import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MiaCode.UI
import "qrc:/preview/runtime/qml" as Preview

Rectangle {
    id: root
    required property var comparison
    required property bool surfaceActive
    property bool syncAvailable: true
    property real canvasRatio: 1
    property bool freeAspect: false
    color: Theme.surfaceColor(Theme.colors.background.panel)
    clip: true

    function formatTime(seconds) {
        const whole = Math.floor(Math.abs(seconds))
        return (seconds < 0 ? "-" : "") + Math.floor(whole / 60) + ":" + String(whole % 60).padStart(2, "0")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Item {
            id: stage
            Layout.fillWidth: true
            Layout.fillHeight: true
            Loader {
                id: surfaceLoader
                anchors.centerIn: parent
                width: root.freeAspect ? parent.width : Math.min(parent.width, parent.height * root.canvasRatio)
                height: root.freeAspect ? parent.height : width / root.canvasRatio
                active: root.surfaceActive && root.visible && !!root.comparison && width >= 64 && height >= 64
                sourceComponent: Preview.PreviewSurface {
                    runtime: root.comparison?.runtime ?? null
                    mediaHost: root.comparison?.mediaHost ?? null
                    surfaceRole: "comparison"
                    backgroundColor: "transparent"
                    hudTextColor: Theme.colors.previewHud.text
                    hudShadowColor: Theme.colors.previewHud.shadow
                }
            }
        }
        RowLayout {
            id: controls
            Layout.fillWidth: true
            Layout.leftMargin: Theme.panelPadding
            Layout.rightMargin: Theme.panelPadding
            IconButton {
                objectName: "closeComparisonPreviewButton"
                iconSource: Qt.resolvedUrl("icons/close.svg")
                tooltip: qsTrId("action.close")
                focusPolicy: Qt.TabFocus
                onClicked: root.comparison?.close()
            }
            IconButton {
                objectName: "comparisonSyncButton"
                iconSource: Qt.resolvedUrl("icons/sync.svg")
                active: root.syncAvailable && !!root.comparison?.syncEnabled
                enabled: root.syncAvailable
                tooltip: qsTrId("preview.comparison.sync")
                Accessible.name: tooltip
                Accessible.checkable: true
                Accessible.checked: active
                onClicked: if (root.comparison) root.comparison.syncEnabled = !root.comparison.syncEnabled
            }
            IconButton {
                iconSource: Qt.resolvedUrl(root.comparison?.playing ? "icons/pause.svg" : "icons/play.svg")
                tooltip: root.comparison?.playing ? qsTrId("preview.pause") : qsTrId("preview.play")
                onClicked: root.comparison?.togglePlayback()
            }
            AppSlider {
                id: seekBar
                Layout.fillWidth: true
                Layout.minimumWidth: 24
                Accessible.name: qsTrId("preview.comparison.seek")
                from: root.comparison?.lowerBound ?? 0
                to: Math.max(0.001, root.comparison?.duration ?? 0)
                enabled: (root.comparison?.duration ?? 0) > 0
                value: root.comparison?.position ?? 0
                onMoved: root.comparison?.seek(value)
            }
            Label {
                Layout.preferredWidth: implicitWidth
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: root.formatTime(seekBar.value) + " / " + root.formatTime(seekBar.to)
                color: Theme.colors.text.secondary
                font.pixelSize: Theme.secondaryFontSize
            }
        }
    }
    Binding {
        target: root.comparison
        property: "surfaceVisible"
        value: surfaceLoader.active && !!root.Window.window?.visible
        when: !!root.comparison
    }
}
