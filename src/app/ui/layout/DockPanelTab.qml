import QtQuick
import QtQuick.Controls
import MiaCode.UI

AppTabButton {
    id: root

    required property string panelId
    required property var workspace

    objectName: "workspacePanelTab_" + panelId
    text: workspace.panelTitle(panelId)
    ToolTip.visible: hovered && !drag.active
    ToolTip.text: qsTrId("workspace.drag_panel")
    Accessible.description: ToolTip.text
    onClicked: workspace.activate(panelId)

    DragHandler {
        id: drag
        target: null
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.ClosedHandCursor
        grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.ApprovesCancellation

        onActiveChanged: {
            if (active)
                root.workspace.updateDrag(root.panelId, root.mapToGlobal(centroid.position))
        }
        onCentroidChanged: {
            if (active)
                root.workspace.updateDrag(root.panelId, root.mapToGlobal(centroid.position))
        }
        onGrabChanged: (transition, point) => {
            if (transition === PointerDevice.UngrabExclusive)
                root.workspace.finishDrag(root.panelId)
            else if (transition === PointerDevice.CancelGrabExclusive)
                root.workspace.cancelDrag()
        }
    }
}
