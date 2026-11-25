import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    required property var library
    required property AppMenu batchMenu
    signal reviewRequested()
    signal editSourceRequested(string path)
    readonly property Item cornerSourceItem: actions
    property bool batchMode: false
    property bool mobileDetail: false
    readonly property bool ready: root.library.rootPath.length > 0
    readonly property bool populated: root.ready && root.library.totalCount > 0
    readonly property bool narrow: width < 755
    spacing: Theme.settingsRowSpacing

    function stage() { return form.stage() }
    function resume() {
        if (form.path && root.library.hasSong(form.path)) form.load(form.path, true)
        else reconcile()
    }
    function reconcile() {
        if (form.path && root.library.rowForPath(form.path) >= 0) {
            if (!form.dirty && !root.library.busy) form.load(form.path, true)
        } else {
            form.load(root.library.pathAt(0), false)
        }
        const index = root.library.rowForPath(form.path)
        songs.currentIndex = index
    }
    function selectSong(path, openDetail) {
        if (!form.load(path, false)) return
        songs.currentIndex = root.library.rowForPath(path)
        if (openDetail) mobileDetail = true
    }
    Connections {
        target: root.library
        function onModelReset() { Qt.callLater(root.reconcile) }
    }
    Component.onCompleted: Qt.callLater(reconcile)

    Flow {
        id: actions
        Layout.minimumWidth: 0
        Layout.fillWidth: true
        spacing: 6
        AppButton { visible: !root.ready; text: qsTrId("library.create"); emphasized: true; enabled: !root.library.busy; onClicked: root.library.chooseNewLibrary() }
        AppButton { visible: !root.ready; text: qsTrId("library.choose_root"); enabled: !root.library.busy; onClicked: root.library.chooseRoot() }
        AppDropDownButton {
            id: libraryMenuButton
            visible: root.ready
            text: qsTrId("library.title")
            enabled: !root.library.busy
            expanded: libraryMenu.active
            onClicked: libraryMenu.active ? libraryMenu.close() : libraryMenu.openAt(libraryMenuButton)
            AppMenu {
                id: libraryMenu
                AppMenuItem { text: qsTrId("library.create"); onTriggered: if (root.stage()) root.library.chooseNewLibrary() }
                AppMenuItem { text: qsTrId("library.choose_root"); onTriggered: if (root.stage()) root.library.chooseRoot() }
                AppMenuItem { text: qsTrId("library.export"); enabled: root.populated; onTriggered: if (root.stage()) root.library.chooseExport() }
                AppMenuItem { text: qsTrId("library.consolidate"); enabled: root.populated; onTriggered: if (root.stage()) dedupDialog.open() }
                AppMenuItem { text: qsTrId("library.refresh"); onTriggered: if (root.stage()) root.library.refresh() }
                AppMenuItem {
                    text: qsTrId("library.delete_pack")
                    enabled: root.library.packs.length > 0
                    onTriggered: if (root.stage()) { deletionDialog.wholeLibrary = false; deletionDialog.open() }
                }
                AppMenuItem {
                    text: qsTrId("library.delete_library")
                    onTriggered: if (root.stage()) { deletionDialog.wholeLibrary = true; deletionDialog.open() }
                }
            }
        }
        AppButton {
            visible: root.ready
            text: qsTrId("library.import_songs")
            emphasized: !root.populated
            enabled: !root.library.busy
            onClicked: if (root.stage()) importDialog.open()
        }
        AppButton {
            objectName: "enterLibraryBatchMode"
            visible: root.populated && !root.batchMode
            text: qsTrId("library.batch")
            onClicked: {
                if (!root.stage()) return
                root.mobileDetail = false
                root.batchMode = true
            }
        }
        AppDropDownButton {
            id: batchActions
            objectName: "libraryBatchActionsButton"
            visible: root.batchMode
            text: qsTrId("library.selected_actions").arg(root.library.selectedCount)
            enabled: root.library.selectedCount > 0 && !root.library.busy
            expanded: root.batchMenu.active
            onClicked: expanded ? root.batchMenu.close() : root.batchMenu.openAt(batchActions)
        }
        AppButton {
            visible: root.batchMode && root.library.proposals.length > 0
            text: qsTrId("library.review").arg(root.library.proposals.length)
            onClicked: root.reviewRequested()
        }
        AppButton {
            visible: root.batchMode && root.library.canUndoBatch
            text: qsTrId("library.undo_batch")
            onClicked: root.library.undoBatch()
        }
        AppButton { visible: root.library.busy; text: qsTrId("library.cancel"); onClicked: root.library.cancel() }
    }
    Flow {
        Layout.minimumWidth: 0
        Layout.fillWidth: true
        visible: root.batchMode && !root.narrow && root.library.selectedCount > 0
        enabled: !root.library.busy
        spacing: 6
        Repeater {
            model: root.batchMenu.count
            delegate: AppButton {
                required property int index
                objectName: "libraryBatchAction_" + index
                action: root.batchMenu.itemAt(index)?.action ?? null
            }
        }
    }
    Label {
        Layout.minimumWidth: 0
        Layout.fillWidth: true
        text: root.ready ? root.library.rootPath : qsTrId("library.empty_start")
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        color: Theme.colors.text.secondary
    }
    Label {
        visible: root.ready && !root.populated && !root.library.busy
        text: qsTrId("library.empty_song_manager"); color: Theme.colors.text.secondary
    }
    RowLayout {
        Layout.minimumWidth: 0
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: Theme.dialogPadding
        ColumnLayout {
            visible: root.populated && (!root.narrow || !root.mobileDetail || !form.path)
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.fillWidth: root.narrow
            Layout.preferredWidth: root.narrow ? root.width : Math.max(246, Math.min(370, root.width * 0.34))
            RowLayout {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                AppComboBox {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    Layout.preferredWidth: 3
                    Accessible.name: qsTrId("library.pack")
                    model: [qsTrId("library.all_packs")].concat(Array.from(root.library.packs))
                    currentIndex: root.library.pack ? model.indexOf(root.library.pack) : 0
                    onActivated: i => { if (root.stage()) root.library.pack = i ? model[i] : "" }
                }
                AppComboBox {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    Layout.preferredWidth: 2
                    Accessible.name: qsTrId("metadata.advanced.genre")
                    model: [qsTrId("library.all_genres"), qsTrId("library.uncategorized")].concat(Array.from(root.library.genres))
                    currentIndex: root.library.genre === root.library.uncategorized ? 1
                                  : root.library.genre ? 2 + root.library.genres.indexOf(root.library.genre) : 0
                    onActivated: i => root.library.genre = i > 1 ? model[i] : (i === 1 ? root.library.uncategorized : "")
                }
            }
            AppComboBox {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Accessible.name: qsTrId("library.version_category")
                model: [qsTrId("library.all_versions"), qsTrId("library.uncategorized")].concat(Array.from(root.library.versions))
                currentIndex: root.library.versionCategory === root.library.uncategorized ? 1
                              : root.library.versionCategory ? 2 + root.library.versions.indexOf(root.library.versionCategory) : 0
                onActivated: i => root.library.versionCategory = i > 1 ? model[i] : (i === 1 ? root.library.uncategorized : "")
            }
            RowLayout {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                AppTextField {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    Accessible.name: qsTrId("library.search")
                    placeholderText: qsTrId("library.search")
                    text: root.library.query
                    onTextEdited: root.library.query = text
                    onAccepted: songs.openCurrent()
                }
                IconButton {
                    id: difficultyButton
                    objectName: "chartLibraryDifficultyFilter"
                    active: difficultyMenu.active || root.library.difficultyMask !== 0
                    iconSource: Qt.resolvedUrl("icons/filter.svg")
                    filledIconSource: Qt.resolvedUrl("icons/filter-fill.svg")
                    tooltip: qsTrId("action.filter")
                    onClicked: difficultyMenu.active ? difficultyMenu.close() : difficultyMenu.openAt(difficultyButton)
                    AppStickyPopup {
                        id: difficultyMenu
                        minimumWidth: 180
                        contentItem: Column {
                            id: difficultyOptions
                            objectName: "chartLibraryDifficultyOptions"
                            Repeater {
                                model: 7
                                delegate: AppMenuItem {
                                    required property int index
                                    objectName: "chartLibraryDifficultyOption"
                                    width: difficultyOptions.width
                                    difficultyId: index + 1
                                    text: qsTrId("library.difficulty." + difficultyId)
                                    checkable: true
                                    checked: !!(root.library.difficultyMask & (1 << difficultyId))
                                    onToggled: root.library.difficultyMask = checked
                                        ? root.library.difficultyMask | (1 << difficultyId)
                                        : root.library.difficultyMask & ~(1 << difficultyId)
                                }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                AppComboBox {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    Accessible.name: qsTrId("library.sort")
                    model: [qsTrId("library.sort_title"), qsTrId("library.sort_artist"), "BPM",
                            qsTrId("metadata.advanced.version"), qsTrId("library.sort_genre"),
                            qsTrId("library.sort_constant"), qsTrId("library.sort_designer")]
                    currentIndex: root.library.sortColumn
                    onActivated: index => root.library.sortColumn = index
                }
                IconButton {
                    iconSource: Qt.resolvedUrl(root.library.sortDescending ? "icons/arrow-down.svg" : "icons/arrow-up.svg")
                    tooltip: root.library.sortDescending ? qsTrId("library.sort_descending") : qsTrId("library.sort_ascending")
                    onClicked: root.library.sortDescending = !root.library.sortDescending
                }
            }
            Flow {
                visible: root.batchMode
                Layout.minimumWidth: 0
                Layout.fillWidth: true; spacing: 4
                AppCheckBox { text: qsTrId("library.duplicates"); checked: root.library.duplicatesOnly; onToggled: root.library.duplicatesOnly = checked }
                AppButton { text: qsTrId("library.select_visible"); onClicked: root.library.selectVisible(true) }
                AppButton { text: qsTrId("library.clear_visible"); onClicked: root.library.selectVisible(false) }
                AppButton { text: qsTrId("library.exit_selection"); onClicked: root.batchMode = false }
            }
            Label { visible: !songs.count && !root.library.busy; text: qsTrId("library.dock_empty"); color: Theme.colors.text.secondary }
            ListView {
                id: songs
                objectName: "managedLibrarySongList"
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true; spacing: 3
                function openCurrent() {
                    if (!root.batchMode && count && form.stage())
                        root.library.openInEditor(root.library.pathAt(Math.max(0, currentIndex)))
                }
                function step(direction) {
                    if (!count) return
                    const next = Math.max(0, Math.min(count - 1, currentIndex + direction))
                    root.selectSong(root.library.pathAt(next), false)
                    positionViewAtIndex(currentIndex, ListView.Contain)
                    currentItem?.forceActiveFocus()
                }
                model: root.library
                ScrollBar.vertical: AppScrollBar {}
                delegate: ChromeRow {
                    id: songRow
                    required property string chartPath
                    required property string chartTitle
                    required property string songId
                    required property string chartKind
                    required property url jacketUrl
                    required property var difficultyBadges
                    required property var model
                    width: Math.max(0, songs.width - 8)
                    height: Math.max(60, body.implicitHeight + 8)
                    highlightFillsBounds: true
                    selected: root.batchMode ? model.selected : form.path === chartPath
                    Accessible.name: [chartTitle, chartKind, songId].filter(Boolean).join(" · ")
                    onClicked: {
                        if (root.batchMode) {
                            root.library.toggleBatchRow(chartPath, root.library.shiftSelectionHeld())
                            return
                        }
                        if (root.library.additionalOpenModifierHeld()) {
                            openDetails.stop()
                            if (form.stage()) root.library.openFromList(chartPath)
                            return
                        }
                        root.selectSong(chartPath, false)
                        if (root.narrow) openDetails.restart()
                    }
                    Timer { id: openDetails; interval: Qt.styleHints.mouseDoubleClickInterval; onTriggered: root.mobileDetail = true }
                    onDoubleClicked: {
                        openDetails.stop()
                        if (!root.batchMode && !root.library.additionalOpenModifierHeld() && form.stage()) root.library.openInEditor(chartPath)
                    }
                    Keys.onReturnPressed: songs.openCurrent()
                    Keys.onEnterPressed: songs.openCurrent()
                    Keys.onDownPressed: songs.step(1)
                    Keys.onUpPressed: songs.step(-1)
                    contentItem: RowLayout {
                        id: body
                        spacing: 8
                        AppCheckBox {
                            visible: root.batchMode
                            Accessible.name: qsTrId("library.select_chart").arg(songRow.chartTitle)
                            checked: songRow.model.selected
                            enabled: !root.library.busy
                            onClicked: {
                                root.library.toggleBatchRow(songRow.chartPath, root.library.shiftSelectionHeld())
                                checked = Qt.binding(() => songRow.model.selected)
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 48; Layout.preferredHeight: 48
                            color: Theme.colors.background.elevated
                            Image { id: cover; anchors.fill: parent; source: songRow.jacketUrl; sourceSize: Qt.size(96,96); asynchronous: true; fillMode: Image.PreserveAspectCrop; clip: true }
                            Label { anchors.centerIn: parent; visible: cover.status !== Image.Ready; text: "♪"; color: Theme.colors.text.secondary }
                        }
                        ColumnLayout {
                            Layout.minimumWidth: 0
                            Layout.fillWidth: true; spacing: 1
                            Label {
                                Layout.minimumWidth: 0
                                Layout.fillWidth: true
                                text: [songRow.songId ? songRow.songId.padStart(6, "0") : "", songRow.chartKind === "UTAGE" ? qsTrId("library.utage") : songRow.chartKind].filter(Boolean).join("  ·  ")
                                font.pixelSize: Theme.secondaryFontSize; color: Theme.colors.text.secondary
                            }
                            Label {
                                Layout.minimumWidth: 0
                                Layout.fillWidth: true
                                text: songRow.chartTitle || qsTrId("library.untitled")
                                textFormat: Text.PlainText; elide: Text.ElideRight
                                color: Theme.colors.text.primary; font.bold: songRow.selected
                            }
                            Flow {
                                Layout.minimumWidth: 0
                                Layout.fillWidth: true
                                spacing: 5
                                Repeater {
                                    model: songRow.difficultyBadges
                                    delegate: Rectangle {
                                        required property var modelData
                                        implicitWidth: badge.implicitWidth + 10
                                        implicitHeight: badge.implicitHeight + 2
                                        radius: height / 2
                                        readonly property color badgeColor: Theme.difficultyColor(modelData.slot)
                                        color: Qt.rgba(badgeColor.r, badgeColor.g, badgeColor.b, 0.24)
                                        Label {
                                            id: badge
                                            anchors.centerIn: parent
                                            text: modelData.name + " " + (modelData.label || "—")
                                            color: Theme.colors.text.primary
                                            font.pixelSize: Theme.secondaryFontSize
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle { visible: root.populated && !root.narrow; Layout.fillHeight: true; implicitWidth: 1; color: Theme.separatorColor }
        LibrarySongForm {
            id: form
            library: root.library
            Layout.minimumWidth: 0
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.populated && !!path && (!root.narrow || root.mobileDetail)
            showBack: root.narrow
            onBackRequested: if (stage()) root.mobileDetail = false
            onEditSourceRequested: path => root.editSourceRequested(path)
        }
        Item { visible: !form.visible && !root.narrow; Layout.fillWidth: true }
    }
    AppDialog {
        id: dedupDialog
        implicitHeight: preferredHeight
        title: qsTrId("library.consolidate")
        body: Label { text: qsTrId("library.consolidate_hint"); color: Theme.colors.text.primary; wrapMode: Text.Wrap }
        footer: DialogFooter {
            cancelText: qsTrId("action.cancel"); acceptText: qsTrId("library.consolidate")
            onRejected: dedupDialog.close()
            onAccepted: { dedupDialog.close(); root.library.consolidateSongs() }
        }
    }
    AppDialog {
        id: importDialog
        readonly property var paths: root.library.importPaths || []
        title: qsTrId("library.import_songs")
        preferredHeight: implicitHeight
        onClosed: root.library.clearImportSources()
        body: ColumnLayout {
            spacing: Theme.settingsRowSpacing
            Flow {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.panelPadding
                AppButton {
                    text: qsTrId("library.choose_folders")
                    enabled: !root.library.pickingImportSources
                    onClicked: root.library.pickImportSources(false)
                }
                AppButton {
                    text: qsTrId("library.choose_zip")
                    enabled: !root.library.pickingImportSources
                    onClicked: root.library.pickImportSources(true)
                }
                AppButton {
                    visible: importDialog.paths.length > 0
                    text: qsTrId("library.clear_import_sources")
                    enabled: !root.library.pickingImportSources
                    onClicked: root.library.clearImportSources()
                }
            }
            Label {
                visible: importDialog.paths.length > 0
                Layout.fillWidth: true
                text: qsTrId("library.import_source_count").arg(importDialog.paths.length)
                color: Theme.colors.text.secondary
            }
            Label {
                visible: importDialog.paths.length > 0
                Layout.fillWidth: true
                text: Array.from(importDialog.paths).join("\n")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: Theme.colors.text.primary
            }
        }
        footer: DialogFooter {
            acceptText: qsTrId("library.import_songs")
            cancelText: qsTrId("action.cancel")
            acceptEnabled: importDialog.paths.length > 0 && !root.library.pickingImportSources && !root.library.busy
            onRejected: importDialog.close()
            onAccepted: {
                root.library.startBatchImport()
                importDialog.close()
            }
        }
    }
    AppDialog {
        id: deletionDialog
        property bool wholeLibrary: false
        property bool failed: false
        readonly property string packName: wholeLibrary ? "" : packToDelete.currentText
        readonly property string expectedName: wholeLibrary ? (root.library.collectionName || "") : packName
        title: wholeLibrary ? qsTrId("library.delete_library") : qsTrId("library.delete_pack")
        preferredHeight: implicitHeight
        onAboutToShow: {
            packToDelete.currentIndex = Math.max(0, root.library.packs.indexOf(root.library.pack))
            typedName.clear()
            failed = false
        }
        onOpened: typedName.forceActiveFocus()
        body: ColumnLayout {
            spacing: Theme.settingsRowSpacing
            AppComboBox {
                id: packToDelete
                visible: !deletionDialog.wholeLibrary
                Layout.fillWidth: true
                model: root.library.packs
                Accessible.name: qsTrId("library.pack")
                onActivated: typedName.clear()
            }
            Label {
                Layout.fillWidth: true
                text: qsTrId("library.delete_collection_prompt").arg(deletionDialog.expectedName)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.colors.text.primary
            }
            AppTextField {
                id: typedName
                Layout.fillWidth: true
                placeholderText: deletionDialog.expectedName
                Accessible.name: qsTrId("library.confirm_collection_name")
                onTextChanged: deletionDialog.failed = false
                onAccepted: if (deletionFooter.acceptEnabled) deletionDialog.removeCollection()
            }
            Label {
                visible: deletionDialog.failed
                Layout.fillWidth: true
                text: root.library.status
                wrapMode: Text.Wrap
                color: Theme.colors.text.secondary
            }
        }
        function removeCollection() {
            if (root.library.deleteCollection(packName, typedName.text)) close()
            else failed = true
        }
        footer: DialogFooter {
            id: deletionFooter
            acceptText: deletionDialog.title
            cancelText: qsTrId("action.cancel")
            acceptEnabled: deletionDialog.expectedName.length > 0
                           && typedName.text === deletionDialog.expectedName && !root.library.busy
            choices: [
                { id: "reject", label: cancelText },
                { id: "accept", label: acceptText, role: "destructive", enabled: acceptEnabled }
            ]
            onRejected: deletionDialog.close()
            onAccepted: deletionDialog.removeCollection()
        }
    }
}
