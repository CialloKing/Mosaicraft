#include "MainWindow.h"

#include "ui_MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextCursor>

#include <utility>

namespace
{
constexpr qint64 responseLimit = 64LL * 1024 * 1024;
}

void MainWindow::setupJobProcess()
{
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
        if (job.type == Job::LibraryCheck) return;
        ui->taskLog->moveCursor(QTextCursor::End);
        ui->taskLog->insertPlainText(text);
        ui->taskLog->ensureCursorVisible();
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            finishJob(false, job.type == Job::LibraryCheck ? process->errorString()
                                                          : tr("无法启动 Mosaicraft CLI：%1").arg(process->errorString()));
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

bool MainWindow::busy() const
{
    return job.type != Job::None || !editorOperation.isEmpty();
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

QString MainWindow::resolveEditorWorker() const
{
#ifdef Q_OS_WIN
    const auto name = QStringLiteral("mosaicraft_desktop_worker.exe");
#else
    const auto name = QStringLiteral("mosaicraft_desktop_worker");
#endif
    const auto cli = resolveCli();
    if (!cli.isEmpty())
    {
        const QFileInfo besideCli(QFileInfo(cli).dir().filePath(name));
        if (besideCli.isFile() && besideCli.isExecutable()) return besideCli.absoluteFilePath();
    }
    const QFileInfo besideGui(QDir(QCoreApplication::applicationDirPath()).filePath(name));
    return besideGui.isFile() && besideGui.isExecutable() ? besideGui.absoluteFilePath() : QString{};
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
        message = tr("正在生成预览图…");
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
        message = completed.diagnostics.trimmed().isEmpty() ? tr("未收到有效的任务结果。") : completed.diagnostics.trimmed().right(1400);
    }
    if (completed.type == Job::LibraryCheck)
    {
        finishLibraryCheck(ok, body, message);
        return;
    }
    ui->exportProgress->hide();
    if (completed.type == Job::PortablePreview)
    {
        ok = finishPortablePreview(completed, ok, message);
    }
    if (completed.type == Job::Export)
    {
        finishExport(completed, ok, body, message);
    }
    if (ok)
    {
        const auto done = completed.type == Job::Export ? tr("导出完成") : tr("预览图已生成");
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
