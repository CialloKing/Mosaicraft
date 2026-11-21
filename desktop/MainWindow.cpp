#include "MainWindow.h"
#include "Dialogs.h"
#include "ui_MainWindow.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalBlocker>
#include <QUrl>

#include <algorithm>

namespace
{
constexpr auto projectSuffix = ".mscproj";
constexpr auto projectType = "mosaicraft-desktop-project";

void initializeCombo(QComboBox *control, const QStringList &values)
{
    for (int index = 0; index < values.size(); ++index)
    {
        control->setItemData(index, values[index]);
    }
}

QJsonObject readDocument(const QString &path, qint64 sizeLimit)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() <= sizeLimit
        ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

QSize sizeFromArray(const QJsonValue &value)
{
    const auto array = value.toArray();
    return array.size() == 2 ? QSize(array[0].toInt(), array[1].toInt()) : QSize{};
}

bool isImage(const QFileInfo &file)
{
    static const QStringList suffixes{"png", "jpg", "jpeg", "webp", "tif", "tiff", "bmp"};
    return file.isFile() && (suffixes.contains(file.suffix().toLower()) || QImageReader(file.absoluteFilePath()).canRead());
}

QSize imageDimensions(const QString &path)
{
    QImageReader reader(path);
    auto size = reader.size();
    if (size.isValid() && reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
    {
        size.transpose();
    }
    return size;
}

QString originalFormat(const QString &path)
{
    const auto imageFormat = QString::fromLatin1(QImageReader::imageFormat(path));
    const auto format = imageFormat.isEmpty() ? QFileInfo(path).suffix().toLower() : imageFormat;
    if (format == "jpeg") return QStringLiteral("jpg");
    if (format == "tif") return QStringLiteral("tiff");
    return QStringList{"jpg", "webp", "tiff"}.contains(format) ? format : QStringLiteral("png");
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

QString nativePath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}
}

QPixmap loadPreviewImage(const QString &path, QSize limit, QSize *sourceSize)
{
    if (path.isEmpty())
    {
        if (sourceSize)
        {
            *sourceSize = {};
        }
        return {};
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    auto size = reader.size();
    if (size.isValid() && (size.width() > limit.width() || size.height() > limit.height()))
    {
        reader.setScaledSize(size.scaled(limit, Qt::KeepAspectRatio));
    }
    const auto image = reader.read();
    if (sourceSize)
    {
        if (size.isValid() && reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        {
            size.transpose();
        }
        *sourceSize = size.isValid() ? size : image.size();
    }
    return QPixmap::fromImage(image);
}

MainWindow::MainWindow(QSettings &settings, QWidget *parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>()), settings(settings),
      canvasScene(new QGraphicsScene(this)), compareScene(new QGraphicsScene(this)),
      network(new QNetworkAccessManager(this)), process(new QProcess(this)), editor(new QProcess(this))
{
    ui->setupUi(this);
    setAcceptDrops(true);
    ui->menuBar->setNativeMenuBar(false);
    ui->editorSplitter->setStretchFactor(0, 1);
    ui->editorSplitter->setSizes({1060, 370});
    ui->exportSplitter->setStretchFactor(1, 1);
    ui->exportSplitter->setSizes({355, 1050});
    ui->exportRightSplitter->setStretchFactor(0, 1);
    ui->exportRightSplitter->setSizes({570, 230});
    recentFiles = settings.value("general/recentFiles").toStringList();
    setupMenu();
    setupEditor();
    setupExport();
    setupLibrary();
    setupPreviewSession();
    showDocument(-1);
    statusBar()->showMessage(tr("就绪"));
}

MainWindow::~MainWindow()
{
    editor->disconnect(this);
    closePreviewSession();
}

void MainWindow::setupMenu()
{
    connect(ui->actionNew, &QAction::triggered, this, &MainWindow::newDocument);
    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::chooseDocument);
    connect(ui->actionSave, &QAction::triggered, this, [this] { saveDocument(activeDocument, false); });
    connect(ui->actionSaveAs, &QAction::triggered, this, [this] { saveDocument(activeDocument, true); });
    connect(ui->actionExport, &QAction::triggered, this, [this] { switchWorkspace(Workspace::Export); });
    const auto openSettings = [this](SettingsPage page) {
        if (editSettings(this, settings, page))
        {
            updateRecentMenu();
            updateEditorControls();
            updateOutputControls();
            statusBar()->showMessage(tr("设置已保存"), 3500);
        }
    };
    connect(ui->actionGeneralSettings, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::General); });
    connect(ui->actionPreferences, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Preferences); });
    connect(ui->actionLanguage, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Language); });
    connect(ui->actionLibrarySettings, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Library); });
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAbout);
    connect(ui->actionCheckUpdates, &QAction::triggered, this, &MainWindow::checkUpdates);
    connect(ui->editWorkspaceButton, &QToolButton::clicked, this, [this] { switchWorkspace(Workspace::Edit); });
    connect(ui->exportWorkspaceButton, &QToolButton::clicked, this, [this] { switchWorkspace(Workspace::Export); });
    connect(ui->libraryWorkspaceButton, &QToolButton::clicked, this, [this] { switchWorkspace(Workspace::Library); });
    updateRecentMenu();
}

