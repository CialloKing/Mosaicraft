#include "CanvasImageItem.h"
#include "DimensionEdit.h"
#include "MainWindow.h"
#include "PreviewImage.h"
#include "ui_MainWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCache>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <utility>

namespace
{
const auto previewName = QStringLiteral("preview.png");

enum CandidateRole { ImageId = Qt::UserRole, AssetVersion, CurrentImage, AssetPath };

class CandidateDelegate : public QStyledItemDelegate
{
public:
    explicit CandidateDelegate(QListWidget *list) : QStyledItemDelegate(list), list(list) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto cell = option;
        initStyleOption(&cell, index);
        cell.state.setFlag(QStyle::State_Selected, index.data(CurrentImage).toBool());
        const auto path = index.data(AssetPath).toString();
        const auto key = path + '|' + index.data(AssetVersion).toString();
        auto icon = thumbnails.object(key);
        if (!icon)
        {
            const auto pixmap = loadPreviewImage(path, list->iconSize());
            const auto fallback = list->style()->standardIcon(QStyle::SP_FileIcon);
            thumbnails.insert(key, new QIcon(pixmap.isNull() ? fallback : QIcon(pixmap)));
            icon = thumbnails.object(key);
        }
        cell.icon = *icon;
        cell.decorationSize = icon->actualSize(list->iconSize());
        cell.features |= QStyleOptionViewItem::HasDecoration;
        list->style()->drawControl(QStyle::CE_ItemViewItem, &cell, painter, list);
    }

private:
    const QListWidget *list;
    mutable QCache<QString, QIcon> thumbnails{160};
};

QPixmap replyPixmap(const QJsonObject &reply, const QString &member)
{
    QPixmap image;
    image.loadFromData(QByteArray::fromBase64(reply[member].toString().toLatin1()), "JPEG");
    return image;
}

void addImage(QGraphicsScene *scene, CanvasImageItem *item, const QRectF &area)
{
    scene->addItem(item);
    const auto size = item->boundingRect().size();
    item->setTransform(QTransform::fromScale(area.width() / size.width(), area.height() / size.height()));
    scene->setSceneRect(area);
}

void markCurrentCandidate(QListWidget *list, int imageId)
{
    QListWidgetItem *current = nullptr;
    for (int row = 0; row < list->count(); ++row)
    {
        auto item = list->item(row);
        const bool matches = item->data(ImageId).toInt() == imageId;
        item->setData(CurrentImage, matches);
        if (matches) current = item;
    }
    list->setCurrentItem(current);
}

void recordRatio(DimensionEdit *width, DimensionEdit *height, QAbstractButton *lock)
{
    lock->setProperty("aspectRatio", double(width->value()) / height->value());
}

void connectRatio(DimensionEdit *width, DimensionEdit *height, QAbstractButton *lock)
{
    QObject::connect(lock, &QAbstractButton::toggled, lock, [=](bool enabled) {
        if (enabled) recordRatio(width, height, lock);
    });
    const auto link = [lock](DimensionEdit *source, DimensionEdit *other, bool fromWidth) {
        QObject::connect(source, &DimensionEdit::valueChanged, lock, [=](int value) {
            if (!lock->isChecked()) return;
            const auto ratio = lock->property("aspectRatio").toDouble();
            const double scale = fromWidth ? 1 / ratio : ratio;
            const QSignalBlocker blockSource(source);
            const QSignalBlocker blockOther(other);
            const int bounded = qMax(DimensionEdit::minimum, int(qMin(double(value), DimensionEdit::maximum / scale)));
            if (bounded != value) source->setValue(bounded);
            other->setValue(qRound(qMin(double(DimensionEdit::maximum), source->value() * scale)));
        });
    };
    recordRatio(width, height, lock);
    link(width, height, true);
    link(height, width, false);
}
}

