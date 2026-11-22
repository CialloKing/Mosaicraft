#pragma once

#include <QByteArray>
#include <QCache>
#include <QImage>
#include <QPixmap>
#include <QRectF>

#include <memory>

class QPainter;
class QTemporaryDir;

class PreviewImage
{
public:
    static std::shared_ptr<PreviewImage> openMosaic(std::shared_ptr<QTemporaryDir> directory, QSize grid,
                                                   QSize tileSize, QPixmap overview, qint64 distinctTiles = -1);
    static std::shared_ptr<PreviewImage> openPyramid(std::shared_ptr<QTemporaryDir> directory);

    QSize size() const { return dimensions; }
    qsizetype cachedKiB() const
    {
        return tiles.totalCost();
    }
    qsizetype setCacheLimitKiB(qsizetype limit);
    bool replaceTile(int position, quint32 key, QPixmap overview);
    void paint(QPainter *painter, QRectF visible, qreal density) const;

private:
    bool isMosaic() const { return grid.isValid(); }
    QImage loadTile(const QString &name) const;

    std::shared_ptr<QTemporaryDir> directory;
    QSize dimensions, tileSize, grid;
    QPixmap overview;
    QByteArray tileKeys;
    quint64 tileCount = 0;
    int topLevel = 0;
    mutable QCache<QString, QImage> tiles{0};
};
