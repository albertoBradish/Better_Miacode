import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ApplicationWindow {
    id: root
    required property var chartTools
    title: qsTrId("tools.bpm.title")
    width: 760
    height: 580
    minimumWidth: 420
    minimumHeight: 320
    visible: false
    color: Theme.colors.background.surface
    palette.text: Theme.colors.text.primary
    palette.base: Theme.colors.background.panel
    function open() { show(); raise(); requestActivate(); }
    Shortcut { sequences: [StandardKey.Close]; onActivated: root.close() }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.dialogPadding
        spacing: Theme.settingsRowSpacing
        AppComboBox {
            Layout.fillWidth: true
            model: [qsTrId("tools.bpm.sequence"), qsTrId("tools.bpm.audio_time"), qsTrId("tools.bpm.empty_maidata")]
            currentIndex: root.chartTools.bpmFormat
            onActivated: root.chartTools.bpmFormat = currentIndex
        }
        Label {
            Layout.fillWidth: true
            visible: root.chartTools.error.length > 0
            text: root.chartTools.error
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Label {
            Layout.fillWidth: true
            visible: root.chartTools.bpmFormat === 2
            text: qsTrId("tools.bpm.empty_timing_note")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                text: root.chartTools.bpmPreview
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                font.family: "monospace"
                Accessible.name: qsTrId("tools.bpm.preview")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton { text: qsTrId("tools.bpm.copy"); enabled: root.chartTools.bpmPreview.length > 0; onClicked: root.chartTools.copyBpm() }
            AppButton { text: qsTrId("tools.bpm.save"); enabled: root.chartTools.bpmPreview.length > 0; onClicked: root.chartTools.saveBpm() }
        }
    }
}
