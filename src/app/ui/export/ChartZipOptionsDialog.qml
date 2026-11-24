import QtQuick
import QtQuick.Layouts
import MiaCode.UI

AppDialog {
    id: root
    property bool normalizeDefault: false
    signal exportRequested(var options)
    title: qsTrId("export.export_as_zip")
    preferredWidth: 420
    preferredHeight: implicitHeight
    onAboutToShow: {
        withoutPv.checked = false
        basic.checked = false
        normalize.checked = normalizeDefault
    }
    body: ColumnLayout {
        AppCheckBox { id: withoutPv; text: qsTrId("export.zip.without_pv") }
        AppCheckBox { id: basic; text: qsTrId("export.zip.basic_metadata") }
        AppCheckBox { id: normalize; text: qsTrId("export.zip.normalize") }
    }
    footer: DialogFooter {
        acceptText: qsTrId("export.export_as_zip")
        cancelText: qsTrId("action.cancel")
        onAccepted: root.accept()
        onRejected: root.reject()
    }
    onAccepted: exportRequested({ withoutPvCopy: withoutPv.checked, basicMetadata: basic.checked, normalize: normalize.checked })
}
