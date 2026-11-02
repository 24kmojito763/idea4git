#include "builtintranslator.h"
#include "mainwindow.h"
#include "paths.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>
#include <cstdio>
#include <memory>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("idea4git"));
    QCoreApplication::setApplicationName(QStringLiteral("idea4git"));
    QCoreApplication::setApplicationVersion(QStringLiteral(IDEA4GIT_VERSION));
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Noto Sans CJK SC"));
    font.setPointSize(10);
    app.setFont(font);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("idea4git — native Qt code & Git workspace"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("directory"),
                                 QStringLiteral("Project directory to open"));
    parser.addOption({QStringLiteral("file"), QStringLiteral("Open a file relative to the project"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("screenshot"),
                      QStringLiteral("Save a window PNG and exit (does not save preferences)"),
                      QStringLiteral("png")});
    parser.addOption({QStringLiteral("english"),
                      QStringLiteral("Preview the English UI without saving the language")});
    parser.process(app);
    const bool preview = parser.isSet("screenshot");
    app.setProperty("previewMode", preview);
    const bool english =
        parser.isSet("english") || QSettings().value("ui/language", "zh_CN").toString() == "en";
    idea4git::BuiltinChineseTranslator chinese;
    if (!english)
        app.installTranslator(&chinese);
    idea4git::MainWindow window(english);
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/idea4git-icon.png")));
    QString directory = parser.positionalArguments().value(0);
    if (directory.isEmpty())
        directory = QSettings().value("project/last", QDir::currentPath()).toString();
    if (!QFileInfo(directory).isDir())
        directory = QDir::currentPath();
    window.openProject(directory);
    if (parser.isSet("file"))
        window.openFile(
            idea4git::repositoryFilePath(QDir(directory).absolutePath(), parser.value("file")));
    if (preview)
        window.show();
    else
        window.showMaximized();
    if (preview) {
        auto* timer = new QTimer(&window);
        timer->setInterval(250);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, &window, [&window, &app, &parser, attempts] {
            if (++*attempts < 6 || window.gitBusy()) {
                if (*attempts < 80)
                    return;
                std::fprintf(stderr, "Git preview timed out\n");
                app.exit(2);
                return;
            }
            const bool saved = window.grab().save(parser.value("screenshot"));
            if (!saved)
                std::fprintf(stderr, "Could not save screenshot\n");
            app.exit(saved ? 0 : 1);
        });
        timer->start();
    }
    return app.exec();
}
