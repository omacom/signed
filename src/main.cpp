// signed — sign PDFs and fill them in with text and dates.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QDir>
#include <QUrl>
#include <cstdio>

#include "backend.h"
#include "cli.h"
#include "pageprovider.h"
#include "portalfilepicker.h"
#include "signatures.h"
#include "textitem.h"
#include "theme.h"

int main(int argc, char *argv[]) {
    // Commands and help draw no window, so they must not need a display, even
    // where the desktop exports QT_QPA_PLATFORM=wayland. Bare signed prints
    // help, as a command line tool should; launchers say signed open.
    const QByteArray first = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    const bool command = argc == 1 || isCliCommand(first) || first == "--help" || first == "-h" ||
                         first == "--version" || first == "-v";
    if (command) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        // Qt's GTK platform theme opens a display of its own.
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("signed");
    app.setOrganizationName("signed");
    app.setApplicationVersion("0.3.1");

    // Matches the window to signed.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("signed");
    app.setWindowIcon(QIcon::fromTheme("signed"));

    QStringList args = app.arguments();
    if (first == "--version" || first == "-v") {
        printf("signed %s\n", qPrintable(app.applicationVersion()));
        return 0;
    }
    if (command)
        return runCli(QStringList{"signed"} + (first == "--help" || first == "-h" ? QStringList{"help"} : args.mid(1)));
    if (first == "open")
        args.removeAt(1);

    QQuickStyle::setStyle("Material");
    qmlRegisterType<TextItem>("Signed", 1, 0, "TextItem");

    Theme theme;
    QSettings settings;
    PortalFilePicker picker;
    SignatureLibrary signatures;
    auto *pages = new PageProvider();
    Backend backend(&picker, pages, &signatures, &settings);

    QQmlApplicationEngine engine;
    // The engine takes ownership of the image providers.
    engine.addImageProvider("pages", pages);
    engine.addImageProvider("items", new FunctionImageProvider([&backend](const QString &id) {
        return backend.items()->image(id.toInt());
    }));
    engine.addImageProvider("preview", new FunctionImageProvider([&backend](const QString &id) {
        return backend.previewImage(id);
    }));
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (args.size() > 1)
        backend.openUrl(QUrl::fromUserInput(args.at(1), QDir::currentPath(), QUrl::AssumeLocalFile));

    return app.exec();
}
