pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import MiaCode.UI

Item {
    id: root

    required property var analysisSession
    required property var documentSession
    required property var timelineSession
    readonly property string currentTabId: (timelineSession.currentTabId === "muri" && timelineSession.muriTabVisible)
        || !timelineSession.validationTabVisible ? "muri" : "validation"
    signal analysisRowActivated(int difficultyId, var revision, int line, int column, int endColumn, double second)

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceColor(Theme.colors.background.panel)
    }
    clip: true
    readonly property int contentTopMargin: 0
    readonly property real minimumHeight: tabs.implicitHeight + 38
    readonly property real minimumWidth: tabs.minimumWidth
    readonly property real analysisTextLeftInset:
        Theme.panelPadding + Theme.chromePadding
    readonly property real analysisListLeftInset:
        Math.max(0, analysisTextLeftInset - Theme.rowPaddingX)
    readonly property real analysisVerticalSpacing: Theme.chromeInsetY
    readonly property int muriWarningCount: root.analysisSession.muriWarningCount
    readonly property int muriIssueCount: root.analysisSession.muriIssueCount

    component AnalysisIssueRow: ChromeRow {
        id: issueRow

        required property var modelData
        required property string leadingText
        required property string bodyText

        readonly property color leadingColor: modelData.severity === "error"
            ? Theme.colors.syntax.error
            : Theme.colors.syntax.warning

        width: ListView.view.width
        height: implicitHeight
        onClicked: root.analysisSession.activateRow(modelData)

        contentItem: Item {
            implicitHeight: issueContent.implicitHeight

            Column {
                id: issueContent

                width: parent.width
                y: (parent.height - implicitHeight) / 2
                spacing: 2

                Label {
                    width: parent.width
                    text: issueRow.leadingText
                    color: issueRow.leadingColor
                    wrapMode: Text.Wrap
                    font.family: Theme.codeFont.family
                    font.pixelSize: Theme.uiFontSize
                }

                Label {
                    width: parent.width
                    text: issueRow.bodyText
                    color: Theme.colors.text.primary
                    wrapMode: Text.Wrap
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.uiFontSize
                }
            }
        }
    }

    // Theme owns timeline surfaces; the palette's timeline group only supplies
    // content colors. The bridge exists before TimelineQuickItem so both sets
    // reach the C++ scene snapshot before its first frame.
    BottomTabBar {
        id: tabs
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        currentTabId: root.currentTabId
        analysisSession: root.analysisSession
        timelineSession: root.timelineSession
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.topMargin: root.contentTopMargin
        anchors.bottom: parent.bottom
        visible: root.currentTabId === "validation"

        Row {
            id: summaryRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: root.analysisTextLeftInset
            anchors.rightMargin: Theme.panelPadding
            anchors.topMargin: root.analysisVerticalSpacing

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: root.analysisSession.validationPending
                    ? qsTrId("qml.analyzing")
                    : qsTrId("qml.1_errors_2_warnings")
                        .arg(root.documentSession.syntaxErrorCount)
                        .arg(root.documentSession.syntaxWarningCount)
                color: Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize
            }
        }

        ListView {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: summaryRow.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: root.analysisVerticalSpacing
            anchors.leftMargin: root.analysisListLeftInset
            anchors.rightMargin: Theme.panelPadding
            clip: true
            model: root.analysisSession.validationRows

            delegate: AnalysisIssueRow {
                id: issueDelegate
                leadingText: modelData.code === "missing_difficulty_level"
                    ? qsTrId("validation.difficulty_header")
                    : "%1 · L%2:C%3".arg(modelData.severity === "error"
                        ? qsTrId("qml.errors") : qsTrId("validation.muri.alert.warning"))
                        .arg(modelData.line).arg(modelData.column)
                bodyText: modelData.detail
            }

            ScrollBar.vertical: AppScrollBar {}
        }

        Label {
            anchors.centerIn: parent
            visible: root.analysisSession.validationRows.length === 0
            text: root.analysisSession.validationPending
                ? qsTrId("qml.analyzing")
                : qsTrId("validation.no_syntax_errors_detected")
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.topMargin: root.contentTopMargin
        anchors.bottom: parent.bottom
        visible: root.currentTabId === "muri"

        Row {
            id: muriSummaryRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: root.analysisTextLeftInset
            anchors.rightMargin: Theme.panelPadding
            anchors.topMargin: root.analysisVerticalSpacing

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: root.analysisSession.pending
                    ? qsTrId("qml.analyzing")
                    : qsTrId("validation.muri.summary")
                        .arg(root.muriIssueCount)
                        .arg(root.muriWarningCount)
                color: Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize
            }
        }

        ListView {
            id: muriList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: muriSummaryRow.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: root.analysisVerticalSpacing
            anchors.leftMargin: root.analysisListLeftInset
            anchors.rightMargin: Theme.panelPadding
            clip: true
            model: root.analysisSession.muriRows
            delegate: AnalysisIssueRow {
                id: muriDelegate
                leadingText: "%1 · %2 · L%3:C%4".arg(modelData.alert === "warning"
                    ? qsTrId("validation.muri.alert.warning")
                    : qsTrId("validation.muri.alert.muri"))
                    .arg(modelData.title)
                    .arg(modelData.line)
                    .arg(modelData.column)
                bodyText: modelData.detail
            }
            ScrollBar.vertical: AppScrollBar {}
        }
        Label {
            anchors.centerIn: parent
            visible: root.analysisSession.muriRowCount === 0
            text: root.analysisSession.pending
                ? qsTrId("qml.analyzing")
                : qsTrId("validation.no_muri_issues_detected")
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }
    }

    Connections {
        target: root.analysisSession
        function onRowActivated(difficultyId, revision, line, column, endColumn, second) {
            root.analysisRowActivated(difficultyId, revision, line, column, endColumn, second)
        }
    }
}
