import QtQuick
import MiaCode.UI

Item {
    id: root

    required property var viewState
    required property var documentSession
    required property var preferences
    required property var commands
    required property var pages
    property bool compact: false
    property bool opensDrawer: false
    property string pendingChartEditor: ""
    readonly property bool efficientMode: preferences.efficientSidebarEnabled
    readonly property bool primarySidebarVisible: compact || viewState.sidebarVisible
    readonly property Item chartSidebarItem: efficientMode
        ? efficientSidebar.navigationContentItem : chartSidebar.navigationContentItem
    readonly property Item cornerSourceItem: root.viewState.activeSidebarView === "export"
        ? exportSidebar.cornerSourceItem : root.viewState.activeSidebarView === "network"
        ? networkSidebar.cornerSourceItem : root.viewState.activeSidebarView === "tools"
        ? toolsSidebar.cornerSourceItem : efficientMode ? efficientSidebar.cornerSourceItem : chartSidebar.cornerSourceItem

    signal mediaToolRequested(string toolId)

    function showMediaToolsMenu() {
        root.viewState.activeSidebarView = "tools"
        revealPanel()
    }

    function revealPanel() {
        viewState.sidebarVisible = true
        preferences.sidebarVisible = true
        if (opensDrawer)
            viewState.compactPanel = "sidebar"
    }

    clip: true

    // Activity Bar 只负责功能域导航。再次选择当前功能时，桌面端切换
    // Primary Sidebar；紧凑布局直接关闭当前覆盖层。
    function activateView(viewId, panelPresented) {
        const pageAlreadyActive = viewId === "export" ? root.pages.activePageId === "export"
            : viewId === "chart" ? !root.pages.overlayActive && !root.viewState.netEditorActive
            : true
        const presented = opensDrawer ? viewState.compactPanel === "sidebar" : panelPresented
        if (viewState.activeSidebarView === viewId && pageAlreadyActive && presented) {
            if (opensDrawer) {
                viewState.compactPanel = ""
                return
            }
            viewState.sidebarVisible = !viewState.sidebarVisible
            root.preferences.sidebarVisible = viewState.sidebarVisible
            return
        }

        if (viewId === "chart" && root.documentSession.hasDocument) {
            const lastChartEditor = root.viewState.lastChartEditor()
            if (root.pages.overlayActive) {
                root.pendingChartEditor = lastChartEditor
                root.pages.rememberEditorReturnTarget(lastChartEditor)
                if (!root.pages.leaveOverlayPage())
                    root.pendingChartEditor = ""
            } else if (lastChartEditor.length > 0) {
                root.viewState.activateEditor(lastChartEditor)
            }
        }

        viewState.activeSidebarView = viewId
        revealPanel()

        if (viewId === "export" && root.documentSession.hasDocument) {
            root.pages.rememberEditorReturnTarget(root.viewState.activeEditorKey)
            root.pages.openVideoExportPage()
        }
    }

    Connections {
        target: root.pages
        function onOverlayPageLeft() {
            const key = root.pendingChartEditor
            root.pendingChartEditor = ""
            if (key.length > 0 && root.viewState.containsEditor(key))
                root.viewState.activateEditor(key)
        }
        function onNavigationRejected() { root.pendingChartEditor = "" }
    }

    Item {
        id: pageHost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        visible: root.primarySidebarVisible

        ChartFieldSidebar {
            id: chartSidebar
            color: root.compact ? "transparent" : Theme.surfaceColor(Theme.colors.background.panel)
            anchors.fill: parent
            visible: !root.efficientMode && root.viewState.activeSidebarView === "chart"
            enabled: root.documentSession.hasDocument
            viewState: root.viewState
            documentSession: root.documentSession
            commands: root.commands
            pages: root.pages
        }

        EfficientSidebar {
            id: efficientSidebar
            anchors.fill: parent
            visible: root.efficientMode && root.viewState.activeSidebarView === "chart"
            nativeMaterialActive: !root.compact && Theme.nativeMaterialActive
            color: root.compact ? "transparent"
                : Theme.chromeSurfaceColor(Theme.colors.background.panel, Theme.nativeMaterialActive)
            viewState: root.viewState
            documentSession: root.documentSession
            commands: root.commands
            pages: root.pages
        }

        UtilitySidebarPage {
            id: networkSidebar
            anchors.fill: parent
            visible: root.viewState.activeSidebarView === "network"
            mode: "network"
            pages: root.pages
            documentAvailable: root.documentSession.hasDocument
        }

        UtilitySidebarPage {
            id: toolsSidebar
            anchors.fill: parent
            visible: root.viewState.activeSidebarView === "tools"
            mode: "tools"
            pages: root.pages
            documentAvailable: root.documentSession.hasDocument
            onToolRequested: toolId => root.mediaToolRequested(toolId)
        }

        ExportSidebarPage {
            id: exportSidebar
            color: root.compact ? "transparent" : Theme.surfaceColor(Theme.colors.background.panel)
            anchors.fill: parent
            visible: root.viewState.activeSidebarView === "export"
            documentAvailable: root.documentSession.hasDocument
            pages: root.pages
        }
    }
}