void MainWindow::setupEditor()
{
    ui->editorSplitter->setStretchFactor(0, 1);
    ui->editorSplitter->setSizes({1060, 370});
    ui->editorImageView->setScene(canvasScene);
    ui->compareImageView->setScene(compareScene);
    ui->compareSplitter->setStretchFactor(0, 1);
    ui->compareSplitter->setStretchFactor(1, 1);
    for (auto view : {ui->editorImageView, ui->compareImageView})
    {
        view->viewport()->installEventFilter(this);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setTransformationAnchor(QGraphicsView::NoAnchor);
        view->setResizeAnchor(QGraphicsView::NoAnchor);
        view->viewport()->setCursor(Qt::OpenHandCursor);
    }
    ui->seedEdit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,10}"), ui->seedEdit));
    ui->scoreMode->setItemData(0, QStringLiteral("fast"));
    ui->scoreMode->setItemData(1, QStringLiteral("compatible"));

    connectRatio(ui->targetWidth, ui->targetHeight, ui->targetLock);
    connectRatio(ui->matchWidth, ui->matchHeight, ui->matchLock);
    connectRatio(ui->tileWidth, ui->tileHeight, ui->tileLock);
    fields = {ui->matchWidth, ui->matchHeight, ui->matchLock, ui->targetWidth, ui->targetHeight, ui->targetLock,
              ui->tileWidth, ui->tileHeight, ui->tileLock, ui->candidates, ui->randomTop, ui->lightnessRange,
              ui->scoreMode, ui->seedEdit, ui->labWeight, ui->gridWeight, ui->tinyWeight, ui->edgeWeight,
              ui->neighborWindow, ui->neighborPenalty, ui->reusePenalty, ui->adaptiveWeights};

    connect(ui->projectTabs, &QTabBar::currentChanged, this, &MainWindow::showDocument);
    connect(ui->projectTabs, &QTabBar::tabCloseRequested, this, &MainWindow::closeDocument);
    connect(ui->newTabButton, &QToolButton::clicked, this, &MainWindow::newDocument);
    connect(ui->generatePreviewButton, &QPushButton::clicked, this, &MainWindow::generatePreview);
    connect(ui->targetModeButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Target); });
    connect(ui->resultModeButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Result); });
    connect(ui->compareModeButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Compare); });
    connect(ui->inspectorTabs, &QTabWidget::currentChanged, this, [this] {
        if (activeDocument >= 0 && ui->inspectorTabs->currentWidget() == ui->candidatesTab &&
            documents[activeDocument].view == ViewMode::Target && ui->resultModeButton->isEnabled())
        {
            setViewMode(ViewMode::Result);
        }
        refreshCanvas(true);
    });
    connect(ui->candidateList, &QListWidget::itemClicked, this, &MainWindow::replaceTile);
    connect(ui->zoomPicker, &QComboBox::activated, this, [this](int index) {
        if (index == 0)
        {
            fitCanvas();
        }
        else
        {
            setZoom(ui->zoomPicker->itemText(index).chopped(1).toDouble() / 100, false);
        }
    });
    connect(ui->zoomPicker->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        auto value = ui->zoomPicker->currentText().trimmed();
        if (value.endsWith('%')) value.chop(1);
        bool ok = false;
        const auto percent = QLocale().toDouble(value, &ok);
        if (ok && percent > 0) setZoom(percent / 100, false);
        else fitCanvas();
    });
}

void MainWindow::updateEditorAspectRatios()
{
    recordRatio(ui->targetWidth, ui->targetHeight, ui->targetLock);
    recordRatio(ui->matchWidth, ui->matchHeight, ui->matchLock);
    recordRatio(ui->tileWidth, ui->tileHeight, ui->tileLock);
}

bool MainWindow::previewCurrent() const
{
    if (activeDocument < 0) return false;
    const auto &document = documents[activeDocument];
    return document.manifest.isEmpty() && document.id == editorOwner && editor->state() == QProcess::Running &&
           editor->program() == resolveEditorWorker() &&
           document.previewRequest == buildEditorRequest(document, previewName, true);
}

