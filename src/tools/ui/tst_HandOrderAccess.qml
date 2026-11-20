import QtQuick
import QtQuick.Controls
import QtTest
import MiaCode.UI

TestCase {
    id: test
    name: "HandOrderAccess"
    when: windowShown
    visible: true
    width: 640
    height: 480

    QtObject {
        id: prefs
        property bool handOrderFeaturesEnabled: false
        function unlockHandOrderFeatures(value) {
            if (value !== "fixture") return "Incorrect password"
            handOrderFeaturesEnabled = true
            return ""
        }
        function disableHandOrderFeatures() {
            handOrderFeaturesEnabled = false
            return true
        }
    }

    HandOrderAccessControl {
        id: controls
        anchors.centerIn: parent
        width: 300
        preferencesModel: prefs
    }

    function test_passwordTransitions() {
        const toggle = findChild(controls, "preferencesHandOrderFeaturesEnabled")
        const dialog = findChild(controls, "handOrderPasswordDialog")
        verify(toggle && dialog)
        const field = findChild(dialog.contentItem, "handOrderPasswordField")
        verify(field)
        compare(field.echoMode, TextInput.Password)
        mouseClick(toggle)
        tryCompare(dialog, "opened", true)
        compare(toggle.checked, false)
        field.text = "incorrect"
        keyClick(Qt.Key_Return)
        tryCompare(dialog, "errorText", "Incorrect password")
        compare(toggle.checked, false)
        keyClick(Qt.Key_Escape)
        tryCompare(dialog, "visible", false)
        compare(field.text, "")
        mouseClick(toggle)
        tryCompare(dialog, "opened", true)
        field.text = "fixture"
        keyClick(Qt.Key_Return)
        tryCompare(toggle, "checked", true)
        tryCompare(dialog, "visible", false)
        compare(field.text, "")
        mouseClick(toggle)
        tryCompare(toggle, "checked", false)
        compare(dialog.visible, false)
        mouseClick(toggle)
        tryCompare(dialog, "opened", true)
        compare(toggle.checked, false)
        dialog.close()
    }
}
