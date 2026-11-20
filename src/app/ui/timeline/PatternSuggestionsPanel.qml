pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import MiaCode.UI
import MiaCode.Preview

Item {
    id: root
    property var service: null
    property var previewRuntime: null
    property string sortMode: "category"
    property var expandedTypes: ({})
    property var checkedKeys: []
    readonly property var similarGroups: group ? groups.filter(item => item.similarityKey === group.similarityKey) : []
    readonly property var selectedKeys: checkedKeys.filter(key => similarGroups.some(item => item.key === key))
    property string selectedPatternKey: ""
    readonly property var groups: {
        const result = service ? service.patternSuggestions.slice() : []
        result.sort((a, b) => {
            if (root.sortMode === "category") {
                if (a.categoryOrder !== b.categoryOrder) return a.categoryOrder - b.categoryOrder
                if (a.registryId !== b.registryId) return a.registryId.localeCompare(b.registryId)
            }
            return a.second - b.second || a.sourceIndex - b.sourceIndex
        })
        return result
    }
    readonly property int selectedGroup: {
        for (let i = 0; i < groups.length; ++i) if (groups[i].key === selectedPatternKey) return i
        return 0
    }
    readonly property var group: groups.length > 0 ? groups[Math.min(selectedGroup, groups.length - 1)] : null
    readonly property var treeRows: {
        if (root.sortMode === "time") return root.groups.map(item => Object.assign({header: false}, item))
        const buckets = []
        for (const item of root.groups) {
            let bucket = buckets.find(value => value.key === item.typeKey)
            if (!bucket) { bucket = {key: item.typeKey, name: item.typeLabel, items: []}; buckets.push(bucket) }
            bucket.items.push(item)
        }
        const rows = []
        for (const bucket of buckets) {
            rows.push({header: true, key: bucket.key, name: bucket.name, count: bucket.items.length,
                expanded: root.expandedTypes[bucket.key] === true})
            if (root.expandedTypes[bucket.key]) {
                const families = []
                for (const item of bucket.items) {
                    let family = families.find(value => value.key === item.similarityKey)
                    if (!family) { family = {key: item.similarityKey, items: []}; families.push(family) }
                    family.items.push(item)
                }
                for (let i = 0; i < families.length; ++i) {
                    const family = families[i]
                    const key = "similar:" + family.key
                    if (family.items.length > 1) rows.push({header: true, family: true, key: key,
                        name: qsTrId("hand_order.pattern.similar_group").arg(i + 1), count: family.items.length,
                        expanded: root.expandedTypes[key] === true})
                    if (family.items.length === 1 || root.expandedTypes[key])
                        for (const item of family.items) rows.push(Object.assign({header: false}, item))
                }
            }
        }
        return rows
    }
    function toggleType(key) {
        const next = Object.assign({}, root.expandedTypes)
        next[key] = !next[key]
        root.expandedTypes = next
    }
    property real previewSecond: 0
    property bool playing: true
    signal choiceRequested(int group, int variant)
    signal choicesRequested(var keys, string choice)
    signal locationRequested(int group)
    objectName: "patternSuggestionsPanel"

    onGroupChanged: previewSecond = group ? group.second - 0.4 : 0
    onGroupsChanged: {
        if (groups.length === 0) { selectedPatternKey = ""; return }
        if (!groups.some(item => item.key === selectedPatternKey)) selectedPatternKey = groups[0].key
    }
    Timer {
        interval: 40
        repeat: true
        running: root.visible && root.playing && root.group !== null
        onTriggered: {
            const end = root.group.endSecond + 0.5
            root.previewSecond = root.previewSecond + 0.02 > end ? root.group.second - 0.4 : root.previewSecond + 0.02
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.panelPadding
        spacing: Theme.chromeInsetY
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: root.service && root.service.pending ? qsTrId("qml.analyzing")
                    : qsTrId("hand_order.pattern.count").arg(root.groups.length)
                color: Theme.colors.text.secondary
                wrapMode: Text.Wrap
            }
            AppButton {
                objectName: "scanPatternSuggestions"
                text: qsTrId("hand_order.pattern.scan")
                enabled: root.service && !root.service.pending
                onClicked: root.service.requestCurrentPatterns()
            }
            LabeledCombo {
                objectName: "patternSortOrder"
                Layout.preferredWidth: 200
                Layout.maximumWidth: 240
                labelWidth: 36
                label: qsTrId("hand_order.pattern.sort")
                currentValue: root.sortMode
                options: [
                    {value: "time", label: qsTrId("hand_order.pattern.sort.time")},
                    {value: "category", label: qsTrId("hand_order.pattern.sort.category")}
                ]
                onPicked: value => root.sortMode = value
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.panelPadding
            visible: root.group !== null
            ListView {
                id: patternList
                Layout.preferredWidth: Math.min(200, root.width * 0.25)
                Layout.fillHeight: true
                clip: true
                model: root.treeRows
                delegate: ChromeRow {
                    id: entry
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    selected: !modelData.header && modelData.key === root.selectedPatternKey
                    Accessible.name: modelData.name
                    leftPadding: Theme.rowPaddingX + (modelData.family ? Theme.panelPadding : modelData.header ? 0 : Theme.panelPadding + 16)
                    objectName: modelData.header ? "patternCategory_" + modelData.key : "patternSuggestionRow_" + modelData.sourceIndex
                    onClicked: {
                        if (modelData.header) { root.toggleType(modelData.key); return }
                        root.selectedPatternKey = modelData.key; root.locationRequested(modelData.sourceIndex)
                    }
                    contentItem: RowLayout {
                        spacing: Theme.chromeInsetX
                        ControlsImpl.IconImage {
                            objectName: "patternCategoryIndicator_" + entry.modelData.key
                            visible: entry.modelData.header
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            sourceSize.width: 16
                            sourceSize.height: 16
                            source: entry.modelData.expanded ? Qt.resolvedUrl("icons/chevron-down.svg") : Qt.resolvedUrl("icons/chevron-right.svg")
                            color: Theme.colors.text.secondary
                        }
                        AppCheckBox {
                            objectName: "selectPattern_" + (entry.modelData.sourceIndex === undefined ? "header" : entry.modelData.sourceIndex)
                            visible: !entry.modelData.header
                            checked: root.selectedKeys.indexOf(entry.modelData.key) >= 0
                            Accessible.name: qsTrId("hand_order.pattern.select_instance")
                            onToggled: {
                                const selected = checked
                                root.selectedPatternKey = entry.modelData.key
                                const keys = root.checkedKeys.filter(key => root.similarGroups.some(item => item.key === key) && key !== entry.modelData.key)
                                if (selected) keys.push(entry.modelData.key)
                                root.checkedKeys = keys
                                root.locationRequested(entry.modelData.sourceIndex)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            objectName: entry.modelData.header ? "patternCategoryText_" + entry.modelData.key : "patternInstanceText_" + entry.modelData.sourceIndex
                            text: entry.modelData.header ? entry.modelData.name + "  (" + entry.modelData.count + ")"
                                : entry.modelData.registryId + " · " + entry.modelData.name + "\n"
                                + "L" + entry.modelData.line + ":" + entry.modelData.col + " · "
                                + (entry.modelData.preferred >= 0 ? qsTrId("hand_order.pattern.default") : qsTrId("hand_order.pattern.choose"))
                            color: Theme.colors.text.primary
                            wrapMode: Text.Wrap
                            font.family: Theme.uiFont
                            font.pixelSize: Theme.uiFontSize
                            font.bold: entry.modelData.header
                        }
                    }
                }
                ScrollBar.vertical: AppScrollBar {}
            }
            ScrollView {
                id: details
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                rightPadding: detailScrollBar.visible ? detailScrollBar.width + Theme.chromeInsetX : 0
                contentWidth: availableWidth
                ScrollBar.vertical: AppScrollBar { id: detailScrollBar }
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                Column {
                    width: details.availableWidth
                    spacing: Theme.panelPadding
                    RowLayout {
                        width: parent.width
                        Label {
                            Layout.fillWidth: true
                            text: root.group ? root.group.registryId + " · " + root.group.name : ""
                            color: Theme.colors.text.primary
                            wrapMode: Text.Wrap
                        }
                        AppButton {
                            objectName: "ignorePatternSuggestion"
                            text: qsTrId("hand_order.pattern.ignore")
                            onClicked: {
                                const keys = root.selectedKeys.length > 0 ? root.selectedKeys : [root.group.key]
                                root.service.ignorePatterns(keys)
                                root.checkedKeys = []
                            }
                        }
                    }
                    RowLayout {
                        width: parent.width
                        visible: root.similarGroups.length > 1
                        AppButton {
                            objectName: "selectSimilarPatterns"
                            text: qsTrId("hand_order.pattern.select_similar").arg(root.similarGroups.length)
                            onClicked: root.checkedKeys = root.similarGroups.map(item => item.key)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTrId("hand_order.pattern.selected_count").arg(root.selectedKeys.length)
                            color: Theme.colors.text.secondary
                            wrapMode: Text.Wrap
                        }
                    }
                    RowLayout {
                        width: parent.width
                        AppButton {
                            text: root.playing ? qsTrId("hand_order.pattern.pause") : qsTrId("hand_order.pattern.play")
                            onClicked: root.playing = !root.playing
                        }
                        AppSlider {
                            Layout.fillWidth: true
                            from: root.group ? root.group.second - 0.4 : 0
                            to: root.group ? root.group.endSecond + 0.5 : 1
                            value: root.previewSecond
                            onMoved: { root.playing = false; root.previewSecond = value }
                        }
                    }
                    Flow {
                        id: candidates
                        width: parent.width
                        spacing: Theme.panelPadding
                        Repeater {
                            model: root.group ? root.group.variants : []
                            delegate: Rectangle {
                                id: card
                                required property var modelData
                                required property int index
                                width: candidates.width >= 540 ? (candidates.width - (root.group.variants.length - 1) * candidates.spacing) / root.group.variants.length
                                    : candidates.width >= 370 ? (candidates.width - candidates.spacing) / 2 : candidates.width
                                height: contents.implicitHeight + 2 * Theme.panelPadding
                                color: Theme.surfaceColor(Theme.colors.background.surface)
                                radius: Theme.chromeInsetY
                                Column {
                                    id: contents
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.panelPadding
                                    spacing: Theme.chromeInsetY
                                    Label {
                                        width: parent.width
                                        height: Math.max(Theme.uiFontSize * 4, implicitHeight)
                                        text: card.modelData.strategy + (root.group && root.group.preferred === card.index
                                            ? " · " + qsTrId("hand_order.pattern.default") : "")
                                        color: Theme.colors.text.primary
                                        wrapMode: Text.Wrap
                                    }
                                    PatternCandidatePreview {
                                        objectName: "patternCandidatePreview_" + card.index
                                        width: Math.min(parent.width, 220)
                                        height: width
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        previewData: card.modelData.previewData
                                        sourceRuntime: root.previewRuntime
                                        second: root.previewSecond
                                    }
                                    AppTextArea {
                                        objectName: "patternCandidateSyntax_" + card.index
                                        width: parent.width
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.WrapAnywhere
                                        text: card.modelData.syntax
                                        font.family: Theme.codeFont.family
                                        font.pixelSize: Theme.uiFontSize
                                    }
                                    AppButton {
                                        objectName: "applyPatternCandidate_" + card.index
                                        text: card.modelData.applied ? qsTrId("hand_order.pattern.applied") : qsTrId("hand_order.pattern.apply")
                                        enabled: root.service && root.service.patternChoicesEnabled && !card.modelData.applied
                                        onClicked: root.choiceRequested(root.group.sourceIndex, card.index)
                                    }
                                    AppButton {
                                        objectName: "applySelectedPatterns_" + card.index
                                        visible: root.selectedKeys.length > 1
                                        text: qsTrId("hand_order.pattern.apply_selected").arg(root.selectedKeys.length)
                                        enabled: root.service && root.service.patternChoicesEnabled
                                            && root.similarGroups.filter(item => root.selectedKeys.indexOf(item.key) >= 0)
                                                .every(item => item.variants.some(value => value.choiceKey === card.modelData.choiceKey))
                                        onClicked: root.choicesRequested(root.selectedKeys, card.modelData.choiceKey)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: root.groups.length === 0
            text: qsTrId("hand_order.pattern.empty")
            color: Theme.colors.text.secondary
            wrapMode: Text.Wrap
        }
    }
}
