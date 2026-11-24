#include "MainWindow.h"

#include "Dialogs.h"
#include "ui_MainWindow.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr qint64 responseLimit = 64LL * 1024 * 1024;
constexpr auto cancellationFile = "cancel";
constexpr int shutdownGraceMs = 1800;
constexpr int logCharacterLimit = 256 * 1024;
}

void MainWindow::setupLogWindow()
{
    logWindow = new QDialog(this, Qt::Window);
    logWindow->setObjectName("developerLogWindow");
    logWindow->setWindowTitle(tr("Mosaicraft 日志"));
    logWindow->setAttribute(Qt::WA_QuitOnClose, false);
    logWindow->resize(780, 460);
    logOutput = new QPlainTextEdit(logWindow);
    logOutput->setReadOnly(true);
    logOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    logOutput->setMaximumBlockCount(1000);
    logOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto layout = new QVBoxLayout(logWindow);
    layout->addWidget(logOutput);
    connect(logWindow, &QDialog::finished, this, [this] {
        settings.setValue(openLogWindowSetting, false);
    });
}

void MainWindow::updateLogWindow()
{
    logWindow->setVisible(settings.value(openLogWindowSetting).toBool());
}

void MainWindow::appendLog(const QString &text, bool completeLine)
{
    if (text.isEmpty()) return;
    auto scrollbar = logOutput->verticalScrollBar();
    const bool followTail = scrollbar->value() == scrollbar->maximum();
    auto document = logOutput->document();
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    if (completeLine && cursor.positionInBlock() > 0) cursor.insertBlock();
    cursor.insertText(text.right(logCharacterLimit));
    if (completeLine && cursor.positionInBlock() > 0) cursor.insertBlock();
    const int excess = document->characterCount() - 1 - logCharacterLimit;
    if (excess > 0)
    {
        cursor.setPosition(0);
        cursor.setPosition(excess, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    if (followTail) scrollbar->setValue(scrollbar->maximum());
}

void MainWindow::readTaskOutput(QProcess *child)
{
    auto &decoder = child == process ? jobOutputDecoder : editorOutputDecoder;
    QString text = decoder(child->readAllStandardError());
    text.replace("\r\n", "\n").replace('\r', '\n');
    if (child == process) job.diagnostics = (job.diagnostics + text).right(16000);
    appendLog(text, false);
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
    refreshPreviewMemory();
    taskMessage->setText(message);
    appendLog(message);
    if (!taskDialog->isHidden()) return;

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
    auto &decoder = child == process ? jobOutputDecoder : editorOutputDecoder;
    decoder.resetState();
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
    updateOutputControls();
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
        readTaskOutput(process);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            finishJob(false, job.type == Job::LibraryCheck ? process->errorString()
                                                          : tr("无法启动 Mosaicraft CLI：%1").arg(process->errorString()));
        }
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        readTaskOutput(process);
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

void MainWindow::runJob(Job type, const QStringList &args, const QString &destination)
{
    if (busy()) return;
    const auto cli = resolveCli();
    if (cli.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("未找到 Mosaicraft CLI。请在「设置 → 通用设置」选择桌面版的程序。"));
        return;
    }
    job = {type, activeDocument, destination, {}, {}};
    updateEditorControls();
    updateOutputControls();
    setTaskMessage(type == Job::Export ? tr("正在导出作品…") : tr("正在生成预览图…"));
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
    if (completed.type == Job::PortablePreview)
    {
        ok = finishPortablePreview(completed, ok, message);
    }
    if (completed.type == Job::Export)
    {
        finishExport(completed, ok, body);
    }
    if (ok)
    {
        appendLog(completed.type == Job::Export ? tr("导出完成") : tr("预览图已生成"));
    }
    else
    {
        appendLog(tr("任务失败") + '\n' + message);
        updateOutputControls();
        QMessageBox::warning(this, windowTitle(), message);
    }
    updateEditorControls();
    updateOutputControls();
}
