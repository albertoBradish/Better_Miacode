pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQml.Models
import MiaCode.UI
import "WorkbenchLayouts.js" as Layouts

Item {
    id: root

    required property var preferences
    required property var availablePanels
    required property var defaultTree
    required property var builtinTree
    required property var floatPreview
    required property var dockPreview
    required property var previewGeometry
    required property Component editorShortcuts
    required property var restorePreviewGeometry
    property var preferencesModel
    property Component nativeFrame
    property var issueCounts: ({})
    property var nativePreviewWindow
    property bool keyboardShortcutsEnabled: true
    property var compactSidebarShown

    readonly property var config: preferences.workspaceLayout
    readonly property var customTree: config.version === 1 ? Layouts.normalizedTree(config.tree) : null
    readonly property var tree: customTree || defaultTree
    readonly property var floating: customTree ? Layouts.normalizedFloating(config.floating) : ({})
    readonly property bool defaultNativeLayout: !!nativeFrame && !customTree && preferences.timelineLayoutMode === "horizontal"
    readonly property bool useNativeFrame: defaultNativeLayout && maximizedPanel.length === 0
    readonly property var nativeFrameItem: nativeFrameLoader.item
    readonly property var panelIds: Layouts.panels
    readonly property var savedNames: preferences.savedWorkspaceLayoutOrder ?? Object.keys(preferences.savedWorkspaceLayouts).sort((a, b) => a.localeCompare(b))
    property var groups: []
    property var floatingHosts: ({})
    property string dragging: ""
    property point dragPosition
    property var drop: null
    property string activePanel: ""
    property string hoveredPanel: ""
    property string maximizedPanel: ""
    readonly property string maximizationTarget: currentPanel()
    readonly property bool targetMaximized: maximizedPanel.length > 0 || windowMaximized(windowFor(maximizationTarget))
    signal savedLayoutApplied()
    signal panelActivated(string panelId)
    signal panelVisibilityChanged(string panelId, bool shown)

    function panelTitle(id) {
        switch (id) {
        case "editor": return qsTrId("workspace.editor")
        case "timeline": return qsTrId("window.timeline")
        case "preview": return qsTrId("workspace.preview_panel")
        case "sidebar": return qsTrId("workspace.sidebar")
        case "validation": return qsTrId("window.syntax")
        case "muri": return qsTrId("window.muri")
        }
        return ""
    }

    function panelShown(id) {
        if (id === "sidebar") return compactSidebarShown ?? preferences.sidebarVisible
        if (id === "timeline") return preferences.bottomPanelVisible && preferences.timelinePanelVisible
        if ((id === "validation" || id === "muri") && !preferences.bottomPanelVisible) return false
        return preferences.workspaceHiddenPanels?.[id] !== true
    }

    function setPanelShown(id, shown) {
        if (!panelIds.includes(id) || panelShown(id) === shown)
            return
        if (id === "sidebar") {
            preferences.sidebarVisible = shown
        } else if (id === "timeline") {
            preferences.timelinePanelVisible = shown
        } else {
            const hidden = Object.assign({}, preferences.workspaceHiddenPanels)
            if (shown) delete hidden[id]
            else hidden[id] = true
            preferences.workspaceHiddenPanels = hidden
        }
        if (shown && (id === "timeline" || id === "validation" || id === "muri"))
            preferences.bottomPanelVisible = true
        panelVisibilityChanged(id, shown)
        if (shown) activate(id)
    }

    function panelDocked(id) {
        return root.availablePanels.includes(id) && !root.isFloating(id)
    }

    function branchOccupied(node) {
        return node ? (node.tabs ? node.tabs.some(root.panelDocked)
                                : branchOccupied(node.first) || branchOccupied(node.second)) : false
    }

    function panelPresent(id) {
        return availablePanels.includes(id) || (id === "preview" && preferences.previewDetached && panelShown(id))
    }

    function windowFor(id) {
        if (id === "preview" && preferences.previewDetached) return nativePreviewWindow
        return isFloating(id) ? floatingHosts[id]?.Window.window : null
    }

    function windowMaximized(window) {
        return window?.visibility === Window.Maximized || window?.visibility === Window.FullScreen
    }

    function isWithin(item, ancestor) {
        for (let current = item; current; current = current.parent) {
            if (current === ancestor) return true
        }
        return false
    }

    function panelForFocus(item) {
        for (const group of groups) {
            if (group?.slot && isWithin(item, group.slot)) return group.selected
        }
        return ""
    }

    function currentPanel() {
        if (maximizedPanel) return maximizedPanel
        if (panelPresent(activePanel)) return activePanel
        if (panelPresent(hoveredPanel)) return hoveredPanel
        return availablePanels.includes("editor") ? "editor" : (availablePanels[0] ?? "")
    }

    function keyboardAllowed(window) {
        // Qt.Tool windows may keep both parent and child marked active. Only
        // the window holding Quick focus may register these shared gestures.
        if (!keyboardShortcutsEnabled || !window || !window.active) return false
        const focusItem = window.activeFocusItem
        if (!focusItem) return false
        if (isWithin(focusItem, window.Overlay.overlay)) return false
        if (focusItem.workspaceSourceEditor === true) return false
        return focusItem.reservesPlainSpace !== true && !(focusItem instanceof TextInput) && !(focusItem instanceof TextEdit)
    }

    function togglePanelMaximization(id, floatingWindow) {
        if (!panelPresent(id)) return
        activePanel = id
        if (floatingWindow && floatingWindow !== root.Window.window) {
            if (floatingWindow === nativePreviewWindow && floatingWindow.windowChrome
                    && floatingWindow.visibility !== Window.FullScreen) {
                floatingWindow.windowChrome.toggleMaximized()
            } else if (windowMaximized(floatingWindow)) {
                floatingWindow.showNormal()
            } else {
                floatingWindow.showMaximized()
            }
            return
        }
        const focus = root.Window.window?.activeFocusItem
        const restoreFocus = focus && panelForFocus(focus) === id
        maximizedPanel = maximizedPanel === id ? "" : id
        if (restoreFocus) Qt.callLater(() => focus.forceActiveFocus())
    }

    function toggleCurrentPanelMaximization() {
        const id = maximizationTarget
        togglePanelMaximization(id, windowFor(id))
    }

    onAvailablePanelsChanged: {
        if (maximizedPanel && !availablePanels.includes(maximizedPanel)) maximizedPanel = ""
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            const id = root.panelForFocus(root.Window.window.activeFocusItem)
            if (id) root.activePanel = id
        }
    }

    Shortcut {
        sequence: "`"
        autoRepeat: false
        enabled: root.maximizationTarget.length > 0 && root.keyboardAllowed(root.Window.window)
        onActivated: root.showManager()
    }

    Shortcut {
        sequence: "Ctrl+`"
        autoRepeat: false
        enabled: root.maximizationTarget.length > 0 && root.keyboardAllowed(root.Window.window)
        onActivated: root.toggleCurrentPanelMaximization()
    }

    function showManager(window) {
        manager.parent = window?.Overlay.overlay ?? root.Overlay.overlay
        manager.open()
    }
    function validSavedLayout(name) {
        const saved = preferences.savedWorkspaceLayouts[name]
        return !!saved && saved.version === 1 && !!Layouts.normalizedTree(saved.tree)
    }

    function isFloating(id) {
        return !!floating[id]
    }

    function registerGroup(group) {
        groups = groups.concat(group)
    }

    function unregisterGroup(group) {
        groups = groups.filter(existing => existing !== group)
    }

    function hostFor(id) {
        if (isFloating(id) && floatingHosts[id])
            return floatingHosts[id]
        for (const group of groups) {
            if (group && group.selected === id)
                return group.slot
        }
        return staging
    }

    function commit(nextTree, floating) {
        preferences.workspaceLayout = { version: 1, tree: nextTree, floating: floating }
    }

    function activate(id) {
        if (!availablePanels.includes(id))
            return
        activePanel = id
        if (maximizedPanel && maximizedPanel !== id) maximizedPanel = ""
        panelActivated(id)
        if (useNativeFrame)
            return
        for (const group of groups) {
            if (group?.tabs.includes(id) && group.selected !== id) {
                commit(Layouts.activate(tree, id), floating)
                return
            }
        }
    }

    function resize(path, ratio) {
        if (ratio <= 0 || ratio >= 1)
            return
        commit(Layouts.replace(tree, path, node => Layouts.split(node.axis, ratio, node.first, node.second)),
               floating)
    }

    function updateDrag(id, point) {
        if (maximizedPanel)
            return
        dragging = id
        dragPosition = point
        drop = null
        for (const group of groups) {
            if (!group?.visible)
                continue
            const p = group.mapFromGlobal(point.x, point.y)
            if (p.x < 0 || p.y < 0 || p.x >= group.width || p.y >= group.height)
                continue
            const onTabStrip = p.y < group.stripHeight
            const edgeX = Math.min(64, group.width * 0.25)
            const edgeY = Math.min(64, group.height * 0.25)
            const edge = onTabStrip ? "center" : p.x < edgeX ? "left"
                : p.x > group.width - edgeX ? "right" : p.y < edgeY + group.stripHeight ? "top"
                : p.y > group.height - edgeY ? "bottom" : "center"
            drop = { group: group, edge: edge, beforeTab: onTabStrip ? group.tabBefore(p) : "" }
            return
        }
    }

    function cancelDrag() {
        dragging = ""
        drop = null
    }

    function finishDrag(id) {
        if (dragging !== id)
            return
        const destination = drop
        const point = dragPosition
        cancelDrag()
        if (destination) {
            if (destination.beforeTab === id || (destination.edge === "center"
                    && destination.group.tabs.length === 1 && destination.group.tabs[0] === id))
                return
            const nextTree = Layouts.dock(tree, id, destination.group.path, destination.edge, destination.beforeTab)
            const nextFloating = Object.assign({}, floating)
            delete nextFloating[id]
            commit(nextTree, nextFloating)
            if (id === "preview" && preferences.previewDetached)
                dockPreview()
            return
        }
        const local = root.mapFromGlobal(point.x, point.y)
        if (local.x >= 0 && local.y >= 0 && local.x < width && local.y < height) {
            if (id === "preview" && preferences.previewDetached)
                dockPreview()
            else if (isFloating(id))
                closeFloating(id)
            return
        }
        if (id === "preview") {
            if (!preferences.previewDetached)
                floatPreview(point)
        } else if (!isFloating(id)) {
            updateFloatingGeometry(id, { x: point.x - 48, y: point.y - 16,
                                         width: id === "editor" ? 700 : 460,
                                         height: id === "timeline" ? 350 : 560 })
        }
    }

    function updateFloatingGeometry(id, geometry) {
        const nextFloating = Object.assign({}, floating)
        nextFloating[id] = geometry
        commit(tree, nextFloating)
    }

    function closeFloating(id) {
        const nextFloating = Object.assign({}, floating)
        delete nextFloating[id]
        commit(Layouts.activate(tree, id), nextFloating)
    }

    function applyTimelinePlacement(mode) {
        if (preferences.timelineLayoutMode === mode)
            return
        const previousTree = customTree
        preferences.timelineLayoutMode = mode
        if (!previousTree || isFloating("timeline"))
            return
        const editorPath = Layouts.pathFor(previousTree, "editor")
        const edge = mode === "horizontal" ? "bottom" : mode === "vertical_before_editor" ? "left" : "right"
        commit(Layouts.dock(previousTree, "timeline", editorPath, edge), floating)
    }

    function applyPreviewSide(left) {
        if (preferencesModel.previewOnLeft === left)
            return
        preferencesModel.previewOnLeft = left
        if (customTree && !preferences.previewDetached)
            commit(Layouts.dock(tree, "preview", Layouts.pathFor(tree, "editor"), left ? "left" : "right"), floating)
    }

    function applyBuiltin(id) {
        maximizedPanel = ""
        if (preferences.previewDetached)
            dockPreview()
        preferences.timelineLayoutMode = id === "preview" ? "vertical_before_editor" : "horizontal"
        preferences.sidebarVisible = true
        preferences.timelinePanelVisible = true
        preferences.bottomPanelVisible = true
        preferences.workspaceHiddenPanels = {}
        if (id === "default") {
            if (preferencesModel) preferencesModel.previewOnLeft = false
            preferences.efficientSidebarEnabled = false
            preferences.sidebarWidth = preferences.sidebarDefaultWidth
            preferences.previewWidthRatio = preferences.previewDefaultWidthRatio
            preferences.timelineVerticalWidthRatio = 0.34
            preferences.bottomPanelHeightRatio = preferences.bottomPanelDefaultHeightRatio
        }
        preferences.workspaceLayout = id === "default" ? {} : { version: 1, tree: builtinTree(id), floating: {} }
    }

    function applySaved(name) {
        const saved = preferences.savedWorkspaceLayouts[name]
        if (!saved || saved.version !== 1 || !Layouts.normalizedTree(saved.tree))
            return
        maximizedPanel = ""
        preferences.timelineLayoutMode = saved.timelineLayoutMode
        preferences.sidebarVisible = saved.sidebarVisible ?? true
        preferences.timelinePanelVisible = saved.timelinePanelVisible ?? true
        preferences.bottomPanelVisible = saved.bottomPanelVisible ?? true
        preferences.workspaceHiddenPanels = saved.hiddenPanels || {}
        if (typeof saved.previewOnLeft === "boolean" && preferencesModel)
            preferencesModel.previewOnLeft = saved.previewOnLeft
        if (typeof saved.efficientSidebar === "boolean")
            preferences.efficientSidebarEnabled = saved.efficientSidebar
        if (Number.isFinite(saved.sidebarWidth))
            preferences.sidebarWidth = saved.sidebarWidth
        if (Number.isFinite(saved.previewWidthRatio))
            preferences.previewWidthRatio = saved.previewWidthRatio
        if (Number.isFinite(saved.bottomPanelHeightRatio))
            preferences.bottomPanelHeightRatio = saved.bottomPanelHeightRatio
        if (Number.isFinite(saved.timelineVerticalWidthRatio))
            preferences.timelineVerticalWidthRatio = saved.timelineVerticalWidthRatio
        if (saved.nativeFrame === true)
            preferences.workspaceLayout = {}
        else
            commit(Layouts.normalizedTree(saved.tree), Layouts.normalizedFloating(saved.floating))
        if (saved.previewDetached && !preferences.previewDetached)
            floatPreview(null)
        else if (!saved.previewDetached && preferences.previewDetached)
            dockPreview()
        if (saved.previewDetached && saved.previewGeometry)
            restorePreviewGeometry(saved.previewGeometry)
        savedLayoutApplied()
    }

    function showSaveDialog() { showManager() }

    function captureLayout() {
        return {
            version: 1, tree: tree, floating: floating, nativeFrame: defaultNativeLayout,
            timelineLayoutMode: preferences.timelineLayoutMode,
            sidebarVisible: preferences.sidebarVisible,
            timelinePanelVisible: preferences.timelinePanelVisible,
            bottomPanelVisible: preferences.bottomPanelVisible,
            hiddenPanels: preferences.workspaceHiddenPanels,
            previewOnLeft: preferencesModel?.previewOnLeft ?? false,
            efficientSidebar: preferences.efficientSidebarEnabled,
            sidebarWidth: preferences.sidebarWidth,
            previewWidthRatio: preferences.previewWidthRatio,
            bottomPanelHeightRatio: preferences.bottomPanelHeightRatio,
            timelineVerticalWidthRatio: preferences.timelineVerticalWidthRatio,
            previewDetached: preferences.previewDetached,
            previewGeometry: preferences.previewDetached ? previewGeometry() : {}
        }
    }

    WorkspaceManagerDialog { id: manager; workspace: root }

    Loader {
        id: nativeFrameLoader
        anchors.fill: parent
        active: root.useNativeFrame
        sourceComponent: root.nativeFrame
    }

    Loader {
        anchors.fill: parent
        active: !root.useNativeFrame
        sourceComponent: Component {
            DockNode {
                tree: root.maximizedPanel ? Layouts.group([root.maximizedPanel]) : root.tree
                path: ""
                workspace: root
            }
        }
    }

    Item { id: staging; visible: false }

    Instantiator {
        model: ["editor", "timeline", "sidebar", "validation", "muri"]
        delegate: DockFloatWindow {
            required property string modelData
            panelId: modelData
            workspace: root
            Component.onCompleted: {
                const hosts = Object.assign({}, root.floatingHosts)
                hosts[panelId] = panelHost
                root.floatingHosts = hosts
            }
        }
    }

    Rectangle {
        readonly property var destination: root.drop
        readonly property var group: destination?.group
        readonly property string edge: destination?.edge ?? ""
        readonly property point origin: group ? group.mapToItem(root, 0, 0) : Qt.point(0, 0)
        x: origin.x + (group && edge === "right" ? group.width - width : 0)
        y: origin.y + (group && edge === "bottom" ? group.height - height : 0)
        width: group ? group.width * (edge === "left" || edge === "right" ? 0.32 : 1) : 0
        height: group ? group.height * (edge === "top" || edge === "bottom" ? 0.32 : 1) : 0
        z: 100
        visible: !!group
        enabled: false
        color: Theme.overlayColor(Theme.colors.state.textSelection)
        opacity: 0.65
        border.width: 2
        border.color: Theme.colors.accent.focus
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.dragging.length > 0
        onActivated: root.cancelDrag()
    }

}
