import QtQuick
import QtQuick.Controls
import QtTest
import MiaCode.UI

Item {
    id: root
    width: 960
    height: 640

    QtObject {
        id: preferences
        property var workspaceLayout: ({})
        property var savedWorkspaceLayouts: ({})
        property bool previewDetached: false
        function boundedWorkspaceWindowGeometry(rect) { return Qt.rect(0, 0, rect.width, rect.height) }
    }

    MainMenuCommands {
        id: commands
        onSaveRequested: ++root.saves
    }

    readonly property var panels: ["editor", "timeline", "preview", "sidebar", "diagnostics"]
    property int saves: 0

    WorkbenchDock {
        id: dock
        anchors.fill: parent
        preferences: preferences
        availablePanels: root.panels
        defaultTree: ({ axis: "horizontal", ratio: 0.56,
                        first: { tabs: ["editor", "timeline"], selected: "editor" },
                        second: { tabs: ["preview", "sidebar", "diagnostics"], selected: "preview" } })
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

        Popup {
            id: editorPopup
            parent: editor.Overlay.overlay
        }
    }

    Item {
        id: preview
        parent: dock.hostFor("preview")
        anchors.fill: parent
        property int frame: 37
    }

    TestCase {
        name: "WorkbenchDock"
        when: windowShown

        function test_reparentingPreservesLiveContent() {
            tryVerify(() => editor.visible && preview.visible)
            const editorInstance = editor
            const previewInstance = preview
            editor.insert(editor.length, "\n")
            verify(editor.canUndo)

            const editorGroup = dock.groups.find(group => group.selected === "editor")
            const previewTab = findChild(dock, "workspacePanelTab_preview")
            verify(previewTab !== null)
            const destination = previewTab.mapFromItem(editorGroup,
                editorGroup.width / 2, editorGroup.height / 2)
            mouseDrag(previewTab, 10, 12, destination.x - 10, destination.y - 12)
            tryVerify(() => dock.groups.some(group => group.tabs.includes("editor")
                && group.tabs.includes("preview")))
            verify(preview === previewInstance && preview.frame === 37)
            verify(editor === editorInstance && editor.canUndo)

            dock.activate("editor")
            tryVerify(() => editor.visible && !preview.visible)
            const outside = dock.mapToGlobal(dock.width + 30, 110)
            dock.updateDrag("editor", outside)
            dock.finishDrag("editor")
            tryVerify(() => editor.Window.window !== root.Window.window && editor.visible)
            verify(editor === editorInstance && editor.canUndo)
            verify(preview === previewInstance && preview.frame === 37)
            editor.Window.window.requestActivate()
            editor.forceActiveFocus()
            tryVerify(() => editor.activeFocus)
            keySequence(StandardKey.Save)
            tryCompare(root, "saves", 1)
            verify(editorPopup.parent === editor.Overlay.overlay)
            verify(editorPopup.parent.Window.window === editor.Window.window)

            const group = dock.groups.find(candidate => candidate.tabs.includes("preview"))
            dock.updateDrag("editor", group.mapToGlobal(group.width / 2, group.height / 2))
            dock.finishDrag("editor")
            tryVerify(() => editor.Window.window === root.Window.window && editor.visible)
            editor.undo()
            compare(editor.text, "(120){4}1,2,E")

            dock.availablePanels = ["preview", "sidebar"]
            tryVerify(() => !editor.visible)
            dock.commit(dock.defaultTree, {})
            dock.availablePanels = root.panels
            tryVerify(() => editor.visible && preview.visible)
            verify(editor === editorInstance && preview === previewInstance)
        }
    }
}
