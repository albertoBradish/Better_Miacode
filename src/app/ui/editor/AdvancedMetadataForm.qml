import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    required property var documentSession
    property int difficultyId: documentSession.currentDifficultyId > 0 ? documentSession.currentDifficultyId : 5
    property string errorText: ""
    property var groups: []
    property var groupsGeneration: -1
    function refreshGroups() {
        if (!documentSession)
            return
        groupsGeneration = documentSession.documentOpenGeneration
        groups = documentSession.advancedMetadataGroups(difficultyId)
    }
    onDifficultyIdChanged: refreshGroups()
    Component.onCompleted: refreshGroups()
    Connections {
        target: root.documentSession
        function onDocumentStateChanged() {
            if (root.groupsGeneration !== root.documentSession.documentOpenGeneration)
                root.refreshGroups()
        }
    }
    spacing: Theme.settingsRowSpacing

    function updateField(key, value) {
        errorText = documentSession.setAdvancedMetadataField(key, value)
        return errorText.length === 0
    }

    LabeledCombo {
        label: qsTrId("metadata.advanced.difficulty")
        options: [
            {value: 1, label: "Easy"}, {value: 2, label: "Basic"},
            {value: 3, label: "Advanced"}, {value: 4, label: "Expert"},
            {value: 5, label: "Master"}, {value: 6, label: "Re:Master"},
            {value: 7, label: "Original"}
        ]
        currentValue: root.difficultyId
        onPicked: value => root.difficultyId = value
    }
    Label {
        Layout.fillWidth: true
        visible: root.errorText.length > 0
        text: root.errorText
        color: Theme.colors.syntax.error
        wrapMode: Text.WordWrap
    }
    Label {
        Layout.fillWidth: true
        visible: root.documentSession.metadataConflictText.length > 0
        text: root.documentSession.metadataConflictText
        color: Theme.colors.syntax.error
        wrapMode: Text.WordWrap
    }
    Repeater {
        model: root.groups
        delegate: SettingsSection {
            id: groupSection
            required property var modelData
            Layout.fillWidth: true
            title: qsTrId(modelData.titleKey)
            Label {
                Layout.fillWidth: true
                visible: groupSection.modelData.id === "metadata.advanced.compatibility"
                text: qsTrId("metadata.advanced.legacy_warning")
                color: Theme.colors.text.secondary
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: groupSection.modelData.fields
                delegate: ColumnLayout {
                    id: fieldRow
                    required property var modelData
                    property bool rejectedDraft: false
                    property string validationError: ""
                    function submit(value) {
                        rejectedDraft = !root.updateField(modelData.key, value)
                        validationError = rejectedDraft ? root.errorText : ""
                    }
                    Layout.fillWidth: true
                    spacing: Theme.settingsLabelSpacing
                    Connections {
                        target: root.documentSession
                        function onMetadataChanged() {
                            if (fieldRow.rejectedDraft) return
                            const value = root.documentSession.advancedMetadataValue(fieldRow.modelData.key)
                            if (!singleInput.activeFocus) singleInput.text = value
                            if (!multilineInput.activeFocus) multilineInput.text = value
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTrId(fieldRow.modelData.labelKey) + "  (&" + fieldRow.modelData.key + ")"
                              + (fieldRow.modelData.unit ? " · " + fieldRow.modelData.unit : "")
                        color: Theme.colors.text.secondary
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTrId(fieldRow.modelData.helpKey)
                        color: Theme.colors.text.secondary
                        font.pixelSize: Theme.secondaryFontSize
                        wrapMode: Text.WordWrap
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.settingsLabelSpacing
                        visible: !fieldRow.modelData.readOnly && fieldRow.modelData.suggestions.length > 0
                        Repeater {
                            model: fieldRow.modelData.suggestions
                            delegate: AppButton {
                                required property string modelData
                                text: modelData
                                Accessible.name: qsTrId("metadata.reference.use_suggestion").arg(modelData)
                                onClicked: {
                                    singleInput.text = modelData
                                    fieldRow.submit(modelData)
                                }
                            }
                        }
                    }
                    AppTextField {
                        id: singleInput
                        Layout.fillWidth: true
                        outlined: true
                        Accessible.name: qsTrId(fieldRow.modelData.labelKey)
                        visible: !fieldRow.modelData.multiline
                        readOnly: fieldRow.modelData.readOnly
                        placeholderText: fieldRow.modelData.example
                        text: fieldRow.modelData.value
                        selectByMouse: true
                        onEditingFinished: {
                            if (text !== root.documentSession.advancedMetadataValue(fieldRow.modelData.key))
                                fieldRow.submit(text)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: fieldRow.validationError
                        visible: text.length > 0
                        color: Theme.colors.syntax.error
                        wrapMode: Text.WordWrap
                    }
                    ScrollView {
                        id: multilineScroll
                        Layout.fillWidth: true
                        Layout.preferredHeight: 110
                        visible: fieldRow.modelData.multiline
                        clip: true
                        AppTextArea {
                            id: multilineInput
                            width: multilineScroll.availableWidth
                            height: Math.max(multilineScroll.availableHeight, implicitHeight)
                            outlined: true
                            wrapMode: TextEdit.Wrap
                            Accessible.name: qsTrId(fieldRow.modelData.labelKey)
                            readOnly: fieldRow.modelData.readOnly
                            text: fieldRow.modelData.value
                            onActiveFocusChanged: {
                                if (!activeFocus && text !== root.documentSession.advancedMetadataValue(fieldRow.modelData.key))
                                    fieldRow.submit(text)
                            }
                        }
                    }
                }
            }
        }
    }
}
