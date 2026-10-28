import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ApplicationWindow {
    id: root
    required property var chartTools
    title: qsTrId("tools.dx_loss.title")
    width: 960
    height: 680
    minimumWidth: 620
    minimumHeight: 360
    visible: false
    color: Theme.colors.background.surface
    function open() { show(); raise(); requestActivate(); }
    Shortcut { sequences: [StandardKey.Close]; onActivated: root.close() }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.dialogPadding
        spacing: Theme.settingsRowSpacing
        Flow {
            Layout.fillWidth: true
            spacing: 12
            Repeater {
                model: root.chartTools.objectCounts
                delegate: Rectangle {
                    required property var modelData
                    width: countLabel.implicitWidth + 50
                    height: Theme.controlMinHeight
                    radius: Theme.controlRadius
                    color: Theme.colors.background.panel
                    ScoringObjectIcon {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        objectType: modelData.name
                        source: modelData.icon
                    }
                    Label {
                        id: countLabel
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name + ": " + modelData.count
                        color: Theme.colors.text.primary
                        Accessible.name: text
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.chartTools.calculationStatus
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Label {
            Layout.fillWidth: true
            text: qsTrId("tools.dx_loss.attributes")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Repeater {
                model: [qsTrId("tools.dx_loss.object"), qsTrId("tools.dx_loss.judgment"), qsTrId("tools.dx_loss.single"), "SSS 100%", "SSS+ 100.5%"]
                delegate: Label { required property string modelData; text: modelData; Layout.fillWidth: true; Layout.preferredWidth: 1; wrapMode: Text.Wrap; color: Theme.colors.text.heading }
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ListView {
                id: lossList
                model: root.chartTools.scoreLossRows
                spacing: 1
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: lossList.width
                    height: Math.max(38, row.implicitHeight + 12)
                    color: index % 2 === 0 ? Theme.colors.background.panel : "transparent"
                    RowLayout {
                        id: row
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 12
                        Repeater {
                            model: [modelData.object, modelData.judgment, modelData.loss, String(modelData.sss), String(modelData.sssPlus)]
                            delegate: Label {
                                required property string modelData
                                text: modelData
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                wrapMode: Text.Wrap
                                color: Theme.colors.text.primary
                            }
                        }
                    }
                }
            }
        }
    }
}
