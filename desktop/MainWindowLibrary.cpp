#include "MainWindow.h"
#include "Dialogs.h"
#include "ui_MainWindow.h"

#include <QCache>
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImageReader>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>

#include <algorithm>

namespace
{
class LibraryThumbnails : public QStyledItemDelegate
{
public:
    LibraryThumbnails(const LibraryCatalog &catalog, QListWidget *list)
        : QStyledItemDelegate(list), catalog(catalog), list(list), images(160) {}

    void clear()
    {
        images.clear();
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem cell(option);
        initStyleOption(&cell, index);
        const int row = index.data(Qt::UserRole).toInt();
        if (row >= 0 && row < catalog.assets.size())
        {
            auto icon = images.object(row);
            if (!icon)
            {
                const auto pixmap = loadPreviewImage(catalog.assetPath(catalog.assets[row]), list->iconSize());
                const auto fallback = list->style()->standardIcon(QStyle::SP_FileIcon);
                images.insert(row, new QIcon(pixmap.isNull() ? fallback : QIcon(pixmap)));
                icon = images.object(row);
            }
            cell.icon = *icon;
            cell.decorationSize = icon->actualSize(list->iconSize());
            cell.features |= QStyleOptionViewItem::HasDecoration;
        }
        list->style()->drawControl(QStyle::CE_ItemViewItem, &cell, painter, list);
    }

private:
    const LibraryCatalog &catalog;
    const QListWidget *list;
    mutable QCache<int, QIcon> images;
};

QString existingAncestor(const QString &path)
{
    QFileInfo info(path);
    while (!info.exists())
    {
        const auto parent = info.absolutePath();
        if (parent == info.absoluteFilePath())
        {
            return {};
        }
        info = QFileInfo(parent);
    }
    return info.absoluteFilePath();
}
}

void MainWindow::setupLibrary()
{
    ui->reloadLibraryButton->setEnabled(false);
    ui->assetList->setWordWrap(true);
    ui->assetList->setLayoutMode(QListView::Batched);
    ui->assetList->setItemDelegate(new LibraryThumbnails(catalog, ui->assetList));
    connect(ui->chooseLibraryButton, &QPushButton::clicked, this, &MainWindow::chooseLibrary);
    connect(ui->reloadLibraryButton, &QPushButton::clicked, this, [this] {
        loadLibrary(catalog.databasePath);
    });
    connect(ui->librarySettingsButton, &QPushButton::clicked, this, [this] { editLibrarySettings(this, settings); });
    connect(ui->importAssetsButton, &QPushButton::clicked, this, &MainWindow::importAssets);
    connect(ui->useLibraryButton, &QPushButton::clicked, this, &MainWindow::useCurrentLibrary);
    connect(ui->exportSelectedButton, &QPushButton::clicked, this, &MainWindow::exportSelectedAssets);
    connect(ui->selectAssetsButton, &QToolButton::toggled, this, [this](bool multiple) {
        if (!multiple)
        {
            selectedAssets.clear();
            ui->assetList->clearSelection();
        }
        ui->assetList->setSelectionMode(multiple ? QAbstractItemView::MultiSelection : QAbstractItemView::SingleSelection);
        updateAssetSelection();
    });
    connect(ui->assetViewMode, &QComboBox::currentIndexChanged, this, [this] { renderAssets(); });
    connect(ui->assetSortMode, &QComboBox::currentIndexChanged, this, [this] { sortAssets(); });
    connect(ui->assetList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateAssetSelection);
    connect(ui->assetList, &QListWidget::itemClicked, this, [this] {
        if (!ui->selectAssetsButton->isChecked()) openAsset();
    });
    connect(ui->assetList, &QListWidget::itemDoubleClicked, this, [this] {
        if (ui->selectAssetsButton->isChecked()) openAsset();
    });
}

void MainWindow::chooseLibrary()
{
    const auto path = QFileDialog::getExistingDirectory(this, tr("打开图库目录"), catalog.root);
    if (!path.isEmpty())
    {
        loadLibrary(path);
    }
}

bool MainWindow::loadLibrary(const QString &path, bool showError)
{
    LibraryCatalog loaded;
    const auto error = readLibrary(path, loaded);
    if (!error.isEmpty())
    {
        if (showError)
        {
            QMessageBox::warning(this, tr("无法打开图库"), error);
        }
        return false;
    }
    selectedAssets.clear();
    catalog = std::move(loaded);
    auto location = QDir::toNativeSeparators(catalog.root);
    if (catalog.normalizedSize.isValid())
    {
        location += tr("  ·  归一化 %1 × %2 px").arg(catalog.normalizedSize.width()).arg(catalog.normalizedSize.height());
    }
    ui->galleryPath->setText(location);
    ui->reloadLibraryButton->setEnabled(true);
    sortAssets();
    updateEditorControls();
    return true;
}

