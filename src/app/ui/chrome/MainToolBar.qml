import QtQuick
import QtQuick.Controls
import MiaCode.UI

Rectangle {
    id: root

    required property var hostWindow
    required property var windowChrome

    signal workspaceSlotRequested(int index)
    property var workspaceSlotTitles: []
    signal openRequested()
    signal openCurrentFolderRequested()
    signal saveRequested()
    signal saveWholeDocumentRequested()
    signal undoRequested()
    signal redoRequested()
    signal handOrderRequested(string command)
    property var handOrderRows: []
    property var shortcuts: null
    property bool handOrderEnabled: false
    property bool handOrderAnalyzeEnabled: false
    property bool handOrderBreakdown: false
    signal audioSettingsRequested()
    signal previewSettingsRequested()
    signal showDetachedPreviewRequested()
    signal unavailableFeatureRequested(string featureName)

    property bool saveEnabled: false
    property bool wholeDocumentSaveEnabled: false
    property bool currentFolderAvailable: false
    property bool canUndo: false
    property bool canRedo: false
    property bool integratedInTitleBar: false
    property real titleBarLeadingInset: 0
    property bool previewSettingsVisible: true

    readonly property Item documentActionsItem: leftActions
    readonly property Item workspaceActionsItem: rightActions
    readonly property real leadingActionsRight: leftActions.x + leftActions.width
    readonly property real trailingActionsWidth: width - rightActions.x
    readonly property real minimumWidth: leftActions.anchors.leftMargin + leftActions.width
        + 16 + rightActions.width + rightActions.anchors.rightMargin

    implicitHeight: root.integratedInTitleBar ? 32 : Theme.windowChromeRowHeight
    color: root.integratedInTitleBar
           ? "transparent"
           : Theme.chromeSurfaceColor(Theme.colors.background.activityBar)

    component ToolBarButton: IconButton {
        height: root.integratedInTitleBar ? implicitHeight : root.height
        anchors.verticalCenter: parent.verticalCenter
        stateColors: Theme.chromeStateColors
    }

    WindowGestureArea {
        anchors.fill: parent
        hostWindow: root.hostWindow
        windowChrome: root.windowChrome
        visible: !root.integratedInTitleBar
        z: 0
    }

    Row {
        id: leftActions
        height: root.height
        anchors.left: parent.left
        anchors.leftMargin: root.integratedInTitleBar
                            ? (root.titleBarLeadingInset > 0
                               ? root.titleBarLeadingInset
                               : Theme.chromePadding)
                            : (Theme.activityButtonSize - openButton.width) / 2
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        z: 1

        ToolBarButton {
            objectName: "openDocumentButton"
            id: openButton
            iconSource: Qt.resolvedUrl("icons/folder-open.svg")
            tooltip: qsTrId("action.open")
            onClicked: root.openRequested()
        }
        ToolBarButton {
            objectName: "openCurrentFolderButton"
            iconSource: Qt.resolvedUrl("icons/folder-arrow-right.svg")
            tooltip: qsTrId("action.open_current_folder")
            enabled: root.currentFolderAvailable
            onClicked: root.openCurrentFolderRequested()
        }
        Item { width: 6; height: 1 }
        ToolBarButton {
            objectName: "saveCurrentEditorButton"
            iconSource: Qt.resolvedUrl("icons/save.svg")
            tooltip: qsTrId("action.save")
            enabled: root.saveEnabled
            onClicked: root.saveRequested()
        }
        ToolBarButton {
            objectName: "saveWholeDocumentButton"
            iconSource: Qt.resolvedUrl("icons/save-multiple.svg")
            tooltip: qsTrId("qml.save_entire_document")
            enabled: root.wholeDocumentSaveEnabled
            onClicked: root.saveWholeDocumentRequested()
        }
        Item { width: 6; height: 1 }
        ToolBarButton {
            property bool preservesEditTarget: true
            iconSource: Qt.resolvedUrl("icons/undo.svg")
            tooltip: qsTrId("qml.undo")
            enabled: root.canUndo
            onClicked: root.undoRequested()
        }
        ToolBarButton {
            property bool preservesEditTarget: true
            iconSource: Qt.resolvedUrl("icons/redo.svg")
            tooltip: qsTrId("action.redo")
            enabled: root.canRedo
            onClicked: root.redoRequested()
        }
    }

    Row {
        id: rightActions
        height: root.height
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        z: 1

        ToolBarButton {
            objectName: "handOrderBasicLayoutButton"
            visible: root.handOrderBreakdown
            iconSource: Qt.resolvedUrl("icons/tools.svg")
            label: qsTrId("hand_order.workflow.basic")
            tooltip: qsTrId("hand_order.basic_layout_scope")
                + "\n" + qsTrId("hand_order.basic_layout_next_step")
                + (root.shortcuts && root.shortcuts.revision >= 0
                    ? "\n" + root.shortcuts.displayText("hand_order.basic_layout") : "")
            enabled: root.handOrderAnalyzeEnabled
            focusPolicy: Qt.NoFocus
            onClicked: root.handOrderRequested("hand_order.basic_layout")
        }
        ToolBarButton {
            objectName: "handOrderContextOptimizationButton"
            visible: root.handOrderBreakdown
            iconSource: Qt.resolvedUrl("icons/tools.svg")
            label: qsTrId("hand_order.workflow.context")
            tooltip: qsTrId("hand_order.workflow.context_help")
            enabled: root.handOrderAnalyzeEnabled
            focusPolicy: Qt.NoFocus
            onClicked: root.handOrderRequested("hand_order.analyze")
        }
        ToolBarButton {
            objectName: "handOrderPlayReviewButton"
            visible: root.handOrderBreakdown
            iconSource: Qt.resolvedUrl("icons/tools.svg")
            label: qsTrId("hand_order.workflow.play_review")
            tooltip: qsTrId("hand_order.play_review.notice")
            enabled: root.handOrderAnalyzeEnabled
            focusPolicy: Qt.NoFocus
            onClicked: root.handOrderRequested("hand_order.play_review")
        }
        ToolBarButton {
            visible: root.previewSettingsVisible
            iconSource: Qt.resolvedUrl("icons/audio-settings.svg")
            label: qsTrId("action.audio_settings")
            tooltip: qsTrId("action.audio_settings")
            onClicked: root.audioSettingsRequested()
        }
        ToolBarButton {
            visible: root.previewSettingsVisible
            iconSource: Qt.resolvedUrl("icons/preview-settings.svg")
            label: qsTrId("action.video_settings")
            tooltip: qsTrId("action.video_settings")
            onClicked: root.previewSettingsRequested()
        }
        ToolBarButton {
            objectName: "showDetachedPreviewButton"
            visible: !root.previewSettingsVisible
            iconSource: Qt.resolvedUrl("icons/preview-detach.svg")
            label: qsTrId("preview.window.show")
            tooltip: label
            onClicked: root.showDetachedPreviewRequested()
        }
        Repeater {
            model: 3
            delegate: ToolBarButton {
                required property int index
                objectName: "workspaceSlot" + (index + 1)
                iconSource: Qt.resolvedUrl("icons/workspace-" + (index + 1) + ".svg")
                tooltip: root.workspaceSlotTitles[index] ?? ""
                onClicked: root.workspaceSlotRequested(index)
            }
        }
    }

}
