pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root

    required property var analysisSession
    required property var documentSession
    required property string panelId
    readonly property bool muri: panelId === "muri"
    readonly property bool playChecks: panelId === "play_checks"
    readonly property var rows: playChecks ? analysisSession.playRows : muri ? analysisSession.muriRows : analysisSession.validationRows
    readonly property bool pending: playChecks ? analysisSession.playPending : muri ? analysisSession.pending : analysisSession.validationPending

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceColor(Theme.colors.background.panel)
    }
    clip: true
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
        property string locationText: ""

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

                RowLayout {
                    width: parent.width
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.alignment: Qt.AlignTop
                        text: issueRow.leadingText
                        color: issueRow.leadingColor
                        wrapMode: Text.Wrap
                        font.family: Theme.uiFont
                        font.pixelSize: Theme.uiFontSize
                    }

                    Label {
                        Layout.alignment: Qt.AlignTop
                        visible: text.length > 0
                        text: issueRow.locationText
                        color: Theme.colors.text.secondary
                        font.family: Theme.codeFont.family
                        font.pixelSize: Theme.uiFontSize
                    }
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

    Label {
        id: summary
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.analysisTextLeftInset
        anchors.rightMargin: Theme.panelPadding
        anchors.topMargin: root.analysisVerticalSpacing
        text: root.pending ? qsTrId("qml.analyzing") : root.playChecks
            ? qsTrId("play.issues.summary").arg(root.analysisSession.playIssueCount) : root.muri
            ? qsTrId("validation.muri.summary").arg(root.muriIssueCount).arg(root.muriWarningCount)
            : qsTrId("qml.1_errors_2_warnings").arg(root.documentSession.syntaxErrorCount)
                .arg(root.documentSession.syntaxWarningCount)
        color: Theme.colors.text.secondary
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
    }

    ListView {
        id: issueList
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: summary.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: root.analysisVerticalSpacing
        anchors.leftMargin: root.analysisListLeftInset
        anchors.rightMargin: Theme.panelPadding
        clip: true
        model: root.rows

        delegate: AnalysisIssueRow {
            leadingText: root.muri || root.playChecks
                ? qsTrId("validation.muri.issue_heading").arg(modelData.title)
                    .arg(modelData.alert === "warning" ? qsTrId("validation.muri.alert.warning")
                        : qsTrId("validation.muri.alert.muri"))
                : modelData.code === "missing_difficulty_level" ? qsTrId("validation.difficulty_header")
                : modelData.severity === "error" ? qsTrId("qml.errors") : qsTrId("validation.muri.alert.warning")
            locationText: !root.muri && !root.playChecks && modelData.code === "missing_difficulty_level" ? ""
                : "L%1:C%2".arg(modelData.line).arg(modelData.column)
            bodyText: modelData.detail
        }

        ScrollBar.vertical: AppScrollBar {}
    }

    Label {
        anchors.centerIn: parent
        visible: issueList.count === 0 && (!root.pending || !root.playChecks)
        text: root.pending ? qsTrId("qml.analyzing") : root.playChecks
            ? qsTrId("play.issues.empty") : root.muri
            ? qsTrId("validation.no_muri_issues_detected") : qsTrId("validation.no_syntax_errors_detected")
        color: Theme.colors.text.secondary
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
    }
}
