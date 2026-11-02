#include "icons.h"

#include <QPainter>
#include <QPainterPath>

namespace idea4git {

QIcon icon(const QString& name, const QColor& color) {
    QPixmap pixmap(48, 48);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "folder" || name == "open") {
        QPainterPath path;
        path.moveTo(3, 6);
        path.lineTo(10, 6);
        path.lineTo(12, 8);
        path.lineTo(21, 8);
        path.lineTo(21, 19);
        path.lineTo(3, 19);
        path.closeSubpath();
        p.setBrush(QColor("#806744"));
        p.drawPath(path);
        p.drawLine(4, 10, 20, 10);
    } else if (name == "branch" || name == "git") {
        p.drawEllipse(QPointF(7, 5), 2, 2);
        p.drawEllipse(QPointF(7, 19), 2, 2);
        p.drawEllipse(QPointF(18, 6), 2, 2);
        p.drawLine(7, 7, 7, 17);
        QPainterPath path;
        path.moveTo(7, 14);
        path.cubicTo(7, 9, 18, 14, 18, 8);
        p.drawPath(path);
    } else if (name == "refresh") {
        p.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 285 * 16);
        p.drawLine(20, 4, 20, 10);
        p.drawLine(20, 10, 14, 10);
    } else if (name == "pull" || name == "fetch" || name == "push") {
        const bool up = name == "push";
        p.drawLine(12, up ? 18 : 4, 12, up ? 4 : 17);
        p.drawLine(7, up ? 9 : 12, 12, up ? 4 : 17);
        p.drawLine(17, up ? 9 : 12, 12, up ? 4 : 17);
        p.drawLine(4, 20, 20, 20);
    } else if (name == "save") {
        p.drawRoundedRect(QRectF(4, 3, 16, 18), 1, 1);
        p.drawRect(QRectF(8, 3, 8, 6));
        p.drawRect(QRectF(8, 14, 8, 7));
    } else if (name == "plus" || name == "minus") {
        p.drawLine(5, 12, 19, 12);
        if (name == "plus")
            p.drawLine(12, 5, 12, 19);
    } else if (name == "close") {
        p.drawLine(7, 7, 17, 17);
        p.drawLine(7, 17, 17, 7);
    } else if (name == "hideLeft") {
        p.drawLine(5, 4, 5, 20);
        p.drawLine(20, 12, 10, 12);
        p.drawLine(10, 12, 14, 8);
        p.drawLine(10, 12, 14, 16);
    } else if (name == "hideRight") {
        p.drawLine(19, 4, 19, 20);
        p.drawLine(4, 12, 14, 12);
        p.drawLine(14, 12, 10, 8);
        p.drawLine(14, 12, 10, 16);
    } else if (name == "hideDown") {
        p.drawLine(4, 18, 20, 18);
        p.drawLine(12, 4, 12, 14);
        p.drawLine(12, 14, 8, 10);
        p.drawLine(12, 14, 16, 10);
    } else if (name == "search") {
        p.drawEllipse(QRectF(4, 3, 12, 12));
        p.drawLine(14, 14, 20, 20);
    } else if (name == "locate") {
        p.drawEllipse(QPointF(12, 12), 6.5, 6.5);
        p.drawEllipse(QPointF(12, 12), 2, 2);
        p.drawLine(12, 1.5, 12, 4);
        p.drawLine(12, 20, 12, 22.5);
        p.drawLine(1.5, 12, 4, 12);
        p.drawLine(20, 12, 22.5, 12);
    } else if (name == "settings") {
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawEllipse(QRectF(9, 9, 6, 6));
        p.drawLine(12, 2, 12, 5);
        p.drawLine(12, 19, 12, 22);
        p.drawLine(2, 12, 5, 12);
        p.drawLine(19, 12, 22, 12);
    } else if (name == "history") {
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawLine(12, 7, 12, 12);
        p.drawLine(12, 12, 16, 14);
    } else if (name == "diff") {
        p.drawRect(QRectF(3, 4, 18, 16));
        p.drawLine(12, 4, 12, 20);
        p.drawLine(6, 9, 9, 9);
        p.drawLine(15, 12, 18, 12);
        p.drawLine(16.5, 10.5, 16.5, 13.5);
    } else if (name == "terminal") {
        p.drawLine(5, 6, 10, 11);
        p.drawLine(10, 11, 5, 16);
        p.drawLine(13, 17, 20, 17);
    } else {
        QPainterPath path;
        path.moveTo(6, 3);
        path.lineTo(14, 3);
        path.lineTo(19, 8);
        path.lineTo(19, 21);
        path.lineTo(6, 21);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(14, 3, 14, 8);
        p.drawLine(14, 8, 19, 8);
        p.drawLine(9, 13, 16, 13);
        p.drawLine(9, 17, 14, 17);
    }
    return QIcon(pixmap);
}

} // namespace idea4git
