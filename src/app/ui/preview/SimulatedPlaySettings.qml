pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    objectName: "simulatedPlaySettings"

    required property var previewSession
    property var draft: ({})
    spacing: Theme.panelPadding

    function setValue(key, value) {
        const updated = Object.assign({}, draft)
        updated[key] = value
        draft = updated
    }

    function refreshDraft() { draft = Object.assign({}, previewSession.playProfile) }
    Component.onCompleted: refreshDraft()

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.panelPadding

        Repeater {
            model: [
                { key: "heightCm", label: qsTrId("play.settings.height"), min: 120, max: 220, suffix: " cm" },
                { key: "standingDistanceCm", label: qsTrId("play.settings.distance"), min: 15, max: 65, suffix: " cm" },
                { key: "standingOffsetCm", label: qsTrId("play.settings.offset"), min: -30, max: 20, suffix: " cm" },
                { key: "armReachPercent", label: qsTrId("play.settings.reach"), min: 80, max: 120, suffix: "%" },
                { key: "reactionMs", label: qsTrId("play.settings.reaction"), min: 0, max: 350, suffix: " ms" }
            ]
            delegate: LabeledSlider {
                required property var modelData
                label: modelData.label
                labelWidth: 150
                from: modelData.min
                to: modelData.max
                value: root.draft[modelData.key] ?? from
                suffix: modelData.suffix
                onMoved: selected => root.setValue(modelData.key, Math.round(selected))
            }
        }

        AppCheckBox {
            text: qsTrId("play.settings.buttons")
            checked: root.draft.preferButtons === true
            onToggled: root.setValue("preferButtons", checked)
        }
        AppCheckBox {
            text: qsTrId("play.settings.adaptive_palm")
            checked: root.draft.adaptivePalm === true
            onToggled: root.setValue("adaptivePalm", checked)
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.colors.text.secondary
            text: {
                const heights = root.previewSession.playStartingHeights(root.draft)
                return qsTrId("play.settings.heights").arg(heights.shoulder.toFixed(1))
                    .arg(heights.hand.toFixed(1)).arg(heights.center.toFixed(1))
            }
        }

        AppCheckBox {
            text: qsTrId("play.settings.auto_height")
            checked: root.draft.automaticHeight === true
            onToggled: root.setValue("automaticHeight", checked)
        }

        Repeater {
            model: [
                { key: "lowerLimitCm", label: qsTrId("play.settings.lower"), min: 0, max: 45, suffix: " cm" },
                { key: "raiseLimitCm", label: qsTrId("play.settings.raise"), min: 0, max: 20, suffix: " cm" },
                { key: "verticalSpeedCmPerSecond", label: qsTrId("play.settings.speed"), min: 20, max: 160, suffix: " cm/s" }
            ]
            delegate: LabeledSlider {
                required property var modelData
                enabled: root.draft.automaticHeight === true
                label: modelData.label
                labelWidth: 150
                from: modelData.min
                to: modelData.max
                stepSize: modelData.key === "verticalSpeedCmPerSecond" ? 5 : 1
                value: root.draft[modelData.key] ?? from
                suffix: modelData.suffix
                onMoved: selected => root.setValue(modelData.key, Math.round(selected))
            }
        }

        AppCheckBox {
            text: qsTrId("play.settings.auto_distance")
            checked: root.draft.automaticDistance === true
            onToggled: root.setValue("automaticDistance", checked)
        }
        Repeater {
            model: [
                { key: "forwardLimitCm", label: qsTrId("play.settings.forward"), min: 0, max: 28, suffix: " cm" },
                { key: "backwardLimitCm", label: qsTrId("play.settings.backward"), min: 0, max: 30, suffix: " cm" },
                { key: "foreAftSpeedCmPerSecond", label: qsTrId("play.settings.depth_speed"), min: 20, max: 150, suffix: " cm/s" }
            ]
            delegate: LabeledSlider {
                required property var modelData
                enabled: root.draft.automaticDistance === true
                label: modelData.label
                labelWidth: 150
                from: modelData.min
                to: modelData.max
                value: root.draft[modelData.key] ?? from
                suffix: modelData.suffix
                onMoved: selected => root.setValue(modelData.key, Math.round(selected))
            }
        }
    }

    AppButton {
        Layout.alignment: Qt.AlignRight
        text: qsTrId("play.settings.apply")
        emphasized: true
        onClicked: {
            root.previewSession.setCanvasSetting("playProfile", root.draft)
            root.refreshDraft()
        }
    }
}
