import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

AppDialog {
    id: root
    property var service: null
    readonly property var parameters: service ? service.simulationParameters : ({})
    title: qsTrId("hand_order.experiment.title")
    objectName: "handOrderExperimentDialog"
    preferredWidth: 880
    preferredHeight: 760
    onClosed: if (service && service.pending) service.cancel()
    body: ColumnLayout {
        spacing: Theme.panelPadding
        Label {
            Layout.fillWidth: true
            text: qsTrId("hand_order.experiment.disclaimer")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.panelPadding
            AppButton {
                text: qsTrId("hand_order.experiment.analyze")
                enabled: root.service && !root.service.pending
                onClicked: root.service.requestAnalysis()
            }
            AppButton {
                text: qsTrId("hand_order.experiment.simulate")
                enabled: root.service && root.service.hasAnalysis && !root.service.pending
                onClicked: root.service.requestSimulation()
            }
            AppButton {
                text: qsTrId("action.cancel")
                enabled: root.service && root.service.pending
                onClicked: root.service.cancel()
            }
        }
        Repeater {
            model: [
                {key: "reactionMs", label: qsTrId("hand_order.experiment.reaction"), from: 0, to: 500, step: 5, decimals: 0, suffix: " ms"},
                {key: "movementSpeed", label: qsTrId("hand_order.experiment.movement"), from: 2, to: 20, step: 0.25, decimals: 2, suffix: ""},
                {key: "stamina", label: qsTrId("hand_order.experiment.stamina"), from: 0.5, to: 2, step: 0.05, decimals: 2, suffix: "×"},
                {key: "judgementJitterMs", label: qsTrId("hand_order.experiment.jitter"), from: 0, to: 100, step: 1, decimals: 0, suffix: " ms"},
                {key: "referenceWindowMs", label: qsTrId("hand_order.experiment.window"), from: 10, to: 500, step: 5, decimals: 0, suffix: " ms"},
                {key: "trials", label: qsTrId("hand_order.experiment.trials"), from: 1, to: 200, step: 1, decimals: 0, suffix: ""}
            ]
            delegate: LabeledSlider {
                required property var modelData
                Layout.fillWidth: true
                labelWidth: 190
                enabled: root.service && !root.service.pending
                label: modelData.label
                from: modelData.from
                to: modelData.to
                stepSize: modelData.step
                decimals: modelData.decimals
                suffix: modelData.suffix
                value: root.parameters[modelData.key] || 0
                onMoved: value => root.service.setSimulationParameter(modelData.key, value)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTrId("hand_order.experiment.seed") }
            AppTextField {
                id: seedField
                Layout.preferredWidth: 180
                enabled: root.service && !root.service.pending
                text: String(root.parameters.seed === undefined ? 20261112 : root.parameters.seed)
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /[0-9]{1,10}/ }
                onEditingFinished: {
                    if (root.service) root.service.setSimulationParameter("seed", Number(text))
                    text = String(root.parameters.seed)
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTrId("hand_order.experiment.seed_help")
                color: Theme.colors.text.secondary
                wrapMode: Text.Wrap
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.service ? root.service.status : ""
            color: Theme.colors.text.primary
            wrapMode: Text.Wrap
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 300
            clip: true
            ListView {
                id: segmentList
                model: root.service ? root.service.rows : []
                spacing: Theme.panelPadding
                delegate: Rectangle {
                    required property var modelData
                    width: segmentList.width
                    height: details.implicitHeight + 16
                    radius: Theme.controlRadius
                    color: Theme.colors.background.panel
                    ColumnLayout {
                        id: details
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            text: qsTrId("hand_order.experiment.segment_title")
                                .arg(modelData.start.toFixed(3)).arg(modelData.end.toFixed(3))
                                .arg(modelData.line).arg(modelData.column).arg(modelData.events)
                            wrapMode: Text.Wrap
                            color: Theme.colors.text.heading
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTrId("hand_order.experiment.segment_metrics")
                                .arg(modelData.peakSpeed.toFixed(2)).arg(modelData.peakTapRate.toFixed(2))
                                .arg(modelData.peakLoad.toFixed(2)).arg(modelData.contacts).arg(modelData.unresolved)
                                .arg(modelData.candidateSupport.toFixed(2))
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.reasons
                            wrapMode: Text.Wrap
                            color: Theme.colors.text.secondary
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: modelData.simulation.length > 0
                            text: modelData.simulation
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
        AppCheckBox {
            id: showReport
            text: qsTrId("hand_order.experiment.report")
        }
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: active ? 200 : 0
            active: showReport.checked && root.service && root.service.hasAnalysis
            sourceComponent: ScrollView {
                AppTextArea {
                    readOnly: true
                    text: root.service ? root.service.report : ""
                    wrapMode: TextEdit.NoWrap
                    selectByMouse: true
                }
            }
        }
    }
    footer: DialogFooter {
        cancelText: qsTrId("action.close")
        onRejected: root.close()
    }
}
