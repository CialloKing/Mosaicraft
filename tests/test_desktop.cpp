#include "CanvasImageItem.h"
#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QApplication>
#include <QDir>
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
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>

#include <algorithm>

namespace
{
QImage renderCanvasSample(const QImage &image, QSize size)
{
    QGraphicsScene scene;
    auto item = new CanvasImageItem(QPixmap::fromImage(image));
    scene.addItem(item);
    scene.setSceneRect(item->boundingRect());
    QImage result(size, QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    scene.render(&painter, QRectF(result.rect()), scene.sceneRect(), Qt::IgnoreAspectRatio);
    return result;
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

bool createFixtureLibrary(const QString &directory)
{
    if (!QDir().mkpath(directory))
    {
        return false;
    }
    const auto connection = QStringLiteral("desktop-smoke-library");
    bool ok = true;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(QDir(directory).filePath("mosaicraft.db"));
        ok = database.open();
        QSqlQuery query(database);
        ok = ok && query.exec(QString("PRAGMA user_version=%1").arg(MOSAICRAFT_LIBRARY_VERSION)) &&
             query.exec("CREATE TABLE meta(key TEXT, value TEXT)") &&
             query.exec("INSERT INTO meta VALUES ('feature_w','180'), ('feature_h','320')") &&
             query.exec("CREATE TABLE images(id INTEGER, file_path TEXT, src_width INTEGER, src_height INTEGER, "
                        "file_size INTEGER, created_at TEXT)") &&
             query.prepare("INSERT INTO images VALUES (?,?,?,?,?,?)");
        for (int index = 1; ok && index <= 45; ++index)
        {
            const auto filename = QString("Tile-%1.png").arg(index, 3, 10, QChar('0'));
            const auto path = QDir(directory).filePath(filename);
            ok = makeFixture(index, {180, 320}).save(path);
            query.bindValue(0, index);
            query.bindValue(1, filename);
            query.bindValue(2, 180);
            query.bindValue(3, 320);
            query.bindValue(4, QFileInfo(path).size());
            query.bindValue(5, QString("2026-01-%1").arg(index % 28 + 1, 2, 10, QChar('0')));
            ok = ok && query.exec();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
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
    if (!require(english.load(":/i18n/mosaicraft_en.qm") && english.translate("MainWindow", "文件") == "File",
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
                 ui->canvasEmpty->findChildren<QLabel *>().isEmpty() && ui->exportPreview->text().isEmpty(), "empty previews without placeholders") ||
        !require(capture("00-empty", this), "empty editor capture") ||
        !require(scratch.isValid() && makeFixture(0, {960, 720}).save(target) && createFixtureLibrary(library), "image and catalog fixtures") ||
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

    if (!require(QApplication::testAttribute(Qt::AA_DontUseNativeDialogs) &&
                 QApplication::translate("QFileDialog", "&Open").startsWith(QStringLiteral("打开")) &&
                 QApplication::translate("QFileDialog", "&Save").startsWith(QStringLiteral("保存")) &&
                 QApplication::translate("QPlatformTheme", "Cancel") == QStringLiteral("取消"),
                 "Chinese file dialog buttons"))
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
                 create.contains("--seed") && !create.contains("--target-upscale") && !create.contains("--lbp-weight") &&
                 exportImage.isNull() && ui->outputQuality->isHidden(), "single image export arguments") ||
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

    ui->libraryWorkspaceButton->click();
    if (!require(ui->workspaces->currentWidget() == ui->libraryPage && loadLibrary(library, false) &&
                 catalog.assets.size() == 45 && ui->assetList->count() == 45, "continuous gallery") ||
        !require(containedFile(library, "../" + QFileInfo(target).fileName()).isEmpty(), "gallery directory boundary"))
    {
        return 1;
    }
    ui->assetViewMode->setCurrentIndex(1);
    if (!require(ui->assetList->viewMode() == QListView::ListMode, "library list view"))
    {
        return 1;
    }
    ui->assetSortMode->setCurrentIndex(1);
    const auto sortedFirst = catalog.assets.front().importedAt;
    if (!require(sortedFirst >= catalog.assets.back().importedAt, "library sorting"))
    {
        return 1;
    }
    ui->assetSortMode->setCurrentIndex(0);
    ui->assetViewMode->setCurrentIndex(0);
    bool originalOpened = false;
    ui->assetList->setCurrentRow(0);
    QTimer::singleShot(0, this, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog)
        {
            const auto original = dialog->findChild<QGraphicsView *>();
            originalOpened = original && original->scene()->sceneRect().size() == QSizeF(180, 320);
            dialog->reject();
        }
    });
    ui->assetList->itemClicked(ui->assetList->item(0));
    ui->selectAssetsButton->setChecked(true);
    ui->assetList->setCurrentRow(0, QItemSelectionModel::Select);
    ui->assetList->item(1)->setSelected(true);
    ui->assetList->item(44)->setSelected(true);
    if (!require(originalOpened && selectedAssets.size() == 3 && ui->assetList->selectedItems().size() == 3 &&
                 ui->exportSelectedButton->isEnabled(), "gallery original image and multi-selection") ||
        !require(capture("03-library", this), "gallery capture"))
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
            if (correctPage && index == 3) correctPage = pages->currentWidget()->findChild<QSpinBox *>("normalizeWidth") != nullptr;
            if (dialog)
            {
                if (index == 1) correctPage = correctPage && capture("04-preferences", dialog);
                dialog->reject();
            }
        });
        settingActions[index]->trigger();
        if (!require(correctPage, "settings menu page routing"))
        {
            return 1;
        }
    }
    bool buildSettings = false;
    QTimer::singleShot(0, this, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog)
        {
            buildSettings = dialog->objectName() == "LibrarySettingsDialog" && capture("05-library-settings", dialog);
            dialog->reject();
        }
    });
    ui->librarySettingsButton->click();
    if (!require(buildSettings, "library settings dialog"))
    {
        return 1;
    }

    if (cli.isEmpty())
    {
        return 0;
    }
    settings.setValue("general/cli", QFileInfo(cli).absoluteFilePath());
    settings.setValue("preferences/backend", "cpu");
    settings.setValue("preferences/threads", 2);
    const auto waitForWork = [&](const auto &start) {
        QEventLoop wait;
        QTimer poll;
        connect(&poll, &QTimer::timeout, &wait, [&] {
            if (auto message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->reject();
            if (!busy()) wait.quit();
        });
        poll.start(25);
        QTimer::singleShot(100000, &wait, &QEventLoop::quit);
        start();
        if (busy()) wait.exec();
        return !busy();
    };
    const auto runAndWait = [&](Job type, const QStringList &args, const QString &destination) {
        return waitForWork([&] { runJob(type, args, destination); });
    };
    const auto source = root.filePath("input");
    const auto built = root.filePath("built library");
    if (!require(root.mkdir("input") && QFile::copy(target, QDir(source).filePath("asset.png")), "CLI import source") ||
        !require(runAndWait(Job::Import, buildLibraryArgs(source, built, false), built) && catalog.assets.size() == 1,
                 "CLI creates and loads a library"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    const auto additional = root.filePath("additional");
    const auto database = libraryDatabasePath(built);
    if (!require(root.mkdir("additional") && makeFixture(9, {180, 320}).save(QDir(additional).filePath("additional.png")),
                 "CLI append source") ||
        !require(runAndWait(Job::Import, buildLibraryArgs(additional, database, true), database) && catalog.assets.size() == 2,
                 "CLI appends assets"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    ui->useLibraryButton->click();
    if (!require(documents[activeDocument].library == catalog.databasePath && ui->workspaces->currentWidget() == ui->editPage,
                 "assign gallery to current project"))
    {
        return 1;
    }
    ui->targetWidth->setValue(96);
    ui->lightnessRange->setValue(255);
    ui->reusePenalty->setValue(0.01);
    ui->outputMode->setCurrentIndex(ui->outputMode->findData("image"));
    if (!require(waitForWork([&] { generatePreview(); }) && previewCurrent() &&
                 documents[activeDocument].previewGrid == QSize(8, 5) && !documents[activeDocument].preview.isNull(),
                 "CLI preview selection and composition"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
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
    const auto priorPreview = documents[activeDocument].preview.toImage();
    if (!require(waitForWork([&] { replaceTile(replacement); }) && currentCandidate == replacementId &&
                 oldId != currentCandidate && documents[activeDocument].edits.size() == 1 &&
                 documents[activeDocument].preview.toImage() != priorPreview, "replace candidate and update preview") ||
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
    const auto material = std::find_if(catalog.assets.cbegin(), catalog.assets.cend(), [&](const LibraryAsset &asset) {
        return asset.id == replacementId;
    });
    if (!require(material != catalog.assets.cend(), "replacement material"))
    {
        return 1;
    }
    const auto expected = QImage(catalog.assetPath(*material)).scaled({24, 32}, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto matchesReplacement = [&expected](const QString &path) {
        const QImage result(path);
        if (result.size() != QSize(192, 160) || expected.isNull()) return false;
        const auto a = expected.pixelColor(12, 16);
        const auto b = result.pixelColor(12, 16);
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) <= 18;
    };
    if (!require(waitForWork([&] { exportDocument(); }) && matchesReplacement(output) && !exportImage.isNull(),
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
    previewFiles = std::make_unique<QTemporaryDir>(root.filePath("preview-XXXXXX"));
    ui->outputMode->setCurrentIndex(ui->outputMode->findData("html"));
    ui->outputPath->setText(portableExport);
    if (!require(waitForWork([&] { exportDocument(); }) && QFileInfo::exists(QDir(portableExport).filePath("project.json")),
                 "export edited selection as a portable project") ||
        !require(openPath(portableExport) && !documents[activeDocument].manifest.isEmpty(), "open portable project") ||
        !require(waitForWork([&] { generatePreview(); }) && !documents[activeDocument].preview.isNull(),
                 "preview reopened portable project"))
    {
        qWarning().noquote() << ui->taskLog->toPlainText();
        return 1;
    }
    closePreviewSession();
    previewFiles.reset();
    return 0;
}
