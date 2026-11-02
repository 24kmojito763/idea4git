#include "builtintranslator.h"
#include "gitdialogs.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>
#include <memory>

// Read-only screenshot harness for scripts/create-advanced-demo.cmake fixtures.
// Usage: idea4git_demo_preview ROOT compare|conflict|stash OUTPUT.png [--english]
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 4)
        return 2;
    app.setOrganizationName("idea4gitQa");
    app.setApplicationName("idea4gitQa");
    app.setProperty("previewMode", true);
    app.setStyle("Fusion");
    app.setFont(QFont(QStringLiteral("Noto Sans CJK SC"), 10));
    const QString root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    const QString mode = QString::fromLatin1(argv[2]);
    const QString output = QString::fromLocal8Bit(argv[3]);
    const bool english = argc > 4 && QString::fromLatin1(argv[4]) == "--english";
    idea4git::BuiltinChineseTranslator translator;
    if (!english)
        app.installTranslator(&translator);
    idea4git::MainWindow host(english);
    idea4git::GitClient client(root);
    idea4git::GitOperations operations(&client, root, english);
    std::unique_ptr<idea4git::GitDialog> dialog;
    if (mode == "compare")
        dialog =
            std::make_unique<idea4git::CompareDialog>(&operations, english, &host, "v0.1", "v0.2");
    else if (mode == "conflict")
        dialog = std::make_unique<idea4git::ConflictDialog>(&operations, "src/palette.cpp", english,
                                                            &host);
    else if (mode == "stash")
        dialog = std::make_unique<idea4git::StashDialog>(&operations, english, &host);
    else
        return 2;
    dialog->show();
    QTimer timer;
    timer.setInterval(200);
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, dialog.get(), [&] {
        if (++ticks > 100) {
            app.exit(2);
            return;
        }
        if (ticks < 6 || dialog->working() || client.busy())
            return;
        if (mode == "compare" && dialog->findChild<QListWidget*>("comparisonFiles")->count() == 0) {
            app.exit(2);
            return;
        }
        if (mode == "stash" && dialog->findChild<QTableWidget*>("stashList")->rowCount() == 0) {
            app.exit(2);
            return;
        }
        app.exit(dialog->grab().save(output) ? 0 : 1);
    });
    timer.start();
    return app.exec();
}
