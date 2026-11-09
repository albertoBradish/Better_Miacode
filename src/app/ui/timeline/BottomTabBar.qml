pragma ComponentBehavior: Bound

import QtQuick
import MiaCode.UI

Item {
    id: root

    required property var workspace
    required property var tabs
    required property string currentTabId

    readonly property int edgePadding: 4
    readonly property int rowVerticalMargin:
        edgePadding - (Theme.controlMinHeight - Theme.controlHighlightHeight) / 2
    readonly property int rowHorizontalMargin: edgePadding - Theme.chromeInsetX
    readonly property real minimumWidth: tabRow.width + 2 * rowHorizontalMargin
    implicitHeight: Theme.controlMinHeight + 2 * rowVerticalMargin

    function tabBefore(position) {
        const x = mapToItem(tabRow, position).x
        for (let i = 0; i < tabRepeater.count; ++i) {
            const tab = tabRepeater.itemAt(i)
            if (tab && x < tab.x + tab.width / 2)
                return root.tabs[i]
        }
        return ""
    }

    Flickable {
        id: tabScroll
        anchors.fill: parent
        anchors.topMargin: root.rowVerticalMargin
        anchors.bottomMargin: root.rowVerticalMargin
        anchors.leftMargin: root.rowHorizontalMargin
        anchors.rightMargin: root.rowHorizontalMargin
        contentWidth: tabRow.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        clip: interactive

        Row {
            id: tabRow
            spacing: 4

            Repeater {
                id: tabRepeater
                model: root.tabs

                DockPanelTab {
                    required property string modelData
                    panelId: modelData
                    workspace: root.workspace
                    checked: panelId === root.currentTabId
                    onActiveFocusChanged: {
                        if (!activeFocus || !tabScroll.interactive)
                            return
                        if (x < tabScroll.contentX)
                            tabScroll.contentX = x
                        else if (x + width > tabScroll.contentX + tabScroll.width)
                            tabScroll.contentX = x + width - tabScroll.width
                    }
                }
            }
        }
    }
}
