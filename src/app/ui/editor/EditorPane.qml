import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root

    required property var viewState
    required property var documentSession
    required property var commands
    required property var editorController
    required property var editorSync
    required property var analysisSession
    property var shortcuts: null
    property var handOrderService: null
    required property var preferences
    required property var preferencesModel
    required property var latency
    required property var previewSession
    required property var pages
    required property var net
    required property var netUpload

    readonly property Item sourceEditorItem: sourceEditor
    readonly property var inputContext: editorInput
    EditorInputContext {
        id: editorInput
        window: root.Window.window
        typingTarget: sourceEditor.inputItem
        normalizationShortcut: root.normalizationAvailable && root.shortcuts && !root.shortcuts.captureActive
                               && root.shortcuts.revision >= 0 ? root.shortcuts.sequence("editor.normalize_chart") : ""
    }
    readonly property Item difficultyFieldsItem: difficultyHeader
    readonly property bool sourceVisible: viewState.difficultyEditorActive
    readonly property bool canUndo: sourceVisible && sourceEditor.canUndo
    readonly property bool canRedo: sourceVisible && sourceEditor.canRedo
    readonly property bool canCut: sourceVisible && sourceEditor.canCut
    readonly property bool canCopy: sourceVisible && sourceEditor.canCopy
    readonly property bool canPaste: sourceVisible && sourceEditor.canPaste
    readonly property bool canTransform: sourceVisible && sourceEditor.canTransform
    readonly property bool normalizationAvailable: sourceVisible && sourceEditor.canNormalize
    readonly property string selectionBeatStatusText:
        sourceVisible ? sourceEditor.selectionBeatStatusText : ""
    readonly property string selectionBeatTooltipText:
        sourceVisible ? sourceEditor.selectionBeatTooltipText : ""
    readonly property real minimumWidth: 384
    property double pendingActivationSequence: 0
    property var pendingActivationCompletion: null
    property var pendingActivationCancellation: null

    signal openRequested()
    signal completionSettingsRequested(string group)
    readonly property Item cornerSourceItem: tabs.visible ? tabs : null
    readonly property color cornerBackingColor: tabs.visible
        ? "transparent"
        : (root.documentSession.hasDocument || root.viewState.netEditorActive
            ? Theme.surfaceColor(Theme.colors.background.surface)
            : Theme.surfaceColor(Theme.colors.background.panel))

    function undo() {
        if (sourceVisible)
            sourceEditor.undo()
    }

    function redo() {
        if (sourceVisible)
            sourceEditor.redo()
    }

    function cut() {
        if (sourceVisible)
            sourceEditor.cut()
    }

    function copy() {
        if (sourceVisible)
            sourceEditor.copy()
    }

    function paste() {
        if (sourceVisible)
            sourceEditor.paste()
    }

    // Keyboard tab-close must use the same three-way guard as the tab's x
    // button. Calling ViewState.closeEditor directly would discard a dirty
    // metadata or difficulty view without asking.
    function requestCloseActiveEditor() {
        if (viewState.activeEditorKey.length > 0)
            tabs.requestCloseTab(viewState.activeEditorKey)
    }

    function selectAll() {
        if (sourceVisible)
            sourceEditor.selectAll()
    }

    function openFindReplace() {
        if (sourceVisible)
            sourceEditor.openFindReplace()
    }

    function selectCurrentLine() {
        if (sourceVisible)
            sourceEditor.selectCurrentLine()
    }

    function applyChartTransform(opId) {
        return sourceVisible && sourceEditor.applyChartTransform(opId)
    }

    function canNormalizeChart() {
        return normalizationAvailable
    }

    function normalizationSelectionDescription() {
        return canNormalizeChart() ? sourceEditor.selectionDescription() : ""
    }

    function applyNormalization(options) {
        return canNormalizeChart() && sourceEditor.applyNormalization(options)
    }

    function revealSyntaxIssue(difficultyId, revision, line, column, endColumn) {
        if (revision !== root.documentSession.validationRevision
                || revision !== root.documentSession.documentRevision
                || root.documentSession.validationPending)
            return
        requestIssueNavigation(difficultyId, revision, line, column, endColumn, null, null)
    }

    function revealAnalysisRow(difficultyId, revision, line, column, endColumn, second, analysisSession, playCheck) {
        const cancel = () => analysisSession.cancelRowActivation(
            difficultyId, revision, line, column, endColumn, second)
        if (revision !== root.documentSession.documentRevision
                || (!playCheck && (revision !== root.documentSession.validationRevision || root.documentSession.validationPending))) {
            cancel()
            return
        }
        requestIssueNavigation(
            difficultyId, revision, line, column, endColumn,
            () => {
                if (analysisSession.completeRowActivation(
                        difficultyId, revision, line, column, endColumn, second)) {
                    if (playCheck) root.previewSession.positionSeconds = second
                    else root.editorSync.seekPreviewToEditorLocation(difficultyId, revision, line, column)
                }
            },
            cancel)
    }

    function revealPatternChoice(group, service) {
        const target = service.patternNavigationTarget(group)
        if (target.start === undefined || target.revision !== root.documentSession.documentRevision
                || target.generation !== root.documentSession.documentOpenGeneration)
            return
        requestIssueNavigation(target.difficultyId, target.revision, target.line, target.column, target.column,
            () => {
                if (target.generation === root.documentSession.documentOpenGeneration
                        && target.revision === root.documentSession.documentRevision)
                    root.editorSync.seekPreviewToEditorLocation(
                        target.difficultyId, target.revision, target.line, target.column)
            }, null, target)
    }

    function requestIssueNavigation(difficultyId, revision, line, column, endColumn,
                                    completion, cancellation, sourceRange = null) {
        if (difficultyId !== root.documentSession.currentDifficultyId)
            root.documentSession.selectDifficulty(difficultyId)
        root.viewState.openDifficultyEditor(difficultyId)
        Qt.callLater(() => {
            if (difficultyId !== root.documentSession.currentDifficultyId
                    || revision !== root.documentSession.documentRevision
                    || (sourceRange && sourceRange.generation !== root.documentSession.documentOpenGeneration)) {
                if (cancellation)
                    cancellation()
                return
            }
            if (line <= 0) {
                if (completion)
                    completion()
                return
            }
            const start = sourceRange ? sourceRange.start : root.documentSession.chartPosition(line, column)
            const end = sourceRange ? sourceRange.end : Math.max(start + 1, root.documentSession.chartPosition(
                line, Math.max(column, endColumn + 1)))
            const sequence = root.editorSync.requestNavigation(
                difficultyId, revision, start, end, true, true)
            if (sequence <= 0) {
                if (cancellation)
                    cancellation()
                return
            }
            root.pendingActivationSequence = sequence
            root.pendingActivationCompletion = completion
            root.pendingActivationCancellation = cancellation
        })
    }

    Connections {
        target: root.editorSync
        function onNavigationFinished(sequence, applied) {
            if (sequence !== root.pendingActivationSequence)
                return
            const completion = root.pendingActivationCompletion
            const cancellation = root.pendingActivationCancellation
            root.pendingActivationSequence = 0
            root.pendingActivationCompletion = null
            root.pendingActivationCancellation = null
            if (applied) {
                if (completion)
                    completion()
            } else if (cancellation) {
                cancellation()
            }
        }
    }

    // The active tab, difficulty header and source share the editor surface.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        y: tabs.y + tabs.height
        height: (root.sourceVisible ? sourceEditor.y : root.height) - y
        color: Theme.surfaceColor(Theme.colors.background.surface)
        visible: root.documentSession.hasDocument || root.viewState.netEditorActive
    }

    Flickable {
        id: songs
        objectName: "songTabStrip"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: (root.documentSession.songTabs?.length ?? 0) > 1
        height: visible ? songButtons.implicitHeight : 0
        contentWidth: songButtons.implicitWidth
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        function reveal() {
            const item = songRepeater.itemAt(root.documentSession.currentSongTab ?? -1)
            if (!item || width <= 0) return
            if (item.x < contentX) contentX = item.x
            else if (item.x + item.width > contentX + width) contentX = Math.max(0, item.x + item.width - width)
        }
        onContentWidthChanged: Qt.callLater(reveal)
        Connections {
            target: root.documentSession
            ignoreUnknownSignals: true
            function onSongTabsChanged() { Qt.callLater(songs.reveal) }
        }
        Row {
            id: songButtons
            Repeater {
                id: songRepeater
                model: root.documentSession.songTabs ?? []
                delegate: Row {
                    id: songTab
                    required property int index
                    required property var modelData
                    AppButton {
                        width: Math.min(implicitWidth, Math.max(120, songs.width * 0.6))
                        text: (songTab.modelData.dirty ? "● " : "") + songTab.modelData.title
                        Accessible.role: Accessible.PageTab
                        Accessible.selected: songTab.modelData.active
                        selected: songTab.modelData.active
                        ToolTip.visible: hovered && songTab.modelData.path.length > 0
                        ToolTip.text: songTab.modelData.path
                        onClicked: root.documentSession.activateSongTab(songTab.index)
                    }
                    IconButton {
                        iconSource: Qt.resolvedUrl("icons/close.svg")
                        tooltip: qsTrId("action.close") + " · " + songTab.modelData.title
                        onClicked: root.documentSession.requestCloseSongTab(songTab.index)
                    }
                }
            }
        }
    }

    EditorTabBar {
        id: tabs
        objectName: "workspaceTabBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: songs.bottom
        clip: true
        readonly property bool stripOpen: root.viewState.openEditorTabs.length > 0
        height: stripOpen ? implicitHeight : 0
        visible: height > 0.5
        viewState: root.viewState
        documentSession: root.documentSession
        commands: root.commands
        pages: root.pages

        Behavior on height {
            enabled: tabs.animationsReady
            NumberAnimation { duration: 170; easing.type: Easing.OutCubic }
        }
    }

    LatencyPage {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        visible: root.viewState.latencyEditorActive && root.pages.activePageId === "latency"
        latency: root.latency
        previewSession: root.previewSession
        pages: root.pages
    }

    NetDownloadPage {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        visible: root.viewState.activeEditorKey === root.viewState.netDownloadEditorKey
        net: root.net
    }

    NetUploadPage {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        visible: root.pages.netUploadEnabled && root.viewState.activeEditorKey === root.viewState.netUploadEditorKey
        uploadModel: root.netUpload
    }

    component DifficultyHeaderField: Item {
        id: fieldGroup

        required property string labelText
        required property string value
        required property real editorWidth
        property bool stacked: false
        property bool fillEditorWidth: false
        property bool commitOnEditingFinished: false
        signal committed(string value)

        implicitWidth: fieldLabel.implicitWidth + 8 + fieldEditor.editorWidth
        implicitHeight: stacked
            ? fieldLabel.implicitHeight + 8 + fieldEditor.implicitHeight
            : fieldEditor.implicitHeight

        Label {
            id: fieldLabel
            x: 0
            y: fieldGroup.stacked ? 0 : (fieldEditor.implicitHeight - height) / 2
            text: fieldGroup.labelText
            color: Theme.colors.text.primary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }

        AppTextField {
            id: fieldEditor
            backgroundColor: Theme.colors.background.panel
            outlined: true
            readonly property real editorWidth: fieldGroup.editorWidth

            x: fieldGroup.stacked ? 0 : fieldLabel.width + 8
            y: fieldGroup.stacked ? fieldLabel.height + 8 : 0
            width: fieldGroup.stacked ? fieldGroup.width
                : fieldGroup.fillEditorWidth
                    ? Math.max(0, fieldGroup.width - x)
                    : editorWidth
            text: fieldGroup.value
            onTextEdited: if (!fieldGroup.commitOnEditingFinished) fieldGroup.committed(text)
            onEditingFinished: if (fieldGroup.commitOnEditingFinished) fieldGroup.committed(text)
        }
    }

    Item {
        id: difficultyHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        height: headerContent.height + 12 - Theme.workspaceHeaderContentOffsetY
        visible: root.viewState.difficultyEditorActive

        Item {
            id: headerContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Theme.panelPadding
            anchors.rightMargin: Theme.panelPadding
            readonly property real gap: 8
            readonly property real metadataButtonWidth: metadataButton.implicitWidth + addPreview.implicitWidth + gap
            readonly property real wideWidth:
                levelField.implicitWidth + designerField.implicitWidth
                + offsetField.implicitWidth + metadataButtonWidth + 3 * gap
            readonly property real firstRowWidth:
                levelField.implicitWidth + offsetField.implicitWidth + gap
            readonly property real twoRowWidth:
                Math.max(firstRowWidth,
                    designerField.implicitWidth + metadataButtonWidth + gap)
            readonly property bool wide: width >= wideWidth
            readonly property bool twoRows: !wide && width >= twoRowWidth

            height: designerField.y + designerField.height

            DifficultyHeaderField {
                id: levelField
                x: 0
                y: 0
                stacked: !headerContent.wide && !headerContent.twoRows
                    && headerContent.width < implicitWidth
                width: stacked ? headerContent.width : implicitWidth
                labelText: qsTrId("qml.level")
                value: root.documentSession.currentDifficultyLevel
                editorWidth: 48
                onCommitted: value => root.documentSession.currentDifficultyLevel = value
            }

            DifficultyHeaderField {
                id: designerField
                x: headerContent.wide
                    ? offsetField.x + offsetField.width + headerContent.gap : 0
                y: headerContent.wide ? 0
                    : headerContent.twoRows
                        ? levelField.height + headerContent.gap
                        : offsetField.y + offsetField.height + headerContent.gap
                stacked: !headerContent.wide && !headerContent.twoRows
                    && headerContent.width < implicitWidth
                // Leave roughly a third of the row for chart information.
                width: Math.max(0, Math.min(
                    Math.floor((headerContent.width - x) * 2 / 3),
                    headerContent.width - x - headerContent.metadataButtonWidth - headerContent.gap))
                labelText: qsTrId("net.designer")
                value: root.documentSession.currentDifficultyDesigner
                editorWidth: 100
                fillEditorWidth: true
                onCommitted: value => root.documentSession.currentDifficultyDesigner = value
            }

            IconButton {
                id: addPreview
                objectName: "addComparisonPreviewButton"
                anchors.right: metadataButton.left
                anchors.rightMargin: headerContent.gap
                anchors.verticalCenter: metadataButton.verticalCenter
                iconSource: Qt.resolvedUrl("icons/add.svg")
                tooltip: qsTrId("preview.comparison.add")
                Accessible.name: tooltip
                enabled: root.sourceVisible && !!root.previewSession?.comparisons
                onClicked: root.previewSession.comparisons.addCurrent()
            }
            IconButton {
                id: metadataButton
                objectName: "chartInformationButton"
                anchors.right: parent.right
                y: designerField.y + Math.max(0, (designerField.height - height) / 2)
                iconSource: Qt.resolvedUrl("icons/metadata.svg")
                label: qsTrId("dialog.unsaved_field_changes.field.metadata")
                tooltip: qsTrId("dialog.unsaved_field_changes.field.metadata")
                enabled: root.sourceVisible
                onClicked: root.viewState.openMetadataEditor()
            }

            DifficultyHeaderField {
                id: offsetField
                x: headerContent.wide || headerContent.twoRows
                    ? levelField.x + levelField.width + headerContent.gap : 0
                y: headerContent.wide || headerContent.twoRows
                    ? 0 : levelField.y + levelField.height + headerContent.gap
                stacked: !headerContent.wide && !headerContent.twoRows
                    && headerContent.width < implicitWidth
                width: stacked ? headerContent.width : implicitWidth
                labelText: qsTrId("qml.offset")
                value: root.documentSession.currentDifficultyOffset
                editorWidth: 64
                commitOnEditingFinished: true
                onCommitted: value => root.documentSession.currentDifficultyOffset = value
            }

        }
    }

    SourceEditor {
        id: sourceEditor
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: difficultyHeader.bottom
        anchors.bottom: parent.bottom
        visible: root.sourceVisible
        navigationVisible: root.visible && root.viewState.difficultyEditorActive
        viewState: root.viewState
        documentSession: root.documentSession
        editorController: root.editorController
        syncController: root.editorSync
        analysisSession: root.analysisSession
        chartStructure: root.previewSession?.structure ?? null
        preferences: root.preferences
        preferencesModel: root.preferencesModel
        shortcuts: root.shortcuts
        handOrderService: root.handOrderService
        onPlaybackToggleRequested: root.previewSession.togglePlayback()
        onNormalizeChartRequested: root.pages.openNormalizeWholeChart()
        onCompletionSettingsRequested: function(group) { root.completionSettingsRequested(group) }
    }

    Connections {
        target: root.viewState
        function onMetadataDialogRequested() { metadataDialog.open() }
    }

    AppDialog {
        id: metadataDialog
        objectName: "chartMetadataDialog"
        title: qsTrId("workspace.metadata_tool")
        preferredWidth: 960
        preferredHeight: 720
        property Item previousFocusItem: null
        onAboutToShow: previousFocusItem = root.Window.window?.activeFocusItem ?? null
        onClosed: {
            if (previousFocusItem?.visible) previousFocusItem.forceActiveFocus()
            previousFocusItem = null
        }
        onAboutToHide: {
            root.forceActiveFocus()
            if (extraFieldsEdit.userEdited) {
                root.documentSession.metadataExtraText = extraFieldsEdit.text
                extraFieldsEdit.userEdited = false
            }
        }
        footer: DialogFooter {
            cancelText: qsTrId("action.close")
            onRejected: metadataDialog.close()
        }
        body: Item {
            implicitHeight: metadataColumn.y + metadataColumn.implicitHeight + Theme.dialogPadding

            ColumnLayout {
                id: metadataColumn
                x: Theme.dialogPadding
                y: Theme.dialogPadding
                width: Math.max(0, parent.width - 2 * Theme.dialogPadding)
                spacing: Theme.settingsRowSpacing
                readonly property int actionWidth: Math.ceil(Math.max(
                    actionMetrics.advanceWidth(qsTrId("metadata.import")),
                    actionMetrics.advanceWidth(qsTrId("metadata.load_audio_info")),
                    actionMetrics.advanceWidth(qsTrId("metadata.remove")),
                    actionMetrics.advanceWidth(qsTrId("document.manage_designer_names"))) + 24)

                FontMetrics {
                    id: actionMetrics
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.uiFontSize
                }

                Label {
                    visible: root.documentSession.metadataNeedsAttention
                    Layout.fillWidth: true
                    text: root.documentSession.metadataAttentionText
                    color: Theme.colors.syntax.warning
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.secondaryFontSize
                    wrapMode: Text.WordWrap
                }

                // Metadata and jacket actions on the left, the difficulty card on
                // the right. Cell widths come from the page width so content never
                // pushes a column out; narrow pages stack the two halves.
                GridLayout {
                    id: metadataHeader
                    readonly property bool twoColumns: metadataColumn.width >= 480
                    readonly property real cellWidth: twoColumns
                        ? (metadataColumn.width - columnSpacing) / 2 : metadataColumn.width

                    Layout.fillWidth: true
                    columns: twoColumns ? 2 : 1
                    columnSpacing: Theme.dialogPadding
                    rowSpacing: Theme.settingsRowSpacing

                    ColumnLayout {
                        id: fieldsColumn
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: metadataHeader.cellWidth
                        Layout.maximumWidth: metadataHeader.cellWidth
                        spacing: Theme.settingsRowSpacing

                        MetadataField {
                            label: qsTrId("net.title")
                            value: root.documentSession.metadataTitle
                            onCommitted: value => root.documentSession.metadataTitle = value
                        }
                        MetadataField {
                            label: qsTrId("metadata.field.artist")
                            value: root.documentSession.metadataArtist
                            onCommitted: value => root.documentSession.metadataArtist = value
                        }
                        MetadataField {
                            label: qsTrId("net.designer")
                            value: root.documentSession.metadataDesigner
                            onCommitted: value => root.documentSession.metadataDesigner = value
                            trailing: AppButton {
                                Layout.preferredWidth: metadataColumn.actionWidth
                                text: qsTrId("document.manage_designer_names")
                                onClicked: designerSlotsDialog.open()
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.settingsRowSpacing

                            MetadataField {
                                Layout.preferredWidth: 1
                                label: qsTrId("metadata.field.first")
                                value: root.documentSession.metadataFirst
                                onCommitted: value => root.documentSession.metadataFirst = value
                            }
                            MetadataField {
                                Layout.preferredWidth: 1
                                label: qsTrId("media_tools.beats")
                                value: root.documentSession.metadataClockCount
                                onCommitted: value => root.documentSession.metadataClockCount = value
                            }
                        }
                        RowLayout {
                            id: coverRow
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.panelPadding
                            spacing: Theme.panelPadding

                            Label {
                                id: coverHeading
                                Layout.fillWidth: true
                                Layout.minimumWidth: implicitWidth
                                text: qsTrId("metadata.field.cover")
                                color: Theme.colors.text.secondary
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.uiFontSize
                            }
                            GridLayout {
                                id: coverActions
                                readonly property real availableWidth: Math.max(0,
                                    metadataHeader.cellWidth - coverHeading.implicitWidth - coverRow.spacing)
                                readonly property real buttonWidth: Math.min(availableWidth,
                                    Math.max(metadataColumn.actionWidth, importCoverButton.implicitWidth,
                                        loadAudioInfoButton.implicitWidth))
                                Layout.alignment: Qt.AlignRight
                                columns: 2 * buttonWidth + columnSpacing <= availableWidth ? 2 : 1
                                columnSpacing: Theme.panelPadding
                                rowSpacing: Theme.panelPadding

                                AppButton {
                                    id: importCoverButton
                                    Layout.preferredWidth: coverActions.buttonWidth
                                    text: qsTrId("metadata.import")
                                    onClicked: root.documentSession.importChartBackgroundImage()
                                }
                                AppButton {
                                    id: loadAudioInfoButton
                                    Layout.preferredWidth: coverActions.buttonWidth
                                    text: qsTrId("metadata.load_audio_info")
                                    onClicked: root.documentSession.loadAudioInfoFromFile()
                                }
                            }
                        }
                    }

                    MetadataDifficultyCard {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: metadataHeader.cellWidth
                        Layout.maximumWidth: metadataHeader.cellWidth
                        documentSession: root.documentSession
                        preferredCardHeight: metadataHeader.twoColumns
                            ? Math.max(160, fieldsColumn.implicitHeight
                                - Theme.controlMinHeight - spacing) : 260
                    }
                }

                SettingsSection {
                    Layout.fillWidth: true
                    title: qsTrId("metadata.field.background_video")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.panelPadding

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.settingsLabelSpacing

                            Text {
                                Layout.fillWidth: true
                                text: root.documentSession.metadataHasVideo
                                    ? root.documentSession.metadataResolvedVideoPath.split(/[/\\]/).pop()
                                    : qsTrId("metadata.no_pv")
                                color: Theme.colors.text.primary
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.uiFontSize
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: root.documentSession.metadataHasVideo
                                text: root.documentSession.metadataResolvedVideoPath
                                color: Theme.colors.text.secondary
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.secondaryFontSize
                                elide: Text.ElideMiddle
                            }
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: metadataColumn.actionWidth
                            text: qsTrId("metadata.import")
                            onClicked: root.documentSession.importChartBackgroundVideo()
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: metadataColumn.actionWidth
                            text: qsTrId("metadata.remove")
                            enabled: root.documentSession.metadataHasVideo
                            onClicked: root.documentSession.removeChartPv()
                        }
                    }
                }

                AdvancedMetadataForm {
                    Layout.fillWidth: true
                    visible: root.preferences.advancedMetadataEnabled
                    documentSession: root.documentSession
                }

                SettingsSection {
                    Layout.fillWidth: true
                    title: qsTrId("qml.other_fields")

                    AppTextArea {
                        id: extraFieldsEdit
                        backgroundColor: Theme.colors.background.panel
                        outlined: true
                        property bool userEdited: false

                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        text: root.documentSession.metadataExtraText
                        placeholderText: qsTrId("qml.one_field_equals_value_per_line")
                        onTextChanged: {
                            if (activeFocus && text !== root.documentSession.metadataExtraText)
                                extraFieldsEdit.userEdited = true
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus && extraFieldsEdit.userEdited) {
                                root.documentSession.metadataExtraText = text
                                // The model retains rejected input and owns its dirty state.
                                extraFieldsEdit.userEdited = false
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.documentSession.metadataExtraError.length > 0
                        text: root.documentSession.metadataExtraError
                        color: Theme.colors.syntax.error
                        font.family: Theme.uiFont
                        font.pixelSize: Theme.secondaryFontSize
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }


    Connections {
        target: root.documentSession
        function onEditingFinishedRequested() {
            editorInput.commitPendingEdits(root)
        }
        function onMetadataChanged() {
            if (extraFieldsEdit.activeFocus)
                return
            extraFieldsEdit.userEdited = false
            extraFieldsEdit.text = root.documentSession.metadataExtraText
        }
    }

    Label {
        anchors.centerIn: parent
        visible: root.documentSession.hasDocument && !root.viewState.hasActiveEditor
        text: root.documentSession.difficulties.length > 0
              ? qsTrId("qml.open_metadata_or_a_difficulty_from_the_sidebar")
              : qsTrId("qml.open_chart_info_or_add_a_difficulty")
        color: Theme.colors.text.secondary
        font.family: Theme.uiFont
        font.pixelSize: Theme.uiFontSize
    }

    WelcomePage {
        anchors.fill: parent
        visible: !root.documentSession.hasDocument && !root.viewState.netEditorActive
        z: 10
        documentSession: root.documentSession
        onNewRequested: root.commands.newDocument()
        onOpenRequested: root.openRequested()
        onOpenRecentRequested: path => root.commands.openRecentDocument(path)
    }

    DesignerSlotsDialog {
        id: designerSlotsDialog
        documentSession: root.documentSession
        commands: root.commands
    }

    component MetadataField: ColumnLayout {
        id: field
        required property string label
        required property string value
        // Optional controls placed after the text field on the same row.
        property alias trailing: trailingSlot.data
        signal committed(string value)
        Layout.fillWidth: true
        spacing: Theme.settingsLabelSpacing

        Label {
            text: field.label
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.panelPadding

            AppTextField {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                backgroundColor: Theme.colors.background.panel
                outlined: true
                text: field.value
                onEditingFinished: field.committed(text)
            }
            RowLayout {
                id: trailingSlot
                visible: children.length > 0
                spacing: Theme.panelPadding
            }
        }
    }
}
