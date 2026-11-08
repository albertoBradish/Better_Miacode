import QtQuick
import MiaCode.Preview

Rectangle {
    id: root

    property var runtime: null
    readonly property var introStyle: runtime ? runtime.introBannerStyle : null
    readonly property bool chartPreviewStyle: !!introStyle && !!introStyle.chartPreviewMode
    readonly property bool chartPreviewSplit: chartPreviewStyle && runtime.introOverlayActive
        && runtime.introOverlayFrame < introStyle.pvPreviewHoldFrames

    color: root.chartPreviewSplit ? "black" : "#191A1B"
    implicitWidth: 480
    implicitHeight: 480

    PreviewQuickSceneRoot {
        id: previewQuickSceneRoot
        objectName: "previewQuickSceneRoot"
        x: root.chartPreviewSplit ? parent.width * 0.5 : 0
        width: parent.width - x
        height: parent.height
        clip: root.chartPreviewSplit
        z: 1
        stageBackgroundEnabled: !root.chartPreviewSplit
        opacity: root.runtime && root.runtime.introHidesChart ? 0 : 1
        runtime: root.runtime
    }

    PreviewQuickHudLayer {
        id: previewQuickHudLayer
        objectName: "previewQuickHudLayer"
        x: root.chartPreviewSplit ? parent.width * 0.5 : 0
        width: parent.width - x
        height: parent.height
        clip: root.chartPreviewSplit
        z: 2
        opacity: root.runtime && root.runtime.introHidesChart ? 0 : 1
        runtime: root.runtime
    }

    Item {
        id: introOverlayLayer
        anchors.fill: parent
        z: 3
        clip: true
        visible: root.runtime && root.runtime.introOverlayActive

        function syncIntroOverlayData() {
            if (!introOverlayLoader.item || !root.runtime)
                return
            introOverlayLoader.item.bannerTrack = root.runtime.introBannerTrack
            introOverlayLoader.item.bannerTemplateData = root.runtime.introBannerTemplate
            introOverlayLoader.item.backgroundImage = root.runtime.introBackgroundImage
            introOverlayLoader.item.logoImage = root.runtime.introLogoImage
            // "片头" tab styling (backdropImage / backdropBlurEnabled /
            // cardShadowEnabled), applied key-by-key like the export mount so the
            // 背景虚化/自定义背景/卡片阴影 toggles take effect live in the preview.
            var style = root.runtime.introBannerStyle
            if (style) {
                for (var k in style)
                    introOverlayLoader.item[k] = style[k]
            }
        }

        function syncIntroOverlayFrame() {
            if (!introOverlayLoader.item || !root.runtime)
                return
            introOverlayLoader.item.stillFrame = root.runtime.introStillFrame
            introOverlayLoader.item.frame = root.runtime.introOverlayFrame
        }

        Loader {
            id: introOverlayLoader
            active: introOverlayLayer.visible
            source: "qrc:/intro/qml/IntroOverlay.qml"
            width: root.chartPreviewStyle ? Math.max(1, introOverlayLayer.width * height
                / Math.max(1, introOverlayLayer.height)) : 1920
            height: 1080
            transformOrigin: Item.Center
            scale: root.chartPreviewStyle ? introOverlayLayer.height / height
                : Math.max(introOverlayLayer.width / width, introOverlayLayer.height / height)
            x: introOverlayLayer.width / 2 - width / 2
            y: introOverlayLayer.height / 2 - height / 2
            onLoaded: {
                introOverlayLayer.syncIntroOverlayData()
                introOverlayLayer.syncIntroOverlayFrame()
            }
        }

        Connections {
            target: root.runtime

            function onIntroOverlayDataChanged() {
                introOverlayLayer.syncIntroOverlayData()
            }

            function onIntroOverlayStateChanged() {
                introOverlayLayer.syncIntroOverlayFrame()
            }
        }
    }
}
