#include "window_chrome.hpp"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>
#include <windowsx.h>

namespace ve {
void configureWindowsIdentity() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(L"Merserk.VisualEnhancer");
}
WindowChrome::WindowChrome(QQuickWindow* window, std::function<void()> suspend)
    : window_(window), suspend_(std::move(suspend)) {
    nativeId_ = window->winId();
    const auto hwnd = reinterpret_cast<HWND>(nativeId_);
    // Install before changing the native style, which sends sizing messages.
    QCoreApplication::instance()->installNativeEventFilter(this);
    // Keep the resize capability permanently: the shell uses it to offer
    // Snap Layouts, including while maximized. Qt removes the visual frame.
    const auto style = (GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_CAPTION) | WS_THICKFRAME | WS_SYSMENU |
                       WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    window->setProperty("nativeChromeActive", true);
}
WindowChrome::~WindowChrome() {
    if (auto* app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}
bool WindowChrome::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
    // Qt can deliver non-client mouse input without return-value storage.
    // The close/maximize actions still need handling, but writing through
    // that null pointer crashes before the queued window action can run.
    qintptr ignoredResult = 0;
    if (!result)
        result = &ignoredResult;
    // winId() can create a platform window. Late teardown messages must never
    // recreate the window whose native handle is being destroyed.
    if (!window_ || !window_->handle())
        return false;
    const auto* msg = static_cast<MSG*>(message);
    const auto hwnd = reinterpret_cast<HWND>(nativeId_);
    if (msg->hwnd != hwnd)
        return false;
    if (msg->message == WM_NCMOUSEMOVE || msg->message == WM_NCMOUSEHOVER || msg->message == WM_NCMOUSELEAVE) {
        // These events do not reach QML MouseAreas. Publish the native hover
        // so all title-bar buttons use the same QML animation.
        const QString hovered = msg->message == WM_NCMOUSELEAVE ? QString{} :
            msg->wParam == HTMAXBUTTON ? QStringLiteral("maximize") :
            msg->wParam == HTCLOSE ? QStringLiteral("close") : QString{};
        window_->setProperty("nativeHoveredButton", hovered);
        if (msg->message != WM_NCMOUSELEAVE) {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        // Keep Windows informed for the custom HTMAXBUTTON Snap Layout flyout.
        *result = DefWindowProcW(hwnd, msg->message, msg->wParam, msg->lParam);
        return true;
    }
    if (msg->message == WM_MOUSEMOVE ||
        (msg->message == WM_ACTIVATE && LOWORD(msg->wParam) == WA_INACTIVE) ||
        (msg->message == WM_SHOWWINDOW && !msg->wParam))
        window_->setProperty("nativeHoveredButton", QString{});
    if (msg->message == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(hwnd, &point);
        const double dpr = window_->devicePixelRatio();
        const QPointF local(point.x / dpr, point.y / dpr);
        const int edge = 8;
        const bool left = local.x()<edge, right = local.x()> window_->width() - edge,
                   top = local.y()<edge, bottom = local.y()> window_->height() - edge;
        if (!IsZoomed(hwnd) && window_->visibility() != QWindow::FullScreen) {
            int hit = 0;
            if (top && left)
                hit = HTTOPLEFT;
            else if (top && right)
                hit = HTTOPRIGHT;
            else if (bottom && left)
                hit = HTBOTTOMLEFT;
            else if (bottom && right)
                hit = HTBOTTOMRIGHT;
            else if (left)
                hit = HTLEFT;
            else if (right)
                hit = HTRIGHT;
            else if (top)
                hit = HTTOP;
            else if (bottom)
                hit = HTBOTTOM;
            if (hit) {
                *result = hit;
                return true;
            }
        }
        const auto maximum = window_->property("maxBtnRect").toRectF();
        if (maximum.contains(local)) {
            *result = HTMAXBUTTON;
            return true;
        }
        if (window_->property("closeBtnRect").toRectF().contains(local)) {
            *result = HTCLOSE;
            return true;
        }
        if (window_->property("minBtnRect").toRectF().contains(local) ||
            window_->property("navTabsRect").toRectF().contains(local)) {
            *result = HTCLIENT;
            return true;
        }
        if (local.y() < std::max(40., maximum.bottom()) && local.x() > 52) {
            *result = HTCAPTION;
            return true;
        }
        *result = HTCLIENT;
        return true;
    }
    if (msg->message == WM_NCLBUTTONDOWN && msg->wParam == HTMAXBUTTON) {
        // Changing the HWND while its non-client mouse message is being
        // dispatched re-enters Qt's geometry/state handling. Finish dispatch
        // before applying the change, and keep no raw window pointer alive.
        const QPointer<QQuickWindow> target = window_;
        QMetaObject::invokeMethod(window_, [target] {
            if (!target) return;
            if (target->visibility() == QWindow::Maximized || target->visibility() == QWindow::FullScreen)
                target->showNormal();
            else
                target->showMaximized();
        }, Qt::QueuedConnection);
        *result = 0;
        return true;
    }
    if (msg->message == WM_NCLBUTTONDOWN && msg->wParam == HTCLOSE) {
        const QPointer<QQuickWindow> target = window_;
        QMetaObject::invokeMethod(window_, [target] { if (target) target->close(); }, Qt::QueuedConnection);
        *result = 0;
        return true;
    }
    if (msg->message == WM_POWERBROADCAST && msg->wParam == PBT_APMSUSPEND && suspend_)
        suspend_();
    return false;
}
} // namespace ve
