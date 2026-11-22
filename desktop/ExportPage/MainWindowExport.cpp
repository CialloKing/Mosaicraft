#include "MainWindow.h"

#include "ImagePreviewView.h"
#include "LibraryLocation.h"
#include "PreviewImage.h"
#include "ui_MainWindow.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QUrl>

#include <type_traits>
#include <utility>

namespace
{
void initializeCombo(QComboBox *control, const QStringList &values)
{
    for (int index = 0; index < values.size(); ++index)
    {
        control->setItemData(index, values[index]);
    }
}

QString dimensions(int width, int height)
{
    return QString("%1x%2").arg(width).arg(height);
}

}

void MainWindow::setupExport()
{
    ui->exportSplitter->setStretchFactor(1, 1);
    ui->exportSplitter->setSizes({355, 1050});
    ui->exportRightSplitter->setStretchFactor(0, 1);
    ui->exportRightSplitter->setSizes({570, 230});
    for (auto label : {ui->summaryLibrary, ui->summaryState})
    {
        label->setTextFormat(Qt::PlainText);
    }
    initializeCombo(ui->outputMode, {"image", "html", "deepzoom", "tiles"});
    initializeCombo(ui->outputFormat, {"png", "jpg", "webp", "tiff"});
    initializeCombo(ui->writeMode, {"auto", "stream", "batch"});

    fields += {ui->outputMode,
               ui->outputPath, ui->outputFormat, ui->outputQuality, ui->outputPng, ui->writeMode, ui->exportThumbnail,
               ui->allowOverwrite, ui->targetBlend, ui->brightnessJitter, ui->analyzeExport, ui->benchmarkExport,
               ui->monitorExport};

    ui->exportProgress->hide();
    auto view = ui->exportPreview;
    connect(ui->exportZoomIn, &QToolButton::clicked, view, &ImagePreviewView::zoomIn);
    connect(ui->exportZoomOut, &QToolButton::clicked, view, &ImagePreviewView::zoomOut);
    connect(ui->exportZoomFit, &QToolButton::clicked, view, &ImagePreviewView::fitImage);
    connect(ui->exportZoomActual, &QToolButton::clicked, view, &ImagePreviewView::actualSize);
    connect(view, &ImagePreviewView::zoomChanged, this, [this](qreal scale) {
        ui->exportZoomLevel->setText(scale > 0 ? QLocale().toString(scale * 100, 'f', scale < 0.1 ? 1 : 0) + "%" : QString{});
    });
    connect(ui->chooseOutputButton, &QPushButton::clicked, this, &MainWindow::chooseOutput);
    connect(ui->startExportButton, &QPushButton::clicked, this, &MainWindow::exportDocument);
    connect(ui->revealExportButton, &QPushButton::clicked, this, [this] {
        if (activeDocument < 0) return;
        const auto path = documents[activeDocument].outputLocation;
        if (!path.isEmpty())
        {
            const QFileInfo info(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
        }
    });
}

QString MainWindow::suggestedOutput() const
{
    if (activeDocument < 0) return {};
    const auto &document = documents[activeDocument];
    const QFileInfo origin(document.target);
    auto folder = settings.value("general/outputFolder").toString();
    if (folder.isEmpty())
    {
        folder = origin.isFile() ? origin.absolutePath() : QDir::homePath();
    }
    auto base = origin.completeBaseName();
    base = base.isEmpty() ? QStringLiteral("mosaic") : base + "_mosaic";
    const auto mode = ui->outputMode->currentData().toString();
    if (mode == "image")
    {
        base += "." + ui->outputFormat->currentData().toString();
    }
    else if (mode != "html")
    {
        base += "_" + mode;
    }
    return QDir(folder).filePath(base);
}

void MainWindow::chooseOutput()
{
    const auto initial = ui->outputPath->text().isEmpty() ? suggestedOutput() : ui->outputPath->text();
    const bool image = ui->outputMode->currentData().toString() == "image";
    const auto destination = QFileDialog::getSaveFileName(this, image ? tr("保存成品图片") : tr("指定输出目录名称"),
        initial, image ? tr("图片 (*.png *.jpg *.jpeg *.webp *.tif *.tiff);;所有文件 (*)") : tr("所有文件 (*)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (!destination.isEmpty())
    {
        ui->outputPath->setText(QDir::toNativeSeparators(destination));
    }
}

void MainWindow::updateOutputControls()
{
    const auto mode = ui->outputMode->currentData().toString();
    const bool image = mode == "image";
    const bool html = mode == "html";
    const bool portable = activeDocument >= 0 && !documents[activeDocument].manifest.isEmpty();
    const auto format = ui->outputFormat->currentData().toString();
    const bool quality = !html && (!image || format == "jpg" || format == "webp");
    const bool png = image && format == "png";
    ui->encodingCard->setVisible(!html);
    ui->formatLabel->setVisible(image);
    ui->outputFormat->setVisible(image);
    ui->qualityLabel->setVisible(quality);
    ui->qualityLabel->setText(image && format == "webp" ? tr("WebP 质量") : tr("JPEG 质量"));
    ui->outputQuality->setVisible(quality);
    ui->pngLabel->setVisible(png);
    ui->outputPng->setVisible(png);
    ui->writeModeLabel->setVisible(image);
    ui->writeMode->setVisible(image);
    ui->exportThumbnail->setVisible(image);
    ui->analyzeExport->setVisible(!portable);
    ui->monitorExport->setVisible(!portable);
    ui->outputPath->setPlaceholderText(QDir::toNativeSeparators(suggestedOutput()));
    ui->exportControls->setEnabled(!busy() && activeDocument >= 0 &&
                                   QFileInfo(documents[activeDocument].target).isFile());
}

void MainWindow::updateSummaries()
{
    ui->summaryCanvas->setText(QStringLiteral("—"));
    ui->summaryLibrary->setText(QStringLiteral("—"));
    if (activeDocument < 0) return;
    const auto &document = documents[activeDocument];
    const bool portable = !document.manifest.isEmpty();
    const qint64 columns = portable ? document.projectGrid.width()
        : (qint64(ui->targetWidth->value()) + ui->matchWidth->value() - 1) / ui->matchWidth->value();
    const qint64 rows = portable ? document.projectGrid.height()
        : (qint64(ui->targetHeight->value()) + ui->matchHeight->value() - 1) / ui->matchHeight->value();
    if (columns > 0 && rows > 0)
    {
        ui->summaryCanvas->setText(tr("%1 × %2 px").arg(QLocale().toString(columns * ui->tileWidth->value()),
                                                        QLocale().toString(rows * ui->tileHeight->value())));
    }
    const auto library = document.library.isEmpty() && !defaultLibraryDatabase(settings).isEmpty()
        ? defaultLibraryDirectory(settings) : document.library;
    ui->summaryLibrary->setText(portable ? tr("包内素材")
                                        : (library.isEmpty() ? tr("默认图库") : QDir::toNativeSeparators(library)));
}

void MainWindow::refreshExportPreview()
{
    const auto document = activeDocument < 0 ? nullptr : &documents[activeDocument];
    if (document && document->preview) ui->exportPreview->setPreview(document->preview);
    else
    {
        auto image = document ? loadPreviewImage(document->resultImage, {}) : QPixmap{};
        if (document && image.isNull() && !document->resultImage.isEmpty())
            image = loadPreviewImage(document->resultImage, {4096, 4096});
        ui->exportPreview->setImage(image);
    }
    for (auto button : {ui->exportZoomIn, ui->exportZoomOut, ui->exportZoomFit, ui->exportZoomActual})
        button->setEnabled(ui->exportPreview->hasImage());
    ui->revealExportButton->setEnabled(document && !document->outputLocation.isEmpty());
}

// 表单值只收集一次；只有真正启动 CLI 才需要将其转成字符串参数。
struct MainWindow::ExportOptions
{
    bool structured = false;
    QJsonObject request;
    QStringList args;

    template <typename T>
    void set(const char *option, const char *field, const T &value)
    {
        if (structured)
        {
            request.insert(field, QJsonValue(value));
        }
        else if constexpr (std::is_same_v<T, QString>)
        {
            args << option << value;
        }
        else
        {
            args << option << QString::number(value);
        }
    }

    void size(const char *option, const char *widthField, const char *heightField, int width, int height)
    {
        if (structured)
        {
            request.insert(widthField, width);
            request.insert(heightField, height);
        }
        else
        {
            args << option << dimensions(width, height);
        }
    }

    void flag(const char *option, const char *field, bool enabled)
    {
        if (!enabled)
        {
            return;
        }
        if (structured)
        {
            request.insert(field, true);
        }
        else
        {
            args << option;
        }
    }
};

QStringList MainWindow::buildExportArgs(const Document &document, const QString &destination, bool preview) const
{
    ExportOptions options;
    collectExportOptions(options, document, destination, preview);
    return std::move(options.args);
}

QJsonObject MainWindow::buildEditorRequest(const Document &document, const QString &destination, bool preview) const
{
    ExportOptions options{true};
    collectExportOptions(options, document, destination, preview);
    return std::move(options.request);
}

void MainWindow::collectExportOptions(ExportOptions &options, const Document &document, const QString &destination,
                                      bool preview) const
{
    const bool portable = !document.manifest.isEmpty();
    const auto mode = preview ? (portable ? QStringLiteral("deepzoom") : QStringLiteral("image"))
                              : ui->outputMode->currentData().toString();
    const bool image = mode == "image";
    const bool html = mode == "html";
    if (options.structured)
    {
        options.request.insert("inputPath", document.target);
    }
    else
    {
        options.args = {portable ? "render" : "create", portable ? document.manifest : document.target};
    }
    options.set("-o", "outputPath", destination);
    options.set("--mode", "outputMode", mode);
    if (!portable)
    {
        const auto library = documentLibrary(document);
        if (!library.isEmpty())
        {
            options.set("-l", "dbPath", QFileInfo(library).absoluteFilePath());
        }
        options.size("--match-tile", "tileW", "tileH", ui->matchWidth->value(), ui->matchHeight->value());
        options.set("--target-size", "targetSize", dimensions(ui->targetWidth->value(), ui->targetHeight->value()));
        options.set("--candidates", "candidates", ui->candidates->value());
        options.set("--random-top", "topNRandom", ui->randomTop->value());
        options.set("--l-range", "lRange", ui->lightnessRange->value());
        options.set("--neighbor-window", "neighborWindow", ui->neighborWindow->value());
        options.set("--neighbor-penalty", "neighborPenalty", ui->neighborPenalty->value());
        options.set("--reuse-penalty", "usePenalty", ui->reusePenalty->value());
        options.set("--lab-weight", "labWeight", ui->labWeight->value());
        options.set("--grid-weight", "gridWeight", ui->gridWeight->value());
        options.set("--tiny-weight", "tinyWeight", ui->tinyWeight->value());
        options.set("--edge-weight", "edgeWeight", ui->edgeWeight->value());
        options.set("--score-mode", "scoreMode", ui->scoreMode->currentData().toString());
        options.flag("--adaptive-weights", "adaptiveWeights", ui->adaptiveWeights->isChecked());
        options.flag("--analyze", "analyze", ui->analyzeExport->isChecked());
        options.flag("--resource-monitor", "resourceMonitor", !preview && ui->monitorExport->isChecked());
        const auto seed = ui->seedEdit->text();
        if (!seed.isEmpty())
        {
            bool numeric = false;
            const auto value = seed.toLongLong(&numeric);
            if (options.structured && numeric)
            {
                options.set("--seed", "seed", value);
            }
            else
            {
                options.set("--seed", "seed", seed);
            }
        }
    }
    collectExecutionOptions(options, !portable);
    options.size("--output-tile", "nativeTileW", "nativeTileH", ui->tileWidth->value(), ui->tileHeight->value());
    options.set("--target-blend", "overlayStrength", ui->targetBlend->value());
    if (!html)
    {
        const auto strength = ui->brightnessJitter->value();
        options.set("--brightness-jitter", "colorStrength", strength);
        if (options.structured)
        {
            options.request.insert("colorAdjust", strength != 0);
        }
    }
    if (preview)
    {
        if (portable)
        {
            options.set("--quality", "quality", 100);
        }
        return;
    }
    options.flag("--benchmark", "benchmark", ui->benchmarkExport->isChecked());
    options.flag("--overwrite", "overwrite", ui->allowOverwrite->isChecked());
    if (image)
    {
        const auto format = ui->outputFormat->currentData().toString();
        options.set("--format", "format", format);
        options.set("--write-mode", "writeMode", ui->writeMode->currentData().toString());
        options.flag("--thumbnail", "generatePreview", ui->exportThumbnail->isChecked());
        if (format == "png")
        {
            options.set("--png-compression", "pngLevel", ui->outputPng->value());
        }
        if (format == "jpg" || format == "webp")
        {
            options.set("--quality", "quality", ui->outputQuality->value());
        }
    }
    else if (!html)
    {
        options.set("--quality", "quality", ui->outputQuality->value());
    }
}

void MainWindow::exportDocument()
{
    if (busy() || activeDocument < 0 || !QFileInfo(documents[activeDocument].target).isFile()) return;
    auto &document = documents[activeDocument];
    const auto seed = ui->seedEdit->text();
    if (document.manifest.isEmpty() && !seed.isEmpty() && seed.toULongLong() > 4294967295ULL)
    {
        QMessageBox::warning(this, windowTitle(), tr("随机种子需在 0 到 4294967295 之间。"));
        return;
    }
    auto destination = ui->outputPath->text().trimmed();
    destination = QFileInfo(destination.isEmpty() ? suggestedOutput() : destination).absoluteFilePath();
    if (ui->allowOverwrite->isChecked() && settings.value("preferences/confirmOverwrite", true).toBool() &&
        QMessageBox::question(this, tr("确认导出"), tr("允许替换最终编码路径上的兼容输出？\n%1").arg(QDir::toNativeSeparators(destination)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }
    if (!document.edits.isEmpty() && !previewCurrent())
    {
        pendingExport = destination;
        generatePreview();
        if (!busy()) pendingExport.clear();
        return;
    }
    runExport(destination);
}

void MainWindow::runExport(const QString &destination)
{
    const auto &document = documents[activeDocument];
    if (previewCurrent())
    {
        sendEditorRequest({{"op", "export"}, {"request", buildEditorRequest(document, destination)},
                           {"destination", destination}});
    }
    else
    {
        runJob(Job::Export, buildExportArgs(document, destination), destination);
    }
}

void MainWindow::collectExecutionOptions(ExportOptions &options, bool useDevice) const
{
    options.set("--threads", "threads", qBound(0, settings.value("preferences/threads", 0).toInt(), 256));
    if (useDevice)
    {
        options.set("--backend", "backend", settings.value("preferences/backend", "auto").toString());
        const auto device = settings.value("preferences/device").toString();
        if (!device.isEmpty())
        {
            options.set("--device", "device", device);
        }
    }
}

void MainWindow::finishExport(const JobState &completed, bool ok, const QJsonObject &body, const QString &message)
{
    auto &document = documents[completed.documentIndex];
    document.resultMessage = ok ? tr("导出完成") : tr("导出失败：%1").arg(message);
    if (ok)
    {
        document.resultImage.clear();
        document.outputLocation = completed.destination;
        QString thumbnail;
        for (const auto value : body["artifacts"].toArray())
        {
            const auto artifact = value.toObject();
            const auto path = artifact["path"].toString();
            if (artifact["kind"].toString() == "image")
            {
                document.resultImage = path;
                document.outputLocation = path;
            }
            if (artifact["id"].toString() == "preview")
            {
                thumbnail = path;
            }
        }
        if (document.resultImage.isEmpty() && QFileInfo(thumbnail).isFile())
        {
            document.resultImage = thumbnail;
        }
        const auto runtime = body["runtime"].toObject();
        document.resultSize = {runtime["width"].toInt(), runtime["height"].toInt()};
        const auto backend = runtime["featureBackend"].toString();
        const auto device = runtime["device"].toString();
        if (!backend.isEmpty() && backend != "none")
        {
            document.resultMessage += tr(" · 后端 %1").arg(backend);
        }
        if (!device.isEmpty() && device != "none")
        {
            document.resultMessage += " / " + device;
        }
    }
    ui->summaryState->setText(document.resultMessage);
    if (ok && document.resultSize.isValid())
    {
        ui->summaryCanvas->setText(tr("%1 × %2 px（实际）").arg(document.resultSize.width()).arg(document.resultSize.height()));
    }
    if (ok && document.view != ViewMode::Target)
    {
        refreshCanvas();
    }
    refreshExportPreview();
}