void MainWindow::updateEditorControls()
{
    const auto document = activeDocument < 0 ? nullptr : &documents[activeDocument];
    const bool open = document && QFileInfo(document->target).isFile();
    const bool available = !busy();
    const bool hasResult = open && (document->preview || !document->resultImage.isEmpty());
    const bool portable = document && !document->manifest.isEmpty();
    const bool canReplace = open && !portable && previewCurrent();
    ui->actionNew->setEnabled(available);
    ui->actionOpen->setEnabled(available);
    ui->actionSave->setEnabled(available && document);
    ui->actionSaveAs->setEnabled(available && document);
    ui->actionExport->setEnabled(open && available);
    ui->newTabButton->setEnabled(available);
    ui->projectTabs->setEnabled(available);
    ui->modeRail->setEnabled(open);
    ui->canvasViews->setEnabled(open);
    ui->canvasFooter->setEnabled(open);
    ui->targetModeButton->setEnabled(open);
    ui->resultModeButton->setEnabled(hasResult);
    ui->compareModeButton->setEnabled(hasResult);
    ui->inspectorTabs->setEnabled(open);
    ui->generatePreviewButton->setEnabled(open && available);
    ui->generatePreviewButton->setText(editorOperation == "preview" || job.type == Job::PortablePreview
                                          ? tr("正在生成预览图…") : tr("生成预览图"));
    ui->parameterContent->setEnabled(open && available);
    ui->matchingCard->setEnabled(!portable);
    ui->advancedMatchingCard->setEnabled(!portable);
    for (QWidget *widget : QList<QWidget *>{ui->targetSizeLabel, ui->targetWidth, ui->targetHeight, ui->targetLock,
                                          ui->matchSizeLabel, ui->matchWidth, ui->matchHeight, ui->matchLock})
    {
        widget->setEnabled(!portable);
    }
    const bool manual = !ui->adaptiveWeights->isChecked();
    for (QWidget *widget : QList<QWidget *>{ui->labWeight, ui->gridWeight, ui->tinyWeight, ui->edgeWeight,
                                          ui->neighborWindow, ui->neighborPenalty, ui->reusePenalty,
                                          ui->labLabel, ui->gridLabel, ui->tinyLabel, ui->edgeLabel,
                                          ui->neighborWindowLabel, ui->neighborPenaltyLabel, ui->reusePenaltyLabel})
    {
        widget->setEnabled(manual);
    }
    ui->candidateList->setEnabled(canReplace && available);
    if (portable)
    {
        ui->candidateHint->setText(tr("便携工程使用已固定的素材排列。"));
    }
    else if (!canReplace)
    {
        ui->candidateHint->setText(tr("生成预览图后，点击成品中的子图查看并更换候选素材。"));
    }
    else
    {
        ui->candidateHint->setText(tr("点击成品中的子图，再选择要使用的候选素材。"));
    }
    const int tile = canReplace ? document->selectedTile : -1;
    ui->candidateTitle->setText(tile < 0 ? tr("当前选格")
        : tr("第 %1 行 · 第 %2 列").arg(tile / document->previewGrid.width() + 1)
                                  .arg(tile % document->previewGrid.width() + 1));
}

void MainWindow::setViewMode(ViewMode mode)
{
    if (activeDocument < 0) return;
    documents[activeDocument].view = mode;
    refreshCanvas();
}

