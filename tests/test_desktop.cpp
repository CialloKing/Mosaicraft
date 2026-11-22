#include "CanvasImageItem.h"
#include "LibraryLocation.h"
#include "MainWindow.h"
#include "PreviewImage.h"
#include "ui_MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QLineF>
#include <QLinearGradient>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>

namespace
{
QImage renderCanvasSample(const QImage &image, QSize size, bool smooth = false)
{
    QGraphicsScene scene;
    auto item = new CanvasImageItem(QPixmap::fromImage(image), smooth);
    scene.addItem(item);
    scene.setSceneRect(item->boundingRect());
    QImage result(size, QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    scene.render(&painter, QRectF(result.rect()), scene.sceneRect(), Qt::IgnoreAspectRatio);
    return result;
}

QImage nativePreviewTile(const PreviewImage &preview, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    preview.paint(&painter, QRectF(QPointF(), QSizeF(size)), 1);
    return image;
}

void scrollPreview(QGraphicsView *view)
{
    const auto position = view->viewport()->rect().center();
    QWheelEvent wheel(position, view->viewport()->mapToGlobal(position), {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &wheel);
}

QImage makeFixture(int index, const QSize &size)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size.width() / 320.0, size.height() / 240.0);
    const int hue = (195 + index * 23) % 360;
    QLinearGradient gradient(0, 0, 320, 240);
    gradient.setColorAt(0, QColor::fromHsl(hue, 100, 37));
    gradient.setColorAt(1, QColor::fromHsl((hue + 45) % 360, 121, 137));
    painter.fillRect(QRect(0, 0, 320, 240), gradient);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromHsl((hue + 118) % 360, 160, 202));
    painter.drawEllipse(QPointF(164 + (index % 4) * 25, 76), 29, 29);
    painter.setBrush(QColor::fromHsl((hue + 14) % 360, 109, 107));
    painter.drawPolygon(QPolygonF{{0, 172}, {69, 113}, {132, 158}, {210, 104}, {320, 161}, {320, 240}, {0, 240}});
    painter.setBrush(QColor::fromHsl(hue, 112, 47));
    painter.drawPolygon(QPolygonF{{0, 207}, {102, 160}, {190, 202}, {262, 145}, {320, 184}, {320, 240}, {0, 240}});
    return image;
}

}

