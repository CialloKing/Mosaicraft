#pragma once

#include <QGraphicsView>
#include <QPixmap>

#include <memory>

class CanvasImageItem;
class PreviewImage;

class ImagePreviewView final : public QGraphicsView
{
    Q_OBJECT

public:
    explicit ImagePreviewView(QWidget *parent = nullptr);
    void setImage(const QPixmap &pixmap);
    void setPreview(std::shared_ptr<PreviewImage> preview);
    bool hasImage() const { return item != nullptr; }
    void fitImage();
    void actualSize();
    void zoomIn();
    void zoomOut();

signals:
    void zoomChanged(qreal scale);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setItem(CanvasImageItem *next);
    void zoomAt(qreal factor, QPoint anchor);
    qreal fitScale() const;

    CanvasImageItem *item = nullptr;
    std::shared_ptr<PreviewImage> tiled;
    qint64 pixmapKey = 0;
    bool fitting = true;
};
