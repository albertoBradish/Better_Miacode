#pragma once
#include <QImage>
#include <QtMath>

namespace miacode::preview::scene {
// Transparent alpha ring: never paints over an original sprite pixel. Disabled
// edges repeat the seam texel, joining hold caps/body without internal borders.
inline QImage selectionOutlineImage(const QImage& source, int radius,
    bool left = true, bool right = true, bool top = true, bool bottom = true)
{
    if (source.isNull()) return {};
    radius = qMax(1, radius);
    const QImage input = source.convertToFormat(QImage::Format_ARGB32);
    const int padL = left ? radius : 0, padR = right ? radius : 0;
    const int padT = top ? radius : 0, padB = bottom ? radius : 0;
    QImage output(input.width() + padL + padR, input.height() + padT + padB,
        QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    const auto alpha = [&](int x, int y) {
        if ((left && x < 0) || (right && x >= input.width())
            || (top && y < 0) || (bottom && y >= input.height())) return 0;
        return qAlpha(input.pixel(qBound(0, x, input.width()-1), qBound(0, y, input.height()-1)));
    };
    for (int y = 0; y < output.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < output.width(); ++x) {
            const int sx = x-padL, sy = y-padT;
            int around = 0;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx)
                    around = qMax(around, alpha(sx+dx, sy+dy));
            const int a = qMax(0, around-alpha(sx, sy));
            row[x] = qRgba(a, (214*a)/255, 0, a);
        }
    }
    output.setDevicePixelRatio(source.devicePixelRatio());
    return output;
}
}
