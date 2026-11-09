pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import MiaCode.UI

Item {
    id: root

    required property var analysisSession
    required property var documentSession
    required property string panelId
    readonly property bool muri: panelId === "muri"
    readonly property var rows: muri ? analysisSession.muriRows : analysisSession.validationRows
    readonly property bool pending: muri ? analysisSession.pending : analysisSession.validationPending

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

    Label {
        id: summary
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.analysisTextLeftInset
        anchors.rightMargin: Theme.panelPadding
        anchors.topMargin: root.analysisVerticalSpacing
        text: root.pending ? qsTrId("qml.analyzing") : root.muri
            ? qsTrId("validation.muri.summary").arg(root.muriIssueCount).arg(root.muriWarningCount)
            : qsTrId("qml.1_errors_2_warnings").arg(root.documentSession.syntaxErrorCount)
                .arg(root.documentSession.syntaxWarningCount)
        color: Theme.colors.text.secondary
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
    }

    ListView {
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
            leadingText: {
                if (root.muri) {
                    return "%1 · %2 · L%3:C%4".arg(modelData.alert === "warning"
                        ? qsTrId("validation.muri.alert.warning") : qsTrId("validation.muri.alert.muri"))
                        .arg(modelData.title).arg(modelData.line).arg(modelData.column)
                }
                if (modelData.code === "missing_difficulty_level")
                    return qsTrId("validation.difficulty_header")
                return "%1 · L%2:C%3".arg(modelData.severity === "error"
                    ? qsTrId("qml.errors") : qsTrId("validation.muri.alert.warning"))
                    .arg(modelData.line).arg(modelData.column)
            }
            bodyText: modelData.detail
        }

        ScrollBar.vertical: AppScrollBar {}
    }

    Label {
        anchors.centerIn: parent
        visible: root.rows.length === 0
        text: root.pending ? qsTrId("qml.analyzing") : root.muri
            ? qsTrId("validation.no_muri_issues_detected") : qsTrId("validation.no_syntax_errors_detected")
        color: Theme.colors.text.secondary
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
    }
}
