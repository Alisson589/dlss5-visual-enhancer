#include "controller.hpp"
#include "infrastructure/log.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/windows_runtime.hpp"
#include "presentation/image_providers.hpp"
#include "presentation/preview_player.hpp"
#include "presentation/window_chrome.hpp"
#include <QFont>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QScreen>
#include <QTimer>
#include <windows.h>

namespace {
void qtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QString level = type == QtCriticalMsg || type == QtFatalMsg ? "ERROR"
                          : type == QtWarningMsg                      ? "WARN"
                                                                      : "INFO";
    ve::Log::instance().append(level, context.category ? QString::fromUtf8(context.category) : QString("qt"),
                               message);
}
} // namespace
int main(int argc, char** argv) {
    try {
        ve::configureWindowsRuntime();
        ve::configureWindowsIdentity();
        ve::Paths::instance().prepare();
        QGuiApplication app(argc, argv);
        app.setApplicationName("Visual Enhancer");
        app.setOrganizationName("Merserk");
        app.setApplicationVersion(VE_APP_VERSION);
        app.setWindowIcon(QIcon(ve::Paths::instance().at("app/resources/desktop/app.ico")));
        app.setFont(QFont("Segoe UI", 10));
        QQuickStyle::setStyle("Basic");
        qInstallMessageHandler(qtMessage);
        qmlRegisterType<ve::PreviewPlayer>("VisualEnhancer", 1, 0, "PreviewPlayer");
        QQmlApplicationEngine engine;
        auto* previews = new ve::PreviewProvider;
        ve::AppController controller(previews);
        engine.addImageProvider("preview", previews);
        engine.addImageProvider("icons", new ve::IconProvider);
        engine.rootContext()->setContextProperty("backend", &controller);
        QObject::connect(&controller, &ve::QmlFacade::languageChanged, &engine,
                         &QQmlApplicationEngine::retranslate);
        QStringList qmlErrors;
        QObject::connect(&engine, &QQmlEngine::warnings, &app, [&qmlErrors](const QList<QQmlError>& errors) {
            for (const auto& error : errors)
                qmlErrors.append(error.toString());
        });
        engine.load(QUrl::fromLocalFile(ve::Paths::instance().at("app/resources/desktop/qml/Main.qml")));
        if (engine.rootObjects().isEmpty())
            throw std::runtime_error(qmlErrors.join('\n').toStdString());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        if (!window)
            throw std::runtime_error("Main.qml did not create the application window.");
        const int x = controller.windowX(), y = controller.windowY();
        window->resize(controller.windowWidth(), controller.windowHeight());
        if (x != -1 && y != -1)
            window->setPosition(x, y);
        controller.attachWindow(window);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller, [window, &controller] {
            ve::info("shutdown", "Close accepted.");
            window->hide();
            // Cancel every decoder together before any destructor joins its
            // worker, so decoder shutdown and processing cancellation overlap.
            for (auto* player : window->findChildren<ve::PreviewPlayer*>())
                player->close();
            controller.shutdown();
            ve::info("shutdown", "Jobs cancelled and settings saved.");
        });
        ve::WindowChrome chrome(window, [&controller] { controller.suspendProcessing(); });
        window->showMaximized();
        QTimer::singleShot(0, &controller, &ve::QmlFacade::initializeRuntime);
        const int result = app.exec();
        app.removeNativeEventFilter(&chrome);
        QElapsedTimer shutdownTime;
        shutdownTime.start();
        // Release video decoders and the render surface before waiting for
        // cancelled processing jobs. The provider remains owned by the engine
        // until those jobs have stopped using it.
        delete window;
        ve::info("shutdown", QString("Media window destroyed in %1 ms.").arg(shutdownTime.elapsed()));
        qInstallMessageHandler(nullptr);
        return result;
    } catch (const std::exception& error) {
        const auto message = QString::fromUtf8(error.what()).toStdWString();
        MessageBoxW(nullptr, message.c_str(), L"Visual Enhancer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
