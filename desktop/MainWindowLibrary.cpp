#include "MainWindow.h"

#include "Dialogs.h"
#include "LibraryLocation.h"
#include "ui_MainWindow.h"

#include <QJsonDocument>
#include <QProcess>
#include <QSettings>

QJsonObject MainWindow::libraryMaintenanceRequest() const
{
    const auto options = libraryBuildOptions(settings);
    const auto source = settings.value("library/source").toString();
    QJsonObject request{{"inputDir", source}, {"outputDir", defaultLibraryDirectory(settings)},
                        {"normalizeWidth", options.size.width()}, {"normalizeHeight", options.size.height()},
                        {"recursive", options.recursive}, {"jpgQuality", options.jpegQuality},
                        {"pngLevel", options.pngLevel}};
    if (options.webpQuality > 0) request.insert("webpQuality", options.webpQuality);
    if (!source.isEmpty())
    {
        request.insert("threads", qBound(0, settings.value("preferences/threads", 0).toInt(), 256));
        request.insert("backend", settings.value("preferences/backend", "auto").toString());
        const auto device = settings.value("preferences/device").toString();
        if (!device.isEmpty()) request.insert("device", device);
    }
    return request;
}

void MainWindow::checkLibrary()
{
    if (closing) return;
    if (busy())
    {
        libraryCheckPending = true;
        return;
    }
    libraryCheckPending = false;
    const auto request = libraryMaintenanceRequest();
    if (request["inputDir"].toString().isEmpty() && settings.value("library/directory").toString().isEmpty() &&
        defaultLibraryDatabase(settings).isEmpty())
    {
        libraryStatus.clear();
        return;
    }
    const auto cli = resolveCli();
    if (cli.isEmpty())
    {
        finishLibraryCheck(false, {}, tr("未找到 Mosaicraft CLI。请在「设置 → 通用设置」选择桌面版的程序。"));
        return;
    }
    job.type = Job::LibraryCheck;
    libraryStatus = tr("正在检查素材库，必要时会重建…");
    setTaskMessage(libraryStatus);
    updateEditorControls();
    updateOutputControls();
    const QJsonObject payload{{"op", "maintain-library"}, {"request", request}};
    prepareTaskProcess(process);
    process->start(cli, {"desktop-worker"}, QIODevice::ReadWrite);
    process->write(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
    process->closeWriteChannel();
}

void MainWindow::finishLibraryCheck(bool ok, const QJsonObject &result, const QString &message)
{
    if (!ok)
    {
        libraryStatus = tr("素材库检查或重建失败。%1").arg(message);
    }
    else if (result["empty"].toBool())
    {
        libraryStatus = tr("素材库为空。请将图片添加到原始素材目录。");
    }
    else
    {
        libraryStatus = result["state"].toString() == "rebuilt" ? tr("素材库已重建。") : tr("素材库完整性检查通过。");
    }
    appendLog(libraryStatus);
    updateSummaries();
    updateEditorControls();
    updateOutputControls();
}

QString MainWindow::documentLibrary(const Document &document) const
{
    return document.library.isEmpty() ? defaultLibraryDatabase(settings) : document.library;
}