void MainWindow::setupEditor()
{
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
    for (auto label : {ui->summaryLibrary, ui->galleryPath, ui->summaryState})
    {
        label->setTextFormat(Qt::PlainText);
    }
    ui->seedEdit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,10}"), ui->seedEdit));
    initializeCombo(ui->scoreMode, {"fast", "compatible"});
    initializeCombo(ui->outputMode, {"image", "html", "deepzoom", "tiles"});
    initializeCombo(ui->outputFormat, {"png", "jpg", "webp", "tiff"});
    initializeCombo(ui->writeMode, {"auto", "stream", "batch"});

    connectRatio(ui->targetWidth, ui->targetHeight, ui->targetLock);
    connectRatio(ui->matchWidth, ui->matchHeight, ui->matchLock);
    connectRatio(ui->tileWidth, ui->tileHeight, ui->tileLock);
    fields = {ui->matchWidth, ui->matchHeight, ui->matchLock, ui->targetWidth, ui->targetHeight, ui->targetLock,
              ui->tileWidth, ui->tileHeight, ui->tileLock, ui->candidates, ui->randomTop, ui->lightnessRange,
              ui->scoreMode, ui->seedEdit, ui->labWeight, ui->gridWeight, ui->tinyWeight, ui->edgeWeight,
              ui->neighborWindow, ui->neighborPenalty, ui->reusePenalty, ui->adaptiveWeights, ui->outputMode,
              ui->outputPath, ui->outputFormat, ui->outputQuality, ui->outputPng, ui->writeMode, ui->exportThumbnail,
              ui->allowOverwrite, ui->targetBlend, ui->brightnessJitter, ui->analyzeExport, ui->benchmarkExport,
              ui->monitorExport};
    defaultOptions = captureOptions();
    for (auto field : fields)
    {
        if (const auto dimension = qobject_cast<DimensionEdit *>(field))
        {
            connect(dimension, &DimensionEdit::valueChanged, this, &MainWindow::formChanged);
        }
        else if (const auto edit = qobject_cast<QLineEdit *>(field))
        {
            connect(edit, &QLineEdit::textChanged, this, &MainWindow::formChanged);
        }
        else if (const auto spin = qobject_cast<QSpinBox *>(field))
        {
            connect(spin, &QSpinBox::valueChanged, this, &MainWindow::formChanged);
        }
        else if (const auto decimal = qobject_cast<QDoubleSpinBox *>(field))
        {
            connect(decimal, &QDoubleSpinBox::valueChanged, this, &MainWindow::formChanged);
        }
        else if (const auto combo = qobject_cast<QComboBox *>(field))
        {
            connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::formChanged);
        }
        else if (const auto button = qobject_cast<QAbstractButton *>(field))
        {
            connect(button, &QAbstractButton::toggled, this, &MainWindow::formChanged);
        }
    }

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

void MainWindow::switchWorkspace(Workspace workspace)
{
    ui->workspaces->setCurrentIndex(static_cast<int>(workspace));
    const QList<QToolButton *> buttons{ui->editWorkspaceButton, ui->exportWorkspaceButton, ui->libraryWorkspaceButton};
    buttons[static_cast<int>(workspace)]->setChecked(true);
    if (workspace == Workspace::Export)
    {
        updateSummaries();
        refreshExportPreview();
    }
}

void MainWindow::newDocument()
{
    if (busy()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("选择目标图"), {},
        tr("图片 (*.png *.jpg *.jpeg *.webp *.tif *.tiff *.bmp);;所有文件 (*)"));
    if (path.isEmpty()) return;

    const auto library = QFileDialog::getExistingDirectory(this, tr("选择工程素材库（可取消并使用当前素材库）"), catalog.root);
    if (!library.isEmpty() && !QFileInfo(libraryDatabasePath(library)).isFile())
    {
        QMessageBox::warning(this, windowTitle(), tr("该目录没有 mosaicraft.db，请先在素材库页导入图片。"));
        return;
    }
    if (openPath(path) && !library.isEmpty())
    {
        documents[activeDocument].library = libraryDatabasePath(library);
        updateSummaries();
    }
}

