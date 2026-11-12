import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

Item {
    id: root
    property var library: null
    readonly property Item cornerSourceItem: browserControls
    signal manageRequested()
    signal openChartRequested(string path)
    implicitWidth: 340
    implicitHeight: 460
    clip: true

    Rectangle { anchors.fill: parent; color: Theme.surfaceColor(Theme.colors.background.surface) }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.panelPadding
        spacing: 8
        Flow {
            id: browserControls
            Layout.fillWidth: true
            spacing: 6
            AppButton {
                text: qsTrId("library.choose_root")
                enabled: root.library && !root.library.busy
                onClicked: root.library.chooseRoot()
            }
            AppButton {
                text: qsTrId("library.refresh")
                enabled: root.library && !root.library.busy && root.library.rootPath.length > 0
                onClicked: root.library.refresh()
            }
            AppButton {
                text: qsTrId("library.cancel")
                enabled: root.library && root.library.busy
                onClicked: root.library.cancel()
            }
            AppButton {
                text: qsTrId("library.open_manager")
                enabled: !!root.library
                onClicked: root.manageRequested()
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTrId("library.dock_hint")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Label {
            Layout.fillWidth: true
            text: root.library ? root.library.rootPath : ""
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: Theme.colors.text.secondary
        }
        AppTextField {
            Layout.fillWidth: true
            placeholderText: qsTrId("library.search")
            text: root.library ? root.library.query : ""
            enabled: !!root.library
            onTextEdited: if (root.library) root.library.query = text
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            AppCheckBox {
                text: qsTrId("library.duplicates")
                checked: root.library ? root.library.duplicatesOnly : false
                enabled: !!root.library
                onToggled: root.library.duplicatesOnly = checked
            }
            Label {
                text: root.library ? qsTrId("library.count").arg(root.library.totalCount).arg(root.library.selectedCount) : ""
                color: Theme.colors.text.secondary
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            AppButton {
                text: qsTrId("library.select_visible")
                enabled: root.library && !root.library.busy
                onClicked: root.library.selectVisible(true)
            }
            AppButton {
                text: qsTrId("library.clear_visible")
                enabled: root.library && !root.library.busy
                onClicked: root.library.selectVisible(false)
            }
        }
        ListView {
            id: charts
            objectName: "dockChartLibraryList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            clip: true
            model: root.library
            spacing: 4
            ScrollBar.vertical: AppScrollBar {}
            delegate: Rectangle {
                id: entry
                required property string chartPath
                required property string chartTitle
                required property string artist
                required property string levels
                required property string chartVersion
                required property string sha256
                required property int copies
                required property bool selected
                width: charts.width
                height: details.implicitHeight + 12
                radius: Theme.controlRadius
                color: Theme.colors.background.panel
                ColumnLayout {
                    id: details
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 3
                    RowLayout {
                        Layout.fillWidth: true
                        AppCheckBox {
                            checked: entry.selected
                            enabled: root.library && !root.library.busy
                            onToggled: root.library.selectPath(entry.chartPath, checked)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: entry.chartTitle || entry.chartPath
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.colors.text.primary
                        }
                        Label { visible: entry.copies > 1; text: "×" + entry.copies; color: Theme.colors.text.secondary }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: entry.artist + " · " + entry.levels + " · " + entry.chartVersion
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.colors.text.secondary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: entry.chartPath
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: Theme.colors.text.secondary
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        AppButton {
                            text: qsTrId("library.open_editor")
                            onClicked: root.openChartRequested(entry.chartPath)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: "SHA-256 " + entry.sha256.slice(0, 12)
                            elide: Text.ElideRight
                            color: Theme.colors.text.secondary
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: charts.count === 0 && !(root.library && root.library.busy)
            text: qsTrId("library.dock_empty")
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
        Label {
            Layout.fillWidth: true
            text: root.library ? root.library.status : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.colors.text.secondary
        }
    }
}