int MainWindow::smokeTest(const QString &captureDirectory)
{
    const auto require = [](bool passed, const char *label) {
        if (!passed) qWarning("Desktop smoke check failed: %s", label);
        return passed;
    };
    if (!captureDirectory.isEmpty() && !require(QDir().mkpath(captureDirectory), "capture directory"))
    {
        return 1;
    }
    const auto capture = [&](const QString &name, QWidget *widget) {
        QApplication::processEvents();
        return captureDirectory.isEmpty() || widget->grab().save(QDir(captureDirectory).filePath(name + ".png"));
    };

    QImage checker(2, 2, QImage::Format_RGB32);
    checker.fill(Qt::black);
    checker.setPixelColor(0, 0, Qt::white);
    const auto enlarged = renderCanvasSample(checker, {10, 10});
    for (int y = 0; y < enlarged.height(); ++y)
    {
        for (int x = 0; x < enlarged.width(); ++x)
        {
            if (!require(enlarged.pixel(x, y) == checker.pixel(x / 5, y / 5), "nearest-neighbor enlargement")) return 1;
        }
    }
    const auto gridded = renderCanvasSample(checker, {12, 12});
    if (!require(gridded.pixelColor(2, 2) == Qt::white && gridded.pixelColor(9, 2) == Qt::black &&
                 gridded.pixelColor(6, 2) != Qt::black && gridded.pixelColor(6, 2) != Qt::white,
                 "pixel grid at 600 percent"))
    {
        return 1;
    }
    const auto smooth = renderCanvasSample(checker, {10, 10}, true);
    if (!require(smooth.pixelColor(4, 2).red() > 0 && smooth.pixelColor(4, 2).red() < 255,
                 "smooth image-viewer enlargement")) return 1;
    QImage stripe(4, 4, QImage::Format_RGB32);
    stripe.fill(Qt::black);
    for (int y = 0; y < stripe.height(); ++y) stripe.setPixelColor(0, y, Qt::white);
    const int filtered = renderCanvasSample(stripe, {1, 1}).pixelColor(0, 0).red();
    if (!require(filtered >= 60 && filtered <= 65, "area-filtered reduction"))
    {
        return 1;
    }

    const auto cli = qEnvironmentVariable("MOSAICRAFT_DESKTOP_SMOKE_CLI");
    const auto scratchPattern = (cli.isEmpty() ? QDir::temp() : QDir::current()).filePath("desktop-smoke-XXXXXX");
    QTemporaryDir scratch(scratchPattern);
    QTranslator english;
    if (!require(english.load(":/i18n/mosaicraft_en.qm") && english.translate("MainWindow", "文件") == "File" &&
                 english.translate("SettingsDialog", "原始素材目录") == "Source image folder",
                 "English translations"))
    {
        return 1;
    }
    const QDir root(scratch.path());
    const auto target = root.filePath(QStringLiteral("目标图 · 蓝调.png"));
    const auto library = root.filePath(QStringLiteral("素材图库"));
    resize(1440, 900);
    if (!require(ui->projectTabs->count() == 0 && !ui->parameterContent->isEnabled() &&
                 !ui->canvasViews->isEnabled() && !ui->generatePreviewButton->isEnabled() &&
                 ui->canvasEmpty->findChildren<QLabel *>().isEmpty() && !ui->exportPreview->hasImage(), "empty previews without placeholders") ||
        !require(menuWidget() == ui->menuBar && ui->workspaceBar->parentWidget() == ui->workspaceRoot &&
                 ui->rootLayout->itemAt(0)->widget() == ui->workspaceBar &&
                 ui->workspaceBarLayout->itemAt(3)->widget() == ui->editWorkspaceButton &&
                 ui->workspaceBarLayout->itemAt(4)->widget() == ui->exportWorkspaceButton &&
                 ui->workspaces->count() == 2 && !findChild<QWidget *>("libraryWorkspaceButton"),
                 "menu and workspace positions with only editor and export") ||
        !require(capture("00-empty", this), "empty editor capture") ||
        !require(scratch.isValid() && makeFixture(0, {960, 720}).save(target) && QDir().mkpath(library), "image fixture") ||
        !require(containedFile(library, "../" + QFileInfo(target).fileName()).isEmpty(), "portable project directory boundary") ||
        !require(openPath(target), "open target") ||
        !require(ui->projectTabs->count() == 1 && ui->canvasViews->currentIndex() == 1, "editor canvas"))
    {
        return 1;
    }

    const auto panel = ui->parameterContent->grab().toImage();
    const auto dividerVisible = [&](const QWidget *divider) {
        const auto scale = panel.devicePixelRatio();
        const int x = panel.width() / 2;
        const int y = static_cast<int>((divider->y() + divider->height() / 2.0) * scale);
        return panel.pixelColor(x, y).lightness() > panel.pixelColor(x, y - qRound(3 * scale)).lightness() + 30;
    };
    if (!require(!panel.isNull() && dividerVisible(ui->basicSettingsSeparator) && dividerVisible(ui->matchingSettingsSeparator),
                 "visible parameter section dividers"))
    {
        return 1;
    }

    if (!require(!QApplication::testAttribute(Qt::AA_DontUseNativeDialogs), "system file dialogs enabled"))
    {
        return 1;
    }

    documents[activeDocument].library = library;
    if (!require(ui->targetWidth->value() == 960 && ui->targetHeight->value() == 720 &&
                 ui->targetLock->isChecked() && ui->targetWidth->text() == "960",
                 "source dimensions and aspect lock"))
    {
        return 1;
    }
    ui->targetWidth->setText("480");
    if (!require(ui->targetHeight->value() == 360, "width follows original aspect"))
    {
        return 1;
    }
    ui->targetHeight->setValue(720);
    if (!require(ui->targetWidth->value() == 960, "height follows original aspect"))
    {
        return 1;
    }
    ui->adaptiveWeights->setChecked(true);
    if (!require(!ui->neighborWindow->isEnabled() && !ui->labWeight->isEnabled(), "automatic weights disable manual controls"))
    {
        return 1;
    }
    ui->adaptiveWeights->setChecked(false);
    ui->matchWidth->setValue(12);
    ui->matchHeight->setValue(16);
    ui->tileWidth->setValue(24);
    ui->tileHeight->setValue(32);
    ui->seedEdit->setText("1024");
    const auto project = root.filePath(QStringLiteral("蓝调作品.mscproj"));
    documents[activeDocument].file = project;
    if (!require(saveDocument(activeDocument, false), "save project"))
    {
        return 1;
    }
    QFile projectFile(project);
    if (!require(projectFile.open(QIODevice::ReadOnly), "read saved project"))
    {
        return 1;
    }
    const auto contents = projectFile.readAll();
    projectFile.close();
    const auto body = QJsonDocument::fromJson(contents).object();
    closeDocument(activeDocument);
    if (!require(body["type"] == "mosaicraft-desktop-project", "project format") ||
        !require(ui->projectTabs->count() == 0 && !ui->parameterContent->isEnabled(), "closing the last tab") ||
        !require(openPath(project) && ui->matchWidth->value() == 12 && ui->seedEdit->text() == "1024", "reopen project options") ||
        !require(ui->projectTabs->count() == 1 && ui->summaryLibrary->text() == QDir::toNativeSeparators(library), "recent project"))
    {
        return 1;
    }
    const auto legacyProject = root.filePath("legacy.mosaicdraft");
    if (!require(QFile::copy(project, legacyProject), "unsupported suffix fixture"))
    {
        return 1;
    }
    recentFiles.prepend(legacyProject);
    updateRecentMenu();
    bool localizedWarning = false;
    QTimer::singleShot(0, this, [&] {
        if (auto dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            const auto button = dialog->button(QMessageBox::Ok);
            localizedWarning = button && button->text() == QStringLiteral("确定");
            dialog->reject();
        }
    });
    if (!require(!recentFiles.contains(legacyProject) && !openPath(legacyProject), "only the new project suffix is accepted") ||
        !require(localizedWarning, "Chinese warning confirmation button"))
    {
        return 1;
    }
    ui->inspectorTabs->setCurrentIndex(1);
    if (!require(ui->inspectorTabs->currentWidget() == ui->candidatesTab, "candidate tab"))
    {
        return 1;
    }
    ui->inspectorTabs->setCurrentIndex(0);
    if (!require(capture("01-editor", this), "editor capture"))
    {
        return 1;
    }

    const auto jpeg = root.filePath("source.jpg");
    if (!require(makeFixture(2, {64, 48}).save(jpeg) && openPath(jpeg) &&
                 ui->outputFormat->currentData() == "jpg" && !ui->outputQuality->isHidden() && ui->outputPng->isHidden(),
                 "source JPEG defaults with corresponding encoding controls"))
    {
        return 1;
    }
    documents[activeDocument].dirty = false;
    closeDocument(activeDocument);
    ui->actionExport->trigger();
    const auto create = buildExportArgs(documents[activeDocument], root.filePath("作品.png"));
    if (!require(ui->workspaces->currentWidget() == ui->exportPage && create.front() == "create" &&
                 create.contains("--match-tile") && create.contains("12x16") && create.contains("--png-compression") &&
                 create.value(create.indexOf("--png-compression") + 1) == "6" &&
                 create.contains("--seed") && !create.contains("--target-upscale") && !create.contains("--lbp-weight") &&
                 !ui->exportPreview->hasImage() && ui->outputQuality->isHidden(), "single image export arguments") ||
        !require(capture("02-export", this), "export capture"))
    {
        return 1;
    }
    ui->outputMode->setCurrentIndex(ui->outputMode->findData("html"));
    const auto html = buildExportArgs(documents[activeDocument], root.filePath("html"));
    if (!require(ui->encodingCard->isHidden() && !ui->brightnessJitter->isHidden() &&
                 !html.contains("--write-mode") && !html.contains("--brightness-jitter") &&
                 !html.contains("--png-compression"), "HTML option filtering"))
    {
        return 1;
    }

    const QList<QAction *> settingActions{ui->actionGeneralSettings, ui->actionPreferences, ui->actionLanguage, ui->actionLibrarySettings};
    for (int index = 0; index < settingActions.size(); ++index)
    {
        bool correctPage = false;
        QTimer::singleShot(0, this, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto pages = dialog ? dialog->findChild<QStackedWidget *>("settingsPages") : nullptr;
            correctPage = pages && pages->currentIndex() == index;
            if (dialog)
            {
                if (index == 1)
                {
                    correctPage = correctPage && !dialog->findChild<QComboBox *>("assetZoomWheel") &&
                                  capture("04-preferences", dialog);
                }
                if (index == 3)
                {
                    correctPage = correctPage && pages->currentWidget()->objectName() == "librarySettingsPage" &&
                                  pages->currentWidget()->findChild<QSpinBox *>("normalizeWidth") &&
                                  dialog->findChild<QLineEdit *>("librarySource") &&
                                  dialog->findChild<QLineEdit *>("libraryDirectory") && capture("05-library-settings", dialog);
                }
                dialog->reject();
            }
        });
        settingActions[index]->trigger();
        if (!require(correctPage, "settings menu page routing"))
        {
            return 1;
        }
    }
    if (cli.isEmpty())
    {
        return 0;
    }
    settings.setValue("general/cli", QFileInfo(cli).absoluteFilePath());
    settings.setValue("preferences/backend", "cpu");
    settings.setValue("preferences/threads", 2);
    const auto waitForWork = [&](const auto &start, bool waitForLibrary = false)
    {
        // 便携预览完成和排队的图库检查启动之间有一个事件循环间隙。
        const auto done = [&]()
        {
            return !busy() && (!waitForLibrary || !libraryCheckPending);
        };
        QEventLoop wait;
        QTimer poll;
        connect(&poll, &QTimer::timeout, &wait, [&] {
            if (auto message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->reject();
            if (done())
            {
                wait.quit();
            }
        });
        poll.start(25);
        QTimer::singleShot(100000, &wait, &QEventLoop::quit);
        start();
        if (!done())
        {
            wait.exec();
        }
        return done();
    };
    const auto source = root.filePath(QStringLiteral("原始素材"));
    const auto built = root.filePath(QStringLiteral("默认素材库"));
    const auto bitmap = QDir(source).filePath("source.bmp");
    const auto second = QDir(source).filePath("second.png");
    if (!require(QDir().mkpath(source) && makeFixture(2, {320, 240}).save(bitmap) &&
                 makeFixture(9, {180, 320}).save(second), "material source fixtures"))
    {
        return 1;
    }
    QTimer::singleShot(0, this, [&] {
        if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        {
            dialog->findChild<QLineEdit *>("librarySource")->setText(source);
            dialog->findChild<QLineEdit *>("libraryDirectory")->setText(built);
            dialog->accept();
        }
    });
    const auto activeLibrary = [&] { return defaultLibraryDatabase(settings); };
    const auto checkLibraryAndWait = [&] { return waitForWork([&] { checkLibrary(); }); };
    const auto indexedImages = [&] {
        return QFileInfo(activeLibrary()).dir().entryList({"*.png"}, QDir::Files);
    };
    if (!require(waitForWork([&] { ui->actionLibrarySettings->trigger(); }) &&
                 settings.value("library/source").toString() == source &&
                 settings.value("library/directory").toString() == built && QFileInfo(activeLibrary()).isFile() &&
                 indexedImages().size() == 2, "saving library settings builds the default library"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    const auto firstLibrary = activeLibrary();
    bool blockedDuringLibraryCheck = false;
    const auto positionBeforeCheck = pos();
    const auto sizeBeforeCheck = size();
    if (!require(waitForWork([&] {
                     checkLibrary();
                     resize(sizeBeforeCheck + QSize(32, 24));
                     move(positionBeforeCheck + QPoint(12, 10));
                     const auto caption = menuBar()->cornerWidget();
                     blockedDuringLibraryCheck = taskDialog->isVisible() && taskDialog->geometry() == ui->workspaceRoot->geometry() &&
                         !ui->workspaceRoot->isEnabled() && !ui->fileMenu->menuAction()->isEnabled() &&
                         isEnabled() && menuBar()->isEnabled() && (!caption || caption->isEnabled()) &&
                         !QApplication::activeModalWidget() && !openPath(target) && capture("10-library-check", this);
                     move(positionBeforeCheck);
                     resize(sizeBeforeCheck);
                 }) && blockedDuringLibraryCheck && taskDialog->isHidden() && ui->workspaceRoot->isEnabled() &&
                 ui->fileMenu->menuAction()->isEnabled() && activeLibrary() == firstLibrary &&
                 libraryStatus == tr("素材库完整性检查通过。"), "startup reuses an unchanged, healthy library"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    const QFileInfo bitmapBefore(bitmap);
    const auto firstGeneration = QFileInfo(firstLibrary).dir();
    QFile indexCache(firstGeneration.filePath("index/lib.ann196"));
    if (!require(firstGeneration.mkpath("index") && indexCache.open(QIODevice::WriteOnly), "optional index cache fixture"))
        return 1;
    indexCache.close();
    const auto bitmapBytes = bitmapBefore.size();
    const auto bitmapTime = bitmapBefore.lastModified();
    QFile bitmapFile(bitmap);
    if (!require(makeFixture(5, {320, 240}).save(bitmap) && QFileInfo(bitmap).size() == bitmapBytes &&
                 bitmapFile.open(QIODevice::ReadWrite) && bitmapFile.setFileTime(bitmapTime, QFileDevice::FileModificationTime),
                 "change source contents while keeping its size and timestamp"))
    {
        return 1;
    }
    bitmapFile.close();
    if (!require(checkLibraryAndWait() && activeLibrary() != firstLibrary && indexedImages().size() == 2 &&
                 !firstGeneration.exists(), "changed source bytes replace the old generation including its index cache"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    auto previous = activeLibrary();
    const auto missing = QFileInfo(previous).dir().filePath(indexedImages().front());
    if (!require(QFile::remove(missing) && checkLibraryAndWait() && activeLibrary() != previous &&
                 indexedImages().size() == 2, "deep verification repairs missing generated materials"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    previous = activeLibrary();
    if (!require(QFile::remove(second) && checkLibraryAndWait() && activeLibrary() != previous &&
                 indexedImages().size() == 1, "removed originals are removed from the library"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    previous = activeLibrary();
    const auto corruptSource = QDir(source).filePath("corrupt.png");
    QFile corrupt(corruptSource);
    if (!require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("bad image") == 9, "corrupt source fixture")) return 1;
    corrupt.close();
    if (!require(checkLibraryAndWait() && activeLibrary() == previous && QFileInfo(previous).isFile() &&
                 libraryStatus.startsWith(tr("素材库检查或重建失败。%1").arg(QString{})), "failed rebuild keeps the published library") ||
        !require(QFile::remove(corruptSource) && QFile::remove(bitmap) && checkLibraryAndWait() &&
                 activeLibrary() != previous && QFileInfo(activeLibrary()).isFile() && indexedImages().isEmpty(),
                 "removing all originals publishes a valid empty library"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    if (!require(makeFixture(2, {320, 240}).save(bitmap) && makeFixture(9, {180, 320}).save(second) &&
                 checkLibraryAndWait() && indexedImages().size() == 2 && documentLibrary(documents[activeDocument]) == library,
                 "adding originals rebuilds and existing project library paths are preserved"))
    {
        qWarning().noquote() << libraryStatus;
        return 1;
    }
    documents[activeDocument].library.clear();
    switchWorkspace(Workspace::Edit);
    if (!require(documentLibrary(documents[activeDocument]) == activeLibrary(), "default projects use the current library")) return 1;
    ui->targetWidth->setValue(96);
    ui->lightnessRange->setValue(255);
    ui->reusePenalty->setValue(0.01);
    ui->outputMode->setCurrentIndex(ui->outputMode->findData("image"));
    previewFiles = std::make_unique<QTemporaryDir>(root.filePath("preview-XXXXXX"));
    ui->tileWidth->setValue(260);
    if (!require(waitForWork([&] { generatePreview(); }) && documents[activeDocument].preview &&
                 documents[activeDocument].preview->size() == QSize(2080, 160) &&
                 canvasScene->sceneRect().size() == QSizeF(2080, 160), "original-size editor preview above 1800 pixels"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    auto previousPreview = documents[activeDocument].preview;
    ui->tileWidth->setValue(24);
    if (!require(waitForWork([&] { generatePreview(); }) && previewCurrent() &&
                 documents[activeDocument].previewGrid == QSize(8, 5) && documents[activeDocument].preview,
                 "CLI preview selection and composition"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    // 严格的小预算可复现多标签页的回收，不依赖测试机器的空闲内存。
    {
        Document background;
        background.preview = std::move(previousPreview);
        documents.append(std::move(background));
        auto &old = *documents.last().preview;
        auto &current = *documents[activeDocument].preview;
        old.setCacheLimitKiB(1024);
        // 两个素材最多只需两张 24×32 RGB32 贴图，而不是按全部 40 格预算。
        const auto currentLimit = current.setCacheLimitKiB(1024);
        nativePreviewTile(old, {260, 32});
        const auto pixels = nativePreviewTile(current, {24, 32});
        const bool warmed = old.cachedKiB() > 3 && current.cachedKiB() > 0 && currentLimit <= 6;
        distributePreviewMemory(12);
        const bool activeRetained = old.cachedKiB() == 0 && current.cachedKiB() > 0 && current.cachedKiB() <= 12;
        distributePreviewMemory(0);
        if (!require(warmed && activeRetained && nativePreviewTile(current, {24, 32}) == pixels &&
                     current.cachedKiB() == 0, "shared preview budget protects active tiles and evicts without changing pixels"))
        {
            return 1;
        }
        documents.removeLast();
        refreshPreviewMemory();
    }
    ui->compareModeButton->click();
    QApplication::processEvents();
    auto left = ui->editorImageView;
    auto right = ui->compareImageView;
    const QPoint wheelPoint = right->viewport()->rect().center() + QPoint(20, 12);
    QWheelEvent wheel(wheelPoint, right->viewport()->mapToGlobal(wheelPoint), {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(right->viewport(), &wheel);
    const auto centersAlign = [&] {
        return QLineF(left->mapToScene(left->viewport()->rect().center()), right->mapToScene(right->viewport()->rect().center())).length() < 2.0;
    };
    if (!require(right->isVisible() && left->transform() == right->transform() && centersAlign(), "synchronized compare zoom"))
    {
        return 1;
    }
    const auto originalCenter = canvasCenter;
    const auto dragFrom = left->viewport()->rect().center();
    const auto dragTo = dragFrom + QPoint(45, 22);
    QMouseEvent press(QEvent::MouseButtonPress, dragFrom, left->viewport()->mapToGlobal(dragFrom),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, dragTo, left->viewport()->mapToGlobal(dragTo),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, dragTo, left->viewport()->mapToGlobal(dragTo),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(left->viewport(), &press);
    QApplication::sendEvent(left->viewport(), &move);
    QApplication::sendEvent(left->viewport(), &release);
    if (!require(canvasCenter != originalCenter && centersAlign(), "synchronized compare pan"))
    {
        return 1;
    }
    fitCanvas();
    if (!require(capture("06-compare", this), "compare capture"))
    {
        return 1;
    }
    ui->inspectorTabs->setCurrentIndex(1);
    if (!require(waitForWork([&] { selectTile(right, right->mapFromScene(QPointF(12, 16))); }) &&
                 ui->candidateList->count() == 2 && ui->candidateList->selectedItems().size() == 1 &&
                 documents[activeDocument].selectedTile == 0,
                 "candidate score list, clicked position and selected asset"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    const int oldId = currentCandidate;
    auto replacement = ui->candidateList->item(ui->candidateList->currentRow() == 0 ? 1 : 0);
    const int replacementId = replacement->data(Qt::UserRole).toInt();
    const auto expected = QImage(replacement->data(Qt::UserRole + 3).toString())
                              .scaled({24, 32}, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto priorPreview = nativePreviewTile(*documents[activeDocument].preview, {24, 32});
    if (!require(waitForWork([&] { replaceTile(replacement); }) && currentCandidate == replacementId &&
                 oldId != currentCandidate && documents[activeDocument].edits.size() == 1 &&
                 nativePreviewTile(*documents[activeDocument].preview, {24, 32}) != priorPreview,
                 "replace candidate and update native preview tile") ||
        !require(capture("07-candidates", this), "candidate capture"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }

    setViewMode(ViewMode::Result);
    setZoom(8, false);
    if (!require(capture("09-pixel-grid", this), "pixel grid capture"))
    {
        return 1;
    }
    const auto output = root.filePath(QStringLiteral("导出 成品.png"));
    ui->outputPath->setText(output);
    switchWorkspace(Workspace::Export);
    QApplication::processEvents();
    const auto exportZoom = ui->exportPreview->transform().m11();
    scrollPreview(ui->exportPreview);
    if (!require(ui->exportPreview->transform().m11() > exportZoom, "export preview wheel zoom")) return 1;
    const auto matchesReplacement = [&expected](const QString &path) {
        const QImage result(path);
        if (result.size() != QSize(192, 160) || expected.isNull()) return false;
        const auto a = expected.pixelColor(12, 16);
        const auto b = result.pixelColor(12, 16);
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) <= 18;
    };
    if (!require(waitForWork([&] { exportDocument(); }) && matchesReplacement(output) && ui->exportPreview->hasImage(),
                 "export uses the manually selected tile") ||
        !require(saveDocument(activeDocument, false), "save project with tile replacement") ||
        !require(capture("08-export-result", this), "export with result capture"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    closeDocument(activeDocument);
    const auto secondOutput = root.filePath(QStringLiteral("导出 重开.png"));
    if (!require(openPath(project) && documents[activeDocument].edits.size() == 1, "restore saved tile replacement"))
    {
        return 1;
    }
    ui->outputPath->setText(secondOutput);
    if (!require(waitForWork([&] { exportDocument(); }) && matchesReplacement(secondOutput),
                 "export saved replacement after reopening project"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    const auto portableExport = root.filePath("portable");
    ui->outputMode->setCurrentIndex(ui->outputMode->findData("html"));
    ui->outputPath->setText(portableExport);
    ui->targetBlend->setValue(0.35);
    const auto manifest = QDir(portableExport).filePath("project.json");
    const auto entry = QDir(portableExport).filePath("index.html");
    if (!require(waitForWork([&] { exportDocument(); }) && QFileInfo::exists(manifest),
                 "export edited selection as a portable project"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    const auto openPortable = [&](const QString &path, bool checkLibraryAtStartup) {
        bool previewStarted = false;
        const auto finished = waitForWork([&] {
            previewStarted = openPath(path) && job.type == Job::PortablePreview && taskDialog->isVisible();
            if (checkLibraryAtStartup)
            {
                checkLibrary();
            }
        }, checkLibraryAtStartup);
        const auto &document = documents[activeDocument];
        return finished && previewStarted && !libraryCheckPending && document.manifest == manifest &&
            QFileInfo(document.target).dir() == QDir(portableExport) && document.view == ViewMode::Result &&
            document.preview && document.preview->size() == QSize(192, 160) && ui->exportPreview->hasImage() &&
            ui->targetWidth->value() == 96 && ui->targetHeight->value() == 72 &&
            ui->matchWidth->value() == 12 && ui->matchHeight->value() == 16 &&
            ui->tileWidth->value() == 24 && ui->tileHeight->value() == 32 &&
            ui->seedEdit->text() == "1024" && ui->targetBlend->value() == 0.35;
    };
    if (!require(openPortable(entry, true), "open index.html with saved parameters and automatic preview"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    const auto htmlPreview = nativePreviewTile(*documents[activeDocument].preview, {24, 32});
    ui->targetModeButton->click();
    if (!require(canvasScene->sceneRect().size() == QSizeF(960, 720), "portable project retains the original target")) return 1;
    closeDocument(activeDocument);
    if (!require(openPortable(manifest, false) &&
                 nativePreviewTile(*documents[activeDocument].preview, {24, 32}) == htmlPreview,
                 "open project.json with the same preview and parameters"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    ui->tileWidth->setValue(260);
    if (!require(waitForWork([&] { generatePreview(); }) && documents[activeDocument].preview &&
                 documents[activeDocument].preview->size() == QSize(2080, 160), "original-size portable preview"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    for (auto &document : documents) document.dirty = false;
    const auto libraryBeforeClose = activeLibrary();
    QElapsedTimer closeTime;
    QString canceledPreviewPath;
    if (!require(waitForWork([&] {
                     generatePreview();
                     if (pendingPreview) canceledPreviewPath = pendingPreview->path();
                     closeTime.start();
                     if (auto captionClose = findChild<QToolButton *>("captionClose")) captionClose->click();
                     else close();
                 }) && !canceledPreviewPath.isEmpty() && !isVisible() && closeTime.elapsed() < 5000 &&
                 process->state() == QProcess::NotRunning && editor->state() == QProcess::NotRunning &&
                 !QFileInfo::exists(canceledPreviewPath) && activeLibrary() == libraryBeforeClose,
                 "window closes promptly during preview and cleans up both workers"))
    {
        return 1;
    }
    return 0;
}