void MainWindow::addDocument(Document document)
{
    document.id = nextDocumentId++;
    const auto size = imageDimensions(document.target);
    auto &options = document.options;
    if (document.projectTile.isValid() && !options.contains("tileWidth"))
    {
        options["tileWidth"] = document.projectTile.width();
        options["tileHeight"] = document.projectTile.height();
    }
    if (size.isValid())
    {
        if (!options.contains("targetWidth")) options["targetWidth"] = size.width();
        if (!options.contains("targetHeight")) options["targetHeight"] = size.height();
    }
    if (options["outputFormat"].toString().isEmpty())
    {
        options["outputFormat"] = originalFormat(document.target);
    }
    const auto caption = documentName(document);
    documents.append(std::move(document));
    const QSignalBlocker blocked(ui->projectTabs);
    const int index = ui->projectTabs->addTab(caption);
    ui->projectTabs->setCurrentIndex(index);
    showDocument(index);
    switchWorkspace(Workspace::Edit);
}

QString MainWindow::documentName(const Document &document) const
{
    QString name = tr("未命名工程");
    if (!document.file.isEmpty())
    {
        name = QFileInfo(document.file).completeBaseName();
    }
    else if (!document.manifest.isEmpty())
    {
        name = QFileInfo(document.manifest).dir().dirName();
    }
    else if (!document.target.isEmpty())
    {
        name = QFileInfo(document.target).completeBaseName();
    }
    return document.dirty ? name + " *" : name;
}

void MainWindow::showDocument(int index)
{
    activeDocument = index >= 0 && index < documents.size() ? index : -1;
    const auto document = activeDocument >= 0 ? &documents[activeDocument] : nullptr;
    applyOptions(document ? document->options : defaultOptions);
    if (document) documents[activeDocument].selectedTile = -1;
    ui->candidateList->clear();
    ui->summaryState->setText(document && !document->resultMessage.isEmpty() ? document->resultMessage : tr("等待导出"));
    updateEditorControls();
    updateOutputControls();
    updateSummaries();
    refreshCanvas();
    refreshExportPreview();
    if (document) updateTab();
    else setWindowTitle(QStringLiteral("Mosaicraft"));
}

void MainWindow::updateTab()
{
    if (activeDocument < 0)
    {
        return;
    }
    const auto caption = documentName(documents[activeDocument]);
    ui->projectTabs->setTabText(activeDocument, caption);
    ui->projectTabs->setTabToolTip(activeDocument, documents[activeDocument].file);
    setWindowTitle(tr("%1 — Mosaicraft").arg(caption));
}

