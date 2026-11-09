import QtQuick
import QtQuick.Controls
import QtQuick.Window
import MiaCode.UI

Item {
    id: nativeLayout

    required property var owner
    required property var workspaceDock
    required property Item sidebar
    required property Item preview
    required property Item editorPane
    required property Item exportVideoPage

    readonly property var workTree: workspaceDock.defaultTree.second
    readonly property var centerTree: owner.previewOnLeft ? workTree.second : workTree.first
    readonly property string centerPath: owner.previewOnLeft ? "11" : "10"
    readonly property bool editorVisible: workspaceDock.panelDocked("editor")
    readonly property bool centerVisible: editorVisible || owner.bottomPanelEffectivelyVisible
    readonly property real bottomMinimumWidth: nativeBottom.tabMinimumWidth
    readonly property real previewEditorAvailableWidth:
        Math.max(1, workspaceSplit.width - (nativePreview.visible ? Theme.splitDividerThickness : 0))
    property real sidebarDragWidth: 0
    property bool sidebarResizing: false
    anchors.fill: parent
    readonly property int orientation: Qt.Horizontal

    ResizeSnapState {
        id: sidebarSnap
        minimumSize: owner.preferences.sidebarMinimumContentWidth
        maximumSize: owner.preferences.sidebarMaximumContentWidth
        targets: [{ size: owner.preferences.sidebarDefaultWidth }]
        alignmentTolerance: 0.5 / nativeLayout.Screen.devicePixelRatio
    }

    function persistBottomPanelHeightRatio(finalHeight, sizeAtPress) {
        if (!owner.bottomPanelEffectivelyVisible || centerSplit.height <= 0)
            return
        const height = Math.max(bottomPanelSnap.minimumSize,
            Math.min(bottomPanelSnap.maximumSize, finalHeight))
        if (height === sizeAtPress)
            return
        // 比例按基础约束保存，吸附期间的固定高度属于临时布局约束。
        owner.preferences.bottomPanelHeightRatio =
            height <= bottomPanelSnap.minimumSize
                ? owner.preferences.bottomPanelMinimumHeightRatio
                : height >= bottomPanelSnap.maximumSize
                    ? owner.preferences.bottomPanelMaximumHeightRatio
                    : height / centerSplit.height
    }

    function persistSidebarWidth() {
        if (owner.compact) return
        if (owner.viewState.sidebarVisible)
            owner.preferences.sidebarWidth = Math.round(nativeLayout.sidebarDragWidth)
        owner.preferences.sidebarVisible = owner.viewState.sidebarVisible
    }

    function resizeSidebar(contentWidth) {
        sidebarSnap.update(contentWidth)
        nativeLayout.sidebarDragWidth = Math.max(owner.preferences.sidebarMinimumContentWidth,
            Math.min(owner.preferences.sidebarMaximumContentWidth,
                     sidebarSnap.snapped ? sidebarSnap.targetSize : contentWidth))
        owner.viewState.sidebarVisible = contentWidth >= owner.preferences.sidebarMinimumContentWidth / 2
    }

    function persistPreviewWidthRatio(finalWidth) {
        if (owner.previewDetached || owner.previewSurfaceMoving
                || !nativePreview.visible || !centerSplit.visible
                || nativeLayout.previewEditorAvailableWidth <= 0)
            return
        const width = Math.max(previewWidthSnap.minimumSize,
            Math.min(previewWidthSnap.maximumSize, finalWidth))
        owner.preferences.previewWidthRatio = width / nativeLayout.previewEditorAvailableWidth
    }

    function syncWorkspacePanelOrder() {
        const targetPreviewIndex = owner.previewOnLeft ? 0 : 1
        const currentPreviewIndex = workspaceSplit.itemAt(0) === nativePreview ? 0 : 1
        if (currentPreviewIndex !== targetPreviewIndex)
            workspaceSplit.moveItem(currentPreviewIndex, targetPreviewIndex)
    }

    Connections {
        target: owner.preferencesModel
        function onInterfaceChanged() {
            nativeLayout.syncWorkspacePanelOrder()
        }
    }

    Item {
        id: sidebarRail
        visible: !owner.compact && owner.viewState.sidebarVisible
        width: visible ? (nativeLayout.sidebarResizing ? nativeLayout.sidebarDragWidth : owner.preferences.sidebarWidth) : 0
        height: parent.height

        DockNode {
            id: sidebarSlot
            anchors.fill: parent
            visible: workspaceDock.panelDocked("sidebar")
            workspace: workspaceDock
            tree: workspaceDock.defaultTree.first
            path: "0"
        }
    }

    // Behind the split, so a fractional device pixel at the divider shows
    // the panel surface instead of the wallpaper. Kept out of the
    // SplitView background so it cannot intercept handle hover.
    Rectangle {
        x: workspaceSplit.x
        y: workspaceSplit.y
        width: workspaceSplit.width
        height: workspaceSplit.height
        color: Theme.surfaceColor(Theme.colors.background.panel)
    }

    SplitView {
        id: workspaceSplit
        orientation: Qt.Horizontal
        x: sidebarRail.width + sidebarHandle.width + sidebarToggle.width
        width: parent.width - x
        height: parent.height
        Component.onCompleted: nativeLayout.syncWorkspacePanelOrder()

        handle: SplitHandle {
            verticalDivider: true
            onReleased: previewWidthSnap.finishDrag(true)
        }

        SplitView {
            id: centerSplit
            orientation: Qt.Vertical
            visible: nativeLayout.centerVisible

            SplitView.fillWidth: true
            SplitView.minimumWidth: nativePreview.visible
                ? nativeLayout.previewEditorAvailableWidth * (1 - owner.preferences.previewMaximumWidthRatio)
                : owner.minimumEditorWidth
            SplitView.maximumWidth: nativePreview.visible
                ? nativeLayout.previewEditorAvailableWidth * (1 - owner.preferences.previewMinimumWidthRatio) : Infinity

            handle: SplitHandle {
                verticalDivider: false
                onReleased: bottomPanelSnap.finishDrag(true)
            }

            DockNode {
                id: nativeEditor
                visible: workspaceDock.panelDocked("editor")
                workspace: workspaceDock
                tree: nativeLayout.centerTree.first
                path: nativeLayout.centerPath + "0"
                SplitView.fillHeight: true
                SplitView.minimumHeight: 180
            }

            DockNode {
                id: nativeBottom
                visible: owner.bottomPanelEffectivelyVisible
                workspace: workspaceDock
                tree: nativeLayout.centerTree.second
                path: nativeLayout.centerPath + "1"
                keepTabStrip: true
                SplitView.fillHeight: !workspaceDock.panelDocked("editor")
                SplitView.minimumHeight: bottomPanelSnap.constrained
                    ? bottomPanelSnap.targetSize : bottomPanelSnap.minimumSize
                SplitView.maximumHeight: bottomPanelSnap.constrained
                    ? bottomPanelSnap.targetSize : bottomPanelSnap.maximumSize
            }
        }

        DockNode {
            id: nativePreview
            visible: workspaceDock.panelDocked("preview")
            workspace: workspaceDock
            tree: owner.previewOnLeft ? nativeLayout.workTree.first : nativeLayout.workTree.second
            path: owner.previewOnLeft ? "10" : "11"
            SplitView.fillWidth: !nativeLayout.centerVisible
            SplitView.minimumWidth: previewWidthSnap.constrained
                ? previewWidthSnap.targetSize : previewWidthSnap.minimumSize
            SplitView.maximumWidth: !nativeLayout.centerVisible ? Infinity : previewWidthSnap.constrained
                ? previewWidthSnap.targetSize : previewWidthSnap.maximumSize
        }
    }

    SplitSnapController {
        id: previewWidthSnap
        x: workspaceSplit.x
        y: workspaceSplit.y
        splitView: workspaceSplit
        resizedItem: nativePreview
        resizeDirection: owner.previewOnLeft ? 1 : -1
        minimumSize: nativeLayout.previewEditorAvailableWidth * owner.preferences.previewMinimumWidthRatio
        maximumSize: nativeLayout.previewEditorAvailableWidth * owner.preferences.previewMaximumWidthRatio
        targets: [{ size: nativeLayout.previewEditorAvailableWidth * owner.preferences.previewDefaultWidthRatio }]
        snapAvailable: dragAvailable && !owner.previewDetached && !owner.previewSurfaceMoving
            && centerSplit.visible
        onFinished: finalSize => nativeLayout.persistPreviewWidthRatio(finalSize)
    }

    SplitSnapController {
        id: bottomPanelSnap
        x: workspaceSplit.x + workspaceSplit.contentItem.x + centerSplit.x
        y: workspaceSplit.y + workspaceSplit.contentItem.y + centerSplit.y
        splitView: centerSplit
        resizedItem: nativeBottom
        resizeDirection: -1
        minimumSize: owner.bottomPanelEffectivelyVisible
            ? Math.max(owner.bottomMinimumHeight,
                centerSplit.height * owner.preferences.bottomPanelMinimumHeightRatio) : 0
        maximumSize: owner.bottomPanelEffectivelyVisible
            ? Math.max(owner.bottomMinimumHeight, Math.min(
                centerSplit.height * owner.preferences.bottomPanelMaximumHeightRatio,
                centerSplit.height - (nativeLayout.editorVisible ? nativeEditor.SplitView.minimumHeight : 0)
                    - Theme.splitDividerThickness)) : 0
        snapAvailable: dragAvailable
        readonly property bool lineSnapAvailable: dragAvailable
            && !owner.previewDetached && !owner.previewSurfaceMoving
            && nativePreview.visible && preview.visible && owner.transportItem?.visible === true
            && owner.transportItem.width > 0 && owner.transportItem.height > 0
        readonly property real activeLineCenterOffset:
            lineCenterOffset(Theme.splitHandleActiveThickness)
        readonly property real idleLineCenterOffset:
            lineCenterOffset(1 / (nativeLayout.Screen.devicePixelRatio * Theme.uiScale))
        // 两个宿主属于同一 SplitView；显式坐标依赖随预览布局变化更新。
        readonly property real targetCenter: lineSnapAvailable
            ? nativePreview.y + preview.y + owner.transportItem.y
                + owner.transportItem.topEdgeCenterY - centerSplit.y : NaN
        readonly property bool lineConstrained: constrained && snappedTargetIndex === 1
        targets: {
            const values = [{ size: centerSplit.height * owner.preferences.bottomPanelDefaultHeightRatio }]
            if (lineSnapAvailable) {
                values.push({
                    size: centerSplit.height - targetCenter + activeLineCenterOffset,
                    releaseSize: centerSplit.height - targetCenter + idleLineCenterOffset,
                    enterDistance: 4,
                    leaveDistance: 7
                })
            }
            return values
        }
        onFinished: (finalSize, sizeAtPress) =>
            nativeLayout.persistBottomPanelHeightRatio(finalSize, sizeAtPress)

        function lineCenterOffset(thickness) {
            return -Theme.splitDividerThickness
                + Math.floor((Theme.splitDividerThickness - thickness)
                             * nativeLayout.Screen.devicePixelRatio / 2) / nativeLayout.Screen.devicePixelRatio
                + thickness / 2
        }
    }

    // 覆盖层连接两侧高亮，横线主体由原生分割槽绘制。
    Rectangle {
        x: workspaceSplit.x + workspaceSplit.contentItem.x + centerSplit.x
            + (nativePreview.x < centerSplit.x ? -width : centerSplit.width)
        y: workspaceSplit.y + workspaceSplit.contentItem.y + centerSplit.y
            + centerSplit.contentItem.y + nativeBottom.y
            + bottomPanelSnap.activeLineCenterOffset - height / 2
        width: Theme.splitDividerThickness
        height: Theme.splitHandleActiveThickness
        z: 1
        visible: bottomPanelSnap.lineConstrained
        color: Theme.colors.accent.focus
        enabled: false
    }

    CornerMask {
        id: workspaceCorner
        visible: !!owner.backgroundSource && !owner.preferences.efficientSidebarEnabled
        x: 0
        backgroundSource: owner.backgroundSource
        backgroundOffset: Qt.point(owner.backgroundOffset.x + workspaceDock.x + x, owner.backgroundOffset.y)
        panelItem: !owner.compact && owner.viewState.sidebarVisible
            ? sidebar.cornerSourceItem
            : owner.previewOnLeft && nativePreview.visible
                ? preview.cornerSourceItem
                : owner.exportVideoActive ? exportVideoPage.cornerSourceItem
                    : editorPane.cornerSourceItem
        panelOffset: Qt.point(0, 0)
        panelBackingColor: panelItem === editorPane.cornerSourceItem
            ? "transparent" : Theme.surfaceColor(Theme.colors.background.panel)
    }

    Rectangle {
        x: workspaceCorner.visible ? workspaceCorner.x + workspaceCorner.width : 0
        y: 0
        width: parent.width - x
        height: 1 / nativeLayout.Screen.devicePixelRatio
        color: workspaceCorner.separatorColor
        enabled: false
    }

    Rectangle {
        x: workspaceCorner.x
        y: workspaceCorner.visible ? workspaceCorner.height : 0
        width: 1 / nativeLayout.Screen.devicePixelRatio
        height: parent.height - y
        visible: !owner.preferences.efficientSidebarEnabled
        color: workspaceCorner.separatorColor
        enabled: false
    }

    Rectangle {
        id: sidebarToggle
        x: sidebarRail.width
        y: 1 / nativeLayout.Screen.devicePixelRatio
        width: visible ? 20 : 0
        height: parent.height - y
        visible: !owner.compact && owner.preferences.efficientSidebarEnabled
        color: Theme.chromeSurfaceColor(Theme.colors.background.panel)

        IconButton {
            id: sidebarToggleButton
            objectName: "efficientSidebarToggleButton"
            anchors.centerIn: parent
            width: parent.width
            height: 32
            iconWidth: 14
            iconHeight: 14
            Accessible.name: tooltip
            background: Rectangle {
                radius: Theme.compactControlRadius
                color: Theme.overlayColor(sidebarToggleButton.down
                    ? Theme.chromeStateColors.pressed
                    : sidebarToggleButton.hovered || sidebarToggleButton.visualFocus
                        ? Theme.chromeStateColors.hover : "transparent")
            }
            iconSource: owner.viewState.sidebarVisible
                ? Qt.resolvedUrl("icons/chevron-left.svg")
                : Qt.resolvedUrl("icons/chevron-right.svg")
            tooltip: owner.viewState.sidebarVisible
                ? qsTrId("qml.close_sidebar") : qsTrId("qml.open_sidebar")
            onClicked: {
                owner.viewState.sidebarVisible = !owner.viewState.sidebarVisible
                owner.preferences.sidebarVisible = owner.viewState.sidebarVisible
            }
        }
    }

    SplitHandle {
        id: sidebarHandle
        verticalDivider: true
        x: sidebarRail.width + sidebarToggle.width
        width: visible && (owner.viewState.sidebarVisible || owner.preferences.efficientSidebarEnabled)
            ? Theme.splitDividerThickness : 0
        height: parent.height
        visible: !owner.compact
        showDivider: owner.viewState.sidebarVisible || owner.preferences.efficientSidebarEnabled
        handlePressed: sidebarDrag.pressed
        handleHovered: sidebarDrag.containsMouse

        MouseArea {
            id: sidebarDrag
            anchors.centerIn: parent
            width: Theme.splitHandleHitExtent
            height: parent.height
            hoverEnabled: true
            cursorShape: Qt.SplitHCursor
            preventStealing: true
            property real startX: 0
            property real startWidth: 0

            onPressed: mouse => {
                startX = mapToItem(nativeLayout, mouse.x, mouse.y).x
                startWidth = owner.viewState.sidebarVisible ? owner.preferences.sidebarWidth : 0
                nativeLayout.sidebarDragWidth = owner.preferences.sidebarWidth
                sidebarSnap.begin(startWidth)
                nativeLayout.sidebarResizing = true
            }
            onPositionChanged: mouse => {
                if (pressed)
                    nativeLayout.resizeSidebar(startWidth + mapToItem(nativeLayout, mouse.x, mouse.y).x - startX)
            }
            onReleased: {
                nativeLayout.persistSidebarWidth()
                sidebarSnap.end()
                nativeLayout.sidebarResizing = false
            }
            onCanceled: {
                nativeLayout.persistSidebarWidth()
                sidebarSnap.end()
                nativeLayout.sidebarResizing = false
            }
        }
    }

    Binding {
        target: owner.transportItem
        property: "topEdgeHighlighted"
        value: bottomPanelSnap.lineConstrained
        restoreMode: Binding.RestoreNone
    }

    // 分离期间保留宽度比例，回归或主窗口缩放时按当前可用宽度恢复。
    Binding {
        target: nativePreview.SplitView
        property: "preferredWidth"
        value: nativeLayout.previewEditorAvailableWidth * owner.preferences.previewWidthRatio
        when: !workspaceSplit.resizing
        restoreMode: Binding.RestoreNone
    }

    // 原生 SplitView 管理拖动高度；松开后接回持久比例绑定。
    // 缩小窗口触及高度下限后，放大仍按用户原有比例计算。
    Binding {
        target: nativeBottom.SplitView
        property: "preferredHeight"
        value: owner.bottomPanelEffectivelyVisible
            ? centerSplit.height * owner.preferences.bottomPanelHeightRatio : 0
        when: !centerSplit.resizing
        restoreMode: Binding.RestoreNone
    }
}
