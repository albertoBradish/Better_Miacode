pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import MiaCode.UI
import MiaCode.Timeline

Item {
    id: root

    required property var preferences
    required property var timelineSession
    required property var previewSession
    readonly property var sceneWindow: root.Window.window
    onSceneWindowChanged: settingsMenu?.close()
    onVisibleChanged: if (!visible) settingsMenu?.close()
    property bool guideSettingsActive: false
    property bool verticalTimeline: false
    readonly property real timelineRulerRightLimit: verticalTimeline ? 0 : Math.max(0, brightnessButton.x - 2)
    readonly property var structure: root.previewSession?.structure ?? null
    readonly property string dropLabel: qsTrId("timeline.drop_region")

    function syncDropRegion() {
        const bridge = root.timelineSession?.stateBridge
        if (!bridge)
            return
        const valid = root.structure && root.structure.dropAvailable
        bridge.setDropRegion(valid ? root.structure.dropStartSeconds : -1,
                             valid ? root.structure.dropEndSeconds : -1,
                             root.dropLabel)
    }

    function syncNoteScale() {
        root.timelineSession.stateBridge?.setNoteScale(root.preferences.timelineNoteScale)
    }

    Component.onCompleted: {
        syncDropRegion()
        syncNoteScale()
    }
    Connections {
        target: root.preferences
        function onTimelineNoteScaleChanged() { root.syncNoteScale() }
    }
    onStructureChanged: syncDropRegion()
    onDropLabelChanged: syncDropRegion()

    Connections {
        target: root.structure
        function onChanged() { root.syncDropRegion() }
    }
    readonly property bool guideSettingsAvailable: root.guideSettingsActive
        && root.visible && timelineItem.visible
    readonly property Item timelineGuideItem: timelineItem
    readonly property Item timelineSettingsGuideItem: settingsMenu.contentItem

    onGuideSettingsAvailableChanged: {
        // Let the menu's focus and close-policy bindings settle before opening.
        if (root.guideSettingsAvailable)
            Qt.callLater(root.openGuideSettingsMenu)
        else
            settingsMenu.close()
    }

    function openGuideSettingsMenu() {
        if (root.guideSettingsAvailable)
            settingsMenu.openAt(brightnessButton)
    }

    clip: true
    readonly property real minimumHeight: verticalTimeline ? 100 : timelineItem.minimumViewportHeight
    readonly property real minimumWidth: verticalTimeline ? 168 : 140

    Timer {
        id: timelineThemeRefresh
        interval: 0
        onTriggered: timelineItem.refreshTheme()
    }

    TimelineThemeBridge {
        id: timelineTheme
        windowColor: Theme.surfaceColor(Theme.colors.background.surface)
        headerColor: Theme.surfaceColor(Theme.colors.background.panel)
        sidebarColor: Theme.surfaceColor(Theme.colors.background.panel)
        baseColor: Theme.surfaceColor(Theme.colors.background.surface)
        borderColor: Theme.colors.timeline.border
        axisColor: Theme.colors.timeline.axis
        gridMajorColor: Theme.colors.timeline.gridMajor
        gridSubdivisionColor: Theme.colors.timeline.gridSubdivision
        gridMinorColor: Theme.colors.timeline.gridMinor
        laneEvenColor: Theme.contentOverlayColor(Theme.colors.timeline.laneEven)
        laneOddColor: Theme.contentOverlayColor(Theme.colors.timeline.laneOdd)
        labelColor: Theme.colors.timeline.label
        textSecondaryColor: Theme.colors.timeline.textSecondary
        waveStrokeColor: Theme.colors.timeline.waveStroke
        onWindowColorChanged: timelineThemeRefresh.restart()
        onHeaderColorChanged: timelineThemeRefresh.restart()
        onSidebarColorChanged: timelineThemeRefresh.restart()
        onBaseColorChanged: timelineThemeRefresh.restart()
        onBorderColorChanged: timelineThemeRefresh.restart()
        onAxisColorChanged: timelineThemeRefresh.restart()
        onGridMajorColorChanged: timelineThemeRefresh.restart()
        onGridSubdivisionColorChanged: timelineThemeRefresh.restart()
        onGridMinorColorChanged: timelineThemeRefresh.restart()
        onLaneEvenColorChanged: timelineThemeRefresh.restart()
        onLaneOddColorChanged: timelineThemeRefresh.restart()
        onLabelColorChanged: timelineThemeRefresh.restart()
        onTextSecondaryColorChanged: timelineThemeRefresh.restart()
        onWaveStrokeColorChanged: timelineThemeRefresh.restart()
    }

    Binding {
        target: root.timelineSession.stateBridge
        property: "laneOrder"
        value: root.preferences.timelineLaneOrder ?? "natural"
    }

    Binding {
        target: root.timelineSession.stateBridge
        property: "axisMode"
        value: root.verticalTimeline ? (root.preferences.timelineVerticalDirection ?? "falling") : "horizontal"
    }

    TimelineQuickItem {
        id: timelineItem
        objectName: "timelineQuickItem"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: root.verticalTimeline ? brightnessButton.height : 0
        anchors.bottom: parent.bottom
        enabled: visible
        stateBridge: root.timelineSession.stateBridge
        lowPerformanceMode: root.preferences.lowPerformanceMode ?? false
        headerRightLimit: root.timelineRulerRightLimit
        headerMarkerRightLimit: root.timelineRulerRightLimit
        onHeaderNavigateRequested: second => root.timelineSession.headerNavigate(second)
        onTimelineWheelNavigateRequested: second => root.timelineSession.wheelNavigate(second)
        onCenterNavigateRequested: second => root.timelineSession.centerNavigate(second)
        onTimelineDragStarted: root.timelineSession.dragStarted()
        onTimelineDragFinished: second => root.timelineSession.dragFinished(second)
        onTimelineUserInteractionStarted: root.timelineSession.userInteractionStarted()
        onTimelineSurfaceReady: root.timelineSession.surfaceReady()
        onFollowPreviewToggled: enabled => root.timelineSession.followPreviewToggled(enabled)
    }

    Tooltip {
        id: timelineMarkerTooltip
        parent: Overlay.overlay
        visible: timelineItem.hoverTooltipText.length > 0
        text: timelineItem.hoverTooltipText
        x: {
            const position = timelineItem.mapToItem(parent, timelineItem.hoverTooltipPosition)
            return position.x + 12
        }
        y: {
            const position = timelineItem.mapToItem(parent, timelineItem.hoverTooltipPosition)
            return position.y + 16
        }
    }

    IconButton {
        id: brightnessButton

        width: implicitWidth
        height: implicitHeight
        x: root.width - 4 - width + horizontalInset
        y: root.verticalTimeline
            ? 0
            : timelineItem.y + Math.max(0, (timelineItem.timelineTop - height) / 2)
        visible: timelineItem.visible
        iconSource: Qt.resolvedUrl("icons/sliders-horizontal.svg")
        filledIconSource: Qt.resolvedUrl("icons/sliders-horizontal-fill.svg")
        tooltip: qsTrId("qml.timeline_settings")
        active: settingsMenu.active
        Accessible.description: qsTrId("qml.open_timeline_settings")
        onClicked: {
            if (settingsMenu.active) {
                settingsMenu.close()
                return
            }
            settingsMenu.openAt(brightnessButton)
        }
    }

    TimelineSettingsMenu {
        id: settingsMenu
        stateBridge: root.timelineSession.stateBridge
        timelineSession: root.timelineSession
        preferences: root.preferences
        // The modal guide owns input and Escape while demonstrating this menu.
        focus: !root.guideSettingsActive
        closePolicy: root.guideSettingsActive ? Popup.NoAutoClose
            : Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        z: root.guideSettingsActive ? 99 : 0
    }

    // The panel bounds let the HUD extend above a compact note viewport.
    PreviewRateToast {
        anchors.fill: parent
        noteViewportRect: Qt.rect(
            timelineItem.x + timelineItem.noteViewportRect.x,
            timelineItem.y + timelineItem.noteViewportRect.y,
            timelineItem.noteViewportRect.width,
            timelineItem.noteViewportRect.height)
        previewSession: root.previewSession
    }

}