bool MainWindow::saveDocument(int index, bool saveAs)
{
    if (index < 0 || index >= documents.size() || busy())
    {
        return false;
    }
    auto &document = documents[index];
    auto path = document.file;
    if (saveAs || path.isEmpty())
    {
        path = QFileDialog::getSaveFileName(this, tr("保存工程"), path, tr("Mosaicraft 工程 (*.mscproj)"));
        if (path.isEmpty())
        {
            return false;
        }
        if (!path.endsWith(projectSuffix, Qt::CaseInsensitive))
        {
            path += projectSuffix;
        }
        const auto duplicate = std::find_if(documents.cbegin(), documents.cend(), [&](const Document &other) {
            return &other != &document && other.file == path;
        });
        if (duplicate != documents.cend())
        {
            QMessageBox::warning(this, windowTitle(), tr("该工程已在另一个标签页打开。"));
            return false;
        }
    }
    QSaveFile file(path);
    const auto body = QJsonDocument(QJsonObject{{"type", projectType}, {"schemaVersion", 1},
                                                {"target", document.target}, {"portableProject", document.manifest},
                                                {"library", document.library}, {"layout", document.layout},
                                                {"edits", document.edits}, {"options", document.options}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit())
    {
        QMessageBox::warning(this, windowTitle(), tr("无法保存工程。%1").arg(file.errorString()));
        return false;
    }
    document.file = QFileInfo(path).absoluteFilePath();
    document.dirty = false;
    ui->projectTabs->setTabText(index, documentName(document));
    rememberFile(document.file);
    updateTab();
    statusBar()->showMessage(tr("工程已保存"), 3500);
    return true;
}

void MainWindow::chooseDocument()
{
    if (busy()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("打开工程或目标图"), {},
        tr("Mosaicraft 工程和图片 (*.mscproj project.json index.html *.png *.jpg *.jpeg *.webp *.tif *.tiff *.bmp);;所有文件 (*)"));
    if (!path.isEmpty())
    {
        openPath(path);
    }
}

bool MainWindow::openPath(const QString &path)
{
    if (busy())
    {
        return false;
    }
    const QFileInfo info(path);
    const auto filename = info.fileName();
    const bool desktopProject = !info.isDir() && filename.endsWith(projectSuffix, Qt::CaseInsensitive);
    const bool portable = info.isDir() || filename == "project.json" || filename == "index.html";
    if (!desktopProject && !portable && !isImage(info))
    {
        QMessageBox::warning(this, windowTitle(), tr("无法打开此文件。请选择 .mscproj 工程、便携工程清单或图片。"));
        return false;
    }

    Document document;
    const auto absolute = info.absoluteFilePath();
    if (desktopProject)
    {
        const auto body = readDocument(absolute, 16LL * 1024 * 1024);
        if (body["type"].toString() != projectType || body["schemaVersion"].toInt() != 1)
        {
            QMessageBox::warning(this, windowTitle(), tr("工程文件无效或版本不受支持。"));
            return false;
        }
        document.file = absolute;
        document.target = body["target"].toString();
        document.manifest = body["portableProject"].toString();
        document.options = body["options"].toObject();
        document.library = body["library"].toString();
        document.edits = body["edits"].toArray();
        document.layout = body["layout"].toString();
    }
    else if (portable)
    {
        document.manifest = info.isDir() ? QDir(absolute).filePath("project.json") : info.dir().filePath("project.json");
    }
    else
    {
        document.target = absolute;
        document.dirty = true;
        document.library = catalog.databasePath;
    }
    for (int index = 0; index < documents.size(); ++index)
    {
        const auto &other = documents[index];
        if ((desktopProject && other.file == absolute) || (portable && other.manifest == document.manifest && other.file.isEmpty()))
        {
            ui->projectTabs->setCurrentIndex(index);
            rememberFile(absolute);
            switchWorkspace(Workspace::Edit);
            return true;
        }
    }
    if (!document.manifest.isEmpty())
    {
        const auto manifest = readDocument(document.manifest, 128LL * 1024 * 1024);
        const auto target = containedFile(QFileInfo(document.manifest).absolutePath(), manifest["target"].toString());
        if (manifest["type"].toString() != "mosaicraft-project" || target.isEmpty())
        {
            QMessageBox::warning(this, windowTitle(), tr("未找到有效的便携工程或包内目标图。ZIP 必须先解压。"));
            return false;
        }
        const auto grid = manifest["grid"].toObject();
        document.target = target;
        document.projectGrid = {grid["columns"].toInt(), grid["rows"].toInt()};
        document.projectTile = sizeFromArray(manifest["outputTile"]);
        if (!desktopProject)
        {
            const auto appearance = manifest["appearance"].toObject();
            document.options["targetBlend"] = appearance["targetBlend"].toDouble();
            document.options["brightnessJitter"] = appearance["brightnessJitter"].toDouble();
        }
    }
    if (document.target.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("该工程没有目标图。"));
        return false;
    }
    addDocument(std::move(document));
    rememberFile(absolute);
    return true;
}

