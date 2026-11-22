#pragma once

#include <QGraphicsItem>
#include <QImage>
#include <QPixmap>
#include <memory>

class PreviewImage;

class CanvasImageItem final : public QGraphicsItem
{
public:
    explicit CanvasImageItem(const QPixmap &source, bool smooth = false);
    explicit CanvasImageItem(std::shared_ptr<PreviewImage> source, bool smooth = false);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    void drawPixelGrid(QPainter *painter, const QRectF &visible) const;

    QImage pixels;
    QImage filtered;
    std::shared_ptr<PreviewImage> preview;
    bool smooth;
};
