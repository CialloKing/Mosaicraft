#include "CanvasImageItem.h"

#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QVarLengthArray>
#include <QtMath>

#include <cmath>

namespace
{
constexpr qreal pixelGridScale = 6.0;

int filterSize(int pixels, qreal scale)
{
    return qMax(1, qRound(pixels * qMin(1.0, scale)));
}
}

CanvasImageItem::CanvasImageItem(const QPixmap &source)
    : pixels(source.toImage().convertToFormat(source.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                                      : QImage::Format_RGB32))
{
    pixels.setDevicePixelRatio(1);
    setFlag(ItemUsesExtendedStyleOption);
}

QRectF CanvasImageItem::boundingRect() const
{
    return QRectF(pixels.rect());
}

void CanvasImageItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    if (pixels.isNull()) return;
    const auto transform = painter->worldTransform();
    const auto scaleX = std::hypot(transform.m11(), transform.m12());
    const auto scaleY = std::hypot(transform.m21(), transform.m22());
    const auto density = painter->device()->devicePixelRatioF();
    const QSize sample(filterSize(pixels.width(), scaleX * density), filterSize(pixels.height(), scaleY * density));
    const auto *image = &pixels;
    if (sample != pixels.size())
    {
        if (filtered.size() != sample)
        {
            filtered = pixels.scaled(sample, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if (!filtered.isNull()) image = &filtered;
    }

    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, sample != pixels.size() && image == &pixels);
    painter->drawImage(boundingRect(), *image, QRectF(image->rect()));
    if (qMin(scaleX, scaleY) >= pixelGridScale)
    {
        drawPixelGrid(painter, boundingRect().intersected(option->exposedRect));
    }
    painter->restore();
}

void CanvasImageItem::drawPixelGrid(QPainter *painter, const QRectF &visible) const
{
    if (visible.isEmpty()) return;
    QVarLengthArray<QLineF, 256> lines;
    const int lastX = qMin(pixels.width() - 1, qFloor(visible.right()));
    const int lastY = qMin(pixels.height() - 1, qFloor(visible.bottom()));
    for (int x = qMax(1, qCeil(visible.left())); x <= lastX; ++x)
    {
        lines.append({qreal(x), visible.top(), qreal(x), visible.bottom()});
    }
    for (int y = qMax(1, qCeil(visible.top())); y <= lastY; ++y)
    {
        lines.append({visible.left(), qreal(y), visible.right(), qreal(y)});
    }
    painter->setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(125, 125, 125, 170), 1);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->drawLines(lines.constData(), static_cast<int>(lines.size()));
}
