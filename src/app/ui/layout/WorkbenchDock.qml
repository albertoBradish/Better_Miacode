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
    property bool simulatedPlay: false
    function builtinTree(preset) {
        return Layouts.builtinTree(preset, preferencesModel?.previewOnLeft ?? false, simulatedPlay)
    }
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
    property bool libraryAvailable: false
    property bool playChecksAvailable: false
    property bool handOrderFeaturesEnabled: false
    property bool exportVideoActive: false

    property var comparisonPanels: []
    property var sessionLayout: null
    onComparisonPanelsChanged: {
        if (!comparisonPanels.length) sessionLayout = null
        if (maximizedPanel.startsWith("comparison-") && !comparisonPanels.some(p => p.panelId === maximizedPanel))
            maximizedPanel = ""
    }
    readonly property var config: sessionLayout ?? preferences.workspaceLayout
    readonly property var customTree: config.version === 1 ? Layouts.normalizedTree(config.tree) : null
    readonly property var baseTree: customTree || defaultTree
    readonly property var tree: {
        const diagnostics = panelShown("play_checks") ? Layouts.ensurePanel(baseTree, "play_checks", "muri", false) : baseTree
        let layout = panelShown("library") ? Layouts.ensurePanel(diagnostics, "library", "sidebar", false) : diagnostics
        if (!comparisonPanels.length) return layout
        layout = Layouts.pruneComparisons(layout, comparisonPanels.map(p => p.panelId))
        for (const item of comparisonPanels) {
            if (Layouts.pathFor(layout, item.panelId) !== null) continue
            const path = Layouts.pathFor(layout, "preview")
            layout = Layouts.replace(layout, path, node => Layouts.split("vertical", 0.55, node, Layouts.group([item.panelId])))
        }
        return layout
    }
    readonly property var floating: customTree ? Layouts.normalizedFloating(config.floating) : ({})
    readonly property bool defaultNativeLayout: !!nativeFrame && comparisonPanels.length === 0 && !customTree && preferences.timelineLayoutMode === "horizontal" && !panelShown("library")
    readonly property bool useNativeFrame: defaultNativeLayout && maximizedPanel.length === 0
    readonly property var nativeFrameItem: nativeFrameLoader.item
    readonly property var registeredPanelIds: Layouts.panels.filter(id => (id !== "library" || root.libraryAvailable)
        && (id !== "play_checks" || root.playChecksAvailable)
        && (id !== "patterns" || root.handOrderFeaturesEnabled)).concat(comparisonPanels.map(p => p.panelId))
    readonly property var panelIds: exportVideoActive
        ? registeredPanelIds.filter(id => id !== "patterns" && id !== "play_checks") : registeredPanelIds
    readonly property var savedNames: preferences.savedWorkspaceLayoutOrder ?? Object.keys(preferences.savedWorkspaceLayouts).sort((a, b) => a.localeCompare(b))
    readonly property var slotKeys: preferences.workspaceSlots ?? ["default", "editing", "preview"]
    readonly property var layoutKeys: preferences.workspaceListOrder
        ?? ["default", "editing", "preview"].concat(savedNames.map(name => "saved:" + name))
    function layoutTitle(key) {
        switch (key) {
        case "default": return qsTrId("workspace.classic")
        case "editing": return qsTrId("workspace.editing")
        case "preview": return qsTrId("workspace.preview")
        default: return key.startsWith("saved:") ? key.slice(6) : ""
        }
    }
    function applyEntry(key) {
        if (!layoutKeys.includes(key)) return
        if (key.startsWith("saved:")) applySaved(key.slice(6))
        else applyBuiltin(key)
    }
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
        if (id.startsWith("comparison-")) {
            const pane = comparisonPanels.find(p => p.panelId === id)
            if (!pane?.workspaceLabel) return ""
            const title = qsTrId("preview.comparison.workspace_title")
            return (title.includes("%1") ? title : "Child Preview %1").arg(pane.workspaceLabel)
        }
        switch (id) {
        case "editor": return qsTrId("workspace.editor")
        case "timeline": return qsTrId("window.timeline")
        case "preview": return qsTrId("workspace.preview_panel")
        case "sidebar": return qsTrId("workspace.sidebar")
        case "library": return qsTrId("workspace.library")
        case "validation": return qsTrId("window.syntax")
        case "muri": return qsTrId("window.muri")
        case "play_checks": return qsTrId("window.play_checks")
        case "patterns": return qsTrId("hand_order.pattern.tab")
        }
        return ""
    }

    function floatingPanelTitle(id) {
        return id.startsWith("comparison-")
            ? comparisonPanels.find(p => p.panelId === id)?.title ?? ""
            : panelTitle(id)
    }

    function panelShown(id) {
        if (!panelIds.includes(id)) return false
        if (id === "library") {
            const hidden = preferences.workspaceHiddenPanels?.library
            return hidden !== undefined ? !hidden : !customTree || Layouts.pathFor(customTree, "library") !== null
        }
        if (id === "play_checks") return preferences.bottomPanelVisible
            && preferences.workspaceHiddenPanels?.play_checks !== true
        if (id === "sidebar") return compactSidebarShown ?? preferences.sidebarVisible
        if (id === "timeline") return preferences.bottomPanelVisible && preferences.timelinePanelVisible
        if ((id === "validation" || id === "muri" || id === "patterns") && !preferences.bottomPanelVisible) return false
        return preferences.workspaceHiddenPanels?.[id] !== true
    }

    function setPanelShown(id, shown) {
        if (id.startsWith("comparison-")) { if (!shown) comparisonPanels.find(p => p.panelId === id)?.close(); return }
        if (!panelIds.includes(id) || panelShown(id) === shown)
            return
        if (id === "sidebar") {
            preferences.sidebarVisible = shown
        } else if (id === "timeline") {
            preferences.timelinePanelVisible = shown
        } else {
            const hidden = Object.assign({}, preferences.workspaceHiddenPanels)
            if (id === "library") hidden[id] = !shown
            else if (shown) delete hidden[id]
            else hidden[id] = true
            preferences.workspaceHiddenPanels = hidden
        }
        if (shown && (id === "timeline" || id === "validation" || id === "muri" || id === "patterns" || id === "play_checks"))
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

    function keyboardAllowed(window, sourceEditor = false) {
        // Qt.Tool windows may keep both parent and child marked active. Only
        // the window holding Quick focus may register these shared gestures.
        if (!keyboardShortcutsEnabled || !window || !window.active) return false
        const focusItem = window.activeFocusItem
        if (!focusItem) return false
        if (isWithin(focusItem, window.Overlay.overlay)) return false
        if (focusItem.inputMethodComposing || focusItem.imeComposing) return false
        return (focusItem.workspaceSourceEditor === true) === sourceEditor
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
        sequences: ["`", "Ctrl+`"]
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
        const layout = { version: 1, tree: nextTree, floating: floating }
        if (comparisonPanels.length) sessionLayout = layout
        else preferences.workspaceLayout = layout
    }

    function activate(id) {
        if (!availablePanels.includes(id))
            return
        activePanel = id
        if (maximizedPanel && maximizedPanel !== id) maximizedPanel = ""
        panelActivated(id)
        if (useNativeFrame || isFloating(id)) return
        // Dock hosts can still be incubating when a hidden panel is reopened.
        const nextTree = Layouts.activate(tree, id)
        if (nextTree !== tree) commit(nextTree, floating)
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

    function applyTimelineStyle(style) {
        preferences.timelineLayoutMode = style === "horizontal" ? "horizontal" : "vertical"
        if (style !== "horizontal") preferences.timelineVerticalDirection = style
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
        preferences.timelineLayoutMode = id === "default" ? "horizontal" : "vertical"
        preferences.timelineVerticalDirection = "falling"
        preferences.sidebarVisible = id !== "editing"
        preferences.timelinePanelVisible = true
        preferences.bottomPanelVisible = true
        preferences.workspaceHiddenPanels = id === "editing" ? { library: true } : {}
        if (id === "default") {
            preferences.efficientSidebarEnabled = false
            preferences.sidebarWidth = preferences.sidebarDefaultWidth
            preferences.previewWidthRatio = preferences.previewDefaultWidthRatio
            preferences.timelineVerticalWidthRatio = 0.34
            preferences.bottomPanelHeightRatio = preferences.bottomPanelDefaultHeightRatio
        }
        sessionLayout = null
        preferences.workspaceLayout = id === "default" ? {} : { version: 1, tree: builtinTree(id), floating: {} }
    }

    function applySaved(name) {
        const saved = preferences.savedWorkspaceLayouts[name]
        if (!saved || saved.version !== 1 || !Layouts.normalizedTree(saved.tree))
            return
        maximizedPanel = ""
        preferences.timelineLayoutMode = saved.timelineLayoutMode
        if (saved.timelineVerticalDirection) preferences.timelineVerticalDirection = saved.timelineVerticalDirection
        preferences.sidebarVisible = saved.sidebarVisible ?? true
        preferences.timelinePanelVisible = saved.timelinePanelVisible ?? true
        preferences.bottomPanelVisible = saved.bottomPanelVisible ?? true
        preferences.workspaceHiddenPanels = Object.assign({ library: Layouts.pathFor(saved.tree, "library") === null }, saved.hiddenPanels || {})
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
        sessionLayout = null
        if (saved.nativeFrame === true)
            preferences.workspaceLayout = {}
        else
            preferences.workspaceLayout = {version: 1, tree: Layouts.normalizedTree(saved.tree), floating: Layouts.normalizedFloating(saved.floating)}
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
        const savedFloating = Object.assign({}, floating)
        for (const id of Object.keys(savedFloating)) if (id.startsWith("comparison-")) delete savedFloating[id]
        return {
            version: 1, tree: Layouts.pruneComparisons(tree, []), floating: savedFloating, nativeFrame: defaultNativeLayout,
            timelineLayoutMode: preferences.timelineLayoutMode,
            timelineVerticalDirection: preferences.timelineVerticalDirection,
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
        model: root.registeredPanelIds.filter(id => id !== "preview")
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
