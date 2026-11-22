#include "MainWindow.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QStyleHints>
#include <QToolButton>
#include <QWindow>
#include <qt_windows.h>
#include <windowsx.h>

void MainWindow::setupWindowsTitleBar()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
    // Keep the native sizing frame. The menu and its controls occupy a single client row.
    setWindowFlags(Qt::Window | Qt::CustomizeWindowHint);
    const auto hwnd = reinterpret_cast<HWND>(winId());
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    auto controls = new QWidget(menuBar());
    controls->setObjectName(QStringLiteral("captionControls"));
    auto layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto addButton = [&](const QString &label, QChar glyph) {
        auto button = new QToolButton(controls);
        button->setText(glyph);
        button->setAccessibleName(label);
        button->setToolTip(label);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(46, 26);
        layout->addWidget(button);
        return button;
    };
    auto minimize = addButton(tr("最小化"), QChar(0xe921));
    auto maximize = addButton(tr("最大化"), QChar(0xe922));
    auto closeButton = addButton(tr("关闭"), QChar(0xe8bb));
    closeButton->setObjectName(QStringLiteral("captionClose"));
    menuBar()->setCornerWidget(controls);
    connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QToolButton::clicked, this, [this] {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    connect(windowHandle(), &QWindow::windowStateChanged, this, [this, maximize](Qt::WindowState state) {
        const bool maximized = state == Qt::WindowMaximized;
        const auto label = maximized ? tr("还原") : tr("最大化");
        maximize->setText(QChar(maximized ? 0xe923 : 0xe922));
        maximize->setAccessibleName(label);
        maximize->setToolTip(label);
    });
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const auto msg = static_cast<const MSG *>(message);
    if (msg->message == WM_NCHITTEST && menuWidget()) {
        POINT client{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (ScreenToClient(msg->hwnd, &client)) {
            const QPoint local = (QPointF(client.x, client.y) / devicePixelRatioF()).toPoint();
            const auto bar = menuBar();
            const auto pos = bar->mapFrom(this, local);
            if (bar->rect().contains(pos) && !bar->actionAt(pos) && !bar->childAt(pos)) {
                *result = HTCAPTION;
                return true;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