void MainWindow::refreshCanvas(bool preserveView)
{
    const auto previousArea = canvasScene->sceneRect();
    canvasScene->clear();
    compareScene->clear();
    const auto document = activeDocument < 0 ? nullptr : &documents[activeDocument];
    const auto mode = document ? document->view : ViewMode::Target;
    const bool comparing = mode == ViewMode::Compare;
    const bool changing = ui->compareImageView->isHidden() == comparing;

    QPixmap left, output;
    std::shared_ptr<PreviewImage> preview;
    if (document)
    {
        if (mode != ViewMode::Target)
        {
            preview = document->preview;
            if (!preview) output = loadPreviewImage(document->resultImage, {});
        }
        if (mode == ViewMode::Target || comparing)
        {
            left = comparing && !document->reference.isNull() ? document->reference
                                                               : loadPreviewImage(document->target, {4096, 4096});
        }
    }
    const auto resultSize = preview ? preview->size() : output.size();
    const bool ready = mode == ViewMode::Result ? !resultSize.isEmpty()
                       : !left.isNull() && (!comparing || !resultSize.isEmpty());
    ui->canvasViews->setCurrentIndex(ready ? 1 : 0);
    ui->compareImageView->setVisible(comparing && ready);
    ui->zoomPicker->setEnabled(ready);
    ui->targetModeButton->setChecked(mode == ViewMode::Target);
    ui->resultModeButton->setChecked(mode == ViewMode::Result);
    ui->compareModeButton->setChecked(comparing);
    if (!ready)
    {
        ui->zoomPicker->setEditText(QStringLiteral("—"));
        ui->canvasInfo->setText(document ? tr("无法预览此图片") : tr("尚未打开图像"));
        return;
    }

    const QRectF area(QPointF{}, mode == ViewMode::Target ? left.size() : resultSize);
    const auto resultItem = [&] { return preview ? new CanvasImageItem(preview) : new CanvasImageItem(output); };
    addImage(canvasScene, mode == ViewMode::Result ? resultItem() : new CanvasImageItem(left), area);
    auto resultScene = canvasScene;
    if (comparing)
    {
        addImage(compareScene, resultItem(), area);
        resultScene = compareScene;
        if (changing)
        {
            const auto half = ui->compareSplitter->width() / 2;
            ui->compareSplitter->setSizes({half, half});
        }
    }
    if (mode != ViewMode::Target && ui->inspectorTabs->currentWidget() == ui->candidatesTab &&
        document->selectedTile >= 0 && previewCurrent())
    {
        const auto grid = document->previewGrid;
        const auto w = area.width() / grid.width();
        const auto h = area.height() / grid.height();
        QPen pen(QColor("#e3e3e3"), 2);
        pen.setCosmetic(true);
        const int column = document->selectedTile % grid.width();
        const int row = document->selectedTile / grid.width();
        const auto x = std::ceil(column * w);
        const auto y = std::ceil(row * h);
        resultScene->addRect(QRectF(QPointF(x, y), QPointF(std::ceil((column + 1) * w), std::ceil((row + 1) * h))), pen);
    }
    ui->canvasInfo->setText(comparing ? tr("对比 · 左侧目标 / 右侧成品")
                                      : (mode == ViewMode::Result ? tr("成品图") : tr("目标图")));
    if (preserveView && previousArea == area && !fitView)
    {
        placeCanvas();
    }
    else
    {
        fitCanvas();
    }
}

void MainWindow::fitCanvas()
{
    const auto area = canvasScene->sceneRect();
    if (ui->canvasViews->currentIndex() != 1 || area.isEmpty()) return;
    auto available = ui->editorImageView->viewport()->size();
    if (!ui->compareImageView->isHidden())
    {
        available = available.boundedTo(ui->compareImageView->viewport()->size());
    }
    canvasCenter = area.center();
    setZoom(qMin((available.width() - 16) / area.width(), (available.height() - 16) / area.height()), true);
}

void MainWindow::setZoom(qreal factor, bool fit)
{
    if (ui->canvasViews->currentIndex() != 1) return;
    fitView = fit;
    zoom = qBound(0.000000001, factor, 16.0);
    const QSignalBlocker block(ui->zoomPicker);
    ui->zoomPicker->setEditText(QLocale().toString(zoom * 100, 'f', 2) + "%");
    placeCanvas();
}

void MainWindow::placeCanvas()
{
    const auto area = canvasScene->sceneRect();
    canvasCenter.setX(qBound(area.left(), canvasCenter.x(), area.right()));
    canvasCenter.setY(qBound(area.top(), canvasCenter.y(), area.bottom()));
    for (auto view : {ui->editorImageView, ui->compareImageView})
    {
        if (view->isHidden()) continue;
        const auto halfWidth = view->viewport()->width() / (2 * zoom);
        const auto halfHeight = view->viewport()->height() / (2 * zoom);
        view->setSceneRect(area.adjusted(-halfWidth, -halfHeight, halfWidth, halfHeight));
        view->setTransform(QTransform::fromScale(zoom, zoom));
        view->centerOn(canvasCenter);
    }
}

