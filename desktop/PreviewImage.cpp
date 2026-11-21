#include "PreviewImage.h"

#include "core/DeepZoomLayout.h"

#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

std::shared_ptr<PreviewImage> PreviewImage::openMosaic(std::shared_ptr<QTemporaryDir> directory, QSize grid,
                                                        QSize tileSize, QPixmap overview)
{
    const qint64 width = qint64(grid.width()) * tileSize.width();
    const qint64 height = qint64(grid.height()) * tileSize.height();
    const qint64 cells = qint64(grid.width()) * grid.height();
    if (!directory || grid.isEmpty() || tileSize.isEmpty() || overview.isNull() ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
        cells > std::numeric_limits<int>::max()) return {};
    QFile index(directory->filePath("cells.bin"));
    const qint64 bytes = cells * qint64(sizeof(quint32));
    if (!index.open(QIODevice::ReadOnly) || index.size() != bytes) return {};
    auto keys = index.readAll();
    if (keys.size() != bytes) return {};

    auto image = std::make_shared<PreviewImage>();
    image->dimensions = {int(width), int(height)};
    image->grid = grid;
    image->tileSize = tileSize;
    image->overview = std::move(overview);
    image->tileKeys = std::move(keys);
    image->directory = std::move(directory);
    return image;
}

std::shared_ptr<PreviewImage> PreviewImage::openPyramid(std::shared_ptr<QTemporaryDir> directory)
{
    if (!directory) return {};
    QFile descriptor(directory->filePath("pyramid/index.dzi"));
    if (!descriptor.open(QIODevice::ReadOnly) || descriptor.size() > 4096) return {};
    QXmlStreamReader xml(descriptor.readAll());
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("Image")) return {};
    const auto attributes = xml.attributes();
    const int edge = attributes.value("TileSize").toInt();
    if (edge != mosaicraft::deepZoomTileSize || attributes.value("Format") != QStringLiteral("jpg") ||
        attributes.value("Overlap") != QStringLiteral("0") || !xml.readNextStartElement() ||
        xml.name() != QStringLiteral("Size")) return {};
    const QSize size(xml.attributes().value("Width").toInt(), xml.attributes().value("Height").toInt());
    if (size.isEmpty()) return {};

    auto image = std::make_shared<PreviewImage>();
    image->dimensions = size;
    image->tileSize = {edge, edge};
    image->topLevel = int(mosaicraft::deepZoomLevels(size.width(), size.height()).size()) - 1;
    image->directory = std::move(directory);
    return image;
}

bool PreviewImage::replaceTile(int position, quint32 key, QPixmap newOverview)
{
    if (!isMosaic() || position < 0 || position >= tileKeys.size() / qsizetype(sizeof(key)) || newOverview.isNull())
        return false;
    std::memcpy(tileKeys.data() + qsizetype(position) * sizeof(key), &key, sizeof(key));
    overview = std::move(newOverview);
    return true;
}

QImage PreviewImage::loadTile(const QString &name) const
{
    if (const auto cached = tiles.object(name)) return *cached;
    QImage tile(directory->filePath(name));
    if (!tile.isNull())
    {
        const auto cost = qMax<qsizetype>(1, tile.sizeInBytes() / 1024);
        if (cost <= tiles.maxCost()) tiles.insert(name, new QImage(tile), cost);
    }
    return tile;
}

void PreviewImage::paint(QPainter *painter, QRectF visible, qreal density) const
{
    const QRectF bounds{QPointF(), QSizeF(dimensions)};
    visible = visible.intersected(bounds);
    if (visible.isEmpty() || density <= 0) return;
    const bool mosaic = isMosaic();
    if (mosaic && density < qMin(qreal(overview.width()) / dimensions.width(),
                                  qreal(overview.height()) / dimensions.height()))
    {
        painter->drawPixmap(bounds, overview, QRectF(overview.rect()));
        return;
    }

    // Deep Zoom levels are powers of two. The highest level is always the native raster.
    const int reduction = mosaic || density >= 1 ? 0 : qMin(topLevel, int(std::floor(std::log2(1 / density))));
    const qreal scale = std::ldexp(1.0, reduction);
    const QSizeF span(tileSize.width() * scale, tileSize.height() * scale);
    const int firstColumn = int(std::floor(visible.left() / span.width()));
    const int lastColumn = int(std::ceil(visible.right() / span.width()));
    const int firstRow = int(std::floor(visible.top() / span.height()));
    const int lastRow = int(std::ceil(visible.bottom() / span.height()));
    painter->save();
    painter->setClipRect(bounds, Qt::IntersectClip);
    for (int row = firstRow; row < lastRow; ++row)
    {
        for (int column = firstColumn; column < lastColumn; ++column)
        {
            QImage tile;
            if (mosaic)
            {
                const qsizetype index = qsizetype(row) * grid.width() + column;
                quint32 key;
                std::memcpy(&key, tileKeys.constData() + index * sizeof(key), sizeof(key));
                if (key) tile = loadTile(QString::number(key) + ".png");
            }
            else
            {
                tile = loadTile(QString("pyramid/index_files/%1/%2_%3.jpg").arg(topLevel - reduction).arg(column).arg(row));
            }
            const QPointF origin(column * span.width(), row * span.height());
            if (tile.isNull())
            {
                painter->fillRect(QRectF(origin, span), Qt::black);
            }
            else
            {
                painter->drawImage(QRectF(origin, QSizeF(tile.width() * scale, tile.height() * scale)),
                                   tile, QRectF(tile.rect()));
            }
        }
    }
    painter->restore();
}
