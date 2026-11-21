#pragma once

#include <QGraphicsItem>
#include <QImage>
#include <QPixmap>

class CanvasImageItem final : public QGraphicsItem
{
public:
    explicit CanvasImageItem(const QPixmap &source);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    void drawPixelGrid(QPainter *painter, const QRectF &visible) const;

    QImage pixels;
    QImage filtered;
};