bool MainWindow::handleCanvasEvent(QObject *object, QEvent *event)
{
    QGraphicsView *view = nullptr;
    if (object == ui->editorImageView->viewport()) view = ui->editorImageView;
    if (object == ui->compareImageView->viewport()) view = ui->compareImageView;
    if (event->type() == QEvent::Resize)
    {
        if (view)
        {
            QTimer::singleShot(0, this, [this] {
                if (ui->canvasViews->currentIndex() != 1) return;
                if (fitView) fitCanvas();
                else placeCanvas();
            });
        }
    }
    if (!view || ui->canvasViews->currentIndex() != 1 || !view->isEnabled())
    {
        return false;
    }
    switch (event->type())
    {
    case QEvent::Wheel:
    {
        const auto wheel = static_cast<QWheelEvent *>(event);
        const auto factor = std::pow(1.25, wheel->angleDelta().y() / 120.0);
        const auto point = wheel->position().toPoint();
        const auto focus = view->mapToScene(point);
        const auto next = qBound(0.000000001, zoom * factor, 16.0);
        const auto delta = QPointF(point) - QPointF(view->viewport()->rect().center());
        canvasCenter = focus - delta / next;
        setZoom(next, false);
        return true;
    }
    case QEvent::MouseButtonPress:
    {
        const auto mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            dragView = view;
            mouseStart = mouse->position().toPoint();
            panStart = canvasCenter;
            view->viewport()->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        break;
    }
    case QEvent::MouseMove:
        if (dragView == view)
        {
            const auto point = static_cast<QMouseEvent *>(event)->position().toPoint();
            if ((point - mouseStart).manhattanLength() >= QApplication::startDragDistance())
            {
                fitView = false;
                canvasCenter = panStart + QPointF(mouseStart - point) / zoom;
                placeCanvas();
            }
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (dragView == view && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        {
            const auto point = static_cast<QMouseEvent *>(event)->position().toPoint();
            dragView = nullptr;
            view->viewport()->setCursor(Qt::OpenHandCursor);
            if ((point - mouseStart).manhattanLength() < QApplication::startDragDistance())
            {
                selectTile(view, point);
            }
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void MainWindow::selectTile(QGraphicsView *view, const QPoint &point)
{
    if (busy() || !previewCurrent() || ui->inspectorTabs->currentWidget() != ui->candidatesTab) return;
    auto &document = documents[activeDocument];
    if (document.view == ViewMode::Target || (document.view == ViewMode::Compare && view != ui->compareImageView)) return;
    const auto pixel = view->mapToScene(point);
    if (!document.preview) return;
    const auto size = document.preview->size();
    if (pixel.x() < 0 || pixel.y() < 0 || pixel.x() >= size.width() || pixel.y() >= size.height()) return;
    const auto grid = document.previewGrid;
    const int column = int(qint64(pixel.x()) * grid.width() / size.width());
    const int row = int(qint64(pixel.y()) * grid.height() / size.height());
    document.selectedTile = row * grid.width() + column;
    ui->candidateList->clear();
    refreshCanvas(true);
    sendEditorRequest({{"op", "candidates"}, {"position", document.selectedTile}});
}

void MainWindow::replaceTile(QListWidgetItem *item)
{
    if (busy() || !previewCurrent()) return;
    const auto &document = documents[activeDocument];
    const auto id = item->data(ImageId).toInt();
    markCurrentCandidate(ui->candidateList, currentCandidate);
    const auto version = item->data(AssetVersion).toString();
    if (document.selectedTile < 0 || id == currentCandidate || version.isEmpty()) return;
    sendEditorRequest({{"op", "replace"}, {"position", document.selectedTile},
                       {"imageId", id}, {"version", version}});
}

void MainWindow::generatePreview()
{
    if (busy() || activeDocument < 0) return;
    auto &document = documents[activeDocument];
    if (!QFileInfo(document.target).isFile()) return;
    if (!previewFiles) previewFiles = std::make_unique<QTemporaryDir>();
    if (previewFiles->isValid())
    {
        pendingPreview = std::make_shared<QTemporaryDir>(previewFiles->filePath("preview-XXXXXX"));
    }
    if (!pendingPreview || !pendingPreview->isValid())
    {
        pendingPreview.reset();
        QMessageBox::warning(this, windowTitle(), tr("无法创建预览文件目录。"));
        return;
    }
    if (!document.manifest.isEmpty())
    {
        const auto path = pendingPreview->filePath("pyramid");
        runJob(Job::PortablePreview, buildExportArgs(document, path, true), path);
        if (!busy()) pendingPreview.reset();
        return;
    }
    sendEditorRequest({{"op", "preview"},
                       {"request", buildEditorRequest(document, previewName, true)},
                       {"previewDirectory", pendingPreview->path()},
                       {"layout", document.layout}, {"edits", document.edits}});
    if (!busy()) pendingPreview.reset();
}

bool MainWindow::finishPortablePreview(const JobState &completed, bool ok, QString &message)
{
    auto files = std::exchange(pendingPreview, {});
    auto preview = ok ? PreviewImage::openPyramid(std::move(files)) : nullptr;
    if (!preview)
    {
        if (ok) message = tr("无法读取生成的预览图。");
        return false;
    }

    auto &document = documents[completed.documentIndex];
    document.preview = std::move(preview);
    document.view = ViewMode::Result;
    refreshCanvas();
    refreshExportPreview();
    return true;
}

void MainWindow::setupPreviewSession()
{
    ui->candidateList->setLayoutMode(QListView::Batched);
    ui->candidateList->setItemDelegate(new CandidateDelegate(ui->candidateList));
    connect(ui->candidateList, &QListWidget::itemActivated, this, &MainWindow::replaceTile);
    connect(editor, &QProcess::started, this, [this] {
        editor->write(editorCommand);
        editorCommand.clear();
    });
    connect(editor, &QProcess::readyReadStandardOutput, this, [this] {
        editorResponse.append(editor->readAllStandardOutput());
        const auto newline = editorResponse.indexOf('\n');
        if (newline >= 0)
        {
            const auto body = QJsonDocument::fromJson(editorResponse.first(newline)).object();
            editorResponse.remove(0, newline + 1);
            receiveEditorReply(body);
        }
    });
    connect(editor, &QProcess::readyReadStandardError, this, [this] {
        const auto message = QString::fromUtf8(editor->readAllStandardError()).replace('\r', '\n');
        if (!message.trimmed().isEmpty()) ui->taskLog->appendPlainText(message.trimmed());
    });
    connect(editor, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) editorDisconnected(editor->errorString());
    });
    connect(editor, &QProcess::finished, this, [this] {
        editorDisconnected(tr("预览进程已结束。请确认桌面匹配程序与当前版本配套。"));
    });
}

void MainWindow::sendEditorRequest(QJsonObject request)
{
    const auto worker = resolveEditorWorker();
    if (worker.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("未找到桌面匹配程序。请检查 mosaicraft_desktop_worker 是否在桌面程序或 Mosaicraft CLI 的同目录。"));
        return;
    }
    if (editor->state() == QProcess::Running && editor->program() != worker)
    {
        closePreviewSession();
        if (editor->state() != QProcess::NotRunning) return;
    }
    editorRequest = std::move(request);
    editorOperation = editorRequest["op"].toString();
    editorCommand = QJsonDocument(editorRequest).toJson(QJsonDocument::Compact) + '\n';
    if (editorOperation == "preview") statusBar()->showMessage(tr("正在生成预览图…"));
    if (editorOperation == "export")
    {
        ui->exportProgress->show();
        ui->summaryState->setText(tr("正在处理…"));
        statusBar()->showMessage(tr("正在导出作品…"));
    }
    updateEditorControls();
    updateOutputControls();
    if (editor->state() == QProcess::NotRunning)
    {
        editorResponse.clear();
        editor->start(worker);
    }
    else
    {
        editor->write(editorCommand);
        editorCommand.clear();
    }
}

void MainWindow::receiveEditorReply(const QJsonObject &reply)
{
    if (editorOperation.isEmpty() || activeDocument < 0) return;
    auto &document = documents[activeDocument];
    const auto operation = std::exchange(editorOperation, QString{});
    const auto request = std::exchange(editorRequest, QJsonObject{});
    const auto diagnostic = reply["diagnostic"].toObject()["code"].toString();
    if (diagnostic == "preview_stale")
    {
        document.previewRequest = {};
    }
    if (operation == "export")
    {
        if (reply["ok"].toBool() && ui->reusePenalty->value() > 0)
        {
            document.previewRequest = {};
        }
        job = {Job::Export, activeDocument, request["destination"].toString(), QJsonDocument(reply).toJson(), {}};
        finishJob(true);
        return;
    }

    auto message = reply["message"].toString(tr("未收到有效的 CLI 结果。"));
    bool ok = reply["ok"].toBool();
    auto files = operation == "preview" ? std::exchange(pendingPreview, {}) : nullptr;
    if (ok && (operation == "preview" || operation == "replace"))
    {
        const auto image = replyPixmap(reply, "image");
        const QSize grid(reply["columns"].toInt(), reply["rows"].toInt());
        if (operation == "preview")
        {
            const QSize tile(reply["tileWidth"].toInt(), reply["tileHeight"].toInt());
            auto preview = PreviewImage::openMosaic(std::move(files), grid, tile, image);
            ok = bool(preview);
            if (ok) document.preview = std::move(preview);
        }
        else
        {
            ok = document.preview && document.preview->replaceTile(request["position"].toInt(-1),
                quint32(reply["tileKey"].toDouble()), image);
        }
        if (!ok) message = tr("无法读取生成的预览图。");
        if (ok)
        {
            if (operation == "preview")
            {
                editorOwner = document.id;
                document.reference = replyPixmap(reply, "reference");
                document.previewGrid = grid;
                document.layout = reply["layout"].toString();
                document.selectedTile = -1;
                ui->candidateList->clear();
                document.view = ViewMode::Result;
                if (ui->seedEdit->text().isEmpty())
                {
                    const QSignalBlocker block(ui->seedEdit);
                    ui->seedEdit->setText(QString::number(qint64(reply["seed"].toDouble())));
                    document.options = captureOptions();
                    document.dirty = true;
                }
                // 记录实际发出的请求；缺省种子由已完成的预览确定。
                document.previewRequest = request["request"].toObject();
                document.previewRequest.insert("seed", reply["seed"]);
                statusBar()->showMessage(tr("预览图已生成"), 4500);
            }
            else
            {
                auto edit = request;
                edit.remove("op");
                for (qsizetype index = document.edits.size(); index-- > 0;)
                {
                    if (document.edits[index].toObject()["position"] == request["position"]) document.edits.removeAt(index);
                }
                document.edits.append(edit);
                currentCandidate = reply["imageId"].toInt();
                markCurrentCandidate(ui->candidateList, currentCandidate);
                document.dirty = true;
            }
            updateTab();
            refreshCanvas(operation == "replace");
            refreshExportPreview();
        }
    }
    else if (ok && operation == "candidates")
    {
        const auto candidates = reply["candidates"].toArray();
        for (const auto value : candidates)
        {
            const auto candidate = value.toObject();
            const auto path = candidate["path"].toString();
            const auto name = candidate["name"].toString();
            const bool scored = candidate["weight"].isDouble();
            const auto weight = scored ? QLocale().toString(candidate["weight"].toDouble(), 'f', 5) : tr("当前素材");
            auto item = new QListWidgetItem(weight, ui->candidateList);
            item->setSizeHint(QSize(92, 88));
            item->setData(ImageId, candidate["id"].toInt());
            item->setData(AssetVersion, candidate["version"].toString());
            item->setData(AssetPath, path);
            item->setToolTip(scored ? tr("%1 · 匹配权重 %2").arg(name, weight) : name);
            if (path.isEmpty()) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        currentCandidate = reply["imageId"].toInt();
        markCurrentCandidate(ui->candidateList, currentCandidate);
        if (candidates.isEmpty()) statusBar()->showMessage(tr("该子图没有候选素材。"), 4000);
    }
    updateEditorControls();
    updateOutputControls();
    if (!ok)
    {
        if (diagnostic == "preview_stale") refreshCanvas(true);
        statusBar()->showMessage(tr("任务失败"), 4500);
        if (reply["code"].toString() == "saved_edits_stale" &&
            QMessageBox::question(this, windowTitle(), tr("目标图、切块或素材库已变更。清除以前的换图记录并重新生成预览？"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            document.edits = {};
            document.layout.clear();
            document.dirty = true;
            updateTab();
            generatePreview();
            if (!busy()) pendingExport.clear();
            return;
        }
        pendingExport.clear();
        ui->taskLog->appendPlainText(message);
        QMessageBox::warning(this, windowTitle(), message);
    }
    else if (operation == "preview" && !pendingExport.isEmpty())
    {
        runExport(std::exchange(pendingExport, QString{}));
    }
}

void MainWindow::editorDisconnected(const QString &error)
{
    editorOwner = -1;
    editorResponse.clear();
    if (!editorOperation.isEmpty())
    {
        receiveEditorReply({{"ok", false}, {"message", error}});
    }
    else
    {
        updateEditorControls();
        updateOutputControls();
    }
}

void MainWindow::closePreviewSession()
{
    editorOwner = -1;
    if (editor->state() == QProcess::NotRunning) return;
    editor->closeWriteChannel();
    if (!editor->waitForFinished(1000))
    {
        editor->kill();
        editor->waitForFinished(1000);
    }
}
