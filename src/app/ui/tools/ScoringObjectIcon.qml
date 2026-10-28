import QtQuick
import MiaCode.UI

Canvas {
    id: root
    required property string objectType
    property url source
    Image {
        id: noteSprite
        anchors.fill: parent
        source: root.source
        fillMode: Image.PreserveAspectFit
        smooth: true
        onStatusChanged: root.requestPaint()
    }
    implicitWidth: 22
    implicitHeight: 22
    property color ink: Theme.colors.text.primary
    onInkChanged: requestPaint()
    onObjectTypeChanged: requestPaint()
    onPaint: {
        const c = getContext("2d");
        c.reset();
        if (noteSprite.status === Image.Ready) return;
        c.strokeStyle = root.ink;
        c.fillStyle = root.ink;
        c.lineWidth = 1.7;
        c.lineCap = "round";
        c.lineJoin = "round";
        const x = width / 2, y = height / 2, r = Math.min(width, height) * 0.36;
        const kind = root.objectType;
        function circle(cx, cy, radius) { c.beginPath(); c.arc(cx, cy, radius, 0, 2 * Math.PI); c.stroke(); }
        if (kind === "Star") {
            c.beginPath();
            for (let i = 0; i < 10; ++i) {
                const a = -Math.PI / 2 + i * Math.PI / 5, radius = i % 2 ? r * 0.43 : r;
                if (i === 0) c.moveTo(x + radius * Math.cos(a), y + radius * Math.sin(a));
                else c.lineTo(x + radius * Math.cos(a), y + radius * Math.sin(a));
            }
            c.closePath(); c.stroke();
        } else if (kind === "Hold" || kind === "TouchHold") {
            c.beginPath(); c.moveTo(x - r, y); c.lineTo(x + r, y); c.stroke();
            circle(x - r * 0.65, y, r * 0.4); circle(x + r * 0.65, y, r * 0.4);
            if (kind === "TouchHold") circle(x, y, r);
        } else if (kind === "Slide") {
            c.beginPath(); c.moveTo(x - r, y + r * 0.7); c.lineTo(x + r, y - r * 0.7);
            c.moveTo(x + r * 0.3, y - r * 0.8); c.lineTo(x + r, y - r * 0.7);
            c.lineTo(x + r * 0.8, y); c.stroke();
        } else if (kind === "Touch") {
            for (let i = 0; i < 4; ++i) {
                c.save(); c.translate(x, y); c.rotate(i * Math.PI / 2);
                c.beginPath(); c.moveTo(-r * 0.4, -r); c.lineTo(0, -r * 0.4); c.lineTo(r * 0.4, -r); c.stroke(); c.restore();
            }
        } else if (kind === "Firework") {
            for (let i = 0; i < 8; ++i) {
                const a = i * Math.PI / 4;
                c.beginPath(); c.moveTo(x + r * 0.45 * Math.cos(a), y + r * 0.45 * Math.sin(a));
                c.lineTo(x + r * Math.cos(a), y + r * Math.sin(a)); c.stroke();
            }
        } else if (kind === "Break" || kind === "Ex") {
            circle(x, y, r);
            c.font = "bold 12px sans-serif";
            c.textAlign = "center"; c.textBaseline = "middle";
            c.fillText(kind === "Break" ? "B" : "EX", x, y);
        } else if (kind === "Total") {
            for (let i = 0; i < 4; ++i) circle(x + (i % 2 ? 1 : -1) * r * 0.5,
                y + (i < 2 ? -1 : 1) * r * 0.5, r * 0.24);
        } else circle(x, y, r);
    }
}
