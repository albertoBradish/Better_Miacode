pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root

    required property string currentTabId
    required property var analysisSession
    required property var timelineSession

    readonly property int edgePadding: 4
    readonly property int rowVerticalMargin:
        edgePadding - (Theme.controlMinHeight - Theme.controlHighlightHeight) / 2
    readonly property int rowHorizontalMargin:
        edgePadding - Theme.chromeInsetX
    implicitHeight: Theme.controlMinHeight + 2 * rowVerticalMargin
    readonly property real minimumWidth: tabLayout.implicitWidth
        + 2 * rowHorizontalMargin

    function revealTab(tab) {
        if (tab.x < tabScroll.contentX)
            tabScroll.contentX = tab.x
        else if (tab.x + tab.width > tabScroll.contentX + tabScroll.width)
            tabScroll.contentX = tab.x + tab.width - tabScroll.width
    }

    function countIssues(rows) {
        let errors = 0
        let warnings = 0
        for (const row of rows) {
            if (row.severity === "error")
                ++errors
            else if (row.severity === "warning")
                ++warnings
        }
        return { errors: errors, warnings: warnings }
    }

    component BottomTab: AppTabButton {
        id: tab

        property int count: -1
        property color countColor: Theme.colors.accent.badge
        readonly property int badgeFontSize: Theme.uiFontSize - 2
        readonly property int badgeHeight: badgeFontSize + 5

        Layout.alignment: Qt.AlignVCenter
        onActiveFocusChanged: if (activeFocus && tabScroll.interactive) root.revealTab(tab)
        Accessible.description: count > 0 ? String(count) : ""

        accessory: count > 0 ? badgeAccessory : null

        Component {
            id: badgeAccessory
            Rectangle {
                implicitWidth: tab.count > 0
                    ? Math.max(implicitHeight, Math.ceil(countLabel.implicitWidth) + 8) : 0
                implicitHeight: tab.count > 0 ? tab.badgeHeight : 0
                radius: height / 2
                color: tab.countColor
                visible: tab.count > 0

                Text {
                    id: countLabel
                    anchors.fill: parent
                    text: String(tab.count)
                    font.family: Theme.uiFont
                    font.pixelSize: tab.badgeFontSize
                    font.preferTypoLineMetrics: true
                    color: Theme.colors.text.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    component AnalysisTab: BottomTab {
        required property var issueCounts
        count: issueCounts.errors > 0 ? issueCounts.errors : issueCounts.warnings
        countColor: issueCounts.errors > 0
            ? Theme.colors.danger.primary : Theme.colors.accent.badge
    }

    Flickable {
        id: tabScroll
        anchors.fill: parent
        anchors.topMargin: root.rowVerticalMargin
        anchors.bottomMargin: root.rowVerticalMargin
        anchors.leftMargin: root.rowHorizontalMargin
        anchors.rightMargin: root.rowHorizontalMargin
        contentWidth: tabLayout.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        clip: interactive

        RowLayout {
            id: tabLayout
            width: Math.max(tabScroll.width, implicitWidth)
            height: tabScroll.height
            spacing: 4

            AnalysisTab {
                visible: root.timelineSession.validationTabVisible
                text: root.timelineSession.validationTabLabel
                issueCounts: root.countIssues(root.analysisSession.validationRows)
                checked: root.currentTabId === "validation"
                onClicked: root.timelineSession.setCurrentTabId("validation")
            }
            AnalysisTab {
                visible: root.timelineSession.muriTabVisible
                text: root.timelineSession.muriTabLabel
                issueCounts: ({ errors: root.analysisSession.muriIssueCount, warnings: root.analysisSession.muriWarningCount })
                checked: root.currentTabId === "muri"
                onClicked: root.timelineSession.setCurrentTabId("muri")
            }

            Item { Layout.fillWidth: true }
        }
    }
}
