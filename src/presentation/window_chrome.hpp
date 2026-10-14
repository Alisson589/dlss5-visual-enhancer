#pragma once
#include <QAbstractNativeEventFilter>
#include <QPointer>
#include <QQuickWindow>
#include <functional>

namespace ve {
class WindowChrome : public QAbstractNativeEventFilter {
  public:
    explicit WindowChrome(QQuickWindow* window, std::function<void()> suspend = {});
    ~WindowChrome() override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

  private:
    QPointer<QQuickWindow> window_;
    WId nativeId_ = 0;
    std::function<void()> suspend_;
};
void configureWindowsIdentity();
} // namespace ve
