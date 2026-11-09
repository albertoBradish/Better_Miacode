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

    readonly property var config: preferences.workspaceLayout
    readonly property var tree: (config.version === 1 ? Layouts.normalizedTree(config.tree) : null) || defaultTree
    readonly property var savedNames: Object.keys(preferences.savedWorkspaceLayouts).sort((a, b) => a.localeCompare(b))
    property var groups: []
    property var floatingHosts: ({})
    property string dragging: ""
    property point dragPosition
    property var drop: null
    signal savedLayoutApplied()

    function panelTitle(id) {
        switch (id) {
        case "editor": return qsTrId("workspace.editor")
        case "timeline": return qsTrId("window.timeline")
        case "preview": return qsTrId("workspace.preview_panel")
        case "sidebar": return qsTrId("workspace.sidebar")
        case "diagnostics": return qsTrId("workspace.diagnostics")
        }
        return ""
    }

    function panelDocked(id) {
        return root.availablePanels.includes(id) && !root.isFloating(id)
    }

    function branchOccupied(node) {
        return node ? (node.tabs ? node.tabs.some(root.panelDocked)
                                : branchOccupied(node.first) || branchOccupied(node.second)) : false
    }

    function isFloating(id) {
        return config.version === 1 && config.floating && !!config.floating[id]
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
        for (const group of groups) {
            if (group?.tabs.includes(id) && group.selected !== id) {
                commit(Layouts.activate(tree, id), config.floating || {})
                return
            }
        }
    }

    function resize(path, ratio) {
        if (ratio <= 0 || ratio >= 1)
            return
        commit(Layouts.replace(tree, path, node => Layouts.split(node.axis, ratio, node.first, node.second)),
               config.floating || {})
    }

    function updateDrag(id, point) {
        dragging = id
        dragPosition = point
        drop = null
        for (const group of groups) {
            if (!group?.visible)
                continue
            const p = group.mapFromGlobal(point.x, point.y)
            if (p.x < 0 || p.y < 0 || p.x >= group.width || p.y >= group.height)
                continue
            const onTabStrip = p.y < Theme.controlMinHeight
            const edgeX = Math.min(64, group.width * 0.25)
            const edgeY = Math.min(64, group.height * 0.25)
            const edge = onTabStrip ? "center" : p.x < edgeX ? "left"
                : p.x > group.width - edgeX ? "right" : p.y < edgeY + Theme.controlMinHeight ? "top"
                : p.y > group.height - edgeY ? "bottom" : "center"
            drop = { group: group, edge: edge }
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
            const nextTree = Layouts.dock(tree, id, destination.group.path, destination.edge)
            const floating = Object.assign({}, config.floating)
            delete floating[id]
            commit(nextTree, floating)
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
        const floating = Object.assign({}, config.floating)
        floating[id] = geometry
        commit(tree, floating)
    }

    function closeFloating(id) {
        const floating = Object.assign({}, config.floating)
        delete floating[id]
        commit(Layouts.activate(tree, id), floating)
    }

    function applyBuiltin(id) {
        if (preferences.previewDetached)
            dockPreview()
        preferences.timelineLayoutMode = id === "preview" ? "vertical_before_editor" : "horizontal"
        preferences.sidebarVisible = true
        preferences.timelinePanelVisible = true
        preferences.bottomPanelVisible = true
        if (id === "default") {
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
        preferences.timelineLayoutMode = saved.timelineLayoutMode
        preferences.sidebarVisible = saved.sidebarVisible ?? true
        preferences.timelinePanelVisible = saved.timelinePanelVisible ?? true
        preferences.bottomPanelVisible = saved.bottomPanelVisible ?? true
        if (Number.isFinite(saved.bottomPanelHeightRatio))
            preferences.bottomPanelHeightRatio = saved.bottomPanelHeightRatio
        commit(Layouts.normalizedTree(saved.tree), saved.floating || {})
        if (saved.previewDetached && !preferences.previewDetached)
            floatPreview(null)
        else if (!saved.previewDetached && preferences.previewDetached)
            dockPreview()
        if (saved.previewDetached && saved.previewGeometry)
            restorePreviewGeometry(saved.previewGeometry)
        savedLayoutApplied()
    }

    function showSaveDialog() {
        nameField.text = ""
        saveDialog.open()
    }

    function saveLayout() {
        const name = nameField.text.trim()
        if (name.length === 0)
            return
        preferences.saveWorkspaceLayout(name, { version: 1, tree: tree, floating: config.floating || {},
                                                 timelineLayoutMode: preferences.timelineLayoutMode,
                                                 sidebarVisible: preferences.sidebarVisible,
                                                 timelinePanelVisible: preferences.timelinePanelVisible,
                                                 bottomPanelVisible: preferences.bottomPanelVisible,
                                                 bottomPanelHeightRatio: preferences.bottomPanelHeightRatio,
                                                 previewDetached: preferences.previewDetached,
                                                 previewGeometry: preferences.previewDetached ? previewGeometry() : {} })
        saveDialog.close()
    }

    DockNode {
        anchors.fill: parent
        tree: root.tree
        path: ""
        workspace: root
    }

    Item { id: staging; visible: false }

    Instantiator {
        model: ["editor", "timeline", "sidebar", "diagnostics"]
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
        x: origin.x + (edge === "right" ? group.width - width : 0)
        y: origin.y + (edge === "bottom" ? group.height - height : 0)
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

    AppDialog {
        id: saveDialog
        title: qsTrId("workspace.save")
        preferredHeight: 165
        onOpened: nameField.forceActiveFocus()
        body: AppTextField {
            id: nameField
            placeholderText: qsTrId("workspace.name")
            Accessible.name: placeholderText
            maximumLength: 48
            onAccepted: root.saveLayout()
        }
        footer: DialogFooter {
            acceptText: qsTrId("action.save")
            cancelText: qsTrId("action.cancel")
            acceptEnabled: nameField.text.trim().length > 0
            onAccepted: root.saveLayout()
            onRejected: saveDialog.close()
        }
    }
}
