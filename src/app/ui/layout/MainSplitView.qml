pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import MiaCode.UI
import "WorkbenchLayouts.js" as Layouts

Item {
    id: root

    required property var viewState
    required property var documentSession
    required property var analysisSession
    required property var preferences
    required property var previewSession
    required property var previewSettings
    required property var commands
    required property var timelineSession
    required property var preferencesModel
    required property var pages
    required property var editorController
    required property var editorSync
    required property var latency
    required property var net
    required property var netUpload
    required property Component floatingEditorShortcuts
    property bool compact: false
    property bool timelineGuideActive: false
    property string documentTitle: ""
    readonly property alias workspace: workspaceDock
    readonly property Item navigationItem: sidebar.navigationItem
    readonly property Item chartSidebarItem: sidebar.chartSidebarItem
    readonly property Item sourceEditorItem: editorPane.sourceEditorItem
    readonly property Item difficultyFieldsItem: editorPane.difficultyFieldsItem
    readonly property Item transportItem: preview.transportItem
    readonly property Item timelineGuideItem: timelinePanel.timelineGuideItem
    readonly property Item timelineSettingsGuideItem: timelinePanel.timelineSettingsGuideItem
    readonly property Item renderModeItem: preview.renderModeItem
    readonly property bool renderModeMenuAvailable: preview.renderModeMenuAvailable
    readonly property bool previewDetached: root.preferences.previewDetached
    readonly property Item settingsDialogParent: root.previewDetached
        ? detachedPreviewWindow.Overlay.overlay : root.Overlay.overlay
    property bool previewSurfaceMoving: false
    readonly property rect activityBarMaterialRect: Qt.rect(0, 0,
        sidebarStrip.width, height)
    readonly property real minimumWorkspaceWidth: previewDetached ? editorPane.minimumWidth : 620
    readonly property real expandedSidebarWidth: sidebar.activityBarWidth + preferences.sidebarWidth
    readonly property real minimumHeight: 210 + (diagnosticsVisible ? bottomPanel.minimumHeight : 0)
        + (timelineVisible && !verticalTimeline && !workspaceDock.isFloating("timeline") ? 110 : 0)
    readonly property bool canUndo: editorPane.canUndo
    readonly property bool canRedo: editorPane.canRedo
    readonly property bool canCut: editorPane.canCut
    readonly property bool canCopy: editorPane.canCopy
    readonly property bool canPaste: editorPane.canPaste
    readonly property bool canTransform: editorPane.canTransform
    readonly property bool normalizationAvailable: editorPane.normalizationAvailable
    readonly property string selectionBeatStatusText: editorPane.selectionBeatStatusText
    readonly property string selectionBeatTooltipText: editorPane.selectionBeatTooltipText
    readonly property bool chartPanelMode: root.viewState.difficultyEditorActive || root.viewState.latencyEditorActive
    readonly property bool diagnosticsVisible: chartPanelMode && viewState.bottomPanelVisible
        && (timelineSession.validationTabVisible || timelineSession.muriTabVisible)
    readonly property bool timelineVisible: chartPanelMode && preferences.timelinePanelVisible
        && timelineSession.timelineTabVisible
    readonly property bool verticalTimeline: preferences.timelineLayoutMode !== "horizontal"
    readonly property bool previewOnLeft: !verticalTimeline && preferencesModel.previewOnLeft
    readonly property bool exportVideoActive: root.pages.activePageId === "export"
    signal openRequested()
    signal settingsRequested()
    signal completionSettingsRequested(string group)
    signal audioSettingsRequested()
    signal previewSettingsRequested()
    signal mediaToolRequested(string toolId)

    function showMediaToolsMenu() {
        sidebar.showMediaToolsMenu()
        workspaceDock.activate("sidebar")
    }

    function defaultDockTree(preset) {
        const editing = preset === "editing"
        const previewing = preset === "preview"
        const editor = Layouts.split("vertical", 1 - preferences.bottomPanelHeightRatio,
                                     Layouts.group(["editor"]), Layouts.group(["diagnostics"]))
        const timeline = Layouts.group(["timeline"])
        const previewPanel = Layouts.group(["preview"])
        const ratio = previewing ? 0.57 : editing ? 0.30 : preferences.previewWidthRatio
        const verticalWidth = preferences.timelineVerticalWidthRatio
        let center = Layouts.split("vertical", editing ? 0.72 : 0.75, editor, timeline)
        if (verticalTimeline) {
            center = preferences.timelineLayoutMode === "vertical_before_editor"
                ? Layouts.split("horizontal", verticalWidth, timeline, editor)
                : Layouts.split("horizontal", 1 - verticalWidth, editor, timeline)
        }
        const work = previewOnLeft ? Layouts.split("horizontal", ratio, previewPanel, center)
                                   : Layouts.split("horizontal", 1 - ratio, center, previewPanel)
        const outlineRatio = Math.max(0.10, Math.min(0.27, (preferences.sidebarWidth + sidebar.activityBarWidth) / 1250))
        return Layouts.split("horizontal", outlineRatio, Layouts.group(["sidebar"]), work)
    }

    function undo() {
        editorPane.undo()
    }

    function redo() {
        editorPane.redo()
    }

    function cut() {
        editorPane.cut()
    }

    function copy() {
        editorPane.copy()
    }

    function paste() {
        editorPane.paste()
    }

    function requestCloseActiveEditor() {
        editorPane.requestCloseActiveEditor()
    }

    function selectAll() {
        editorPane.selectAll()
    }

    function showFindReplace() {
        editorPane.openFindReplace()
    }

    function selectCurrentLine() {
        editorPane.selectCurrentLine()
    }

    function canNormalizeChart() {
        return editorPane.canNormalizeChart()
    }

    function normalizationSelectionDescription() {
        return editorPane.normalizationSelectionDescription()
    }

    function applyNormalization(options) {
        return editorPane.applyNormalization(options)
    }

    function applyChartTransform(opId) {
        return editorPane.applyChartTransform(opId)
    }

    function toggleRenderModeMenu() {
        if (!preview.renderModeMenuAvailable)
            return
        if (root.previewDetached) {
            detachedPreviewWindow.raise()
            detachedPreviewWindow.requestActivate()
        }
        preview.toggleRenderModeMenu()
    }

    function detachPreview(position) {
        preview.closeMenus()
        root.previewSurfaceMoving = true
        root.preferences.previewDetached = true
        detachedPreviewWindow.windowChrome.showRestored()
        if (position) {
            detachedPreviewWindow.x = position.x - 48
            detachedPreviewWindow.y = position.y - 16
        }
        detachedPreviewWindow.raise()
        detachedPreviewWindow.requestActivate()
        Qt.callLater(root.finishPreviewMove)
    }

    function dockPreview() {
        preview.closeMenus()
        root.previewSurfaceMoving = true
        detachedPreviewWindow.windowChrome.saveWindowState()
        root.preferences.previewDetached = false
        detachedPreviewWindow.hide()
        root.Window.window.requestActivate()
        Qt.callLater(root.finishPreviewMove)
    }

    Connections {
        target: root.Window.window
        function onVisibleChanged() {
            if (root.Window.window.visible && root.previewDetached)
                root.detachPreview()
        }
    }

    function finishPreviewMove() {
        root.previewSurfaceMoving = false
        preview.forceActiveFocus()
    }

    function previewGeometry() {
        return { x: detachedPreviewWindow.x, y: detachedPreviewWindow.y,
                 width: detachedPreviewWindow.width, height: detachedPreviewWindow.height }
    }

    function restorePreviewGeometry(geometry) {
        const rect = preferences.boundedWorkspaceWindowGeometry(Qt.rect(geometry.x, geometry.y, geometry.width, geometry.height))
        detachedPreviewWindow.x = rect.x
        detachedPreviewWindow.y = rect.y
        detachedPreviewWindow.width = rect.width
        detachedPreviewWindow.height = rect.height
    }

    PreviewPane {
        id: preview
        parent: root.previewDetached ? detachedPreviewContent : workspaceDock.hostFor("preview")
        anchors.fill: parent
        documentAvailable: root.documentSession.hasDocument
        surfaceActive: !root.previewSurfaceMoving
        detached: root.previewDetached
        transportMirrored: root.previewOnLeft && !root.previewDetached
        previewSession: root.previewSession
        preferences: root.preferences
        exportSession: root.pages.exportSession
        exportPageActive: root.exportVideoActive
        latencyActive: root.viewState.latencyEditorActive
                       && root.pages.activePageId === "latency"
        onDetachRequested: root.detachPreview(null)
        onDockRequested: root.dockPreview()
    }

    ApplicationWindow {
        id: detachedPreviewWindow
        objectName: "detachedPreviewWindow"
        readonly property Item backdropSource: detachedSceneContent
        property var windowChrome: null
        property bool alwaysOnTop: true
        onAlwaysOnTopChanged: if (windowChrome) windowChrome.setAlwaysOnTop(alwaysOnTop)
        onWindowChromeChanged: if (windowChrome) windowChrome.setAlwaysOnTop(alwaysOnTop)
        readonly property bool settingsInTitleBar: Qt.platform.os !== "windows"
        readonly property bool nativeMaterialActive: windowChrome
            && windowChrome.nativeMaterialAvailable && Theme.blurMaterialsEnabled
            && !Theme.backgroundActive
        readonly property string captionTitle: root.documentTitle.length > 0
            ? (root.documentSession.dirty ? "* " : "") + root.documentTitle
            : qsTrId("preview.window.title")
        title: captionTitle.replace(/ — /g, " - ") + (root.documentTitle.length > 0
            ? " - " + qsTrId("preview.window.title") : "")
        transientParent: null
        visible: false
        width: 560
        height: 680
        minimumWidth: Math.min(Screen.desktopAvailableWidth, Math.ceil(preview.minimumWidth * Theme.uiScale))
        minimumHeight: Math.min(Screen.desktopAvailableHeight,
            Math.ceil((preview.minimumHeight + detachedTitleBar.height + detachedToolBar.height + detachedPreviewTab.height) * Theme.uiScale))
        flags: {
            let value = Qt.Window
            if (Qt.platform.os !== "windows" && alwaysOnTop)
                value |= Qt.WindowStaysOnTopHint
            if (root.Window.window.platform.captionButtons) {
                value |= Qt.CustomizeWindowHint
                        | Qt.WindowTitleHint
                        | Qt.WindowSystemMenuHint
                        | Qt.WindowMinimizeButtonHint
                        | Qt.WindowMaximizeButtonHint
                        | Qt.WindowCloseButtonHint
            }
            if (Qt.platform.os === "linux")
                value |= Qt.FramelessWindowHint
            if (Qt.platform.os === "osx")
                value |= Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
            return value
        }
        // Allocate the alpha surface before WindowChrome creates the native handle.
        color: Qt.platform.os === "osx" || Qt.platform.os === "windows"
            ? "transparent" : Theme.colors.background.panel
        background: null
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
        topPadding: 0
        leftPadding: 0
        rightPadding: 0
        bottomPadding: 0
        palette.window: Theme.colors.background.surface
        palette.windowText: Theme.colors.text.primary
        palette.base: Theme.colors.background.surface
        palette.text: Theme.colors.text.primary
        palette.button: Theme.colors.background.panel
        palette.buttonText: Theme.colors.text.primary
        palette.highlight: Theme.colors.state.textSelection
        palette.highlightedText: Theme.colors.text.active
        palette.disabled.text: Theme.colors.text.disabled
        palette.disabled.buttonText: Theme.colors.text.disabled
        onClosing: root.dockPreview()

        Binding {
            target: detachedPreviewWindow.windowChrome
            property: "materialRegions"
            when: (Qt.platform.os === "osx" || Qt.platform.os === "windows") && target !== null
            value: detachedPreviewWindow.visible && detachedTitleBar.visible
                && Theme.blurMaterialsEnabled && !Theme.backgroundActive
                ? [{ rect: Qt.rect(0, 0, detachedPreviewWindow.width,
                    (detachedTitleBar.height + detachedToolBar.height) * Theme.uiScale) }]
                : []
            restoreMode: Binding.RestoreNone
        }

        UiScaleSurface {
            id: detachedSceneContent

            WindowBackground {
                anchors.fill: parent
                nativeMaterialActive: detachedPreviewWindow.nativeMaterialActive
            }

            WindowTitleBar {
                id: detachedTitleBar
                color: Theme.chromeSurfaceColor(Theme.colors.background.titleBar,
                    detachedPreviewWindow.nativeMaterialActive)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                visible: detachedPreviewWindow.visibility !== Window.FullScreen
                height: visible ? implicitHeight : 0
                hostWindow: detachedPreviewWindow
                windowChrome: detachedPreviewWindow.windowChrome
                platform: root.Window.window.platform
                applicationMenusVisible: false
                titleText: detachedPreviewWindow.captionTitle
                nativeHeight: detachedPreviewWindow.windowChrome
                    ? detachedPreviewWindow.windowChrome.titleBarHeight / Theme.uiScale : 0
                leadingInset: detachedPreviewWindow.windowChrome
                    ? detachedPreviewWindow.windowChrome.titleBarLeadingInset / Theme.uiScale : 0
                leadingToolAreaWidth: leadingInset
                trailingToolAreaWidth: detachedPreviewWindow.settingsInTitleBar
                    ? detachedSettingsActions.width + 8 : 0

                Row {
                    id: detachedSettingsActions
                    parent: detachedPreviewWindow.settingsInTitleBar ? detachedTitleBar : detachedToolBar
                    anchors.right: parent.right
                    anchors.rightMargin: detachedPreviewWindow.settingsInTitleBar
                        ? detachedTitleBar.captionButtonsWidth + 8 : 8
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height
                    spacing: 5
                    z: 2

                    IconButton {
                        objectName: "detachedPreviewAudioSettingsButton"
                        height: Math.min(implicitHeight, detachedSettingsActions.height)
                        anchors.verticalCenter: parent.verticalCenter
                        stateColors: Theme.chromeStateColorsFor(detachedPreviewWindow.nativeMaterialActive)
                        iconSource: Qt.resolvedUrl("icons/audio-settings.svg")
                        label: qsTrId("action.audio_settings")
                        tooltip: qsTrId("action.audio_settings")
                        onClicked: root.audioSettingsRequested()
                    }

                    IconButton {
                        objectName: "detachedPreviewPreviewSettingsButton"
                        height: Math.min(implicitHeight, detachedSettingsActions.height)
                        anchors.verticalCenter: parent.verticalCenter
                        stateColors: Theme.chromeStateColorsFor(detachedPreviewWindow.nativeMaterialActive)
                        iconSource: Qt.resolvedUrl("icons/preview-settings.svg")
                        label: qsTrId("action.video_settings")
                        tooltip: qsTrId("action.video_settings")
                        onClicked: root.previewSettingsRequested()
                    }

                    IconButton {
                        objectName: "detachedPreviewAlwaysOnTopButton"
                        height: Math.min(implicitHeight, detachedSettingsActions.height)
                        anchors.verticalCenter: parent.verticalCenter
                        stateColors: Theme.chromeStateColorsFor(detachedPreviewWindow.nativeMaterialActive)
                        iconSource: Qt.resolvedUrl("icons/pin.svg")
                        filledIconSource: Qt.resolvedUrl("icons/pin-filled.svg")
                        checkable: true
                        checked: detachedPreviewWindow.alwaysOnTop
                        tooltip: checked ? qsTrId("preview.window.disable_always_on_top")
                                         : qsTrId("preview.window.always_on_top")
                        Accessible.name: qsTrId("preview.window.always_on_top")
                        onToggled: detachedPreviewWindow.alwaysOnTop = checked
                    }
                }
            }

            Rectangle {
                id: detachedToolBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: detachedTitleBar.bottom
                visible: !detachedPreviewWindow.settingsInTitleBar && detachedTitleBar.visible
                height: visible ? Theme.windowChromeRowHeight : 0
                color: Theme.chromeSurfaceColor(Theme.colors.background.activityBar,
                    detachedPreviewWindow.nativeMaterialActive)

                WindowGestureArea {
                    anchors.fill: parent
                    hostWindow: detachedPreviewWindow
                    windowChrome: detachedPreviewWindow.windowChrome
                }
            }

            DockPanelTab {
                id: detachedPreviewTab
                anchors.top: detachedToolBar.bottom
                workspace: workspaceDock
                panelId: "preview"
                checked: true
            }

            Item {
                id: detachedPreviewContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: detachedPreviewTab.bottom
                anchors.bottom: parent.bottom
            }
        }

        Shortcut {
            sequences: [StandardKey.Close]
            onActivated: detachedPreviewWindow.close()
        }

        UiScaleShortcuts {
            shortcuts: root.Window.window.applicationContext.shortcuts
            preferencesModel: root.preferencesModel
        }

        Shortcut {
            sequence: "Escape"
            enabled: detachedPreviewWindow.visibility === Window.FullScreen
            onActivated: detachedPreviewWindow.windowChrome.showRestored()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceColor(Theme.colors.background.panel)
        z: -1
    }

    Item {
        id: sidebarStrip
        width: !root.compact && !root.viewState.sidebarVisible && !root.preferences.efficientSidebarEnabled
            ? sidebar.activityBarWidth : 0
        height: parent.height
    }

    Sidebar {
        id: sidebar
        parent: sidebarStrip.width > 0 ? sidebarStrip : workspaceDock.hostFor("sidebar")
        anchors.fill: parent
        viewState: root.viewState
        documentSession: root.documentSession
        preferences: root.preferences
        commands: root.commands
        pages: root.pages
        compact: false
        onSettingsRequested: root.settingsRequested()
        onMediaToolRequested: toolId => root.mediaToolRequested(toolId)
    }

    WorkbenchDock {
        id: workspaceDock
        anchors.left: sidebarStrip.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        preferences: root.preferences
        defaultTree: root.defaultDockTree("default")
        builtinTree: root.defaultDockTree
        floatPreview: root.detachPreview
        dockPreview: root.dockPreview
        previewGeometry: root.previewGeometry
        restorePreviewGeometry: root.restorePreviewGeometry
        editorShortcuts: root.floatingEditorShortcuts
        availablePanels: {
            const panels = ["editor"]
            if (!root.previewDetached) panels.push("preview")
            if (root.timelineVisible) panels.push("timeline")
            if (root.diagnosticsVisible) panels.push("diagnostics")
            if (!root.compact && root.viewState.sidebarVisible) panels.push("sidebar")
            return panels
        }
    }

    Connections {
        target: root.pages
        function onActivePageIdChanged() { workspaceDock.activate("editor") }
    }
    Connections {
        target: root.viewState
        function onActiveEditorKeyChanged() { workspaceDock.activate("editor") }
        function onSidebarVisibleChanged() {
            if (root.viewState.sidebarVisible)
                workspaceDock.activate("sidebar")
        }
    }

    Item {
        id: editorHost
        parent: workspaceDock.hostFor("editor")
        anchors.fill: parent
        clip: true

        EditorPane {
            id: editorPane
            anchors.fill: parent
            visible: !root.pages.overlayActive && !root.exportVideoActive
            editorController: root.editorController
            editorSync: root.editorSync
            analysisSession: root.analysisSession
            viewState: root.viewState
            documentSession: root.documentSession
            commands: root.commands
            preferences: root.preferences
            latency: root.latency
            previewSession: root.previewSession
            net: root.net
            netUpload: root.netUpload
            pages: root.pages
            onOpenRequested: root.openRequested()
            onCompletionSettingsRequested: group => root.completionSettingsRequested(group)
        }

        ExportVideoPage {
            id: exportVideoPage
            anchors.fill: parent
            visible: root.exportVideoActive
            pages: root.pages
            previewSession: root.previewSession
            previewSettings: root.previewSettings
        }
    }

    BottomPanel {
        id: bottomPanel
        parent: workspaceDock.hostFor("diagnostics")
        anchors.fill: parent
        visible: root.diagnosticsVisible
        analysisSession: root.analysisSession
        documentSession: root.documentSession
        timelineSession: root.timelineSession
        onAnalysisRowActivated: (difficultyId, revision, line, column, endColumn, second) => {
            workspaceDock.activate("editor")
            editorPane.revealAnalysisRow(difficultyId, revision, line, column, endColumn, second, root.analysisSession)
        }
    }

    TimelinePanel {
        id: timelinePanel
        parent: workspaceDock.hostFor("timeline")
        anchors.fill: parent
        visible: root.timelineVisible
        verticalTimeline: root.verticalTimeline
        guideSettingsActive: root.timelineGuideActive
        preferences: root.preferences
        timelineSession: root.timelineSession
        previewSession: root.previewSession
    }
}
