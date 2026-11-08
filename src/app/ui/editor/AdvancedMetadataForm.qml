import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    required property var documentSession
    property int difficultyId: documentSession.currentDifficultyId > 0 ? documentSession.currentDifficultyId : 5
    property string errorText: ""
    readonly property var groups: {
        const generation = documentSession.documentOpenGeneration
        return documentSession.advancedMetadataGroups(difficultyId)
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
            title: modelData.title
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
                        text: fieldRow.modelData.label + "  (&" + fieldRow.modelData.key + ")"
                        color: Theme.colors.text.secondary
                        wrapMode: Text.WordWrap
                    }
                    AppTextField {
                        id: singleInput
                        Layout.fillWidth: true
                        visible: !fieldRow.modelData.multiline
                        text: fieldRow.modelData.value
                        selectByMouse: true
                        onEditingFinished: {
                            if (text !== root.documentSession.advancedMetadataValue(fieldRow.modelData.key))
                                fieldRow.rejectedDraft = !root.updateField(fieldRow.modelData.key, text)
                        }
                    }
                    AppTextArea {
                        id: multilineInput
                        Layout.fillWidth: true
                        Layout.preferredHeight: 90
                        visible: fieldRow.modelData.multiline
                        text: fieldRow.modelData.value
                        onActiveFocusChanged: {
                            if (!activeFocus && text !== root.documentSession.advancedMetadataValue(fieldRow.modelData.key))
                                fieldRow.rejectedDraft = !root.updateField(fieldRow.modelData.key, text)
                        }
                    }
                }
            }
        }
    }
}
