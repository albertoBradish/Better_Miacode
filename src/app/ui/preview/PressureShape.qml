import QtQuick

Canvas {
    id: root
    required property bool smooth
    required property real endSecond
    required property var spansForPaint
    property color fillColor: "#dedede"
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onSmoothChanged: requestPaint()
    onEndSecondChanged: requestPaint()
    onFillColorChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()
    onPaint: {
        const context = getContext("2d")
        context.clearRect(0, 0, width, height)
        if (!visible || width < 1 || endSecond <= 0) return
        const spans = spansForPaint()
        if (!spans.length) return
        const pixelsPerSecond = width / root.endSecond
        if (root.smooth) {
            // Integrate pressure over screen-space bins, including quiet gaps.
            const count = Math.max(2, Math.ceil(width / 8))
            const step = root.endSecond / count
            const levels = new Array(count).fill(0)
            for (let i = 0; i < spans.length; i += 3) {
                const first = Math.max(0, Math.floor(spans[i] / step))
                const last = Math.min(count - 1, Math.ceil(spans[i + 1] / step) - 1)
                for (let j = first; j <= last; ++j) {
                    const overlap = Math.min(spans[i + 1], (j + 1) * step) - Math.max(spans[i], j * step)
                    levels[j] += spans[i + 2] * overlap / step
                }
            }
            context.beginPath()
            context.moveTo(0, height)
            let x = 0
            let y = height
            for (let j = 0; j < count; ++j) {
                const nextX = (j + 0.5) * width / count
                const nextY = height - Math.min(1, levels[j]) * (height - 1)
                context.quadraticCurveTo(x, y, (x + nextX) / 2, (y + nextY) / 2)
                x = nextX
                y = nextY
            }
            context.quadraticCurveTo(x, y, width, height)
            context.closePath()
            context.fillStyle = root.fillColor
            context.fill()
            return
        }
        let previousEnd = -1
        context.beginPath()
        for (let i = 0; i < spans.length; i += 3) {
            const left = spans[i] * pixelsPerSecond
            const right = spans[i + 1] * pixelsPerSecond
            const y = height - spans[i + 2] * (height - 1)
            if (spans[i] > previousEnd) {
                if (previousEnd >= 0) {
                    context.lineTo(previousEnd * pixelsPerSecond, height)
                    context.closePath()
                }
                context.moveTo(left, height)
                context.lineTo(left, y)
            } else {
                // Round the edge only after the change, inside the occupied span.
                const ramp = Math.min(1.5, (right - left) * 0.25)
                context.quadraticCurveTo(left, y, left + ramp, y)
            }
            context.lineTo(right, y)
            previousEnd = spans[i + 1]
        }
        context.lineTo(previousEnd * pixelsPerSecond, height)
        context.closePath()
        context.fillStyle = root.fillColor
        context.fill()
    }
}
