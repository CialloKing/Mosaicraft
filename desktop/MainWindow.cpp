#include "MainWindow.h"
#include "LibraryLocation.h"
#include "core/ManagedLibrary.h"
#include "Dialogs.h"
#include "EditPage/DimensionEdit.h"
#include "ui_MainWindow.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <algorithm>

namespace
{
constexpr auto projectSuffix = ".mscproj";
constexpr auto projectType = "mosaicraft-desktop-project";

const QUrl releasePage("https://github.com/CialloKing/Mosaicraft/releases/latest");

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
    if (limit.isValid() && !limit.isEmpty() && size.isValid() &&
        (size.width() > limit.width() || size.height() > limit.height()))
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
    recentFiles = settings.value("general/recentFiles").toStringList();
    setupMenu();
    setupEditor();
    setupExport();
    setupLogWindow();
    setupDocumentOptions();
    setupJobProcess();
    setupPreviewSession();
    setupTaskDialog();
    showDocument(-1);
    appendLog(tr("就绪"));
    QTimer::singleShot(0, this, &MainWindow::updateLogWindow);
}

MainWindow::~MainWindow()
{
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning)
    {
        process->kill();
        process->waitForFinished(1000);
    }
    editor->disconnect(this);
    closePreviewSession();
}

void MainWindow::setupMenu()
{
    ui->menuBar->setNativeMenuBar(false);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows"))
    {
        setupWindowsTitleBar();
    }
#endif
    connect(ui->actionNew, &QAction::triggered, this, &MainWindow::newDocument);
    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::chooseDocument);
    connect(ui->actionSave, &QAction::triggered, this, [this] { saveDocument(activeDocument, false); });
    connect(ui->actionSaveAs, &QAction::triggered, this, [this] { saveDocument(activeDocument, true); });
    connect(ui->actionExport, &QAction::triggered, this, [this] { switchWorkspace(Workspace::Export); });
    const auto openSettings = [this](SettingsPage page) {
        const auto previous = libraryMaintenanceRequest();
        if (editSettings(this, settings, page, libraryStatus))
        {
            updateRecentMenu();
            updateEditorControls();
            updateOutputControls();
            updateSummaries();
            appendLog(tr("设置已保存"));
            updateLogWindow();
            if (page == SettingsPage::Library || previous != libraryMaintenanceRequest())
            {
                checkLibrary();
            }
        }
    };
    connect(ui->actionGeneralSettings, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::General); });
    connect(ui->actionPreferences, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Preferences); });
    connect(ui->actionLanguage, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Language); });
    connect(ui->actionLibrarySettings, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Library); });
    connect(ui->actionDeveloperOptions, &QAction::triggered, this, [openSettings] { openSettings(SettingsPage::Developer); });
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAbout);
    connect(ui->actionCheckUpdates, &QAction::triggered, this, &MainWindow::checkUpdates);
    connect(ui->editWorkspaceButton, &QToolButton::clicked, this, [this] { switchWorkspace(Workspace::Edit); });
    connect(ui->exportWorkspaceButton, &QToolButton::clicked, this, [this] { switchWorkspace(Workspace::Export); });
    updateRecentMenu();
}

