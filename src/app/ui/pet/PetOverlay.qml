import QtQuick
import QtQuick.Window
import QtQuick.Controls
import MiaCode.UI

Window {
    id: petWindow

    required property var controller
    required property Window hostWindow

    property bool placementApplied: false
    property bool throttleAuxiliary: false
    property string animationState: "idle"
    property real animationPhase: 0
    property real lifeTime: 0
    // Supplied by the chart's authoritative transport, never integrated here.
    property real rhythmQuarterBeat: 0
    property bool rhythmPlaying: false
    property real rhythmSecond: 0
    property bool dropAvailable: false
    property real dropStart: 0
    property real dropEnd: 0
    property real previousRhythmBeat: 0
    property real phaseCorrection: 0
    readonly property real dropEnvelope: {
        if (!visible || !dropAvailable || rhythmSecond <= dropStart || rhythmSecond >= dropEnd)
            return 0
        const ramp = Math.min(0.33, (dropEnd - dropStart) * 0.25)
        const t = Math.min(1, Math.min(rhythmSecond - dropStart, dropEnd - rhythmSecond) / ramp)
        return t * t * (3 - 2 * t)
    }
    property real dropWeight: 0
    readonly property real dropEyeEnvelope: {
        if (!dropAvailable || dropEnd <= dropStart)
            return 0
        const ramp = 0.12
        const t = Math.max(0, Math.min(1, 1 + Math.min(rhythmSecond - dropStart, dropEnd - rhythmSecond) / ramp))
        return t * t * (3 - 2 * t)
    }
    property real eyeDanceWeight: rhythmPlaying && visible ? 1 : 0
    Behavior on eyeDanceWeight {
        enabled: !petWindow.rhythmPlaying
        NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
    }
    NumberAnimation {
        id: phaseRecovery
        target: petWindow
        property: "phaseCorrection"
        to: 0
        duration: 220
        easing.type: Easing.OutCubic
    }
    onRhythmQuarterBeatChanged: {
        // Preserve the current pose on a seek, then recover the transport phase.
        if (visible && Math.abs(rhythmQuarterBeat - previousRhythmBeat) > 0.5) {
            phaseRecovery.stop()
            const delta = previousRhythmBeat + phaseCorrection - rhythmQuarterBeat
            phaseCorrection = ((delta + 1) % 2 + 2) % 2 - 1
            phaseRecovery.restart()
        }
        previousRhythmBeat = rhythmQuarterBeat
    }
    property real rhythmWeight: rhythmPlaying && visible
        ? (animationState === "interaction" ? 0.18 : 1) : 0
    Behavior on rhythmWeight { NumberAnimation { duration: 300; easing.type: Easing.InOutSine } }
    onRhythmPlayingChanged: {
        // An explicit click gesture owns its two-second lifetime even when
        // playback starts or stops. Its timer returns to the current idle/dance.
        if (animationState === "interaction")
            return
        if (visible)
            enterState("idle")
    }

    width: controller.size
    height: controller.size
    visible: controller.visible && placementApplied
    color: "transparent"
    title: qsTrId("pet.window_title")
    transientParent: null
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.NoDropShadowWindowHint

    function applyInitialPosition() {
        controller.prepareWindow(petWindow)
        const point = controller.initialPosition(
            width, height,
            hostWindow.x, hostWindow.y, hostWindow.width, hostWindow.height)
        x = point.x
        y = point.y
        placementApplied = true
    }

    function resizePet(size) {
        const previous = controller.size
        controller.size = size
        const delta = controller.size - previous
        x -= delta / 2
        y -= delta
        correctPosition(true)
    }

    function correctPosition(save) {
        const point = controller.correctedPosition(x, y, width, height)
        x = point.x
        y = point.y
        if (save)
            controller.savePosition(x, y, width, height)
    }

    function enterState(nextState) {
        animationState = nextState
        animationPhase = 0
        stateTimer.stop()
        if (nextState === "sleeping" || !visible)
            return
        // Interaction must finish during playback too. Only the autonomous
        // idle/sit/sleep cycle yields to the chart-driven dance.
        if (rhythmPlaying && nextState !== "interaction")
            return
        stateTimer.interval = nextState === "interaction" ? 2000
                             : nextState === "sitting" ? 12000 : 8000
        stateTimer.restart()
    }

    function advancePoseCycle() {
        enterState(animationState === "interaction" ? "idle"
                   : animationState === "idle" ? "sitting" : "sleeping")
    }

    function interact() {
        enterState("interaction")
    }

    Component.onCompleted: applyInitialPosition()

    onVisibleChanged: {
        if (!visible) {
            stateTimer.stop()
            dropWeight = 0
            return
        }
        correctPosition(false)
        enterState("idle")
    }

    onClosing: function(close) {
        close.accepted = false
        controller.visible = false
    }

    Screen.onVirtualXChanged: correctPosition(true)
    Screen.onVirtualYChanged: correctPosition(true)
    Screen.onDesktopAvailableWidthChanged: correctPosition(true)
    Screen.onDesktopAvailableHeightChanged: correctPosition(true)
    Screen.onDevicePixelRatioChanged: correctPosition(true)

    Item {
        id: spriteHost
        anchors.fill: parent

        Accessible.role: Accessible.Animation
        Accessible.name: qsTrId("pet.accessible_name")

        PetScene {
            id: petScene
            anchors.fill: parent
            cameraMirrored: petWindow.controller.cameraMirrored

            model: Fox {
                clip: petWindow.animationState === "sleeping"
                      ? "animation.fox.sleep"
                      : petWindow.animationState === "sitting"
                        ? "animation.fox.sit"
                        : petWindow.animationState === "interaction"
                          ? "animation.fox.wiggle"
                          : ""
                phase: petWindow.animationPhase
                lifeTime: petWindow.lifeTime
                rhythmQuarterBeat: petWindow.rhythmQuarterBeat + petWindow.phaseCorrection
                rhythmWeight: petWindow.rhythmWeight
                rhythmDropWeight: petWindow.dropWeight
                rhythmDropEyeClosure: petWindow.dropEyeEnvelope * petWindow.eyeDanceWeight
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property bool dragging: false
            property point pressPosition
            property point windowPosition

            onPressed: function(mouse) {
                dragging = false
                const point = petWindow.controller.pointerPosition()
                pressPosition = Qt.point(point.x, point.y)
                windowPosition = Qt.point(petWindow.x, petWindow.y)
            }
            onPositionChanged: function(mouse) {
                if (!(pressedButtons & Qt.LeftButton))
                    return
                const point = petWindow.controller.pointerPosition()
                const dx = point.x - pressPosition.x
                const dy = point.y - pressPosition.y
                if (!dragging && Math.hypot(dx, dy) >= Qt.styleHints.startDragDistance)
                    dragging = true
                if (dragging) {
                    petWindow.x = windowPosition.x + dx
                    petWindow.y = windowPosition.y + dy
                }
            }
            onReleased: function(mouse) {
                if (dragging)
                    petWindow.correctPosition(true)
                else if (mouse.button === Qt.LeftButton)
                    petWindow.interact()
                else if (mouse.button === Qt.RightButton)
                    contextMenu.popup(mouse.x, mouse.y)
                dragging = false
            }
            onCanceled: {
                if (dragging)
                    petWindow.correctPosition(true)
                dragging = false
            }
            onWheel: function(wheel) {
                if (wheel.angleDelta.y !== 0)
                    petWindow.resizePet(petWindow.controller.size + (wheel.angleDelta.y > 0 ? 20 : -20))
                wheel.accepted = true
            }
        }

        Menu {
            id: contextMenu
            popupType: Popup.Native
            MenuItem {
                text: qsTrId("pet.camera_mirror")
                checkable: true
                checked: petWindow.controller.cameraMirrored
                onTriggered: petWindow.controller.cameraMirrored = !petWindow.controller.cameraMirrored
            }
            MenuItem {
                text: qsTrId("pet.enlarge")
                enabled: petWindow.controller.size < 440
                onTriggered: petWindow.resizePet(petWindow.controller.size + 20)
            }
            MenuItem {
                text: qsTrId("pet.shrink")
                enabled: petWindow.controller.size > 140
                onTriggered: petWindow.resizePet(petWindow.controller.size - 20)
            }
            MenuItem {
                text: qsTrId("pet.reset_size")
                onTriggered: petWindow.resizePet(220)
            }
            MenuSeparator { }
            MenuItem {
                text: qsTrId("pet.hide")
                onTriggered: petWindow.controller.visible = false
            }
        }

    }

    FrameAnimation {
        property real elapsed: 0
        running: petWindow.visible
        onTriggered: {
            elapsed += frameTime
            if (petWindow.throttleAuxiliary && elapsed < 1 / 31)
                return
            petWindow.animationPhase += elapsed
            petWindow.lifeTime += elapsed
            // Filter envelope jumps after seeks on the same cadence as pose rendering.
            const remaining = petWindow.dropEnvelope - petWindow.dropWeight
            petWindow.dropWeight += Math.abs(remaining) < 0.001 ? remaining
                : remaining * (1 - Math.exp(-elapsed / 0.06))
            elapsed = 0
            petScene.advanceFrame()
        }
    }

    Timer {
        id: stateTimer
        repeat: false
        onTriggered: petWindow.advancePoseCycle()
    }
}
