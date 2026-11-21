#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextCursor>
#include <QUrl>
#include <QVersionNumber>

#include <utility>

namespace
{
constexpr qint64 responseLimit = 64LL * 1024 * 1024;
const QUrl releasePage("https://github.com/CialloKing/Mosaicraft/releases/latest");

QString dimensions(int width, int height)
{
    return QString("%1x%2").arg(width).arg(height);
}

}

void MainWindow::setupExport()
{
    ui->exportProgress->hide();
    ui->exportPreview->installEventFilter(this);
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
    connect(process, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = process->readAllStandardOutput();
        if (job.response.size() <= responseLimit)
        {
            job.response.append(bytes);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        auto text = QString::fromUtf8(process->readAllStandardError());
        text.replace('\r', '\n');
        job.diagnostics = (job.diagnostics + text).right(16000);
        ui->taskLog->moveCursor(QTextCursor::End);
        ui->taskLog->insertPlainText(text);
        ui->taskLog->ensureCursorVisible();
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            finishJob(false, tr("无法启动 Mosaicraft CLI：%1").arg(process->errorString()));
        }
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const auto bytes = process->readAllStandardOutput();
        if (job.response.size() <= responseLimit)
        {
            job.response.append(bytes);
        }
        finishJob(status == QProcess::NormalExit && exitCode == 0);
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
    ui->startExportButton->setEnabled(!busy() && activeDocument >= 0 &&
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
    ui->summaryLibrary->setText(portable ? tr("包内素材")
                                        : (document.library.isEmpty() ? tr("默认图库") : QDir::toNativeSeparators(document.library)));
}

void MainWindow::showExportImage()
{
    if (exportImage.isNull())
    {
        ui->exportPreview->clear();
    }
    else
    {
        const auto size = ui->exportPreview->contentsRect().size() - QSize(16, 16);
        ui->exportPreview->setPixmap(size.isValid() ? exportImage.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                                   : exportImage);
    }
}

void MainWindow::refreshExportPreview()
{
    const auto document = activeDocument < 0 ? nullptr : &documents[activeDocument];
    exportImage = document ? document->preview : QPixmap{};
    const bool generated = !exportImage.isNull();
    if (document && !generated)
    {
        exportImage = loadPreviewImage(document->resultImage, {1920, 1440});
    }
    ui->exportPreviewBadge->setText(!generated && !exportImage.isNull() ? tr("最近一次导出") : tr("成品预览"));
    showExportImage();
    ui->revealExportButton->setEnabled(document && !document->outputLocation.isEmpty());
}

QStringList MainWindow::buildExportArgs(const Document &document, const QString &destination, bool preview) const
{
    const bool portable = !document.manifest.isEmpty();
    const auto mode = preview ? QStringLiteral("image") : ui->outputMode->currentData().toString();
    const bool image = mode == "image";
    const bool html = mode == "html";
    QStringList args{portable ? "render" : "create", portable ? document.manifest : document.target, "-o", destination,
                     "--mode", mode};
    const auto value = [&args](const QString &option, auto number) { args << option << QString::number(number); };
    if (!portable)
    {
        if (!document.library.isEmpty())
        {
            args << "-l" << QFileInfo(document.library).absoluteFilePath();
        }
        args << "--match-tile" << dimensions(ui->matchWidth->value(), ui->matchHeight->value());
        args << "--target-size" << dimensions(ui->targetWidth->value(), ui->targetHeight->value());
        value("--candidates", ui->candidates->value());
        value("--random-top", ui->randomTop->value());
        value("--l-range", ui->lightnessRange->value());
        value("--neighbor-window", ui->neighborWindow->value());
        value("--neighbor-penalty", ui->neighborPenalty->value());
        value("--reuse-penalty", ui->reusePenalty->value());
        value("--lab-weight", ui->labWeight->value());
        value("--grid-weight", ui->gridWeight->value());
        value("--tiny-weight", ui->tinyWeight->value());
        value("--edge-weight", ui->edgeWeight->value());
        args << "--score-mode" << ui->scoreMode->currentData().toString();
        if (ui->adaptiveWeights->isChecked()) args << "--adaptive-weights";
        if (ui->analyzeExport->isChecked()) args << "--analyze";
        if (!preview && ui->monitorExport->isChecked()) args << "--resource-monitor";
        if (!ui->seedEdit->text().isEmpty()) args << "--seed" << ui->seedEdit->text();
    }
    appendExecutionArgs(args, !portable);
    auto width = ui->tileWidth->value();
    auto height = ui->tileHeight->value();
    if (preview && portable && document.projectGrid.isValid())
    {
        const auto longest = qMax(double(width) * document.projectGrid.width(), double(height) * document.projectGrid.height());
        const auto scale = qMin(1.0, 1800 / longest);
        width = qMax(1, qRound(width * scale));
        height = qMax(1, qRound(height * scale));
    }
    args << "--output-tile" << dimensions(width, height);
    value("--target-blend", ui->targetBlend->value());
    if (!html) value("--brightness-jitter", ui->brightnessJitter->value());
    if (preview)
    {
        if (portable) args << "--format" << "png" << "--png-compression" << "1" << "--overwrite";
        return args;
    }
    if (ui->benchmarkExport->isChecked()) args << "--benchmark";
    if (ui->allowOverwrite->isChecked()) args << "--overwrite";
    if (image)
    {
        const auto format = ui->outputFormat->currentData().toString();
        args << "--format" << format << "--write-mode" << ui->writeMode->currentData().toString();
        if (ui->exportThumbnail->isChecked()) args << "--thumbnail";
        if (format == "png") value("--png-compression", ui->outputPng->value());
        if (format == "jpg" || format == "webp") value("--quality", ui->outputQuality->value());
    }
    else if (!html)
    {
        value("--quality", ui->outputQuality->value());
    }
    return args;
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
    if (document.manifest.isEmpty() && document.library.isEmpty() && !catalog.databasePath.isEmpty())
    {
        document.library = catalog.databasePath;
        document.dirty = true;
        updateTab();
        updateSummaries();
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
    const auto args = buildExportArgs(documents[activeDocument], destination);
    if (previewCurrent())
    {
        sendEditorRequest({{"op", "export"}, {"args", QJsonArray::fromStringList(args)}, {"destination", destination}});
    }
    else
    {
        runJob(Job::Export, args, destination);
    }
}

void MainWindow::appendExecutionArgs(QStringList &args, bool useDevice) const
{
    args << "--threads" << QString::number(qBound(0, settings.value("preferences/threads", 0).toInt(), 256));
    if (useDevice)
    {
        args << "--backend" << settings.value("preferences/backend", "auto").toString();
        const auto device = settings.value("preferences/device").toString();
        if (!device.isEmpty())
        {
            args << "--device" << device;
        }
    }
}

QString MainWindow::resolveCli() const
{
    const auto configured = settings.value("general/cli").toString();
    if (!configured.isEmpty())
    {
        const QFileInfo file(configured);
        return file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString{};
    }
#ifdef Q_OS_WIN
    const auto filename = QStringLiteral("mosaicraft.exe");
#else
    const auto filename = QStringLiteral("mosaicraft");
#endif
    const QFileInfo adjacent(QDir(QCoreApplication::applicationDirPath()).filePath(filename));
    return adjacent.isFile() && adjacent.isExecutable() ? adjacent.absoluteFilePath() : QStandardPaths::findExecutable(filename);
}

void MainWindow::runJob(Job type, const QStringList &args, const QString &destination)
{
    if (busy()) return;
    const auto cli = resolveCli();
    if (cli.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("未找到 Mosaicraft CLI。请先编译 CLI 或在「设置 → 通用设置」选择程序。"));
        return;
    }
    job = {type, activeDocument, destination, {}, {}};
    ui->taskLog->clear();
    updateEditorControls();
    updateOutputControls();
    QString message;
    if (type == Job::Export)
    {
        ui->exportProgress->show();
        ui->summaryState->setText(tr("正在处理…"));
        message = tr("正在导出作品…");
    }
    else
    {
        message = type == Job::Import ? tr("正在导入素材…") : tr("正在生成预览图…");
    }
    statusBar()->showMessage(message);
    process->start(cli, args + QStringList{"--json"}, QIODevice::ReadOnly);
}

void MainWindow::finishJob(bool processSucceeded, const QString &failure)
{
    if (job.type == Job::None)
    {
        return;
    }
    const auto completed = std::exchange(job, JobState{});
    const auto body = QJsonDocument::fromJson(completed.response).object();
    bool ok = processSucceeded && body["ok"].toBool();
    auto message = failure.isEmpty() ? body["message"].toString() : failure;
    if (message.isEmpty())
    {
        message = completed.diagnostics.trimmed().isEmpty() ? tr("未收到有效的 CLI 结果。") : completed.diagnostics.trimmed().right(1400);
    }
    ui->exportProgress->hide();
    if (completed.type == Job::PortablePreview && ok)
    {
        auto &document = documents[completed.documentIndex];
        auto path = completed.destination;
        for (const auto value : body["artifacts"].toArray())
        {
            const auto artifact = value.toObject();
            if (artifact["kind"].toString() == "image") path = artifact["path"].toString(path);
        }
        const auto preview = loadPreviewImage(path, {1800, 1800});
        ok = !preview.isNull();
        if (ok)
        {
            document.preview = preview;
            document.view = ViewMode::Result;
            refreshCanvas();
            refreshExportPreview();
        }
        else
        {
            message = tr("无法读取生成的预览图。");
        }
    }

    if (completed.type == Job::Export)
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
            if (QFileInfo(thumbnail).isFile())
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
    else if (completed.type == Job::Import && ok)
    {
        loadLibrary(completed.destination);
    }
    if (ok)
    {
        const auto done = completed.type == Job::Export ? tr("导出完成")
            : (completed.type == Job::Import ? tr("素材导入完成") : tr("预览图已生成"));
        statusBar()->showMessage(done, 6000);
    }
    else
    {
        statusBar()->showMessage(tr("任务失败"), 6000);
        ui->taskLog->appendPlainText(message);
        QMessageBox::warning(this, windowTitle(), message);
    }
    updateEditorControls();
    updateOutputControls();
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
    statusBar()->showMessage(tr("正在检查更新…"));
    QNetworkRequest request(QUrl("https://api.github.com/repos/CialloKing/Mosaicraft/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mosaicraft Desktop");
    request.setTransferTimeout(10000);
    auto reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        ui->actionCheckUpdates->setEnabled(true);
        statusBar()->clearMessage();
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