bool MainWindow::confirmClose(int index)
{
    if (!documents[index].dirty)
    {
        return true;
    }
    const auto reply = QMessageBox::question(this, windowTitle(), tr("要保存“%1”的更改吗？").arg(documentName(documents[index])),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return reply == QMessageBox::Discard || (reply == QMessageBox::Save && saveDocument(index, false));
}

void MainWindow::closeDocument(int index)
{
    if (busy() || index < 0 || index >= documents.size() || !confirmClose(index))
    {
        return;
    }
    if (documents[index].id == editorOwner) closePreviewSession();
    const int previous = activeDocument;
    {
        const QSignalBlocker blocked(ui->projectTabs);
        documents.removeAt(index);
        ui->projectTabs->removeTab(index);
    }
    if (documents.isEmpty())
    {
        showDocument(-1);
    }
    else
    {
        const int current = qMin(index < previous ? previous - 1 : previous, documents.size() - 1);
        const QSignalBlocker blocked(ui->projectTabs);
        ui->projectTabs->setCurrentIndex(current);
        showDocument(current);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (busy())
    {
        statusBar()->showMessage(tr("任务仍在运行，请等待完成后关闭。"), 5000);
        event->ignore();
        return;
    }
    for (int index = 0; index < documents.size(); ++index)
    {
        if (!confirmClose(index))
        {
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::rememberFile(const QString &path)
{
    recentFiles.removeAll(path);
    recentFiles.prepend(path);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
    const int limit = qBound(0, settings.value("general/recentLimit", 10).toInt(), 30);
    recentFiles.removeIf([](const QString &path) { return path.endsWith(".mosaicdraft", Qt::CaseInsensitive); });
    recentFiles = recentFiles.mid(0, limit);
    settings.setValue("general/recentFiles", recentFiles);
    ui->recentMenu->clear();
    if (recentFiles.isEmpty())
    {
        ui->recentMenu->addAction(ui->actionNoRecent);
        return;
    }
    for (const auto &path : recentFiles)
    {
        auto display = nativePath(path);
        if (display.size() > 90)
        {
            display = display.left(32) + QStringLiteral("…") + display.right(52);
        }
        auto action = ui->recentMenu->addAction(display.replace("&", "&&"));
        action->setToolTip(nativePath(path));
        connect(action, &QAction::triggered, this, [this, path] {
            if (!QFileInfo::exists(path))
            {
                QMessageBox::information(this, windowTitle(), tr("该文件已移动或删除。"));
                recentFiles.removeAll(path);
                updateRecentMenu();
                return;
            }
            openPath(path);
        });
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!busy() && ui->workspaces->currentWidget() == ui->editPage &&
        event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 &&
        event->mimeData()->urls().front().isLocalFile())
    {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!busy() && event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 &&
        openPath(event->mimeData()->urls().front().toLocalFile()))
    {
        event->acceptProposedAction();
    }
}

QJsonObject MainWindow::captureOptions() const
{
    QJsonObject options;
    for (auto field : fields)
    {
        const auto key = field->objectName();
        if (const auto dimension = qobject_cast<DimensionEdit *>(field)) options[key] = dimension->value();
        else if (const auto edit = qobject_cast<QLineEdit *>(field)) options[key] = edit->text();
        else if (const auto spin = qobject_cast<QSpinBox *>(field)) options[key] = spin->value();
        else if (const auto decimal = qobject_cast<QDoubleSpinBox *>(field)) options[key] = decimal->value();
        else if (const auto combo = qobject_cast<QComboBox *>(field)) options[key] = combo->currentData().toString();
        else if (const auto button = qobject_cast<QAbstractButton *>(field)) options[key] = button->isChecked();
    }
    return options;
}

void MainWindow::applyOptions(const QJsonObject &options)
{
    for (auto field : fields)
    {
        const QSignalBlocker blocked(field);
        const auto key = field->objectName();
        const auto value = options.value(key);
        const auto fallback = defaultOptions.value(key);
        if (const auto dimension = qobject_cast<DimensionEdit *>(field)) dimension->setValue(value.toInt(fallback.toInt()));
        else if (const auto edit = qobject_cast<QLineEdit *>(field)) edit->setText(value.toString(fallback.toString()));
        else if (const auto spin = qobject_cast<QSpinBox *>(field)) spin->setValue(value.toInt(fallback.toInt()));
        else if (const auto decimal = qobject_cast<QDoubleSpinBox *>(field)) decimal->setValue(value.toDouble(fallback.toDouble()));
        else if (const auto combo = qobject_cast<QComboBox *>(field))
        {
            const int index = combo->findData(value.toString(fallback.toString()));
            combo->setCurrentIndex(qMax(0, index));
        }
        else if (const auto button = qobject_cast<QAbstractButton *>(field)) button->setChecked(value.toBool(fallback.toBool()));
    }
    recordRatio(ui->targetWidth, ui->targetHeight, ui->targetLock);
    recordRatio(ui->matchWidth, ui->matchHeight, ui->matchLock);
    recordRatio(ui->tileWidth, ui->tileHeight, ui->tileLock);
}

void MainWindow::formChanged()
{
    if (activeDocument < 0)
    {
        return;
    }
    auto &document = documents[activeDocument];
    document.options = captureOptions();
    document.dirty = true;
    if (!previewCurrent() && document.selectedTile >= 0)
    {
        document.selectedTile = -1;
        ui->candidateList->clear();
        refreshCanvas(true);
    }
    updateTab();
    updateEditorControls();
    updateOutputControls();
    updateSummaries();
}
