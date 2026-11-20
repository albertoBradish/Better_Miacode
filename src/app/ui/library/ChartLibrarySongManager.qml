import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

ColumnLayout {
    id: root
    required property var library
    signal editSourceRequested(string path)
    readonly property Item cornerSourceItem: actions
    property bool batchMode: false
    property bool mobileDetail: false
    property string awaitingPath: ""
    function categories(key) {
        if (!stage()) return
        catalog.key = key
        catalog.open()
    }
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
        if (awaitingPath && root.library.hasSong(awaitingPath)) {
            selectSong(awaitingPath, true)
            awaitingPath = ""
        } else if (form.path && root.library.hasSong(form.path)
                   && (!root.library.pack || form.song.pack === root.library.pack)) {
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
                AppMenuItem { visible: root.narrow; height: visible ? implicitHeight : 0; text: qsTrId("library.import_songs"); onTriggered: if (root.stage()) { importDialog.manual = true; importDialog.open() } }
                AppMenuItem { visible: root.narrow; height: visible ? implicitHeight : 0; text: qsTrId("library.new_song"); onTriggered: if (root.stage()) createDialog.open() }
                AppMenuItem { visible: root.narrow; height: visible ? implicitHeight : 0; text: qsTrId("library.manage_genres"); onTriggered: root.categories("genre") }
                AppMenuItem { visible: root.narrow; height: visible ? implicitHeight : 0; text: qsTrId("library.manage_versions"); onTriggered: root.categories("miacode_version_category") }
                AppMenuItem { visible: root.narrow; height: visible ? implicitHeight : 0; text: qsTrId("library.batch"); onTriggered: { root.batchMode = !root.batchMode; root.mobileDetail = false } }
                AppMenuItem { text: qsTrId("library.refresh"); onTriggered: if (root.stage()) root.library.refresh() }
            }
        }
        AppButton { visible: root.ready; text: qsTrId("library.import_packs"); emphasized: !root.populated; enabled: !root.library.busy; onClicked: if (root.stage()) { importDialog.manual = false; importDialog.open() } }
        AppButton {
            visible: root.ready && !root.narrow
            text: qsTrId("library.import_songs")
            enabled: !root.library.busy
            onClicked: if (root.stage()) { importDialog.manual = true; importDialog.open() }
        }
        AppButton { visible: root.ready && !root.narrow; text: qsTrId("library.new_song"); enabled: !root.library.busy; onClicked: if (root.stage()) createDialog.open() }
        AppButton { visible: root.ready && !root.narrow; text: qsTrId("library.manage_genres"); enabled: !root.library.busy; onClicked: root.categories("genre") }
        AppButton { visible: root.ready && !root.narrow; text: qsTrId("library.manage_versions"); enabled: !root.library.busy; onClicked: root.categories("miacode_version_category") }
        AppButton { visible: root.populated && !root.narrow; text: qsTrId("library.batch"); selected: root.batchMode; onClicked: { root.batchMode = !root.batchMode; root.mobileDetail = false } }
        AppButton { visible: root.library.busy; text: qsTrId("library.cancel"); onClicked: root.library.cancel() }
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
                    model: [qsTrId("library.all_genres")].concat(Array.from(root.library.genres))
                    currentIndex: root.library.genre ? model.indexOf(root.library.genre) : 0
                    onActivated: i => root.library.genre = i ? model[i] : ""
                }
            }
            AppComboBox {
                visible: root.library.versions.length > 0
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Accessible.name: qsTrId("library.version_category")
                model: [qsTrId("library.all_versions")].concat(Array.from(root.library.versions))
                currentIndex: root.library.versionCategory ? model.indexOf(root.library.versionCategory) : 0
                onActivated: i => root.library.versionCategory = i ? model[i] : ""
            }
            AppTextField {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Accessible.name: qsTrId("library.search")
                placeholderText: qsTrId("library.search")
                text: root.library.query
                onTextEdited: root.library.query = text
            }
            Flow {
                visible: root.batchMode
                Layout.minimumWidth: 0
                Layout.fillWidth: true; spacing: 4
                AppCheckBox { text: qsTrId("library.duplicates"); checked: root.library.duplicatesOnly; onToggled: root.library.duplicatesOnly = checked }
                AppButton { text: qsTrId("library.select_visible"); onClicked: root.library.selectVisible(true) }
                AppButton { text: qsTrId("library.clear_visible"); onClicked: root.library.selectVisible(false) }
            }
            Label { visible: !songs.count && !root.library.busy; text: qsTrId("library.dock_empty"); color: Theme.colors.text.secondary }
            ListView {
                id: songs
                objectName: "managedLibrarySongList"
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true; spacing: 3
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
                    selected: form.path === chartPath
                    Accessible.name: [chartTitle, chartKind, songId].filter(Boolean).join(" · ")
                    onClicked: {
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
                        if (!root.library.additionalOpenModifierHeld() && form.stage()) root.library.openInEditor(chartPath)
                    }
                    Keys.onDownPressed: songs.step(1)
                    Keys.onUpPressed: songs.step(-1)
                    contentItem: RowLayout {
                        id: body
                        spacing: 8
                        AppCheckBox {
                            visible: root.batchMode
                            Accessible.name: qsTrId("library.select_chart").arg(songRow.chartTitle)
                            checked: songRow.model.selected
                            onToggled: root.library.selectPath(songRow.chartPath, checked)
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
        property bool manual: false
        title: manual ? qsTrId("library.import_songs") : qsTrId("library.import_packs")
        onAboutToShow: packChoice.currentIndex = Math.max(0, root.library.packs.indexOf(root.library.pack || form.song.pack || ""))
        function pick(archives) {
            const pack = manual && root.library.packs.length ? packChoice.currentText : ""
            close()
            if (pack) root.library.pack = pack
            root.library.chooseImportFolders(pack, archives, manual)
        }
        body: ColumnLayout {
            Label { visible: packChoice.visible; text: qsTrId("library.target_pack"); color: Theme.colors.text.secondary }
            AppComboBox { id: packChoice; visible: importDialog.manual && root.library.packs.length > 0; Layout.fillWidth: true; Accessible.name: qsTrId("library.target_pack"); model: root.library.packs }
            Label { visible: !importDialog.manual; Layout.fillWidth: true; text: qsTrId("library.batch_media_hint"); wrapMode: Text.Wrap; color: Theme.colors.text.secondary }
            RowLayout {
                AppButton { text: qsTrId("library.choose_folders"); onClicked: importDialog.pick(false) }
                AppButton { text: qsTrId("library.choose_zip"); onClicked: importDialog.pick(true) }
            }
        }
        footer: DialogFooter { cancelText: qsTrId("action.cancel"); onRejected: importDialog.close() }
    }
    AppDialog {
        id: catalog
        property string key: "genre"
        readonly property var names: key === "genre" ? root.library.genres : root.library.versions
        readonly property string current: categoryList.currentIndex >= 0 ? (names[categoryList.currentIndex] ?? "") : ""
        readonly property int usage: {
            root.library.totalCount
            return current ? root.library.categoryUsage(key, current) : 0
        }
        preferredHeight: implicitHeight
        title: key === "genre" ? qsTrId("library.manage_genres") : qsTrId("library.manage_versions")
        onAboutToShow: { categoryName.text = ""; categoryList.currentIndex = names.length ? 0 : -1 }
        body: ColumnLayout {
            AppComboBox { id: categoryList; Layout.fillWidth: true; model: catalog.names; Accessible.name: catalog.title }
            Label { text: qsTrId("library.category_usage").arg(catalog.usage); visible: !!catalog.current; color: Theme.colors.text.secondary }
            AppTextField { id: categoryName; Layout.fillWidth: true; outlined: true; placeholderText: qsTrId("library.category_name"); Accessible.name: placeholderText }
            Flow {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                spacing: 6
                AppButton {
                    text: qsTrId("library.category_add")
                    enabled: !!categoryName.text.trim() && !catalog.names.includes(categoryName.text.trim())
                    onClicked: { root.library.changeCategory(catalog.key, "", categoryName.text); catalog.close() }
                }
                AppButton {
                    text: qsTrId("library.category_rename").arg(catalog.usage)
                    enabled: !!catalog.current && !!categoryName.text.trim() && !catalog.names.includes(categoryName.text.trim())
                    onClicked: { root.library.changeCategory(catalog.key, catalog.current, categoryName.text); catalog.close() }
                }
                AppButton {
                    text: qsTrId("library.category_remove")
                    enabled: !!catalog.current && catalog.usage === 0
                    onClicked: { root.library.changeCategory(catalog.key, catalog.current, ""); catalog.close() }
                }
            }
        }
        footer: DialogFooter { cancelText: qsTrId("action.close"); onRejected: catalog.close() }
    }
    AppDialog {
        id: createDialog
        title: qsTrId("library.new_song")
        preferredHeight: implicitHeight
        onAboutToShow: {
            newTitle.text = ""
            newPack.text = root.library.pack || root.library.packs[0] || qsTrId("library.custom_pack")
            newSlot.currentIndex = 3
        }
        body: ColumnLayout {
            Label { text: qsTrId("library.pack"); color: Theme.colors.text.secondary }
            AppTextField { id: newPack; Layout.fillWidth: true; Accessible.name: qsTrId("library.pack") }
            Label { text: qsTrId("net.ui.song_title"); color: Theme.colors.text.secondary }
            AppTextField { id: newTitle; Layout.fillWidth: true; Accessible.name: qsTrId("net.ui.song_title") }
            AppComboBox { id: newSlot; Layout.fillWidth: true; Accessible.name: qsTrId("library.difficulty"); model: ["Basic","Advanced","Expert","Master","Re:Master","UTAGE"] }
        }
        footer: DialogFooter {
            acceptEnabled: newPack.text.trim().length > 0 && newTitle.text.trim().length > 0
            acceptText: qsTrId("library.new_song")
            cancelText: qsTrId("action.cancel")
            onRejected: createDialog.close()
            onAccepted: {
                const path = root.library.createSong(newPack.text, newTitle.text, newSlot.currentIndex + 2)
                if (path) { root.awaitingPath = path; createDialog.close() }
            }
        }
    }
}
