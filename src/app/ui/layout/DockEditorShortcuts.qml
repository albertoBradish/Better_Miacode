pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import MiaCode.UI

Item {
    id: root

    required property var menuCommands
    required property var shortcutModel
    required property var commandService
    required property var previewSession
    property bool nativeMenuBar: false
    property bool editorActive: false
    property bool saveEnabled: false
    property bool documentAvailable: false
    property bool chartEditorActive: false
    property bool editorCommandsEnabled: false
    signal closeEditorRequested()

    Instantiator {
        model: [
            { key: StandardKey.Save, enabled: root.saveEnabled, invoke: () => root.menuCommands.saveRequested() },
            { key: StandardKey.SaveAs, enabled: root.documentAvailable, invoke: () => root.menuCommands.saveAsRequested() },
            { key: StandardKey.Close, enabled: root.editorActive, invoke: () => root.closeEditorRequested() },
            { key: StandardKey.Find, enabled: root.editorCommandsEnabled, invoke: () => root.menuCommands.findRequested() },
            { key: StandardKey.Undo, enabled: root.menuCommands.canUndo, invoke: () => root.menuCommands.undoRequested() },
            { key: StandardKey.Redo, enabled: root.menuCommands.canRedo, invoke: () => root.menuCommands.redoRequested() },
            { key: StandardKey.Cut, enabled: root.menuCommands.canCut, invoke: () => root.menuCommands.cutRequested() },
            { key: StandardKey.Copy, enabled: root.menuCommands.canCopy, invoke: () => root.menuCommands.copyRequested() },
            { key: StandardKey.Paste, enabled: root.menuCommands.canPaste, invoke: () => root.menuCommands.pasteRequested() },
            { key: StandardKey.SelectAll, enabled: root.editorCommandsEnabled, invoke: () => root.menuCommands.selectAllRequested() }
        ]
        delegate: Shortcut {
            required property var modelData
            sequences: [modelData.key]
            context: Qt.WindowShortcut
            enabled: !root.nativeMenuBar && root.menuCommands.commandsEnabled && modelData.enabled
            onActivated: modelData.invoke()
        }
    }

    ShortcutBindings {
        shortcuts: root.shortcutModel
        commands: root.commandService
        previewSession: root.previewSession
        chartCommandsEnabled: root.chartEditorActive && root.menuCommands.commandsEnabled
        playbackCommandsEnabled: false
        menuOwnsChartTransformShortcuts: root.nativeMenuBar
        onChartTransformRequested: opId => root.menuCommands.chartTransformRequested(opId)
    }
}
