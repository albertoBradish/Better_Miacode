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
    readonly property bool previewOnLeft: preferencesModel.previewOnLeft
    property bool useBuiltinTree: false
    property bool compact: false
    property bool previewDetached: false
    property bool previewSurfaceMoving: false
    property bool exportVideoActive: false
    property bool bottomPanelEffectivelyVisible: dock.availablePanels.some(id => ["timeline", "validation", "muri", "patterns"].includes(id))
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
        property var workspaceListOrder: ["default", "editing", "preview"]
        function moveWorkspaceEntry(key, destination) {
            const keys = workspaceListOrder.slice()
            keys.splice(keys.indexOf(key), 1)
            keys.splice(destination, 0, key)
            workspaceListOrder = keys
        }
        property real timelineVerticalWidthRatio: 0.34
        property bool previewDetached: false
        property bool sidebarVisible: true
        property bool efficientSidebarEnabled: false
        property bool bottomPanelVisible: true
        property bool timelinePanelVisible: true
        property string timelineLayoutMode: "horizontal"
        property string timelineVerticalDirection: "falling"
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
        onChartTransformRequested: operation => {
            if (operation === "hand_order.set_left") ++root.leftHandCalls
        }
    }

    readonly property var panels: ["editor", "timeline", "preview", "sidebar", "validation", "muri", "patterns", "library"]
    property var enabledPanels: panels
    property int saves: 0
    property int leftHandCalls: 0
    property int workspaceMenuCalls: 0
    property string leftHandSequence: "Alt+W"

    WorkbenchDock {
        id: dock
        exportVideoActive: root.exportVideoActive
        handOrderFeaturesEnabled: true
        anchors.left: activityRail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        preferences: root.preferences
        libraryAvailable: true
        availablePanels: root.enabledPanels.filter(id => dock.panelShown(id)).concat(comparisonPanels.map(p => p.panelId))
        preferencesModel: root.preferencesModel
        defaultTree: root.useBuiltinTree ? builtinTree("default") : ({ axis: "horizontal", ratio: 0.21,
                        first: { tabs: ["sidebar"], selected: "sidebar" },
                        second: { axis: "horizontal", ratio: 0.55,
                            first: { axis: "vertical", ratio: 0.75,
                                first: { tabs: ["editor"], selected: "editor" },
                                second: { tabs: ["timeline", "validation", "muri", "patterns"], selected: root.selectedBottom } },
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
            if (panelId === "timeline" || panelId === "validation" || panelId === "muri" || panelId === "patterns")
                root.selectedBottom = panelId
        }
        floatPreview: function() { preferences.previewDetached = true }
        dockPreview: function() { preferences.previewDetached = false }
        editorShortcuts: Component {
            DockEditorShortcuts {
                menuCommands: commands
                shortcutModel: ({ revision: 1, captureActive: false,
                    sequence: id => id === "hand_order.set_left" ? root.leftHandSequence : "" })
                commandService: ({ shortcutCommandIds: () => ["hand_order.set_left"] })
                previewSession: ({handOrderFeaturesEnabled: dock.handOrderFeaturesEnabled})
                saveEnabled: true
                chartEditorActive: true
                onWorkspaceMenuRequested: anchor => ++root.workspaceMenuCalls
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
        readonly property color cornerBackingColor: "transparent"
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
        property int difficultyMask: 0
        property var packs: ["Test"]
        property var versions: []
        property string versionCategory: ""
        property var sessions: []
        property var activeSession: null
        property var proposals: []
        property var progress: ({indeterminate: false, percent: 0})
        property bool canUndoBatch: false
        property int openedSongs: 0
        signal changed()
        signal editorOpened()
        signal noteCountReady(string path, int difficulty, int notes)
        signal songAudioReady(string path)
        function rowForPath(path) { return path === get(0).chartPath ? 0 : -1 }
        function pathAt(row) { return row === 0 ? get(0).chartPath : "" }
        function hasSong(path) { return rowForPath(path) >= 0 }
        function songDetails(path) { return {path, fields: {title:"Generated fixture"}, stamp:"qa", difficulties:[], pack:"Test", track:"", jacket:""} }
        property bool controlHeld: false
        function additionalOpenModifierHeld() { return controlHeld }
        function openFromList(path) { openInEditor(path, 0) }
        function openInEditor(path) { if (hasSong(path)) ++openedSongs }
        function categoryUsage(key, name) { return 0 }
        property var genres: ["Test"]
        property string pack: ""
        property string genre: ""
        property string rootPath: "/generated"
        property string query: ""
        property bool busy: false
        property bool duplicatesOnly: false
        property int totalCount: count
        property int selectedCount: 0
        property string status: "Generated test index"
        function chartCaption(path) { return "Generated fixture · SD · 18" }
        function chooseRoot() {}
        function refresh() {}
        function cancel() {}
        function selectVisible(selected) { selectPath("/generated/maidata.txt", selected) }
        function selectPath(path, selected) { setProperty(0, "selected", selected); selectedCount = selected ? 1 : 0 }
        ListElement {
            chartPath: "/generated/maidata.txt"; chartTitle: "Generated fixture"
            artist: "QA"; levels: "5:12"; chartVersion: "test"
            sha256: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            chartPack: "Test"; chartGenre: "Test"; songId: "18"; chartKind: "SD"; jacketUrl: ""; difficultyBadges: ListElement { slot: 5; name: "MAS"; label: "12" }
            copies: 1; selected: false
        }
    }

    ChartLibraryPage {
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
            preferences.workspaceHiddenPanels = { library: true }
            // Hiding a Qt.Tool window can leave the offscreen main window marked
            // active without a Quick focus item. Restore both before each case.
            root.Window.window.requestActivate()
            root.forceActiveFocus()
            tryVerify(() => root.Window.window.active && root.activeFocus)
        }

        function cleanup() {
            root.exportVideoActive = false
            dock.playChecksAvailable = false
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
            preferences.timelineLayoutMode = "horizontal"
            preferences.timelineVerticalDirection = "falling"
            preferences.workspaceHiddenPanels = {}
            preferences.savedWorkspaceLayouts = {}
            preferences.savedWorkspaceLayoutOrder = []
            preferences.workspaceListOrder = ["default", "editing", "preview"]
            root.useBuiltinTree = false
            root.preferencesModel.previewOnLeft = false
            sharedLibrary.difficultyMask = 0
            sharedLibrary.query = ""
            sharedLibrary.controlHeld = false
            preferences.sidebarVisible = true
            preferences.bottomPanelVisible = true
            root.selectedBottom = "timeline"
            root.enabledPanels = root.panels
            root.leftHandSequence = "Alt+W"
            root.leftHandCalls = 0
            root.workspaceMenuCalls = 0
            commands.canTransform = false
        }

        function test_libraryDifficultyFilterMenuKeepsMultipleChecks() {
            dock.setPanelShown("library", true)
            dock.activate("library")
            const button = findChild(libraryPane, "chartLibraryDifficultyFilter")
            tryVerify(() => button && button.visible && button.width > 0)
            verify(waitForRendering(button))
            mouseClick(button, button.width / 2, button.height / 2)
            tryCompare(button, "active", true)
            const options = findChild(root.Overlay.overlay, "chartLibraryDifficultyOptions")
            tryVerify(() => options && options.visible)
            const expert = Array.from(options.children).find(option => option.difficultyId === 4)
            const master = Array.from(options.children).find(option => option.difficultyId === 5)
            mouseClick(expert)
            mouseClick(master)
            compare(sharedLibrary.difficultyMask, (1 << 4) | (1 << 5))
            verify(expert.checked && master.checked)
            mouseClick(expert)
            compare(sharedLibrary.difficultyMask, 1 << 5)
            mouseClick(master)
            compare(sharedLibrary.difficultyMask, 0)
            keyClick(Qt.Key_Escape)
            tryCompare(options, "visible", false)
        }

        function test_openLibraryRetainsPreviewSideAndCustomPanels() {
            root.useBuiltinTree = true
            root.preferencesModel.previewOnLeft = true
            for (const preset of ["default", "editing", "preview"]) {
                dock.applyBuiltin(preset)
                verify(root.preferencesModel.previewOnLeft)
                dock.setPanelShown("library", true)
                dock.activate("library")
                tryVerify(() => dock.hostFor("preview").mapToItem(root, 0, 0).x
                    < dock.hostFor("editor").mapToItem(root, 0, 0).x)
            }
            // Move the library into its own pane to the right of the editor.
            const editorGroup = dock.groups.find(group => group.tabs.includes("editor"))
            dock.dragging = "library"
            dock.drop = { group: editorGroup, edge: "right", beforeTab: "" }
            dock.finishDrag("library")
            const saved = dock.captureLayout()
            preferences.savedWorkspaceLayouts = { custom: saved }
            dock.applyBuiltin("default")
            dock.applySaved("custom")
            tryVerify(() => dock.customTree !== null)
            dock.setPanelShown("library", false)
            dock.setPanelShown("library", true)
            dock.activate("library")
            compare(JSON.stringify(dock.tree), JSON.stringify(saved.tree))
            verify(root.preferencesModel.previewOnLeft)
            tryVerify(() => dock.hostFor("preview").mapToItem(root, 0, 0).x
                < dock.hostFor("editor").mapToItem(root, 0, 0).x)
        }

        function test_comparisonsUseDockLayout() {
            const before = JSON.stringify(root.preferences.workspaceLayout)
            const id = "comparison-test"
            dock.comparisonPanels = [{panelId: id, title: "Test Master", workspaceLabel: "A"}]
            tryVerify(() => !!dock.hostFor(id) && dock.hostFor(id).visible)
            verify(dock.hostFor(id) !== dock.hostFor("preview"))
            compare(dock.panelTitle(id), "Child Preview A")
            compare(dock.floatingPanelTitle(id), "Test Master")
            dock.togglePanelMaximization(id, null)
            compare(dock.maximizedPanel, id)
            dock.comparisonPanels = []
            compare(dock.maximizedPanel, "")
            compare(JSON.stringify(root.preferences.workspaceLayout), before)
        }

        function test_workspaceSlotDefaultsAndTimelineStyle() {
            compare(dock.slotKeys.join(","), "default,editing,preview")
            preferences.workspaceHiddenPanels = {}
            verify(dock.panelShown("library"))
            const tree = JSON.stringify(dock.tree)
            dock.applyTimelineStyle("rising")
            compare(preferences.timelineVerticalDirection, "rising")
            compare(preferences.timelineLayoutMode, "vertical")
            compare(JSON.stringify(dock.tree), tree)
            dock.applyTimelineStyle("horizontal")
        }

        function test_managerPreviewsUntilApply() {
            const before = JSON.stringify(preferences.workspaceLayout)
            const manager = findChild(dock, "workspaceManagerDialog")
            dock.showManager()
            tryCompare(manager, "opened", true)
            const list = findChild(manager.body, "workspaceEntryList")
            tryVerify(() => list.itemAtIndex(2) !== null)
            const dragRow = list.itemAtIndex(1)
            mouseDrag(dragRow, dragRow.width - 17, dragRow.height / 2, 0, dragRow.height + 2, Qt.LeftButton)
            compare(preferences.workspaceListOrder.join(","), "default,preview,editing")
            const row = list.itemAtIndex(2)
            mouseClick(row, row.width / 2, row.height / 2)
            compare(manager.selectedKey, "editing")
            compare(JSON.stringify(preferences.workspaceLayout), before)
            const accept = findChild(manager.footer, "dialogFooterAccept")
            verify(accept.enabled)
            mouseClick(accept)
            tryCompare(manager, "visible", false)
            compare(preferences.workspaceLayout.version, 1)
        }

        function test_handOrderFeatureGate() {
            dock.handOrderFeaturesEnabled = false
            verify(!dock.panelIds.includes("patterns"))
            verify(!dock.panelShown("patterns"))
            dock.handOrderFeaturesEnabled = true
            verify(dock.panelIds.includes("patterns"))
        }

        function test_exportHidesHandOrderPanels_data() {
            return [{tag: "docked", floating: false}, {tag: "floating", floating: true}]
        }

        function test_exportHidesHandOrderPanels(data) {
            dock.playChecksAvailable = true
            root.enabledPanels = ["editor", "sidebar", "preview", "patterns", "play_checks"]
            root.selectedBottom = "patterns"
            if (data.floating)
                dock.updateFloatingGeometry("patterns", {x: 30, y: 30, width: 400, height: 300})
            tryVerify(() => data.floating ? dock.windowFor("patterns")?.visible : dock.hostFor("patterns").visible)
            if (!data.floating)
                tryCompare(dock.hostFor("patterns").parent, "height", root.height * preferences.bottomPanelHeightRatio)
            const floatingWindow = dock.windowFor("patterns")
            const layout = JSON.stringify(preferences.workspaceLayout)
            const hidden = JSON.stringify(preferences.workspaceHiddenPanels)
            const editorHeight = editor.height

            root.exportVideoActive = true
            for (const id of ["patterns", "play_checks"]) {
                verify(!dock.panelIds.includes(id))
                verify(!dock.panelPresent(id))
                dock.setPanelShown(id, true)
                verify(!dock.panelShown(id))
            }
            if (data.floating)
                tryCompare(floatingWindow, "visible", false)
            else
                tryVerify(() => editor.height > editorHeight)
            verify(dock.panelPresent("preview"))

            root.exportVideoActive = false
            tryVerify(() => data.floating ? floatingWindow.visible : dock.hostFor("patterns").visible)
            verify(dock.panelPresent("play_checks"))
            compare(dock.windowFor("patterns"), floatingWindow)
            compare(JSON.stringify(preferences.workspaceLayout), layout)
            compare(JSON.stringify(preferences.workspaceHiddenPanels), hidden)
            if (!data.floating)
                tryCompare(editor, "height", editorHeight)
        }

        function test_nativeDefaultAndFixedActivityRail() {
            tryVerify(() => dock.useNativeFrame && outlinePane.visible && previewPane.visible && editor.visible)
            const bottom = dock.groups.find(group => group.tabs.includes("timeline"))
            compare(bottom.tabs.join(","), "timeline,validation,muri,patterns")
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
            tryCompare(bottom, "tabs", ["timeline", "muri", "patterns"])
            dock.setPanelShown("muri", false)
            tryCompare(bottom, "tabs", ["timeline", "patterns"])
            dock.activate("patterns")
            tryCompare(bottom, "selected", "patterns")
            dock.setPanelShown("patterns", false)
            tryCompare(bottom, "tabs", ["timeline"])
            dock.setPanelShown("patterns", true)
            tryCompare(bottom, "selected", "patterns")
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
            // Native ShortcutOverride routing from text fields is covered by qml_shortcut_binding_spec.
            verify(dock.keyboardAllowed(root.Window.window))
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
            compare(manager.visible, false)
            tryVerify(() => dock.maximizedPanel.length > 0)
            keyClick(Qt.Key_QuoteLeft)
            tryCompare(dock, "maximizedPanel", "")
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
            const list = findChild(libraryPane, "managedLibrarySongList")
            verify(list !== null)
            const row = list.itemAtIndex(0)
            verify(row !== null)
            tryVerify(() => row.width <= libraryPane.width && row.width > 0 && list.height >= row.height)
            const beforeOpen = sharedLibrary.openedSongs
            mouseClick(row, row.width / 2, row.height / 2, Qt.LeftButton)
            compare(sharedLibrary.openedSongs, beforeOpen)
            sharedLibrary.controlHeld = true
            mouseClick(row, row.width / 2, row.height / 2, Qt.LeftButton, Qt.ControlModifier)
            compare(sharedLibrary.openedSongs, beforeOpen + 1)
            sharedLibrary.controlHeld = false
            mouseDoubleClickSequence(row, row.width / 2, row.height / 2, Qt.LeftButton)
            tryCompare(sharedLibrary, "openedSongs", beforeOpen + 2)
            const docked = dock.captureLayout()
            compare(docked.hiddenPanels.library, false)
            preferences.savedWorkspaceLayouts = { "Library docked": docked }
            preferences.savedWorkspaceLayoutOrder = ["Library docked"]
            dock.applyBuiltin("default")
            verify(dock.panelShown("library"))
            verify(!dock.useNativeFrame)
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
            verify(dock.panelShown("library"))
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
            floatingEditor.sourceEditorFocused = true
            commands.canTransform = true
            keyClick(Qt.Key_W, Qt.AltModifier)
            tryCompare(root, "leftHandCalls", 1)
            compare(root.workspaceMenuCalls, 0)
            commands.canTransform = false
            keyClick(Qt.Key_W, Qt.AltModifier)
            tryCompare(root, "workspaceMenuCalls", 1)
            commands.canTransform = true
            root.leftHandSequence = "Alt+Q"
            keyClick(Qt.Key_W, Qt.AltModifier)
            tryCompare(root, "workspaceMenuCalls", 2)
            compare(root.leftHandCalls, 1)
            const manager = findChild(dock, "workspaceManagerDialog")
            floatingShortcutFocus.parent = floatingEditor.contentItem
            floatingShortcutFocus.visible = true
            floatingShortcutFocus.forceActiveFocus()
            tryVerify(() => floatingShortcutFocus.activeFocus)
            verify(dock.keyboardAllowed(floatingEditor))
            verify(!dock.keyboardAllowed(root.Window.window))
            keyClick(Qt.Key_QuoteLeft)
            tryVerify(() => dock.windowMaximized(floatingEditor))
            compare(manager.visible, false)
            keyClick(Qt.Key_QuoteLeft)
            tryVerify(() => !dock.windowMaximized(floatingEditor))
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
