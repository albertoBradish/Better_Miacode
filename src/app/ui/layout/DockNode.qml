pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import MiaCode.UI

Item {
    id: root

    required property var workspace
    required property var tree
    required property string path
    property bool keepTabStrip: false
    readonly property real tabMinimumWidth: leaf ? (loader.item?.tabMinimumWidth ?? 0) : 0

    readonly property bool leaf: tree ? !!tree.tabs : false
    readonly property var tabs: leaf ? tree.tabs.filter(workspace.panelDocked) : []
    readonly property string selected: leaf && tabs.length > 0
        ? (tabs.includes(tree.selected) ? tree.selected : tabs[0]) : ""
    readonly property bool occupied: leaf ? tabs.length > 0
        : workspace.branchOccupied(tree)

    Loader {
        id: loader
        anchors.fill: parent
        active: root.occupied
        sourceComponent: root.leaf ? tabGroup : branch
    }

    component ChildNode: Loader {
        id: child
        required property var subTree
        required property string subPath
        active: root.workspace.branchOccupied(subTree)
        visible: active
        Component.onCompleted: {
            setSource(Qt.resolvedUrl("DockNode.qml"), {
                workspace: root.workspace, tree: Qt.binding(() => child.subTree), path: subPath
            })
        }
    }

    Component {
        id: branch

        SplitView {
            id: splitView
            readonly property bool horizontal: root.tree.axis === "horizontal"
            orientation: horizontal ? Qt.Horizontal : Qt.Vertical

            handle: SplitHandle {
                verticalDivider: splitView.horizontal
                onReleased: {
                    if (!firstPane.visible || !secondPane.visible)
                        return
                    const usable = splitView.horizontal
                        ? firstPane.width + secondPane.width : firstPane.height + secondPane.height
                    if (usable > 0)
                        root.workspace.resize(root.path, (splitView.horizontal ? firstPane.width : firstPane.height) / usable)
                }
            }

            ChildNode {
                id: firstPane
                subTree: root.tree.first
                subPath: root.path + "0"
                SplitView.fillWidth: !secondPane.active
                SplitView.fillHeight: !secondPane.active
                SplitView.minimumWidth: splitView.horizontal ? Math.min(110, splitView.width * 0.2) : 0
                SplitView.minimumHeight: !splitView.horizontal ? Math.min(90, splitView.height * 0.2) : 0
            }

            ChildNode {
                id: secondPane
                subTree: root.tree.second
                subPath: root.path + "1"
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                SplitView.minimumWidth: firstPane.SplitView.minimumWidth
                SplitView.minimumHeight: firstPane.SplitView.minimumHeight
            }

            Binding {
                target: firstPane.SplitView
                property: "preferredWidth"
                when: splitView.horizontal && !splitView.resizing
                value: Math.max(0, splitView.width - Theme.splitDividerThickness) * root.tree.ratio
                restoreMode: Binding.RestoreNone
            }
            Binding {
                target: firstPane.SplitView
                property: "preferredHeight"
                when: !splitView.horizontal && !splitView.resizing
                value: Math.max(0, splitView.height - Theme.splitDividerThickness) * root.tree.ratio
                restoreMode: Binding.RestoreNone
            }
        }
    }

    Component {
        id: tabGroup

        Item {
            id: groupItem
            readonly property string path: root.path
            readonly property var tabs: root.tabs
            readonly property string selected: root.selected
            readonly property Item slot: panelSlot
            readonly property bool showsTabs: tabs.length > 1 || root.keepTabStrip
            readonly property real stripHeight: showsTabs ? strip.height : 0
            readonly property real tabMinimumWidth: showsTabs ? tabBar.minimumWidth : 0

            function tabBefore(point) { return showsTabs ? tabBar.tabBefore(point) : "" }

            Component.onCompleted: root.workspace.registerGroup(groupItem)
            Component.onDestruction: root.workspace.unregisterGroup(groupItem)

            Rectangle {
                id: strip
                width: parent.width
                height: groupItem.showsTabs ? tabBar.implicitHeight : 0
                visible: groupItem.showsTabs
                color: Theme.surfaceColor(Theme.colors.background.panel)

                BottomTabBar {
                    id: tabBar
                    anchors.fill: parent
                    workspace: root.workspace
                    tabs: groupItem.tabs
                    currentTabId: groupItem.selected
                }
            }

            Item {
                id: panelSlot
                anchors.top: strip.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true

                TapHandler {
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: if (groupItem.selected) root.workspace.activePanel = groupItem.selected
                }

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered) root.workspace.hoveredPanel = groupItem.selected
                        else if (root.workspace.hoveredPanel === groupItem.selected) root.workspace.hoveredPanel = ""
                    }
                }
            }

            DockPanelGrip {
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !root.workspace.maximizedPanel && !groupItem.showsTabs && groupItem.selected.length > 0
                panelId: groupItem.selected
                workspace: root.workspace
            }
        }
    }
}
