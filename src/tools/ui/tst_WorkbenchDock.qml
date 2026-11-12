import QtQuick
import QtQuick.Controls
import QtQml.Models
import QtTest
import MiaCode.UI

Item {
    id: root
    width: 960
    height: 640
    readonly property alias preferences: preferences
    readonly property alias preferencesModel: preferencesModel
    readonly property alias viewState: viewState
    readonly property alias testCommands: commands
    property bool previewOnLeft: false
    property bool compact: false
    property bool previewDetached: false
    property bool previewSurfaceMoving: false
    property bool exportVideoActive: false
    property bool bottomPanelEffectivelyVisible: dock.availablePanels.some(id => ["timeline", "validation", "muri"].includes(id))
    property real minimumEditorWidth: 280
    property real bottomMinimumHeight: 102
    property Item transportItem: null
    property Item backgroundSource: null
    property point backgroundOffset: Qt.point(0, 0)
    property string selectedBottom: "timeline"

    QtObject {
        id: preferences
        property var workspaceLayout: ({})
        property var savedWorkspaceLayouts: ({})
        property var savedWorkspaceLayoutOrder: []
        property real timelineVerticalWidthRatio: 0.34
        property bool previewDetached: false
        property bool sidebarVisible: true
        property bool efficientSidebarEnabled: false
        property bool bottomPanelVisible: true
        property bool timelinePanelVisible: true
        property string timelineLayoutMode: "horizontal"
        property var workspaceHiddenPanels: ({})
        property int sidebarWidth: 190
        readonly property int sidebarDefaultWidth: 190
        readonly property int sidebarMinimumContentWidth: 100
        readonly property int sidebarMaximumContentWidth: 320
        property real previewWidthRatio: 0.45
        readonly property real previewDefaultWidthRatio: 0.45
        readonly property real previewMinimumWidthRatio: 0.25
        readonly property real previewMaximumWidthRatio: 0.65
        property real bottomPanelHeightRatio: 0.25
        readonly property real bottomPanelDefaultHeightRatio: 0.25
        readonly property real bottomPanelMinimumHeightRatio: 0.15
        readonly property real bottomPanelMaximumHeightRatio: 0.65
        function boundedWorkspaceWindowGeometry(rect) { return Qt.rect(0, 0, rect.width, rect.height) }
    }

    QtObject {
        id: preferencesModel
        property bool previewOnLeft: false
        signal interfaceChanged()
    }

    ViewState { id: metadataState }
    SignalSpy { id: metadataRequests; target: metadataState; signalName: "metadataDialogRequested" }

    QtObject {
        id: viewState
        readonly property bool sidebarVisible: preferences.sidebarVisible
    }

    ActivityBar {
        id: activityRail
        width: implicitWidth
        height: parent.height
        onViewRequested: dock.setPanelShown("sidebar", true)
    }

    MainMenuCommands {
        id: commands
        onSaveRequested: ++root.saves
    }

    readonly property var panels: ["editor", "timeline", "preview", "sidebar", "validation", "muri", "library"]
    property var enabledPanels: panels
    property int saves: 0

    WorkbenchDock {
        id: dock
        anchors.left: activityRail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        preferences: root.preferences
        libraryAvailable: true
        availablePanels: root.enabledPanels.filter(id => dock.panelShown(id))
        defaultTree: ({ axis: "horizontal", ratio: 0.21,
                        first: { tabs: ["sidebar"], selected: "sidebar" },
                        second: { axis: "horizontal", ratio: 0.55,
                            first: { axis: "vertical", ratio: 0.75,
                                first: { tabs: ["editor"], selected: "editor" },
                                second: { tabs: ["timeline", "validation", "muri"], selected: root.selectedBottom } },
                            second: { tabs: ["preview"], selected: "preview" } } })
        nativeFrame: Component {
            NativeWorkspaceFrame {
                owner: root
                workspaceDock: dock
                sidebar: outlinePane
                editorPane: editor
                exportVideoPage: editor
                preview: previewPane
            }
        }
        onPanelActivated: panelId => {
            if (panelId === "timeline" || panelId === "validation" || panelId === "muri")
                root.selectedBottom = panelId
        }
        builtinTree: function() { return defaultTree }
        floatPreview: function() { preferences.previewDetached = true }
        dockPreview: function() { preferences.previewDetached = false }
        editorShortcuts: Component {
            DockEditorShortcuts {
                menuCommands: commands
                shortcutModel: ({ revision: -1 })
                commandService: ({ shortcutCommandIds: () => [] })
                previewSession: null
                saveEnabled: true
            }
        }
        previewGeometry: function() { return {} }
        restorePreviewGeometry: function() {}
    }

    TextEdit {
        id: editor
        parent: dock.hostFor("editor")
        anchors.fill: parent
        text: "(120){4}1,2,E"
        readonly property Item cornerSourceItem: editor

        Popup {
            id: editorPopup
            parent: editor.Overlay.overlay
        }
    }

    Item {
        id: previewPane
        parent: dock.hostFor("preview")
        anchors.fill: parent
        property int frame: 37
        readonly property Item cornerSourceItem: previewPane
    }

    Item {
        id: outlinePane
        parent: dock.hostFor("sidebar")
        anchors.fill: parent
        readonly property Item cornerSourceItem: outlinePane
    }

    ListModel {
        id: sharedLibrary
        property string rootPath: "/generated"
        property string query: ""
        property bool busy: false
        property bool duplicatesOnly: false
        property int totalCount: count
        property int selectedCount: 0
        property string status: "Generated test index"
        function chooseRoot() {}
        function refresh() {}
        function cancel() {}
        function selectVisible(selected) { selectPath("/generated/maidata.txt", selected) }
        function selectPath(path, selected) { setProperty(0, "selected", selected); selectedCount = selected ? 1 : 0 }
        ListElement {
            chartPath: "/generated/maidata.txt"; chartTitle: "Generated fixture"
            artist: "QA"; levels: "5:12"; chartVersion: "test"
            sha256: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            copies: 1; selected: false
        }
    }

    ChartLibraryBrowser {
        id: libraryPane
        parent: dock.hostFor("library")
        anchors.fill: parent
        visible: dock.panelPresent("library")
        library: sharedLibrary
    }

    Item {
        id: sourceInputStandIn
        property bool workspaceSourceEditor: true
        visible: false
    }

    TextField {
        id: ordinaryInput
        visible: false
    }

    Item { id: floatingShortcutFocus; visible: false }

    QtObject {
        id: metadataTestDocument
        property bool hasDocument: true
        property var availableDifficulties: []
        property var difficulties: []
        property int currentDifficultyId: 5
    }
    QtObject {
        id: metadataTestPages
        property bool overlayActive: true
        property string activePageId: "latency"
        property int leaveRequests: 0
        function leaveOverlayPage() { ++leaveRequests; return false }
    }
    Component {
        id: normalMetadataSidebar
        ChartFieldSidebar { viewState: metadataState; documentSession: metadataTestDocument; commands: root.testCommands; pages: metadataTestPages }
    }
    Component {
        id: efficientMetadataSidebar
        EfficientSidebar { viewState: metadataState; documentSession: metadataTestDocument; commands: root.testCommands; pages: metadataTestPages }
    }

    TestCase {
        name: "WorkbenchDock"
        when: windowShown

        function init() {
            // Hiding a Qt.Tool window can leave the offscreen main window marked
            // active without a Quick focus item. Restore both before each case.
            root.Window.window.requestActivate()
            root.forceActiveFocus()
            tryVerify(() => root.Window.window.active && root.activeFocus)
        }

        function cleanup() {
            dock.maximizedPanel = ""
            dock.activePanel = ""
            const manager = findChild(dock, "workspaceManagerDialog")
            if (manager) manager.close()
            ordinaryInput.visible = false
            sourceInputStandIn.visible = false
            ordinaryInput.text = ""
            floatingShortcutFocus.visible = false
            floatingShortcutFocus.parent = root
            preferences.workspaceLayout = {}
            preferences.workspaceHiddenPanels = {}
            preferences.savedWorkspaceLayouts = {}
            preferences.savedWorkspaceLayoutOrder = []
            sharedLibrary.query = ""
            preferences.sidebarVisible = true
            preferences.bottomPanelVisible = true
            root.selectedBottom = "timeline"
            root.enabledPanels = root.panels
        }

        function test_nativeDefaultAndFixedActivityRail() {
            tryVerify(() => dock.useNativeFrame && outlinePane.visible && previewPane.visible && editor.visible)
            const bottom = dock.groups.find(group => group.tabs.includes("timeline"))
            compare(bottom.tabs.join(","), "timeline,validation,muri")
            compare(bottom.selected, "timeline")
            compare(outlinePane.width, preferences.sidebarWidth)
            compare(Math.round(editor.mapToItem(root, 0, 0).x), activityRail.width + outlinePane.width + 1)
            tryCompare(bottom, "height", root.height * preferences.bottomPanelHeightRatio)
            compare(bottom.mapToItem(root, 0, 0).x, editor.mapToItem(root, 0, 0).x)
            verify(previewPane.mapToItem(root, 0, 0).x > editor.mapToItem(root, 0, 0).x)
            compare(bottom.mapToItem(root, 0, 0).y, editor.height + 1)

            dock.activate("muri")
            tryCompare(bottom, "selected", "muri")
            dock.setPanelShown("validation", false)
            tryCompare(bottom, "tabs", ["timeline", "muri"])
            dock.setPanelShown("muri", false)
            tryCompare(bottom, "tabs", ["timeline"])
            dock.setPanelShown("validation", true)
            tryCompare(bottom, "selected", "validation")
            verify(!dock.panelShown("muri"))

            const railPosition = activityRail.mapToGlobal(0, 0)
            const outlineGrip = findChild(dock, "workspacePanelGrip_sidebar")
            verify(outlineGrip !== null)
            const previewGroup = dock.groups.find(group => group.selected === "preview")
            const destination = outlineGrip.mapFromItem(previewGroup, previewGroup.width / 2, previewGroup.height / 2)
            mouseDrag(outlineGrip, 12, 7, destination.x - 12, destination.y - 7)
            tryVerify(() => dock.groups.some(group => group.tabs.includes("sidebar") && group.tabs.includes("preview")))
            compare(activityRail.mapToGlobal(0, 0), railPosition)
            dock.updateDrag("sidebar", dock.mapToGlobal(dock.width + 30, 100))
            dock.finishDrag("sidebar")
            tryVerify(() => outlinePane.Window.window !== root.Window.window && outlinePane.visible)
            compare(activityRail.mapToGlobal(0, 0), railPosition)
            outlinePane.Window.window.close()
            tryVerify(() => !dock.panelShown("sidebar"))
            activityRail.viewRequested("chart")
            tryVerify(() => dock.panelShown("sidebar") && outlinePane.visible)
            compare(activityRail.mapToGlobal(0, 0), railPosition)
        }

        function test_maximizationIsTemporaryAndRespectsInput() {
            tryVerify(() => dock.useNativeFrame && previewPane.visible && previewPane.width > 100)
            const savedTree = JSON.stringify(preferences.workspaceLayout)
            const originalPreviewWidth = previewPane.width
            const activityPosition = activityRail.mapToGlobal(0, 0)
            mouseClick(previewPane, previewPane.width / 2, previewPane.height / 2)
            tryCompare(dock, "activePanel", "preview")
            dock.toggleCurrentPanelMaximization()
            tryCompare(dock, "maximizedPanel", "preview")
            tryVerify(() => !editor.visible && previewPane.visible && previewPane.width === dock.width)
            compare(activityRail.mapToGlobal(0, 0), activityPosition)
            compare(JSON.stringify(preferences.workspaceLayout), savedTree)
            dock.toggleCurrentPanelMaximization()
            tryVerify(() => dock.useNativeFrame && editor.visible)
            compare(previewPane.width, originalPreviewWidth)
            compare(JSON.stringify(preferences.workspaceLayout), savedTree)

            ordinaryInput.visible = true
            ordinaryInput.forceActiveFocus()
            tryVerify(() => ordinaryInput.activeFocus)
            keyClick("`")
            compare(ordinaryInput.text, "`")
            compare(dock.maximizedPanel, "")
            sourceInputStandIn.visible = true
            sourceInputStandIn.forceActiveFocus()
            tryVerify(() => sourceInputStandIn.activeFocus)
            verify(!dock.keyboardAllowed(root.Window.window))
        }

        function test_managerShortcutAndMetadataIsolation() {
            mouseClick(previewPane, previewPane.width / 2, previewPane.height / 2)
            const original = JSON.stringify(preferences.workspaceLayout)
            keyClick(Qt.Key_QuoteLeft)
            const manager = findChild(dock, "workspaceManagerDialog")
            verify(manager !== null)
            tryCompare(manager, "visible", true)
            compare(dock.maximizedPanel, "")
            keyClick(Qt.Key_Escape)
            tryCompare(manager, "visible", false)
            compare(JSON.stringify(preferences.workspaceLayout), original)

            metadataState.openEditor("difficulty:5", false)
            const editorKey = metadataState.activeEditorKey
            const tabs = JSON.stringify(metadataState.openEditorTabs)
            const previousRequests = metadataRequests.count
            metadataState.openMetadataEditor()
            compare(metadataRequests.count, previousRequests + 1)
            compare(metadataState.activeEditorKey, editorKey)
            compare(JSON.stringify(metadataState.openEditorTabs), tabs)
            verify(metadataState.difficultyEditorActive)
        }

        function test_sidebarMetadataDoesNotLeaveOverlay() {
            metadataState.openEditor("difficulty:5", false)
            const activeEditor = metadataState.activeEditorKey
            const tabs = JSON.stringify(metadataState.openEditorTabs)
            metadataTestPages.leaveRequests = 0
            for (const entry of [
                [normalMetadataSidebar, "chartMetadataNavigation"],
                [efficientMetadataSidebar, "efficientMetadataNavigation"]
            ]) {
                const sidebar = createTemporaryObject(entry[0], root, { width: 300, height: 400, z: 100 })
                verify(sidebar !== null)
                const button = findChild(sidebar, entry[1])
                verify(button !== null)
                tryVerify(() => button.visible && button.width > 0)
                const previousRequests = metadataRequests.count
                mouseClick(button)
                compare(metadataRequests.count, previousRequests + 1)
                compare(metadataTestPages.leaveRequests, 0)
                compare(metadataTestPages.activePageId, "latency")
                verify(metadataTestPages.overlayActive)
                compare(metadataState.activeEditorKey, activeEditor)
                compare(JSON.stringify(metadataState.openEditorTabs), tabs)
                sidebar.visible = false
            }
        }

        function test_layoutCaptureDoesNotIncludeTransientState() {
            dock.maximizedPanel = "preview"
            const snapshot = dock.captureLayout()
            verify(!("maximizedPanel" in snapshot))
            verify(!("activeEditorKey" in snapshot))
            verify(!("metadataDialog" in snapshot))
            compare(snapshot.timelineVerticalWidthRatio, preferences.timelineVerticalWidthRatio)
            compare(JSON.stringify(snapshot.tree), JSON.stringify(dock.tree))
        }

        function test_legacyLayoutKeepsItsOriginalTreeWithoutLibrary() {
            tryVerify(() => dock.useNativeFrame)
            verify(!dock.panelShown("library"))
            verify(!libraryPane.visible)
            const legacy = JSON.parse(JSON.stringify(dock.defaultTree))
            dock.commit(legacy, {})
            tryVerify(() => dock.customTree !== null)
            compare(JSON.stringify(dock.customTree), JSON.stringify(legacy))
            compare(JSON.stringify(dock.tree), JSON.stringify(legacy))
            verify(!dock.panelShown("library"))
            verify(!libraryPane.visible)
            preferences.workspaceLayout = { version: 1, tree: { tabs: ["editor"], selected: "editor" }, floating: {} }
            tryCompare(dock, "customTree", null)
        }

        function test_libraryDockFloatAndRestoreKeepLiveState() {
            tryVerify(() => dock.useNativeFrame && editor.visible)
            const editorInstance = editor
            const libraryInstance = libraryPane
            const modelInstance = libraryPane.library
            const text = editor.text
            editor.insert(editor.length, "\n")
            verify(editor.canUndo)
            const editedText = editor.text
            sharedLibrary.query = "kept filter"

            dock.setPanelShown("library", true)
            tryVerify(() => libraryPane.visible && !dock.useNativeFrame)
            verify(dock.groups.some(group => group.tabs.includes("library")))
            verify(editor === editorInstance && editor.text === editedText && editor.canUndo)
            const docked = dock.captureLayout()
            compare(docked.hiddenPanels.library, false)
            preferences.savedWorkspaceLayouts = { "Library docked": docked }
            preferences.savedWorkspaceLayoutOrder = ["Library docked"]
            dock.applyBuiltin("default")
            tryVerify(() => dock.useNativeFrame && !libraryPane.visible)
            dock.applySaved("Library docked")
            tryVerify(() => libraryPane.visible && !dock.isFloating("library"))
            verify(libraryPane === libraryInstance && libraryPane.library === modelInstance)
            compare(sharedLibrary.query, "kept filter")

            dock.updateDrag("library", dock.mapToGlobal(dock.width + 30, 100))
            dock.finishDrag("library")
            tryVerify(() => libraryPane.visible && libraryPane.Window.window !== root.Window.window)
            const floated = dock.captureLayout()
            verify(!!floated.floating.library)
            preferences.savedWorkspaceLayouts = { "Library floated": floated }
            preferences.savedWorkspaceLayoutOrder = ["Library floated"]
            dock.applyBuiltin("default")
            tryVerify(() => !libraryPane.visible)
            dock.applySaved("Library floated")
            tryVerify(() => libraryPane.visible && dock.isFloating("library"))
            verify(libraryPane === libraryInstance && libraryPane.library === modelInstance)
            compare(sharedLibrary.query, "kept filter")
            compare(editor.text, editedText)
            verify(editor.canUndo)
            dock.closeFloating("library")
            tryVerify(() => libraryPane.visible && libraryPane.Window.window === root.Window.window)
            verify(editor === editorInstance && libraryPane.library === modelInstance)
            editor.undo()
            compare(editor.text, text)
        }

        function test_reparentingPreservesLiveContent() {
            tryVerify(() => editor.visible && previewPane.visible && editor.width > 200 && previewPane.width > 100)
            const editorInstance = editor
            const previewInstance = previewPane
            editor.insert(editor.length, "\n")
            verify(editor.canUndo)

            const editorGroup = dock.groups.find(group => group.selected === "editor")
            const previewGrip = findChild(dock, "workspacePanelGrip_preview")
            verify(previewGrip !== null)
            const destination = previewGrip.mapFromItem(editorGroup,
                editorGroup.width / 2, editorGroup.height / 2)
            mouseDrag(previewGrip, 12, 7, destination.x - 12, destination.y - 7)
            tryVerify(() => dock.groups.some(group => group.tabs.includes("editor")
                && group.tabs.includes("preview")))
            verify(previewPane === previewInstance && previewPane.frame === 37)
            verify(editor === editorInstance && editor.canUndo)

            dock.activate("editor")
            tryVerify(() => editor.visible && !previewPane.visible)
            const outside = dock.mapToGlobal(dock.width + 30, 110)
            dock.updateDrag("editor", outside)
            dock.finishDrag("editor")
            tryVerify(() => editor.Window.window !== root.Window.window && editor.visible)
            verify(editor === editorInstance && editor.canUndo)
            verify(previewPane === previewInstance && previewPane.frame === 37)
            editor.Window.window.requestActivate()
            editor.forceActiveFocus()
            tryVerify(() => editor.activeFocus)
            keySequence(StandardKey.Save)
            tryCompare(root, "saves", 1)
            verify(editorPopup.parent === editor.Overlay.overlay)
            verify(editorPopup.parent.Window.window === editor.Window.window)

            const floatingEditor = editor.Window.window
            const manager = findChild(dock, "workspaceManagerDialog")
            const textBeforeShortcut = editor.text
            keyClick(Qt.Key_QuoteLeft)
            compare(editor.text.length, textBeforeShortcut.length + 1)
            compare(manager.visible, false)
            verify(!dock.windowMaximized(floatingEditor))
            editor.undo()
            compare(editor.text, textBeforeShortcut)

            floatingShortcutFocus.parent = floatingEditor.contentItem
            floatingShortcutFocus.visible = true
            floatingShortcutFocus.forceActiveFocus()
            tryVerify(() => floatingShortcutFocus.activeFocus)
            verify(dock.keyboardAllowed(floatingEditor))
            verify(!dock.keyboardAllowed(root.Window.window))
            keyClick(Qt.Key_QuoteLeft)
            tryCompare(manager, "visible", true)
            verify(manager.parent.Window.window === floatingEditor)
            verify(!dock.windowMaximized(floatingEditor))
            keyClick(Qt.Key_Escape)
            tryCompare(manager, "visible", false)
            floatingShortcutFocus.forceActiveFocus()
            const geometryBefore = JSON.stringify(dock.floating.editor)
            keyClick(Qt.Key_QuoteLeft, Qt.ControlModifier)
            tryVerify(() => dock.windowMaximized(floatingEditor))
            keyClick(Qt.Key_QuoteLeft, Qt.ControlModifier)
            tryVerify(() => !dock.windowMaximized(floatingEditor))
            compare(JSON.stringify(dock.floating.editor), geometryBefore)
            floatingShortcutFocus.visible = false
            floatingShortcutFocus.parent = root

            const group = dock.groups.find(candidate => candidate.tabs.includes("preview"))
            dock.updateDrag("editor", group.mapToGlobal(group.width / 2, group.height / 2))
            dock.finishDrag("editor")
            tryVerify(() => editor.Window.window === root.Window.window && editor.visible)
            editor.undo()
            compare(editor.text, "(120){4}1,2,E")

            root.enabledPanels = ["preview", "sidebar"]
            tryVerify(() => !editor.visible)
            dock.commit(dock.defaultTree, {})
            root.enabledPanels = root.panels
            tryVerify(() => editor.visible && previewPane.visible)
            verify(editor === editorInstance && previewPane === previewInstance)
        }
    }
}
