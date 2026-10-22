#pragma once

// Small line icons drawn in code, so they look the same on every OBS theme
// and need no image resources. Drawn in the widget's text colour.

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QColor>

enum class LineIcon { Pencil, Trash };

inline QIcon MakeLineIcon(LineIcon kind, const QColor& color)
{
    const int S = 64; // drawn large, scaled down by Qt (crisp on HiDPI)
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 5.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == LineIcon::Pencil) {
        // Body of the pencil as a rotated rectangle with a pointed tip.
        p.translate(S / 2.0, S / 2.0);
        p.rotate(-45);
        QPainterPath body;
        body.moveTo(-22, -7);
        body.lineTo(16, -7);
        body.lineTo(16, 7);
        body.lineTo(-22, 7);
        body.closeSubpath();
        p.drawPath(body);
        // Tip
        QPainterPath tip;
        tip.moveTo(-22, -7);
        tip.lineTo(-31, 0);
        tip.lineTo(-22, 7);
        p.drawPath(tip);
        // Eraser band
        p.drawLine(QPointF(8, -7), QPointF(8, 7));
    } else {
        // Lid and handle
        p.drawLine(QPointF(12, 17), QPointF(52, 17));
        QPainterPath handle;
        handle.moveTo(25, 17);
        handle.lineTo(25, 10);
        handle.lineTo(39, 10);
        handle.lineTo(39, 17);
        p.drawPath(handle);
        // Can, slightly tapered
        QPainterPath can;
        can.moveTo(17, 23);
        can.lineTo(20, 55);
        can.lineTo(44, 55);
        can.lineTo(47, 23);
        p.drawPath(can);
        // Ribs
        p.drawLine(QPointF(27, 30), QPointF(28, 48));
        p.drawLine(QPointF(37, 30), QPointF(36, 48));
    }
    p.end();
    return QIcon(pm);
}
