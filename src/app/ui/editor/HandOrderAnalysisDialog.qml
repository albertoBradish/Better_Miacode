import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

AppDialog {
    id: root
    required property var service
    required property var applyEdit
    required property var analyzeSelection
    required property var analyzeDifficulty
    property var preferencesModel: null
    property var navigateResult: null
    signal editingFinished()
    objectName: "handOrderAnalysisDialog"
    readonly property bool reviewingPlay: service?.reviewingPlay === true
    title: reviewingPlay ? qsTrId("hand_order.play_review")
        : service && service.optimizingPatterns ? qsTrId("hand_order.optimize") : qsTrId("hand_order.analyze")
    preferredWidth: 760
    preferredHeight: 580
    HandOrderExperimentDialog {
        id: experimentDialog
        service: root.service ? root.service.experiments : null
    }
    onClosed: {
        if (service && service.pending) service.cancel()
        editingFinished()
    }
    body: Column {
        spacing: Theme.panelPadding
        LabeledCombo {
            width: parent.width
            visible: !root.reviewingPlay
            label: qsTrId("hand_order.auto.input_preference")
            currentValue: root.service ? root.service.inputPreference : 0
            options: [
                { value: 0, label: qsTrId("hand_order.auto.input_buttons") },
                { value: 1, label: qsTrId("hand_order.auto.input_screen") },
                { value: 2, label: qsTrId("hand_order.auto.input_mixed") }
            ]
            onPicked: value => { root.service.inputPreference = value }
        }
        AppCheckBox {
            objectName: "handOrderRecomputeExisting"
            visible: !root.reviewingPlay && (!root.service || !root.service.optimizingPatterns)
            text: qsTrId("hand_order.workflow.protect_existing")
            checked: root.service && !root.service.overwriteExisting
            onClicked: root.service.overwriteExisting = !checked
        }
        AppCheckBox {
            objectName: "handOrderPreservePatternAnnotations"
            visible: root.service && root.service.optimizingPatterns
            text: qsTrId("hand_order.optimize.preserve_existing")
            checked: root.service && root.service.preservePatternAnnotations
            onClicked: root.service.preservePatternAnnotations = checked
        }
        AppCheckBox {
            objectName: "handOrderHighConfidenceOnly"
            visible: !root.reviewingPlay && (!root.service || !root.service.optimizingPatterns)
            text: qsTrId("hand_order.auto.high_confidence_only")
            checked: root.service && root.service.highConfidenceOnly
            onClicked: {
                root.service.highConfidenceOnly = checked
                if (checked && root.preferencesModel)
                    root.preferencesModel.handOrderColorUnresolved = false
            }
        }
        AppCheckBox {
            objectName: "handOrderColorUnresolved"
            visible: !root.reviewingPlay && (!root.service || !root.service.optimizingPatterns)
            text: qsTrId("hand_order.auto.color_unresolved")
            enabled: root.service && !root.service.highConfidenceOnly && root.preferencesModel
            checked: root.preferencesModel && root.preferencesModel.handOrderColorUnresolved
                && root.service && !root.service.highConfidenceOnly
            onClicked: root.preferencesModel.handOrderColorUnresolved = checked
        }
        Label {
            width: parent.width
            text: qsTrId("hand_order.auto.color_unresolved_help")
            visible: !root.reviewingPlay && (!root.service || !root.service.optimizingPatterns)
            color: Theme.colors.text.secondary
            wrapMode: Text.Wrap
        }
        Flow {
            width: parent.width
            spacing: Theme.panelPadding
            AppButton {
                text: qsTrId("hand_order.experiment.title")
                visible: !root.reviewingPlay
                enabled: root.service && !root.service.pending
                onClicked: experimentDialog.open()
            }
            AppButton {
                text: qsTrId("hand_order.auto.selection")
                enabled: root.service && !root.service.pending
                onClicked: root.analyzeSelection()
            }
            AppButton {
                text: qsTrId("hand_order.auto.difficulty")
                enabled: root.service && !root.service.pending
                onClicked: root.analyzeDifficulty()
            }
            AppButton {
                objectName: "handOrderAnalysisCancel"
                text: qsTrId("hand_order.auto.cancel")
                enabled: root.service && root.service.pending
                onClicked: root.service.cancel()
            }
        }
        Label {
            width: parent.width
            text: root.service ? root.service.message : ""
            color: Theme.colors.text.primary
            wrapMode: Text.Wrap
        }
        AppButton {
            visible: !root.reviewingPlay && root.service && root.service.patternCount > 0
            text: qsTrId("hand_order.pattern.open")
            onClicked: { root.service.showPatternPanel(); root.close() }
        }
        Repeater {
            model: root.service && root.reviewingPlay ? root.service.rows : []
            delegate: RowLayout {
                required property var modelData
                required property int index
                width: parent.width

                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: modelData.line + ":" + modelData.col + "  " + modelData.original + " → " + modelData.replacement
                    color: Theme.colors.text.primary
                    wrapMode: Text.Wrap
                }
                AppButton {
                    text: qsTrId("hand_order.auto.locate")
                    enabled: root.navigateResult !== null
                    onClicked: if (root.navigateResult(index)) root.close()
                }
            }
        }
        Repeater {
            model: root.service && !root.reviewingPlay && !root.service.optimizingPatterns ? root.service.rows : []
            delegate: Column {
                required property var modelData
                required property int index
                width: parent.width
                spacing: 3
                RowLayout {
                    width: parent.width
                    Label {
                        Layout.fillWidth: true
                        text: modelData.line + ":" + modelData.col + " · " + modelData.statusText
                        color: Theme.colors.text.primary
                        wrapMode: Text.Wrap
                    }
                    AppButton {
                        objectName: "handOrderResultLocation_" + index
                        text: qsTrId("hand_order.auto.locate")
                        enabled: root.navigateResult !== null
                        onClicked: if (root.navigateResult(index)) root.close()
                    }
                }
                Label {
                    objectName: "handOrderAutomaticIntervals_" + modelData.line + "_" + modelData.col
                    width: parent.width
                    visible: modelData.intervals.length > 0
                    text: modelData.intervals
                    color: Theme.colors.text.secondary
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    text: modelData.noteAssignments
                    visible: text.length > 0
                    color: Theme.colors.text.secondary
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    footer: DialogFooter {
        acceptText: qsTrId("hand_order.auto.apply")
        cancelText: qsTrId("action.close")
        acceptEnabled: root.service && root.service.canApply
        onAccepted: if (root.applyEdit()) root.close()
        onRejected: root.close()
    }
}