void MainWindow::sortAssets()
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const int sortKey = ui->assetSortMode->currentIndex();
    std::stable_sort(catalog.assets.begin(), catalog.assets.end(), [&](const LibraryAsset &first, const LibraryAsset &second) {
        if (sortKey == 1 && first.importedAt != second.importedAt)
        {
            return first.importedAt > second.importedAt;
        }
        if (sortKey == 2 && first.bytes != second.bytes)
        {
            return first.bytes > second.bytes;
        }
        return collator.compare(first.relativePath, second.relativePath) < 0;
    });
    renderAssets();
}

void MainWindow::renderAssets()
{
    auto list = ui->assetList;
    {
        const QSignalBlocker blocked(list);
        static_cast<LibraryThumbnails *>(list->itemDelegate())->clear();
        list->clear();
        const bool grid = ui->assetViewMode->currentIndex() == 0;
        list->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
        list->setFlow(grid ? QListView::LeftToRight : QListView::TopToBottom);
        list->setWrapping(grid);
        list->setMovement(QListView::Static);
        list->setIconSize(grid ? QSize(124, 96) : QSize(42, 36));
        list->setGridSize(grid ? QSize(148, 132) : QSize{});
        for (int index = 0; index < catalog.assets.size(); ++index)
        {
            const auto &asset = catalog.assets[index];
            auto item = new QListWidgetItem(QFileInfo(asset.relativePath).fileName(), list);
            item->setData(Qt::UserRole, index);
            item->setToolTip(QDir::toNativeSeparators(asset.relativePath));
            item->setSizeHint(grid ? QSize(142, 124) : QSize(0, 49));
            if (selectedAssets.contains(asset.id))
            {
                item->setSelected(true);
                if (!list->currentItem()) list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
            }
        }
    }
    updateAssetSelection();
}

const LibraryAsset *MainWindow::activeAsset() const
{
    const auto item = ui->assetList->currentItem();
    if (!item)
    {
        return nullptr;
    }
    const int index = item->data(Qt::UserRole).toInt();
    return index >= 0 && index < catalog.assets.size() ? &catalog.assets[index] : nullptr;
}

void MainWindow::updateAssetSelection()
{
    selectedAssets.clear();
    for (const auto item : ui->assetList->selectedItems())
    {
        selectedAssets.insert(catalog.assets[item->data(Qt::UserRole).toInt()].id);
    }
    const auto count = QLocale().toString(catalog.assets.size());
    ui->assetCountLabel->setText(selectedAssets.isEmpty() ? tr("%1 张素材").arg(count)
        : tr("%1 张素材 · 已选 %2").arg(count, QLocale().toString(selectedAssets.size())));
    ui->exportSelectedButton->setEnabled(!selectedAssets.isEmpty() && !busy());
}