void MainWindow::switchWorkspace(Workspace workspace)
{
    ui->workspaces->setCurrentIndex(static_cast<int>(workspace));
    const QList<QToolButton *> buttons{ui->editWorkspaceButton, ui->exportWorkspaceButton};
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

    const auto directory = defaultLibraryDirectory(settings);
    const auto chosen = QFileDialog::getExistingDirectory(this, tr("选择工程素材库（取消则使用默认素材库）"), directory);
    const bool useDefault = chosen.isEmpty() || QFileInfo(chosen).canonicalFilePath() == QFileInfo(directory).canonicalFilePath();
    const auto database = useDefault ? QString{} : QDir(chosen).absoluteFilePath(mosaicraft::managed_library::databaseFile);
    if (!useDefault && !QFileInfo(database).isFile())
    {
        QMessageBox::warning(this, windowTitle(), tr("该目录没有 mosaicraft.db。可在「设置 → 素材库设置」选择原始素材目录并建库。"));
        return;
    }
    if (openPath(path) && !useDefault)
    {
        const auto fallback = defaultLibraryDatabase(settings);
        if (fallback.isEmpty() || QFileInfo(database).canonicalFilePath() != QFileInfo(fallback).canonicalFilePath())
        {
            documents[activeDocument].library = database;
        }
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
    refreshPreviewMemory();
    applyOptions(document ? document->options : defaultOptions);
    if (document) documents[activeDocument].selectedTile = -1;
    ui->candidateList->clear();
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
    appendLog(tr("工程已保存"));
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
        const auto restore = [&](const char *field, const QJsonValue &value) {
            if (!value.isUndefined() && !document.options.contains(field)) document.options.insert(field, value);
        };
        const auto restoreSize = [&](const char *width, const char *height, QSize size) {
            if (size.isEmpty()) return;
            restore(width, size.width());
            restore(height, size.height());
        };
        restoreSize("targetWidth", "targetHeight", sizeFromArray(manifest["targetPreparation"].toObject()["resize"]));
        restoreSize("matchWidth", "matchHeight", sizeFromArray(grid["matchTile"]));
        const auto appearance = manifest["appearance"].toObject();
        restore("targetBlend", appearance["targetBlend"]);
        restore("brightnessJitter", appearance["brightnessJitter"]);
        const auto seed = manifest["seed"].toInteger(-1);
        if (seed >= 0 && seed <= 4294967295LL) restore("seedEdit", QString::number(seed));
    }
    if (document.target.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("该工程没有目标图。"));
        return false;
    }
    const bool restorePreview = !document.manifest.isEmpty();
    addDocument(std::move(document));
    rememberFile(absolute);
    if (restorePreview) generatePreview();
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
        event->ignore();
        if (!closing) stopTasks();
        return;
    }
    for (int index = 0; index < documents.size(); ++index)
    {
        if (!confirmClose(index))
        {
            event->ignore();
            if (libraryCheckPending) QTimer::singleShot(0, this, &MainWindow::checkLibrary);
            return;
        }
    }
    logWindow->hide();
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

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (object == ui->workspaceRoot && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
    {
        taskDialog->setGeometry(ui->workspaceRoot->geometry());
    }
    return handleCanvasEvent(object, event) || QMainWindow::eventFilter(object, event);
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

void MainWindow::setupDocumentOptions()
{
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
    updateEditorAspectRatios();
}

void MainWindow::formChanged()
{
    if (activeDocument < 0)
    {
        return;
    }
    auto &document = documents[activeDocument];
    const auto savedBlend = document.options.value("targetBlend");
    document.options = captureOptions();
    if (!document.manifest.isEmpty() && sender() != ui->targetBlend && savedBlend.isDouble())
    {
        document.options.insert("targetBlend", savedBlend);
    }
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

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("关于 Mosaicraft"),
        tr("Mosaicraft Desktop %1\n\nQt Widgets 本地桌面工作区。\n使用 Mosaicraft CLI 建库、匹配和导出。")
            .arg(QCoreApplication::applicationVersion()));
}

void MainWindow::checkUpdates()
{
    ui->actionCheckUpdates->setEnabled(false);
    appendLog(tr("正在检查更新…"));
    QNetworkRequest request(QUrl("https://api.github.com/repos/CialloKing/Mosaicraft/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mosaicraft Desktop");
    request.setTransferTimeout(10000);
    auto reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        ui->actionCheckUpdates->setEnabled(true);
        if (closing) return;
        if (reply->error() != QNetworkReply::NoError)
        {
            QMessageBox::warning(this, tr("检查更新"), tr("无法检查更新：%1").arg(reply->errorString()));
            return;
        }
        auto version = QJsonDocument::fromJson(reply->readAll()).object()["tag_name"].toString();
        if (version.startsWith('v')) version.remove(0, 1);
        const auto latest = QVersionNumber::fromString(version);
        const auto current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if (latest.isNull())
        {
            QMessageBox::warning(this, tr("检查更新"), tr("没有收到可识别的发布版本。"));
        }
        else if (latest > current)
        {
            if (QMessageBox::question(this, tr("检查更新"), tr("Mosaicraft 已发布 %1。打开发布页？").arg(version)) == QMessageBox::Yes)
            {
                QDesktopServices::openUrl(releasePage);
            }
        }
        else
        {
            QMessageBox::information(this, tr("检查更新"), tr("没有发现更新的 Mosaicraft 发布版本。"));
        }
    });
}
