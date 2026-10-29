import QtQuick
import QtQuick.Controls
import MiaCode.UI

AppStickyPopup {
    id: root

    required property var preferences
    required property var previewSession
    openAbove: true

    readonly property var aidSettings: root.previewSession ? root.previewSession.canvasSettings : ({})

    function putAid(key, value) {
        if (root.previewSession) root.previewSession.setCanvasSetting(key, value)
    }

    readonly property var mediaHost: root.previewSession ? root.previewSession.mediaHost : null

    contentItem: Column {
        AppMenuItem {
            width: parent.width
            text: qsTrId("preview.canvas.free_aspect")
            checkable: true
            checked: root.preferences && root.preferences.previewCanvasFreeAspect
            onTriggered: {
                if (!root.preferences)
                    return
                root.preferences.previewCanvasFreeAspect = !root.preferences.previewCanvasFreeAspect
                checked = Qt.binding(() => root.preferences && root.preferences.previewCanvasFreeAspect)
            }
        }

        AppMenuItem {
            id: hidePvItem
            width: parent.width
            text: qsTrId("preview.canvas.hide_pv")
            checkable: true
            enabled: !!(root.mediaHost && root.mediaHost.chartHasVideoBackground)
            checked: root.preferences && root.preferences.previewHidePv
            onTriggered: {
                if (!root.preferences || !hidePvItem.enabled)
                    return
                root.preferences.previewHidePv = !root.preferences.previewHidePv
                if (root.mediaHost)
                    root.mediaHost.hidePv = root.preferences.previewHidePv
                checked = Qt.binding(() => root.preferences && root.preferences.previewHidePv)
            }
        }

        AppMenuSeparator { width: parent.width }
        AppMenuItem {
            objectName: "previewShowMeasureLinesItem"
            width: parent.width
            text: qsTrId("preview.canvas.show_measure_lines")
            checkable: true
            checked: root.aidSettings.showMeasureLines === true
            onTriggered: {
                root.putAid("showMeasureLines", !root.aidSettings.showMeasureLines)
                checked = Qt.binding(() => root.aidSettings.showMeasureLines === true)
            }
        }
        Column {
            width: parent.width
            leftPadding: 12
            rightPadding: 16
            spacing: 4
            visible: root.aidSettings.showMeasureLines === true
            AppComboBox {
                objectName: "previewMeasureLineModeCombo"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitWidth: 280
                model: [qsTrId("preview.canvas.measure_chart_clock"),
                        qsTrId("preview.canvas.measure_custom_beats"),
                        qsTrId("preview.canvas.measure_text_lines")]
                currentIndex: root.aidSettings.measureLineMode || 0
                onActivated: root.putAid("measureLineMode", currentIndex)
                Accessible.name: qsTrId("preview.canvas.measure_mode")
            }
            AppTextField {
                objectName: "previewMeasureLineCustomBeatsField"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitWidth: 280
                visible: root.aidSettings.measureLineMode === 1
                text: String(root.aidSettings.measureLineCustomBeats || 4)
                validator: IntValidator { bottom: 1; top: 2147483647 }
                inputMethodHints: Qt.ImhDigitsOnly
                Accessible.name: qsTrId("preview.canvas.measure_custom_beats")
                onEditingFinished: {
                    if (acceptableInput) root.putAid("measureLineCustomBeats", Number(text))
                    text = Qt.binding(() => String(root.aidSettings.measureLineCustomBeats || 4))
                }
            }
            Item { width: 1; height: 4 }
        }
        AppMenuItem {
            objectName: "previewShowJudgementSkeletonItem"
            width: parent.width
            text: qsTrId("preview.canvas.show_judgement_skeleton")
            checkable: true
            checked: root.aidSettings.showJudgementSkeleton === true
            onTriggered: {
                root.putAid("showJudgementSkeleton", !root.aidSettings.showJudgementSkeleton)
                checked = Qt.binding(() => root.aidSettings.showJudgementSkeleton === true)
            }
        }
        AppMenuItem {
            objectName: "previewShowSlideJudgementTimingItem"
            width: parent.width
            text: qsTrId("preview.canvas.show_slide_judgement_timing")
            checkable: true
            checked: root.aidSettings.showSlideJudgementTiming === true
            onTriggered: {
                root.putAid("showSlideJudgementTiming", !root.aidSettings.showSlideJudgementTiming)
                checked = Qt.binding(() => root.aidSettings.showSlideJudgementTiming === true)
            }
        }
        AppMenuItem {
            objectName: "previewSlideTimingFromLaunchBeatItem"
            width: parent.width
            text: qsTrId("preview.canvas.slide_timing_from_launch_beat")
            enabled: root.aidSettings.showSlideJudgementTiming === true
            checkable: true
            checked: root.aidSettings.slideTimingFromLaunchBeat === true
            onTriggered: {
                root.putAid("slideTimingFromLaunchBeat", !root.aidSettings.slideTimingFromLaunchBeat)
                checked = Qt.binding(() => root.aidSettings.slideTimingFromLaunchBeat === true)
            }
        }
    }
}
