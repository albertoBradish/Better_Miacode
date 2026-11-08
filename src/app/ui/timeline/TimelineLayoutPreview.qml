pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import MiaCode.UI

AbstractButton {
    id: card

    required property string layoutMode
    required property string layoutLabel
    required property bool selected
    readonly property bool vertical: layoutMode !== "horizontal"
    readonly property bool beforeEditor: layoutMode === "vertical_before_editor"

    implicitWidth: 154
    implicitHeight: miniature.height + caption.implicitHeight + 24
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.RadioButton
    Accessible.name: layoutLabel
    Accessible.checkable: true
    Accessible.checked: selected

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.overlayColor(card.selected ? Theme.colors.state.selected
             : card.down ? Theme.colors.state.pressed
             : card.hovered ? Theme.colors.state.hover : Theme.colors.background.control)
        border.width: card.selected || card.visualFocus ? 2 : 1
        border.color: card.selected || card.visualFocus
            ? Theme.colors.accent.primary : Theme.colors.border.control
    }

    contentItem: Item {
        Rectangle {
            id: miniature
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            height: width * 10 / 16
            radius: Theme.compactControlRadius
            color: Theme.surfaceColor(Theme.colors.background.surface)
            border.width: 1
            border.color: Theme.colors.border.normal

            Rectangle {
                x: 5; y: 5
                width: Math.max(0, parent.width - 10)
                height: 4
                radius: 2
                color: Theme.colors.border.control
            }

            Item {
                id: panes
                anchors.fill: parent
                anchors.margins: 5
                anchors.topMargin: 13
                readonly property real gap: 4
                readonly property real timelineWidth: card.vertical ? width * 0.2 : 0
                readonly property real previewWidth: width * 0.29
                readonly property real sidebarWidth: width * 0.13

                Rectangle {
                    width: panes.sidebarWidth
                    height: panes.height
                    radius: 2
                    color: Theme.colors.border.control
                }

                Item {
                    id: editor
                    x: panes.sidebarWidth + panes.gap
                        + (card.beforeEditor ? panes.timelineWidth + panes.gap : 0)
                    width: Math.max(0, panes.width - panes.sidebarWidth - panes.previewWidth
                        - panes.timelineWidth - panes.gap * (card.vertical ? 3 : 2))
                    height: card.vertical ? panes.height : panes.height * 0.57

                    Repeater {
                        model: [0.85, 0.58, 0.74, 0.43]
                        delegate: Rectangle {
                            required property int index
                            required property real modelData
                            y: (index + 1) * editor.height / 5
                            width: editor.width * modelData
                            height: 1
                            color: Theme.colors.text.disabled
                        }
                    }
                }

                Rectangle {
                    id: timeline
                    x: card.beforeEditor ? panes.sidebarWidth + panes.gap
                        : card.vertical ? editor.x + editor.width + panes.gap : editor.x
                    y: card.vertical ? 0 : editor.height + panes.gap
                    width: card.vertical ? panes.timelineWidth : editor.width
                    height: Math.max(0, panes.height - y)
                    radius: 2
                    color: Theme.colors.border.control
                    border.width: 1
                    border.color: Theme.colors.accent.primary

                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            required property int index
                            x: (index + 1) * timeline.width / 4
                            y: 3
                            width: 1
                            height: Math.max(0, timeline.height - 6)
                            color: Theme.colors.text.disabled
                        }
                    }
                }

                Item {
                    x: panes.width - width
                    width: panes.previewWidth
                    height: panes.height
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.max(0, Math.min(parent.width, parent.height) - 5)
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.colors.accent.primary
                    }
                }
            }
        }

        Text {
            id: caption
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: miniature.bottom
            anchors.margins: 8
            text: card.layoutLabel
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
            color: card.selected ? Theme.colors.text.active : Theme.colors.text.secondary
        }
    }
}
