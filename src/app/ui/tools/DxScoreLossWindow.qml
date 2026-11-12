import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ApplicationWindow {
    id: root
    required property var chartTools
    title: qsTrId("tools.dx_loss.title")
    width: 1100
    height: 760
    minimumWidth: 720
    minimumHeight: 520
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
            text: qsTrId("tools.dx_loss.rules")
            wrapMode: Text.Wrap
            color: Theme.colors.text.heading
        }
        Label {
            Layout.fillWidth: true
            text: root.chartTools.scoreBasisSummary
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: resultColumn.implicitHeight + 24
            radius: Theme.controlRadius
            color: Theme.colors.background.panel
            ColumnLayout {
                id: resultColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: Theme.settingsLabelSpacing
                Label {
                    Layout.fillWidth: true
                    text: root.chartTools.scenarioSummary
                    font.bold: true
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.heading
                }
                Label {
                    Layout.fillWidth: true
                    text: root.chartTools.scenarioError
                    visible: text.length > 0
                    wrapMode: Text.Wrap
                    color: Theme.colors.syntax.error
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTrId("tools.dx_loss.input_help")
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.secondary
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            AppCheckBox {
                id: showTolerance
                text: qsTrId("tools.dx_loss.show_tolerance")
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTrId("tools.dx_loss.reset_quantities")
                enabled: root.chartTools.calculable
                onClicked: root.chartTools.resetLossQuantities()
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.chartTools.simpleLossSummary
            visible: showTolerance.checked && text.length > 0
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            spacing: 12
            Repeater {
                model: [qsTrId("tools.dx_loss.object"), qsTrId("tools.dx_loss.judgment"),
                        qsTrId("tools.dx_loss.eligible"), qsTrId("tools.dx_loss.quantity"), qsTrId("tools.dx_loss.single")]
                delegate: Label {
                    required property string modelData
                    required property int index
                    text: modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: index === 3 ? 80 : 1
                    Layout.maximumWidth: index === 3 ? 90 : Infinity
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.heading
                }
            }
            Label { visible: showTolerance.checked; text: "SSS 100%"; Layout.fillWidth: true; Layout.preferredWidth: 1; color: Theme.colors.text.heading }
            Label { visible: showTolerance.checked; text: "SSS+ 100.5%"; Layout.fillWidth: true; Layout.preferredWidth: 1; color: Theme.colors.text.heading }
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
                            model: [modelData.object, modelData.judgment, String(modelData.count)]
                            delegate: Label {
                                required property string modelData
                                text: modelData
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                wrapMode: Text.Wrap
                                color: Theme.colors.text.primary
                            }
                        }
                        AppTextField {
                            id: quantityInput
                            Layout.fillWidth: true
                            Layout.preferredWidth: 80
                            Layout.maximumWidth: 90
                            enabled: root.chartTools.calculable
                            outlined: true
                            selectByMouse: true
                            inputMethodHints: Qt.ImhDigitsOnly
                            validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
                            text: root.chartTools.lossQuantity(modelData.rowIndex)
                            Accessible.name: modelData.object + " " + modelData.judgment + " " + qsTrId("tools.dx_loss.quantity")
                            onTextEdited: root.chartTools.setLossQuantity(modelData.rowIndex, text)
                            Connections {
                                target: root.chartTools
                                function onScenarioChanged() {
                                    if (!quantityInput.activeFocus)
                                        quantityInput.text = root.chartTools.lossQuantity(modelData.rowIndex)
                                }
                            }
                        }
                        Label {
                            text: modelData.loss
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            wrapMode: Text.Wrap
                            color: Theme.colors.text.primary
                        }
                        Label { visible: showTolerance.checked; text: String(modelData.sss); Layout.fillWidth: true; Layout.preferredWidth: 1; color: Theme.colors.text.primary }
                        Label { visible: showTolerance.checked; text: String(modelData.sssPlus); Layout.fillWidth: true; Layout.preferredWidth: 1; color: Theme.colors.text.primary }
                    }
                }
            }
        }
    }
}
