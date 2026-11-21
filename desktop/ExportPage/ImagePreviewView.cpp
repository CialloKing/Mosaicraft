#include "ImagePreviewView.h"

#include "CanvasImageItem.h"
#include "PreviewImage.h"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QWheelEvent>

#include <cmath>
#include <utility>

namespace
{
constexpr qreal zoomStep = 1.25;
}

ImagePreviewView::ImagePreviewView(QWidget *parent) : QGraphicsView(new QGraphicsScene, parent)
{
    scene()->setParent(this);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(NoAnchor);
    setResizeAnchor(AnchorViewCenter);
    setFocusPolicy(Qt::StrongFocus);
}

void ImagePreviewView::setImage(const QPixmap &pixmap)
{
    if (!tiled && pixmapKey == pixmap.cacheKey()) return;
    tiled.reset();
    pixmapKey = pixmap.cacheKey();
    setItem(pixmap.isNull() ? nullptr : new CanvasImageItem(pixmap, true));
}

void ImagePreviewView::setPreview(std::shared_ptr<PreviewImage> preview)
{
    if (tiled == preview && !pixmapKey)
    {
        viewport()->update();
        return;
    }
    tiled = std::move(preview);
    pixmapKey = 0;
    setItem(tiled ? new CanvasImageItem(tiled, true) : nullptr);
}

void ImagePreviewView::setItem(CanvasImageItem *next)
{
    scene()->clear();
    item = next;
    if (item) scene()->addItem(item);
    setDragMode(item ? ScrollHandDrag : NoDrag);
    scene()->setSceneRect(item ? item->boundingRect() : QRectF());
    setSceneRect(scene()->sceneRect());
    if (item) fitImage();
    else emit zoomChanged(0);
}

qreal ImagePreviewView::fitScale() const
{
    const auto size = sceneRect().size();
    return qMin(qMax(1, viewport()->width() - 12) / size.width(),
                qMax(1, viewport()->height() - 12) / size.height());
}

void ImagePreviewView::fitImage()
{
    if (!item) return;
    fitting = true;
    const auto scale = qMin(1.0, fitScale());
    setTransform(QTransform::fromScale(scale, scale));
    centerOn(item);
    emit zoomChanged(scale);
}

void ImagePreviewView::actualSize()
{
    if (!item) return;
    zoomAt(1, viewport()->rect().center());
}

void ImagePreviewView::zoomIn()
{
    zoomAt(transform().m11() * zoomStep, viewport()->rect().center());
}

void ImagePreviewView::zoomOut()
{
    zoomAt(transform().m11() / zoomStep, viewport()->rect().center());
}

void ImagePreviewView::zoomAt(qreal factor, QPoint anchor)
{
    if (!item) return;
    const auto point = mapToScene(anchor);
    const auto scale = qBound(qMin(0.01, fitScale()), factor, 16.0);
    fitting = false;
    setTransform(QTransform::fromScale(scale, scale));
    centerOn(point + QPointF(viewport()->rect().center() - anchor) / scale);
    emit zoomChanged(scale);
}

void ImagePreviewView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (fitting) fitImage();
}

void ImagePreviewView::wheelEvent(QWheelEvent *event)
{
    const qreal steps = event->angleDelta().y() ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 64.0;
    if (item && steps && event->modifiers() == Qt::NoModifier)
    {
        zoomAt(transform().m11() * std::pow(zoomStep, steps), event->position().toPoint());
        event->accept();
    }
    else
    {
        QGraphicsView::wheelEvent(event);
    }
}

void ImagePreviewView::keyPressEvent(QKeyEvent *event)
{
    if (item)
    {
        switch (event->key())
        {
        case Qt::Key_Plus:
        case Qt::Key_Equal: zoomIn(); return;
        case Qt::Key_Minus: zoomOut(); return;
        case Qt::Key_0: fitImage(); return;
        case Qt::Key_1: actualSize(); return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}
