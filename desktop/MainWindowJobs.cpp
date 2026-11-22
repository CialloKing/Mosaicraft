#include "MainWindow.h"

#include "ui_MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr qint64 responseLimit = 64LL * 1024 * 1024;
constexpr auto cancellationFile = "cancel";
constexpr int shutdownGraceMs = 1800;
}

void MainWindow::setupTaskDialog()
{
    // A child overlay leaves the window frame and the Windows caption buttons interactive.
    taskDialog = new QWidget(this);
    taskDialog->setObjectName("taskDialog");
    taskDialog->setAttribute(Qt::WA_StyledBackground);
    taskDialog->setFocusPolicy(Qt::StrongFocus);
    taskDialog->setAccessibleName(tr("任务进度"));
    taskDialog->setStyleSheet(
        "QWidget#taskDialog { background: rgba(0, 0, 0, 116); }"
        "QFrame#taskDialogPanel { background: #303030; border: 1px solid #686868; border-radius: 5px; }");
    auto layout = new QGridLayout(taskDialog);
    auto panel = new QFrame(taskDialog);
    panel->setObjectName("taskDialogPanel");
    panel->setMinimumWidth(330);
    auto panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 20);
    panelLayout->setSpacing(14);
    taskMessage = new QLabel(panel);
    auto progress = new QProgressBar(panel);
    progress->setAccessibleName(tr("任务进度"));
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    panelLayout->addWidget(taskMessage);
    panelLayout->addWidget(progress);
    layout->addWidget(panel, 0, 0, Qt::AlignCenter);
    taskDialog->hide();
    ui->workspaceRoot->installEventFilter(this);

    shutdownTimer = new QTimer(this);
    shutdownTimer->setSingleShot(true);
    connect(shutdownTimer, &QTimer::timeout, this, [this] {
        for (auto child : {process, editor})
        {
            if (child->state() != QProcess::NotRunning) child->kill();
        }
    });
    for (auto child : {process, editor})
    {
        connect(child, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
            if (closing && state == QProcess::NotRunning)
                QTimer::singleShot(0, this, &MainWindow::finishClosing);
        });
    }
}

void MainWindow::setTaskMessage(const QString &message)
{
    taskMessage->setText(message);
    if (!taskDialog->isHidden()) return;

    statusBar()->clearMessage();
    for (auto action : ui->menuBar->actions()) action->setEnabled(false);
    ui->workspaceRoot->setEnabled(false);
    taskDialog->setGeometry(ui->workspaceRoot->geometry());
    taskDialog->show();
    taskDialog->raise();
    taskDialog->setFocus();
}

void MainWindow::dismissTaskDialog()
{
    taskDialog->hide();
    ui->workspaceRoot->setEnabled(true);
    for (auto action : ui->menuBar->actions()) action->setEnabled(true);
    if (libraryCheckPending && !closing)
    {
        QTimer::singleShot(0, this, [this] {
            if (libraryCheckPending && !closing && isVisible()) checkLibrary();
        });
    }
}

void MainWindow::prepareTaskProcess(QProcess *child)
{
    if (!taskControl) taskControl = std::make_unique<QTemporaryDir>();
    if (!taskControl->isValid()) return;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("MOSAICRAFT_DESKTOP_CANCEL_FILE", taskControl->filePath(cancellationFile));
    child->setProcessEnvironment(environment);
}

void MainWindow::stopTasks()
{
    closing = true;
    setTaskMessage(tr("正在结束任务…"));
    if (taskControl && taskControl->isValid())
    {
        QFile request(taskControl->filePath(cancellationFile));
        if (request.open(QIODevice::WriteOnly)) request.close();
    }
    editor->closeWriteChannel();
    shutdownTimer->start(shutdownGraceMs);
    QTimer::singleShot(0, this, &MainWindow::finishClosing);
}

void MainWindow::finishClosing()
{
    if (!closing || process->state() != QProcess::NotRunning || editor->state() != QProcess::NotRunning) return;

    shutdownTimer->stop();
    if (job.type == Job::LibraryCheck)
    {
        libraryStatus.clear();
        libraryCheckPending = true;
    }
    job = {};
    editorCommand.clear();
    editorResponse.clear();
    editorRequest = {};
    editorOperation.clear();
    editorOwner = -1;
    pendingExport.clear();
    pendingPreview.reset();
    taskControl.reset();
    ui->exportProgress->hide();
    ui->summaryState->setText(activeDocument >= 0 && !documents[activeDocument].resultMessage.isEmpty()
        ? documents[activeDocument].resultMessage : tr("等待导出"));
    statusBar()->clearMessage();
    dismissTaskDialog();
    closing = false;
    updateEditorControls();
    updateOutputControls();
    close();
}

void MainWindow::setupJobProcess()
{
    connect(process, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = process->readAllStandardOutput();
        if (closing) return;
        if (job.response.size() <= responseLimit)
        {
            job.response.append(bytes);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        auto text = QString::fromUtf8(process->readAllStandardError());
        if (closing) return;
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
    return closing || job.type != Job::None || !editorOperation.isEmpty();
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
    setTaskMessage(message);
    prepareTaskProcess(process);
    process->start(cli, args + QStringList{"--json"}, QIODevice::ReadOnly);
}

void MainWindow::finishJob(bool processSucceeded, const QString &failure)
{
    if (closing || job.type == Job::None)
    {
        return;
    }
    const auto completed = std::exchange(job, JobState{});
    dismissTaskDialog();
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
