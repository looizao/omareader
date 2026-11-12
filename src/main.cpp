// omareader: an offline ebook reader for Omarchy that reads books aloud with
// Kokoros and highlights each word as it is spoken.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>
#include <QtQml>

#include "cli.h"
#include "exporter.h"
#include "highlight.h"
#include "library.h"
#include "narrator.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "sync.h"
#include "theme.h"
#include "titleagent.h"
#include "tokenstore.h"
#include "translator.h"
#include "webimport.h"

#ifndef OMAREADER_VERSION
#define OMAREADER_VERSION "0.0.0"
#endif

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments.append(QString::fromLocal8Bit(argv[i]));
    const bool command = arguments.size() > 1 && Cli::isCommand(arguments.at(1));
    // Commands run without a display, so they work over SSH and for agents.
    if (command)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setOrganizationName("omareader");
    app.setApplicationName("omareader");
    app.setApplicationVersion(OMAREADER_VERSION);
    if (command)
        return Cli::run(arguments);

    // Matches the window to omareader.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("omareader");
    app.setWindowIcon(QIcon::fromTheme("omareader"));
    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Library library;
    Narrator narrator;
    PortalFilePicker picker;
    Reader reader(&library, &narrator, &picker);
    Exporter exporter;
    exporter.setPicker(&picker);
    reader.setExporter(&exporter);
    TitleAgent titleAgent;
    WebImport webImport;
    KeyringTokenStore keyring;
    SyncService sync(&library, &keyring);
    sync.setNarrator(&narrator);
    sync.setReader(&reader);
    sync.setExporter(&exporter);

    qmlRegisterType<ReadingHighlight>("Omareader", 1, 0, "ReadingHighlight");
    qmlRegisterUncreatableType<Narrator>("Omareader", 1, 0, "Narrator", "Use the narrator context property.");
    qmlRegisterUncreatableType<Exporter>("Omareader", 1, 0, "Exporter", "Use the exporter context property.");

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("library", &library);
    engine.rootContext()->setContextProperty("narrator", &narrator);
    engine.rootContext()->setContextProperty("reader", &reader);
    engine.rootContext()->setContextProperty("exporter", &exporter);
    engine.rootContext()->setContextProperty("titleAgent", &titleAgent);
    engine.rootContext()->setContextProperty("webImport", &webImport);
    engine.rootContext()->setContextProperty("sync", &sync);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // `omareader book.epub` and `omareader open book.epub` both open the book.
    const QStringList files = arguments.mid(arguments.value(1) == "open" ? 2 : 1);
    if (!files.isEmpty())
        reader.open(files.first());

    const int status = app.exec();
    narrator.stop();
    exporter.cancel();
    reader.saveNow();
    return status;
}
