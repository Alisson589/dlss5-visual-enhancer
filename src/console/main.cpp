#include "cli.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/windows_runtime.hpp"
#include <QGuiApplication>
#include <cstdio>
#include <exception>
#include <windows.h>
#include <shellapi.h>
int main(int argc, char** argv) {
    // Qt consumes its own --style option. Preserve the application's arguments
    // before constructing QGuiApplication, including Unicode Windows paths.
    int originalCount = 0;
    auto** original = CommandLineToArgvW(GetCommandLineW(), &originalCount);
    QStringList arguments;
    for (int index = 1; original && index < originalCount; ++index)
        arguments.append(QString::fromWCharArray(original[index]));
    LocalFree(original);
    try {
        ve::configureWindowsRuntime();
        ve::Paths::instance().prepare();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 1;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setApplicationName("Visual Enhancer CLI");
    app.setOrganizationName("Merserk");
    app.setApplicationVersion(VE_APP_VERSION);
    return ve::runCli(arguments);
}