void MainWindow::openAsset()
{
    const auto asset = activeAsset();
    if (!asset) return;
    const auto path = catalog.assetPath(*asset);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto pixmap = QPixmap::fromImage(reader.read());
    if (pixmap.isNull())
    {
        QMessageBox::warning(this, windowTitle(), tr("无法打开素材：%1").arg(QDir::toNativeSeparators(asset->relativePath)));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QFileInfo(path).fileName());
    dialog.resize(940, 700);
    auto layout = new QVBoxLayout(&dialog);
    auto scene = new QGraphicsScene(&dialog);
    const auto item = scene->addPixmap(pixmap);
    item->setTransformationMode(Qt::SmoothTransformation);
    auto view = new QGraphicsView(scene, &dialog);
    view->setDragMode(QGraphicsView::ScrollHandDrag);
    layout->addWidget(view, 1);
    auto toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(tr("%1 × %2 px").arg(pixmap.width()).arg(pixmap.height()), &dialog));
    toolbar->addStretch();
    auto fit = new QPushButton(tr("适合画布"), &dialog);
    auto actual = new QPushButton(tr("100%"), &dialog);
    auto close = new QPushButton(tr("关闭"), &dialog);
    const auto fitImage = [view, item] { view->fitInView(item, Qt::KeepAspectRatio); };
    connect(fit, &QPushButton::clicked, &dialog, fitImage);
    connect(actual, &QPushButton::clicked, &dialog, [view, item] {
        view->resetTransform();
        view->centerOn(item);
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    toolbar->addWidget(fit);
    toolbar->addWidget(actual);
    toolbar->addWidget(close);
    layout->addLayout(toolbar);
    QTimer::singleShot(0, &dialog, fitImage);
    dialog.exec();
}

void MainWindow::importAssets()
{
    if (busy())
    {
        return;
    }
    const auto source = QFileDialog::getExistingDirectory(this, tr("选择要导入的图片目录"));
    if (source.isEmpty())
    {
        return;
    }
    bool append = !catalog.databasePath.isEmpty();
    auto destination = catalog.databasePath;
    if (!append)
    {
        destination = QFileDialog::getSaveFileName(this, tr("指定新图库目录名称"),
            QDir(QFileInfo(source).absolutePath()).filePath("mosaicraft-library"), tr("所有文件 (*)"),
            nullptr, QFileDialog::DontConfirmOverwrite);
        if (destination.isEmpty())
        {
            return;
        }
        destination = QFileInfo(destination).absoluteFilePath();
        const QFileInfo info(destination);
        const auto database = QDir(destination).filePath("mosaicraft.db");
        if (QFileInfo(database).isFile())
        {
            append = true;
            destination = database;
        }
        else if (info.exists() &&
                 (!info.isDir() || !QDir(destination).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()))
        {
            QMessageBox::warning(this, windowTitle(), tr("新图库必须写入新目录或空目录。"));
            return;
        }
    }
    const auto root = append ? QFileInfo(destination).absolutePath() : destination;
    if (directoryContains(source, existingAncestor(root)) || directoryContains(root, source))
    {
        QMessageBox::warning(this, windowTitle(), tr("图片来源目录与图库目录不能重叠。"));
        return;
    }
    if (append && QMessageBox::question(this, tr("追加素材"),
            tr("追加到此图库？入库后可能重新排列图库内的文件编号。\n%1").arg(QDir::toNativeSeparators(root)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }
    closePreviewSession();
    runJob(Job::Import, buildLibraryArgs(source, destination, append), destination);
}

QStringList MainWindow::buildLibraryArgs(const QString &source, const QString &destination, bool append) const
{
    const auto options = libraryBuildOptions(settings);
    QStringList args{"library", append ? "add" : "build", QFileInfo(source).absoluteFilePath(), append ? "-l" : "-o", destination};
    if (!append)
    {
        args << "--size" << QString("%1x%2").arg(options.size.width()).arg(options.size.height());
    }
    if (options.recursive) args << "-r";
    args << "--jpg-quality" << QString::number(options.jpegQuality) << "--png-compression" << QString::number(options.pngLevel);
    if (options.webpQuality > 0) args << "--webp-quality" << QString::number(options.webpQuality);
    appendExecutionArgs(args, true);
    return args;
}

void MainWindow::exportSelectedAssets()
{
    const auto folder = QFileDialog::getExistingDirectory(this, tr("将所选素材复制到"));
    if (folder.isEmpty())
    {
        return;
    }
    if (directoryContains(catalog.root, folder))
    {
        QMessageBox::warning(this, windowTitle(), tr("请选择图库之外的目录，避免复制出未索引的文件。"));
        return;
    }
    int copied = 0;
    int skipped = 0;
    for (const auto &item : catalog.assets)
    {
        if (!selectedAssets.contains(item.id))
        {
            continue;
        }
        const auto path = catalog.assetPath(item);
        const auto destination = QDir(folder).filePath(QFileInfo(item.relativePath).fileName());
        if (!path.isEmpty() && QFile::copy(path, destination))
        {
            ++copied;
        }
        else
        {
            ++skipped;
        }
    }
    QMessageBox::information(this, tr("导出所选素材"), tr("已复制 %1 张。跳过 %2 张（同名、缺失或不可写）。").arg(copied).arg(skipped));
}

void MainWindow::useCurrentLibrary()
{
    if (busy() || activeDocument < 0 || catalog.databasePath.isEmpty()) return;
    auto &document = documents[activeDocument];
    if (!document.manifest.isEmpty()) return;
    const auto current = QFileInfo(libraryDatabasePath(document.library)).canonicalFilePath();
    if (current != catalog.databasePath)
    {
        if (!document.edits.isEmpty() &&
            QMessageBox::question(this, windowTitle(), tr("切换图库并清除当前工程的手动换图记录？"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        {
            return;
        }
        if (editorOwner == document.id) closePreviewSession();
        document.library = catalog.databasePath;
        document.edits = {};
        document.layout.clear();
        document.preview = {};
        document.reference = {};
        document.previewArgs.clear();
        document.dirty = true;
        document.view = ViewMode::Target;
        showDocument(activeDocument);
    }
    switchWorkspace(Workspace::Edit);
}
