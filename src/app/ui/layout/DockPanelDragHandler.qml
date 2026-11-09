import QtQuick

DragHandler {
    id: root

    required property string panelId
    required property var workspace

    target: null
    acceptedButtons: Qt.LeftButton
    cursorShape: Qt.ClosedHandCursor
    grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.ApprovesCancellation

    function updateDrag() {
        if (active)
            workspace.updateDrag(panelId, parent.mapToGlobal(centroid.position))
    }

    onActiveChanged: updateDrag()
    onCentroidChanged: updateDrag()
    onGrabChanged: (transition, point) => {
        if (transition === PointerDevice.UngrabExclusive)
            workspace.finishDrag(panelId)
        else if (transition === PointerDevice.CancelGrabExclusive)
            workspace.cancelDrag()
    }
}
