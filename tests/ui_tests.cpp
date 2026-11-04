#include "builtintranslator.h"
#include "codeeditor.h"
#include "gitdialogs.h"
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <functional>
#include <memory>

using namespace idea4git;

namespace {
bool write(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
QByteArray git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    if (!process.waitForFinished(10000) || process.exitCode() != 0)
        return "TEST_GIT_ERROR";
    return process.readAllStandardOutput();
}
} // namespace

class UiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("idea4gitTests");
        QCoreApplication::setApplicationName("idea4gitTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        qApp->setProperty("previewMode", true);
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        const auto config = m_settings.filePath("gitconfig");
        QVERIFY(write(config, "[user]\n name = idea4git Tests\n email = tests@example.invalid\n"
                              "[commit]\n gpgSign = false\n[core]\n hooksPath = /dev/null\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(config));
    }

    void maximizedWindowRestoresMaximizedAfterMinimizeAndActivation() {
        MainWindow window;
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());

        window.showMinimized();
        QTRY_VERIFY(window.isMinimized());

        // Model the desktop shell restoring it as normal after another window took focus.
        window.setWindowState(Qt::WindowNoState);
        QTRY_VERIFY(!window.isMinimized());
        QVERIFY(!window.isMaximized());
        QWidget otherWindow;
        otherWindow.show();
        otherWindow.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(otherWindow.isActiveWindow(), 1000);
        window.activateWindow();

        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(window.isMaximized(), 1000);
    }

    void maximizedWindowRestoresAfterNormalStateActivation() {
        MainWindow window;
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());

        // Some Wayland minimize/restore paths report max -> normal without a minimized state.
        window.setWindowState(Qt::WindowNoState);
        QVERIFY(!window.isMaximized());

        QWidget otherWindow;
        otherWindow.show();
        otherWindow.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(otherWindow.isActiveWindow(), 1000);
        window.activateWindow();

        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(window.isMaximized(), 1000);
    }

    void maximizedWindowRestoresWhenDeactivationArrivesAfterStateChange() {
        MainWindow window;
        QWidget otherWindow;
        otherWindow.show();
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());
        window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);

        // Wayland can deliver max -> normal first, then the deactivation event later.
        window.setWindowState(Qt::WindowNoState);
        QVERIFY(!window.isMaximized());
        QTest::qWait(25); // Let the current zero-delay cleanup run before focus is lost.

        otherWindow.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(otherWindow.isActiveWindow(), 1000);
        window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(window.isMaximized(), 1000);
    }

    void intentionalRestoreToNormalStaysNormalAfterActivation() {
        MainWindow window;
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());

        window.setWindowState(Qt::WindowNoState);
        QTRY_VERIFY(!window.isMaximized());
        QTest::qWait(300); // Remain active past the Wayland deactivation grace period.

        QWidget otherWindow;
        otherWindow.show();
        otherWindow.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(otherWindow.isActiveWindow(), 1000);
        window.activateWindow();

        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);
        QVERIFY(!window.isMaximized());
    }

    void bomAndCrLfRoundTrip() {
        QTemporaryDir directory;
        const auto path = directory.filePath("sample.cpp");
        const QByteArray contents =
            QByteArray::fromHex("efbbbf") + "// 中文\r\nint main() { return 0; }\r\n";
        QVERIFY(write(path, contents));
        CodeEditor editor;
        QString error;
        QVERIFY2(editor.loadFile(path, &error), qPrintable(error));
        QCOMPARE(editor.eolMode(), QsciScintilla::EolWindows);
        QVERIFY(editor.saveFile(&error));
        QCOMPARE(read(path), contents);
        QVERIFY(!editor.isModified());
    }

    void markdownOpensRenderedWithSourceToggle() {
        QTemporaryDir directory;
        const auto path = directory.filePath("guide.md");
        const auto contents = QStringLiteral("# Markdown preview\n\nA **formatted** paragraph.\n\n"
                                             "- first item\n- second item\n\n"
                                             "```cpp\nint answer = 42;\n```\n\n"
                                             "![diagram](images/diagram.png)\n");
        QVERIFY(write(path, contents.toUtf8()));

        CodeEditor editor;
        QString error;
        QVERIFY2(editor.loadFile(path, &error), qPrintable(error));
        editor.setEnglish(false);
        QVERIFY(editor.isMarkdownPreviewVisible());

        auto* preview = editor.findChild<QTextBrowser*>("markdownPreview");
        QVERIFY(preview);
        const auto rendered = preview->toPlainText();
        QVERIFY(rendered.contains("Markdown preview"));
        QVERIFY(rendered.contains("first item"));
        QVERIFY(rendered.contains("int answer = 42;"));
        QCOMPARE(preview->document()->baseUrl(),
                 QUrl::fromLocalFile(QFileInfo(path).absolutePath() + QLatin1Char('/')));

        auto* toggle = editor.findChild<QToolButton*>("markdownModeToggle");
        QVERIFY(toggle);
        QCOMPARE(toggle->text(), QString::fromUtf8("源码"));
        editor.showMarkdownSource();
        QVERIFY(!editor.isMarkdownPreviewVisible());
        QCOMPARE(toggle->text(), QString::fromUtf8("预览"));

        editor.setText("# Updated title");
        editor.showMarkdownPreview();
        QVERIFY(editor.isMarkdownPreviewVisible());
        QVERIFY(preview->toPlainText().contains("Updated title"));

        editor.setEnglish(true);
        QCOMPARE(toggle->text(), QStringLiteral("Source"));
    }

    void mainWindowSearchSwitchesMarkdownPreviewToSource() {
        QTemporaryDir directory;
        const auto path = directory.filePath("notes.md");
        QVERIFY(write(path, "# Searchable heading\n\nA needle in this document.\n"));

        MainWindow window;
        window.openFile(path);
        auto* editor = window.currentEditor();
        QVERIFY(editor);
        QVERIFY(editor->isMarkdownPreviewVisible());

        auto* search = window.findChild<QLineEdit*>("currentFindInput");
        QVERIFY(search);
        search->setText(QStringLiteral("needle"));
        QVERIFY(!editor->isMarkdownPreviewVisible());
        QCOMPARE(editor->selectedText(), QStringLiteral("needle"));
    }

    void markdownPreviewScrollbarCanBeDragged() {
        QTemporaryDir directory;
        const auto path = directory.filePath("long-guide.md");
        QString contents;
        for (int line = 0; line < 100; ++line)
            contents += QStringLiteral("## Section %1\n\nScrollable paragraph %1.\n\n").arg(line);
        QVERIFY(write(path, contents.toUtf8()));

        MainWindow window;
        window.resize(1240, 820);
        window.openFile(path);
        auto* editor = window.currentEditor();
        QVERIFY(editor);
        window.show();
        QTRY_VERIFY(window.isVisible());
        QTRY_VERIFY(editor->isVisible());

        auto* preview = editor->findChild<QTextBrowser*>("markdownPreview");
        QVERIFY(preview);
        auto* scrollBar = preview->verticalScrollBar();
        QTRY_VERIFY(scrollBar->maximum() > 0);

        QStyleOptionSlider option;
        option.initFrom(scrollBar);
        option.orientation = Qt::Vertical;
        option.minimum = scrollBar->minimum();
        option.maximum = scrollBar->maximum();
        option.sliderPosition = scrollBar->sliderPosition();
        option.sliderValue = scrollBar->value();
        option.singleStep = scrollBar->singleStep();
        option.pageStep = scrollBar->pageStep();
        const auto thumb = scrollBar->style()->subControlRect(
            QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, scrollBar);
        QVERIFY(thumb.isValid());

        const auto start = scrollBar->value();
        const QPoint pressGlobal = scrollBar->mapToGlobal(thumb.center());
        QWidget* hitTarget = &window;
        while (auto* child = hitTarget->childAt(hitTarget->mapFromGlobal(pressGlobal)))
            hitTarget = child;
        QVERIFY(hitTarget);
        QCOMPARE(hitTarget, static_cast<QWidget*>(scrollBar));
        QMouseEvent press(QEvent::MouseButtonPress, hitTarget->mapFromGlobal(pressGlobal),
                          pressGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hitTarget, &press);

        const QPoint endGlobal(
            scrollBar->mapToGlobal(QPoint(thumb.center().x(), scrollBar->height() - 3)));
        QMouseEvent move(QEvent::MouseMove, hitTarget->mapFromGlobal(endGlobal), endGlobal,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hitTarget, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, hitTarget->mapFromGlobal(endGlobal),
                            endGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(hitTarget, &release);

        QVERIFY(scrollBar->value() > start);
        QCOMPARE(editor->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(editor->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

        editor->showMarkdownSource();
        QCOMPARE(editor->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QCOMPARE(editor->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    }

    void refuseBinaryAndInvalidUtf8() {
        QTemporaryDir directory;
        CodeEditor editor;
        QString error;
        QVERIFY(write(directory.filePath("binary"), QByteArray("a\0b", 3)));
        QVERIFY(!editor.loadFile(directory.filePath("binary"), &error));
        QVERIFY(write(directory.filePath("invalid"), QByteArray::fromHex("fffe41")));
        QVERIFY(!editor.loadFile(directory.filePath("invalid"), &error));
        QVERIFY(write(directory.filePath("truncated"), QByteArray::fromHex("e282")));
        QVERIFY(!editor.loadFile(directory.filePath("truncated"), &error));
    }

    void builtinLanguageMessages() {
        BuiltinChineseTranslator translator;
        QVERIFY(qApp->installTranslator(&translator));
        QCOMPARE(QCoreApplication::translate("QPlatformTheme", "&Cancel"),
                 QString::fromUtf8("取消"));
        QCOMPARE(
            QCoreApplication::translate("idea4git::CodeEditor", "Binary files cannot be edited."),
            QString::fromUtf8("无法在文本编辑器中编辑二进制文件。"));
        qApp->removeTranslator(&translator);
        QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));
    }

    void languageChoicePersists() {
        MainWindow window;
        auto* action = window.findChild<QAction*>("languageSettingsAction");
        QVERIFY(action);
        QTimer::singleShot(0, &window, [] {
            auto* dialog = qobject_cast<QDialog*>(qApp->activeModalWidget());
            QVERIFY(dialog);
            for (auto* radio : dialog->findChildren<QRadioButton*>())
                if (radio->text() == "English")
                    radio->setChecked(true);
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(buttons);
            buttons->button(QDialogButtonBox::Save)->click();
        });
        action->trigger();
        QCOMPARE(QSettings().value("ui/language").toString(), QStringLiteral("en"));
        MainWindow nextLaunch(QSettings().value("ui/language").toString() == "en");
        QCOMPARE(nextLaunch.findChild<QAction*>("languageSettingsAction")->text(),
                 QStringLiteral("Interface language…"));
        QSettings().remove("ui/language");
    }

    void literalUiFilePaths() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
#ifdef Q_OS_WIN
        const QString relative = QString::fromUtf8("中文 [1] file.txt");
#else
        const QString relative = QString::fromUtf8(":(glob)中文 [1]\nfile.txt");
#endif
        const QString path = repository.path() + '/' + relative;
        QVERIFY(write(path, "contents\n"));
        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);
        auto* changes = window.findChild<QTreeWidget*>("changesTree");
        QVERIFY(changes);
        auto* item = changes->topLevelItem(1)->child(0);
        QVERIFY(item);
        const auto area = changes->visualItemRect(item);
        QTest::mouseClick(changes->viewport(), Qt::LeftButton, Qt::NoModifier, area.center());
        QVERIFY(window.findChild<QPlainTextEdit*>("diffView")->toPlainText().contains("+contents"));
        QTest::mouseDClick(changes->viewport(), Qt::LeftButton, Qt::NoModifier, area.center());
        QVERIFY(window.currentEditor());
        QCOMPARE(window.currentEditor()->filePath(), path);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);
        QVERIFY(!window.findChild<QToolButton*>("stageButton"));
        QVERIFY(!window.findChild<QToolButton*>("unstageButton"));
        auto* message = window.findChild<QPlainTextEdit*>("commitMessage");
        QVERIFY(message);
        message->setPlainText("Commit special path");
        auto* commit = window.findChild<QPushButton*>("commitButton");
        QVERIFY(commit && commit->isEnabled());
        QTest::mouseClick(commit, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QCOMPARE(git(repository.path(), {"show", QStringLiteral("HEAD:") + relative}).trimmed(),
                 QByteArray("contents"));
    }

    void projectTreeExpansionPersistsPerProject() {
        QTemporaryDir firstProject;
        QTemporaryDir secondProject;
        QVERIFY(firstProject.isValid() && secondProject.isValid());
        for (const auto& root : {firstProject.path(), secondProject.path()}) {
            QVERIFY(QDir().mkpath(QDir(root).filePath("src/nested/leaf")));
            QVERIFY(QDir().mkpath(QDir(root).filePath("custom/deep")));
            QVERIFY(write(QDir(root).filePath("custom/deep/file.txt"), "tree state\n"));
        }

        struct PreviewModeGuard {
            QVariant previous = qApp->property("previewMode");
            ~PreviewModeGuard() { qApp->setProperty("previewMode", previous); }
        } previewModeGuard;
        qApp->setProperty("previewMode", false);
        const auto projectHash =
            QCryptographicHash::hash(QDir::cleanPath(firstProject.path()).toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex();
        const auto settingsKey = QStringLiteral("projectTree/%1/expandedDirectories")
                                     .arg(QString::fromLatin1(projectHash));

        auto treeFor = [](MainWindow& window) {
            return window.findChild<QTreeView*>("projectTree");
        };
        auto directoryProxyIndex = [](QTreeView* tree, const QString& path) {
            auto* proxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
            auto* fileSystem =
                proxy ? qobject_cast<QFileSystemModel*>(proxy->sourceModel()) : nullptr;
            return proxy && fileSystem ? proxy->mapFromSource(fileSystem->index(path))
                                       : QModelIndex{};
        };

        {
            MainWindow window;
            QVERIFY(window.openProject(firstProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tree = treeFor(window);
            QVERIFY(tree);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, firstProject.filePath("src")).isValid(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(
                tree->isExpanded(directoryProxyIndex(tree, firstProject.filePath("src"))), 5000);
            QVERIFY(!tree->isExpanded(directoryProxyIndex(tree, firstProject.filePath("custom"))));

            auto custom = directoryProxyIndex(tree, firstProject.filePath("custom"));
            QVERIFY(custom.isValid());
            tree->expand(custom);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, firstProject.filePath("custom/deep")).isValid(), 5000);
            const auto deep = directoryProxyIndex(tree, firstProject.filePath("custom/deep"));
            tree->expand(deep);
            QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(deep), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            QVERIFY(window.close());
        }
        const auto savedPaths = QSettings().value(settingsKey).toStringList();
        QVERIFY(savedPaths.contains(QStringLiteral("custom")));
        QVERIFY(savedPaths.contains(QStringLiteral("custom/deep")));
        QVERIFY(!savedPaths.contains(firstProject.filePath("custom/deep")));

        {
            MainWindow window;
            QVERIFY(window.openProject(secondProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tree = treeFor(window);
            QVERIFY(tree);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, secondProject.filePath("custom")).isValid(), 5000);
            QVERIFY(!tree->isExpanded(directoryProxyIndex(tree, secondProject.filePath("custom"))));
            QTRY_VERIFY_WITH_TIMEOUT(
                tree->isExpanded(directoryProxyIndex(tree, secondProject.filePath("src"))), 5000);
            window.close();
        }

        {
            MainWindow window;
            QVERIFY(window.openProject(firstProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tree = treeFor(window);
            QVERIFY(tree);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, firstProject.filePath("custom/deep")).isValid(), 5000);
            auto custom = directoryProxyIndex(tree, firstProject.filePath("custom"));
            auto deep = directoryProxyIndex(tree, firstProject.filePath("custom/deep"));
            QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(custom) && tree->isExpanded(deep), 5000);

            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            QVERIFY(window.openProject(secondProject.path()));
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, secondProject.filePath("custom")).isValid(), 5000);
            QVERIFY(!tree->isExpanded(directoryProxyIndex(tree, secondProject.filePath("custom"))));

            QVERIFY(window.openProject(firstProject.path()));
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, firstProject.filePath("custom/deep")).isValid(), 5000);
            custom = directoryProxyIndex(tree, firstProject.filePath("custom"));
            deep = directoryProxyIndex(tree, firstProject.filePath("custom/deep"));
            QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(custom) && tree->isExpanded(deep), 5000);

            tree->collapse(custom);
            tree->collapse(directoryProxyIndex(tree, firstProject.filePath("src")));
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            window.close();
        }

        QVERIFY(QSettings().contains(settingsKey));
        QVERIFY(QSettings().value(settingsKey).toStringList().isEmpty());

        {
            MainWindow window;
            QVERIFY(window.openProject(firstProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tree = treeFor(window);
            QVERIFY(tree);
            QTRY_VERIFY_WITH_TIMEOUT(
                directoryProxyIndex(tree, firstProject.filePath("src")).isValid(), 5000);
            QVERIFY(!tree->isExpanded(directoryProxyIndex(tree, firstProject.filePath("src"))));
            QVERIFY(!tree->isExpanded(directoryProxyIndex(tree, firstProject.filePath("custom"))));
            window.close();
        }
    }

    void externalChangesNeedExplicitOverwrite() {
        QTemporaryDir directory;
        const auto path = directory.filePath("file.txt");
        QVERIFY(write(path, "original\n"));
        CodeEditor editor;
        QString error;
        QVERIFY(editor.loadFile(path, &error));
        editor.setText("edited\n");
        QVERIFY(write(path, "external\n"));
        QVERIFY(editor.changedOnDisk());
        QVERIFY(!editor.saveFile(&error));
        QCOMPARE(read(path), QByteArray("external\n"));
        QVERIFY(editor.saveFile(&error, true));
        QCOMPARE(read(path), QByteArray("edited\n"));
    }

    void currentAndGlobalSearchOptions() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        QVERIFY(QDir().mkpath(repository.filePath("src/nested")));
        const auto sourcePath = repository.filePath("src/nested/alpha.cpp");
        const auto otherPath = repository.filePath("src/other.cpp");
        const auto notesPath = repository.filePath("notes.txt");
        QVERIFY(write(sourcePath, "aXb\nAb\nnearby context\n"));
        QVERIFY(write(otherPath, "AXb\n"));
        QVERIFY(write(notesPath, "aXb\n"));
        QVERIFY(git(repository.path(), {"add", "."}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"commit", "-m", "search fixture"}) != "TEST_GIT_ERROR");

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        window.openFile(sourcePath);
        auto* editor = window.currentEditor();
        QVERIFY(editor);

        auto* findAction = window.findChild<QAction*>("findAction");
        auto* globalFindAction = window.findChild<QAction*>("globalFindAction");
        QVERIFY(findAction && globalFindAction);
        QCOMPARE(findAction->shortcut(), QKeySequence::Find);
        QCOMPARE(globalFindAction->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+F")));

        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_F, Qt::ControlModifier);
        auto* findBar = window.findChild<QWidget*>("currentFindBar");
        auto* editorTabs = window.findChild<QTabWidget*>("editorTabs");
        auto* query = window.findChild<QLineEdit*>("currentFindInput");
        auto* mode = window.findChild<QComboBox*>("currentSearchMode");
        auto* matchCase = window.findChild<QCheckBox*>("currentSearchCaseSensitive");
        auto* matchCount = window.findChild<QLabel*>("currentFindCount");
        auto* previous = window.findChild<QPushButton*>("findPreviousButton");
        auto* next = window.findChild<QPushButton*>("findNextButton");
        QVERIFY(findBar && findBar->isVisible() && editorTabs);
        QVERIFY(findBar->geometry().top() < editorTabs->geometry().top());
        QVERIFY(query && mode && matchCase && matchCount && previous && next);
        matchCase->setChecked(true);
        query->setText(QStringLiteral("AB"));
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("activityStatus")->text(),
                                  QString::fromUtf8("未找到匹配内容"), 1000);
        QCOMPARE(matchCount->text(), QStringLiteral("0/0"));

        matchCase->setChecked(false);
        QTRY_COMPARE_WITH_TIMEOUT(matchCount->text(), QStringLiteral("1/1"), 1000);
        int line = -1;
        int index = -1;
        editor->getCursorPosition(&line, &index);
        QCOMPARE(line, 1);

        mode->setCurrentIndex(1);
        query->setText(QStringLiteral("ab"));
        matchCase->setChecked(true);
        QTRY_COMPARE_WITH_TIMEOUT(matchCount->text(), QStringLiteral("1/2"), 1000);
        editor->getCursorPosition(&line, &index);
        QCOMPARE(line, 0);

        mode->setCurrentIndex(0);
        matchCase->setChecked(false);
        query->setText(QStringLiteral("a"));
        QTRY_COMPARE_WITH_TIMEOUT(matchCount->text(), QStringLiteral("1/3"), 1000);
        next->click();
        QTRY_COMPARE_WITH_TIMEOUT(matchCount->text(), QStringLiteral("2/3"), 1000);
        int fromLine = -1;
        int fromIndex = -1;
        int toLine = -1;
        int toIndex = -1;
        editor->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        QCOMPARE(fromLine, 1);
        previous->click();
        QTRY_COMPARE_WITH_TIMEOUT(matchCount->text(), QStringLiteral("1/3"), 1000);
        editor->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        QCOMPARE(fromLine, 0);
        QCOMPARE(fromIndex, 0);

        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        auto* dialog = window.findChild<QDialog*>("globalSearchDialog");
        QVERIFY(dialog && dialog->isVisible());
        auto* searchSplitter = dialog->findChild<QSplitter*>();
        QVERIFY(searchSplitter);
        QTRY_VERIFY_WITH_TIMEOUT(
            searchSplitter->sizes().size() == 2 && searchSplitter->sizes().at(0) > 0, 1000);
        const auto initialSearchSizes = searchSplitter->sizes();
        auto* searchHandle = searchSplitter->handle(1);
        QVERIFY(searchHandle && searchHandle->isVisible());
        QVERIFY2(searchHandle->width() >= 6,
                 qPrintable(QStringLiteral("Global-search splitter handle is only %1 px wide")
                                .arg(searchHandle->width())));
        const auto handleCenter = searchHandle->rect().center();
        const auto movedHandleCenter = handleCenter + QPoint(60, 0);
        QTest::mousePress(searchHandle, Qt::LeftButton, Qt::NoModifier, handleCenter);
        QTest::mouseMove(searchHandle, movedHandleCenter, 100);
        QTest::mouseRelease(searchHandle, Qt::LeftButton, Qt::NoModifier, movedHandleCenter);
        QTRY_VERIFY_WITH_TIMEOUT(searchSplitter->sizes().at(0) > initialSearchSizes.at(0) + 20,
                                 1000);
        const auto enlargedSearchSizes = searchSplitter->sizes();
        const auto reverseHandleCenter = searchHandle->rect().center();
        const auto movedReverseHandleCenter = reverseHandleCenter - QPoint(40, 0);
        QTest::mousePress(searchHandle, Qt::LeftButton, Qt::NoModifier, reverseHandleCenter);
        QTest::mouseMove(searchHandle, movedReverseHandleCenter, 100);
        QTest::mouseRelease(searchHandle, Qt::LeftButton, Qt::NoModifier, movedReverseHandleCenter);
        QTRY_VERIFY_WITH_TIMEOUT(searchSplitter->sizes().at(0) < enlargedSearchSizes.at(0) - 20,
                                 1000);
        auto* globalQuery = dialog->findChild<QLineEdit*>("globalSearchQuery");
        auto* globalMode = dialog->findChild<QComboBox*>("globalSearchMode");
        auto* globalCase = dialog->findChild<QCheckBox*>("globalSearchCaseSensitive");
        auto* suffix = dialog->findChild<QLineEdit*>("globalSearchSuffix");
        auto* search = dialog->findChild<QPushButton*>("globalSearchButton");
        auto* results = dialog->findChild<QListWidget*>("globalSearchResults");
        auto* preview = dialog->findChild<QPlainTextEdit*>("globalSearchPreview");
        auto* globalStatus = dialog->findChild<QLabel*>("globalSearchStatus");
        auto* treeFilter = window.findChild<QLineEdit*>("projectTreeFilter");
        auto* projectTree = window.findChild<QTreeView*>("projectTree");
        QVERIFY(globalQuery && globalMode && globalCase && suffix && search && results && preview &&
                globalStatus);
        QVERIFY(treeFilter && projectTree);
        globalQuery->setText(QStringLiteral("ab"));
        globalMode->setCurrentIndex(1);
        globalCase->setChecked(true);
        suffix->setText(QStringLiteral("*.cpp"));
        treeFilter->setText(QStringLiteral("not-a-result"));
        QTRY_COMPARE_WITH_TIMEOUT(results->count(), 2, 5000);
        QVERIFY(globalStatus->text().contains(QStringLiteral("1/2")));
        QTRY_VERIFY_WITH_TIMEOUT(preview->toPlainText().contains(QStringLiteral("nearby context")),
                                 5000);
        QVERIFY(results->item(0)->text().contains(QStringLiteral("alpha.cpp:1")));
        QTRY_COMPARE_WITH_TIMEOUT(projectTree->currentIndex().data().toString(),
                                  QStringLiteral("alpha.cpp"), 5000);
        QVERIFY(treeFilter->text().isEmpty());
        QVERIFY(projectTree->isExpanded(projectTree->currentIndex().parent()));
        QVERIFY(projectTree->isExpanded(projectTree->currentIndex().parent().parent()));
        const auto nestedFolder = projectTree->currentIndex().parent();
        projectTree->collapse(nestedFolder);
        const auto folderRect = projectTree->visualRect(nestedFolder);
        QVERIFY(folderRect.isValid());
        QTest::mouseClick(projectTree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          folderRect.center());
        QTRY_VERIFY_WITH_TIMEOUT(projectTree->isExpanded(nestedFolder), 1000);

        globalCase->setChecked(false);
        QTRY_COMPARE_WITH_TIMEOUT(results->count(), 4, 5000);
        QListWidgetItem* alphaResult = nullptr;
        for (int row = 0; row < results->count(); ++row) {
            if (results->item(row)->data(Qt::UserRole).toString() == sourcePath)
                alphaResult = results->item(row);
        }
        QVERIFY(alphaResult);
        const auto rect = results->visualItemRect(alphaResult);
        QTest::mouseDClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTRY_COMPARE_WITH_TIMEOUT(window.currentEditor()->filePath(), sourcePath, 5000);
        editor = window.currentEditor();
        editor->getCursorPosition(&line, &index);
        QCOMPARE(line, 0);
        QVERIFY(index >= 1);
    }

    void plainDirectoryAndDuplicateTabs() {
        QTemporaryDir directory;
        const auto path = directory.filePath("README.md");
        QVERIFY(write(path, "# Test project\n"));
        MainWindow window;
        QVERIFY(window.openProject(directory.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);
        QVERIFY(window.currentEditor());
        window.openFile(path);
        const auto* tabs = window.findChild<QTabWidget*>("editorTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!window.findChild<QPushButton*>("commitButton")->isEnabled());
        window.currentEditor()->append("new text\n");
        QVERIFY(tabs->tabText(0).endsWith(" *"));
        QString error;
        QVERIFY(window.currentEditor()->saveFile(&error));
        QVERIFY(!tabs->tabText(0).endsWith(" *"));
        QVERIFY(window.grab().width() >= 1080);
    }

    void editorTabsRestorePerProject() {
        QTemporaryDir firstProject;
        QTemporaryDir secondProject;
        QVERIFY(firstProject.isValid() && secondProject.isValid());
        const auto firstReadme = firstProject.filePath("README.md");
        const auto firstAlpha = firstProject.filePath("alpha.cpp");
        const auto firstBeta = firstProject.filePath("beta.cpp");
        const auto secondReadme = secondProject.filePath("README.md");
        const auto secondFile = secondProject.filePath("other.cpp");
        QVERIFY(write(firstReadme, "first project\n"));
        QVERIFY(write(firstAlpha, "int alpha;\n"));
        QVERIFY(write(firstBeta, "int beta;\n"));
        QVERIFY(write(secondReadme, "second project\n"));
        QVERIFY(write(secondFile, "int other;\n"));

        struct PreviewModeGuard {
            QVariant previous = qApp->property("previewMode");
            ~PreviewModeGuard() { qApp->setProperty("previewMode", previous); }
        } previewModeGuard;
        qApp->setProperty("previewMode", false);

        {
            MainWindow window;
            QVERIFY(window.openProject(firstProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            window.openFile(firstAlpha);
            window.openFile(firstBeta);
            window.openFile(firstAlpha);
            QCOMPARE(window.findChild<QTabWidget*>("editorTabs")->count(), 3);
            QCOMPARE(window.currentEditor()->filePath(), QFileInfo(firstAlpha).canonicalFilePath());

            QVERIFY(window.openProject(secondProject.path()));
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tabs = window.findChild<QTabWidget*>("editorTabs");
            QVERIFY(tabs);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
            QCOMPARE(window.currentEditor()->filePath(),
                     QFileInfo(secondReadme).canonicalFilePath());
            window.openFile(secondFile);
            QCOMPARE(tabs->count(), 2);
            QVERIFY(window.close());
        }

        {
            MainWindow window;
            QVERIFY(window.openProject(firstProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tabs = window.findChild<QTabWidget*>("editorTabs");
            QVERIFY(tabs);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 5000);
            QCOMPARE(qobject_cast<CodeEditor*>(tabs->widget(0))->filePath(),
                     QFileInfo(firstReadme).canonicalFilePath());
            QCOMPARE(qobject_cast<CodeEditor*>(tabs->widget(1))->filePath(),
                     QFileInfo(firstAlpha).canonicalFilePath());
            QCOMPARE(qobject_cast<CodeEditor*>(tabs->widget(2))->filePath(),
                     QFileInfo(firstBeta).canonicalFilePath());
            QCOMPARE(window.currentEditor()->filePath(), QFileInfo(firstAlpha).canonicalFilePath());
            QVERIFY(window.close());
        }

        {
            MainWindow window;
            QVERIFY(window.openProject(secondProject.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* tabs = window.findChild<QTabWidget*>("editorTabs");
            QVERIFY(tabs);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
            QCOMPARE(qobject_cast<CodeEditor*>(tabs->widget(0))->filePath(),
                     QFileInfo(secondReadme).canonicalFilePath());
            QCOMPARE(qobject_cast<CodeEditor*>(tabs->widget(1))->filePath(),
                     QFileInfo(secondFile).canonicalFilePath());
            QCOMPARE(window.currentEditor()->filePath(), QFileInfo(secondFile).canonicalFilePath());
            QVERIFY(window.close());
        }
    }

    void projectTreeFoldersExpandOnSingleAndDoubleClick() {
        QTemporaryDir project;
        QVERIFY(QDir().mkpath(project.filePath("src/nested/deeper")));
        QVERIFY(write(project.filePath("src/nested/deeper/file.txt"), "contents\n"));
        MainWindow window;
        QVERIFY(window.openProject(project.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);

        auto* tree = window.findChild<QTreeView*>("projectTree");
        QVERIFY(tree);
        const auto findFolder = [tree](const QString& name) {
            std::function<QModelIndex(const QModelIndex&)> visit =
                [&](const QModelIndex& parent) -> QModelIndex {
                for (int row = 0; row < tree->model()->rowCount(parent); ++row) {
                    const auto index = tree->model()->index(row, 0, parent);
                    if (index.data().toString() == name)
                        return index;
                    const auto nested = visit(index);
                    if (nested.isValid())
                        return nested;
                }
                return {};
            };
            return visit(tree->rootIndex());
        };
        QTRY_VERIFY_WITH_TIMEOUT(findFolder(QStringLiteral("src")).isValid(), 5000);
        const auto folder = findFolder(QStringLiteral("src"));
        tree->collapse(folder);
        QVERIFY(!tree->isExpanded(folder));
        const auto rect = tree->visualRect(folder);
        QVERIFY(rect.isValid());

        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(folder), 1000);

        tree->collapse(folder);
        const QPoint branchPoint(rect.left() + tree->indentation() / 2, rect.center().y());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, branchPoint);
        QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(folder), 1000);

        tree->collapse(folder);
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(folder), 1000);

        QTRY_VERIFY_WITH_TIMEOUT(findFolder(QStringLiteral("nested")).isValid(), 5000);
        const auto nested = findFolder(QStringLiteral("nested"));
        tree->collapse(nested);
        const auto nestedRect = tree->visualRect(nested);
        QVERIFY(nestedRect.isValid());
        const QPoint nestedBranchPoint(nestedRect.left() - tree->indentation() / 2,
                                       nestedRect.center().y());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, nestedBranchPoint);
        QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(nested), 1000);

        tree->collapse(nested);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, nestedRect.center());
        QTRY_VERIFY_WITH_TIMEOUT(tree->isExpanded(nested), 1000);
    }

    void projectTreeDisclosureArrowIsVisible() {
        QTemporaryDir project;
        QVERIFY(QDir().mkpath(project.filePath("src/nested/deeper")));
        QVERIFY(write(project.filePath("src/nested/deeper/file.txt"), "contents\n"));
        MainWindow window;
        QVERIFY(window.openProject(project.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);

        auto* tree = window.findChild<QTreeView*>("projectTree");
        QVERIFY(tree);
        const auto findFolder = [tree](const QString& name) {
            std::function<QModelIndex(const QModelIndex&)> visit =
                [&](const QModelIndex& parent) -> QModelIndex {
                for (int row = 0; row < tree->model()->rowCount(parent); ++row) {
                    const auto index = tree->model()->index(row, 0, parent);
                    if (index.data().toString() == name)
                        return index;
                    const auto nested = visit(index);
                    if (nested.isValid())
                        return nested;
                }
                return {};
            };
            return visit(tree->rootIndex());
        };
        QTRY_VERIFY_WITH_TIMEOUT(findFolder(QStringLiteral("src")).isValid(), 5000);
        tree->setExpanded(findFolder(QStringLiteral("src")), true);
        QTRY_VERIFY_WITH_TIMEOUT(findFolder(QStringLiteral("nested")).isValid(), 5000);

        const auto nested = findFolder(QStringLiteral("nested"));
        const auto row = tree->visualRect(nested);
        QVERIFY(row.isValid());
        const QRect arrow(row.left() - tree->indentation(), row.top(), tree->indentation(),
                          row.height());
        QVERIFY(arrow.left() >= 0);

        tree->clearSelection();
        QTest::mouseMove(tree->viewport(), tree->viewport()->rect().bottomRight());
        QImage rendered(tree->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(QColor(QStringLiteral("#25262a")));
        tree->viewport()->render(&rendered);
        const auto visibleArrow = arrow.intersected(tree->viewport()->rect());
        QVERIFY(!visibleArrow.isEmpty());
        const auto hasVisibleArrow = [&](const QImage& image) {
            const QColor background =
                image.pixelColor(tree->viewport()->width() - 2, row.center().y());
            int contrastingPixels = 0;
            for (int y = visibleArrow.top(); y <= visibleArrow.bottom(); ++y) {
                for (int x = visibleArrow.left(); x <= visibleArrow.right(); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (qAbs(pixel.red() - background.red()) +
                            qAbs(pixel.green() - background.green()) +
                            qAbs(pixel.blue() - background.blue()) >
                        60)
                        ++contrastingPixels;
                }
            }
            return contrastingPixels >= 3;
        };
        QVERIFY2(hasVisibleArrow(rendered), "The collapsed folder has no visible disclosure arrow");

        tree->setExpanded(nested, true);
        QImage expandedRendered(tree->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        expandedRendered.fill(QColor(QStringLiteral("#25262a")));
        tree->viewport()->render(&expandedRendered);
        QVERIFY2(hasVisibleArrow(expandedRendered),
                 "The expanded folder has no visible disclosure arrow");
    }

    void commitOnlySelectedFilesFromSubdirectory() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        QDir().mkpath(repository.filePath("sub"));
        const auto path = repository.filePath("sub/file.txt");
        const auto otherPath = repository.filePath("other.txt");
        QVERIFY(write(path, "first\n"));
        QVERIFY(write(otherPath, "other base\n"));
        QVERIFY(git(repository.path(), {"add", "."}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"commit", "-m", "Initial"}) != "TEST_GIT_ERROR");
        QVERIFY(write(path, "second\n"));
        QVERIFY(write(otherPath, "other staged\n"));
        QVERIFY(git(repository.path(), {"add", "--", "other.txt"}) != "TEST_GIT_ERROR");
        MainWindow window;
        QVERIFY(window.openProject(repository.filePath("sub")));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 5000);
        auto* changes = window.findChild<QTreeWidget*>("changesTree");
        QVERIFY(changes);
        QVERIFY(!window.findChild<QToolButton*>("stageButton"));
        QVERIFY(!window.findChild<QToolButton*>("unstageButton"));
        const auto findFile = [changes](const QString& pathText) {
            for (int groupIndex = 0; groupIndex < changes->topLevelItemCount(); ++groupIndex) {
                auto* group = changes->topLevelItem(groupIndex);
                for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
                    auto* item = group->child(childIndex);
                    if (item->toolTip(0) == pathText)
                        return item;
                }
            }
            return static_cast<QTreeWidgetItem*>(nullptr);
        };
        auto* selectedFile = findFile(QStringLiteral("sub/file.txt"));
        auto* excludedStagedFile = findFile(QStringLiteral("other.txt"));
        QVERIFY(selectedFile && excludedStagedFile);
        QVERIFY(selectedFile->flags() & Qt::ItemIsUserCheckable);
        QCOMPARE(selectedFile->checkState(0), Qt::Checked);
        excludedStagedFile->setCheckState(0, Qt::Unchecked);
        QVERIFY(git(repository.path(), {"diff", "--cached", "--", "other.txt"})
                    .contains("+other staged"));
        QVERIFY(git(repository.path(), {"diff", "--cached", "--", "sub/file.txt"}).isEmpty());
        auto* message = window.findChild<QPlainTextEdit*>("commitMessage");
        QVERIFY(message);
        message->setPlainText("Commit via Qt UI");
        auto* button = window.findChild<QPushButton*>("commitButton");
        QVERIFY(button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QCOMPARE(git(repository.path(), {"show", "HEAD:sub/file.txt"}), QByteArray("second\n"));
        QCOMPARE(git(repository.path(), {"show", "HEAD:other.txt"}), QByteArray("other base\n"));
        QVERIFY(git(repository.path(), {"diff", "--cached", "--", "other.txt"})
                    .contains("+other staged"));
        QVERIFY(message->toPlainText().isEmpty());
    }

    void historyFiltersChangedFilesAndDiffNavigation() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto path = repository.filePath("two-hunks.txt");
        const QByteArray base = "first\nold one\ncontext 1\ncontext 2\ncontext 3\ncontext 4\n"
                                "context 5\nold two\nlast\n";
        QVERIFY(write(path, base));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=Alice", "-c", "user.email=alice@example.invalid", "commit",
                     "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(write(path, "first\nbob one\ncontext 1\ncontext 2\ncontext 3\ncontext 4\n"
                            "context 5\nbob two\nlast\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(
            git(repository.path(), {"-c", "user.name=Bob", "-c", "user.email=bob@example.invalid",
                                    "commit", "-m", "Bob update"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"branch", "feature"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"switch", "feature"}) != "TEST_GIT_ERROR");
        QVERIFY(write(path, "first\nalice one\ncontext 1\ncontext 2\ncontext 3\ncontext 4\n"
                            "context 5\nalice two\nlast\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=Alice", "-c", "user.email=alice@example.invalid", "commit",
                     "-m", "Alice update"}) != "TEST_GIT_ERROR");

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        auto* history = window.findChild<QTableWidget*>("historyTable");
        auto* branches = window.findChild<QComboBox*>("historyBranchFilter");
        auto* authors = window.findChild<QComboBox*>("historyAuthorFilter");
        auto* files = window.findChild<QListWidget*>("commitFiles");
        auto* editorTabs = window.findChild<QTabWidget*>("editorTabs");
        QVERIFY(history && branches && authors && files && editorTabs);
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QCOMPARE(branches->currentData().toString(), QStringLiteral("refs/heads/feature"));

        const int mainBranch = branches->findData(QStringLiteral("refs/heads/main"));
        QVERIFY(mainBranch >= 0);
        branches->setCurrentIndex(mainBranch);
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        branches->setCurrentIndex(branches->findData(QStringLiteral("refs/heads/feature")));
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        branches->setCurrentIndex(branches->findData(QStringLiteral("ALL")));
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        branches->setCurrentIndex(branches->findData(QStringLiteral("refs/heads/feature")));
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);

        authors->setEditText(QStringLiteral("Bob"));
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QCOMPARE(history->item(0, 2)->text(), QStringLiteral("Bob"));
        authors->setEditText(authors->itemText(0));
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);

        QCOMPARE(files->count(), 1);
        QVERIFY(files->item(0)->text().contains(QStringLiteral("two-hunks.txt")));
        QVERIFY(files->isVisible());
        const auto area = files->visualItemRect(files->item(0));
        QVERIFY(area.isValid());
        QSignalSpy doubleClicked(files, &QListWidget::itemDoubleClicked);
        QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, area.center());
        QTest::mouseDClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, area.center());
        QCOMPARE(doubleClicked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* diff = window.findChild<FileDiffView*>("fileDiffView");
        QVERIFY2(diff && diff->isVisible(),
                 qPrintable(window.findChild<QLabel*>("activityStatus")->text()));
        QVERIFY(!diff->isWindow());
        QCOMPARE(editorTabs->currentWidget(), static_cast<QWidget*>(diff));
        QTRY_COMPARE_WITH_TIMEOUT(diff->findChild<QLabel*>("differencePosition")->text(),
                                  QStringLiteral("1/2"), 5000);
        auto* next = diff->findChild<QToolButton*>("nextDifferenceButton");
        auto* previous = diff->findChild<QToolButton*>("previousDifferenceButton");
        QVERIFY(next && previous);
        QVERIFY(next->isEnabled());
        QVERIFY(!previous->isEnabled());
        next->click();
        QCOMPARE(diff->findChild<QLabel*>("differencePosition")->text(), QStringLiteral("2/2"));
        QVERIFY(previous->isEnabled());
        QVERIFY(!next->isEnabled());

        auto closeDiffTab = [&](FileDiffView* view) {
            const int tab = editorTabs->indexOf(view);
            QVERIFY(tab >= 0);
            auto* close = qobject_cast<QToolButton*>(
                editorTabs->tabBar()->tabButton(tab, QTabBar::RightSide));
            QVERIFY(close);
            close->click();
            QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<FileDiffView*>().contains(view), 5000);
        };
        closeDiffTab(diff);
        history->setCurrentCell(2, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(files->count(), 1, 5000);
        const auto rootArea = files->visualItemRect(files->item(0));
        QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, rootArea.center());
        QTest::mouseDClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, rootArea.center());
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* rootDiff = window.findChild<FileDiffView*>("fileDiffView");
        QVERIFY(rootDiff && rootDiff->isVisible());
        QVERIFY(!rootDiff->isWindow());
        QVERIFY(rootDiff->findChild<QPlainTextEdit*>("fileDiffPatch")
                    ->toPlainText()
                    .contains("+old one"));
        closeDiffTab(rootDiff);
        history->setCurrentCell(0, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);

        struct MenuState {
            bool shown = false;
            bool workingEnabled = false;
            bool committedEnabled = false;
        };
        auto inspectEditorMenu = [&](bool triggerWorking, bool triggerCommitted) {
            MenuState state;
            QTimer::singleShot(0, &window, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                state.shown = true;
                auto* working = menu->findChild<QAction*>("editorUncommittedDiffAction");
                auto* committed = menu->findChild<QAction*>("editorCommittedDiffAction");
                state.workingEnabled = working && working->isEnabled();
                state.committedEnabled = committed && committed->isEnabled();
                if (triggerWorking && working && working->isEnabled())
                    working->trigger();
                if (triggerCommitted && committed && committed->isEnabled())
                    committed->trigger();
                menu->close();
            });
            const QPoint local(8, 8);
            auto* editor = window.currentEditor();
            QMetaObject::invokeMethod(editor, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, local));
            return state;
        };

        window.openFile(path);
        auto menuState = inspectEditorMenu(false, false);
        QVERIFY(menuState.shown);
        QVERIFY(!menuState.workingEnabled);
        QVERIFY(menuState.committedEnabled);

        QTRY_VERIFY_WITH_TIMEOUT(window.findChildren<FileDiffView*>().isEmpty(), 5000);
        QVERIFY(write(path, "first\nalice one\ncontext 1\ncontext 2\ncontext 3\ncontext 4\n"
                            "context 5\nworking two\nlast\n"));
        window.refreshRepository();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        menuState = inspectEditorMenu(true, false);
        QVERIFY(menuState.shown);
        QVERIFY(menuState.workingEnabled);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* workingDiff = window.findChild<FileDiffView*>("fileDiffView");
        QVERIFY(workingDiff && workingDiff->isVisible());
        QVERIFY(!workingDiff->isWindow());
        QCOMPARE(editorTabs->currentWidget(), static_cast<QWidget*>(workingDiff));
        auto* patch = workingDiff->findChild<QPlainTextEdit*>("fileDiffPatch");
        QVERIFY(patch);
        QVERIFY(patch->toPlainText().contains("+working two"));
        QVERIFY(patch->toPlainText().contains("-alice two"));

        closeDiffTab(workingDiff);
        menuState = inspectEditorMenu(false, true);
        QVERIFY(menuState.shown);
        QVERIFY(menuState.committedEnabled);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* committedDiff = window.findChild<FileDiffView*>("fileDiffView");
        QVERIFY(committedDiff && committedDiff->isVisible());
        QVERIFY(!committedDiff->isWindow());
        QCOMPARE(editorTabs->currentWidget(), static_cast<QWidget*>(committedDiff));
        patch = committedDiff->findChild<QPlainTextEdit*>("fileDiffPatch");
        QVERIFY(patch);
        QVERIFY(patch->toPlainText().contains("+alice two"));
        QVERIFY(patch->toPlainText().contains("-bob two"));
    }

    void commitChangedFilesUseGitStatusColors() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto modifiedPath = repository.filePath("modified.txt");
        const auto deletedPath = repository.filePath("deleted.txt");
        const auto addedPath = repository.filePath("added.txt");
        QVERIFY(write(modifiedPath, "before\n"));
        QVERIFY(write(deletedPath, "remove me\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");

        QVERIFY(write(modifiedPath, "after\n"));
        QVERIFY(QFile::remove(deletedPath));
        QVERIFY(write(addedPath, "new file\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Status colors"}) != "TEST_GIT_ERROR");

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        auto* history = window.findChild<QTableWidget*>("historyTable");
        auto* files = window.findChild<QListWidget*>("commitFiles");
        QVERIFY(history && files);
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 2, 10000);
        history->setCurrentCell(0, 0);
        QTRY_COMPARE_WITH_TIMEOUT(files->count(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);

        QListWidgetItem* added = nullptr;
        QListWidgetItem* modified = nullptr;
        QListWidgetItem* deleted = nullptr;
        for (int i = 0; i < files->count(); ++i) {
            auto* item = files->item(i);
            if (item->text().startsWith(QStringLiteral("A   ")))
                added = item;
            else if (item->text().startsWith(QStringLiteral("M   ")))
                modified = item;
            else if (item->text().startsWith(QStringLiteral("D   ")))
                deleted = item;
        }
        QVERIFY(added && modified && deleted);
        QCOMPARE(added->foreground().color(), stateColor(FileState::Added));
        QCOMPARE(modified->foreground().color(), stateColor(FileState::Modified));
        QCOMPARE(deleted->foreground().color(), stateColor(FileState::Deleted));
        QVERIFY(added->foreground().color() != modified->foreground().color());
        QVERIFY(added->foreground().color() != deleted->foreground().color());
        QVERIFY(modified->foreground().color() != deleted->foreground().color());
    }

    void workspacePanelsCanBeCollapsedAndRestored() {
        QTemporaryDir project;
        QVERIFY(write(project.filePath("README.md"), "panel test\n"));
        MainWindow window;
        QVERIFY(window.openProject(project.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);

        auto* projectPanel = window.findChild<QWidget*>("projectPanel");
        auto* commitPanel = window.findChild<QWidget*>("commitPanel");
        auto* historyPanel = window.findChild<QTabWidget*>("bottomTabs");
        auto* horizontal = window.findChild<QSplitter*>("mainHorizontalSplitter");
        auto* vertical = window.findChild<QSplitter*>("mainVerticalSplitter");
        auto* editorCommit = window.findChild<QSplitter*>("editorCommitSplitter");
        auto* hideProject = window.findChild<QToolButton*>("hideProjectPanelButton");
        auto* hideCommit = window.findChild<QToolButton*>("hideCommitPanelButton");
        auto* hideHistory = window.findChild<QToolButton*>("hideHistoryPanelButton");
        auto* projectRail = window.findChild<QToolButton*>("projectPanelToggleButton");
        auto* commitRail = window.findChild<QToolButton*>("commitPanelToggleButton");
        auto* historyRail = window.findChild<QToolButton*>("historyPanelToggleButton");
        auto* projectAction = window.findChild<QAction*>("toggleProjectPanelAction");
        auto* commitAction = window.findChild<QAction*>("toggleCommitPanelAction");
        auto* historyAction = window.findChild<QAction*>("toggleHistoryPanelAction");
        QVERIFY(projectPanel && commitPanel && historyPanel);
        QVERIFY(horizontal && vertical && editorCommit);
        QVERIFY(hideProject && hideCommit && hideHistory);
        QVERIFY(projectRail && commitRail && historyRail);
        QVERIFY(projectAction && commitAction && historyAction);
        QVERIFY(projectRail->isVisible() && commitRail->isVisible() && historyRail->isVisible());
        QVERIFY(commitRail->width() >= 32 && historyRail->width() >= 32);
        QCOMPARE(commitRail->text(), QString::fromUtf8("提交"));
        QCOMPARE(historyRail->text(), QString::fromUtf8("日志"));
        QTRY_VERIFY_WITH_TIMEOUT(horizontal->sizes().value(0) >= 180, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(editorCommit->sizes().value(1) >= 270, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(vertical->sizes().value(1) >= 100, 5000);

        hideProject->click();
        QVERIFY(projectPanel->isHidden());
        QVERIFY(!projectAction->isChecked());
        projectRail->click();
        QVERIFY(!projectPanel->isHidden());
        QVERIFY(projectAction->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(horizontal->sizes().value(0) >= 180, 5000);

        QTest::mouseClick(hideCommit, Qt::LeftButton, Qt::NoModifier, hideCommit->rect().center());
        QVERIFY(commitPanel->isHidden());
        QCOMPARE(commitRail->toolTip(), QString::fromUtf8("展开提交面板"));
        QTRY_VERIFY_WITH_TIMEOUT(commitRail->isVisible(), 1000);
        QTest::mouseClick(commitRail, Qt::LeftButton, Qt::NoModifier, commitRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(commitPanel->isVisible(), 1000);
        QCOMPARE(commitRail->toolTip(), QString::fromUtf8("隐藏提交面板"));
        QVERIFY(commitAction->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(editorCommit->sizes().value(1) >= 270, 5000);
        QTest::mouseClick(commitRail, Qt::LeftButton, Qt::NoModifier, commitRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(commitPanel->isHidden(), 1000);
        QCOMPARE(commitRail->toolTip(), QString::fromUtf8("展开提交面板"));
        QTest::mouseClick(commitRail, Qt::LeftButton, Qt::NoModifier, commitRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(commitPanel->isVisible(), 1000);

        QTest::mouseClick(hideHistory, Qt::LeftButton, Qt::NoModifier,
                          hideHistory->rect().center());
        QVERIFY(historyPanel->isHidden());
        QCOMPARE(historyRail->toolTip(), QString::fromUtf8("展开 Git 日志面板"));
        QTRY_VERIFY_WITH_TIMEOUT(historyRail->isVisible(), 1000);
        QTest::mouseClick(historyRail, Qt::LeftButton, Qt::NoModifier,
                          historyRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(historyPanel->isVisible(), 1000);
        QCOMPARE(historyRail->toolTip(), QString::fromUtf8("隐藏 Git 日志面板"));
        QVERIFY(historyAction->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(vertical->sizes().value(1) >= 100, 5000);
        QTest::mouseClick(historyRail, Qt::LeftButton, Qt::NoModifier,
                          historyRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(historyPanel->isHidden(), 1000);
        QCOMPARE(historyRail->toolTip(), QString::fromUtf8("展开 Git 日志面板"));
        QTest::mouseClick(historyRail, Qt::LeftButton, Qt::NoModifier,
                          historyRail->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(historyPanel->isVisible(), 1000);

        projectRail->click();
        QVERIFY(projectPanel->isHidden());
        QVERIFY(!projectAction->isChecked());
        projectRail->click();
        QVERIFY(!projectPanel->isHidden());
        QVERIFY(projectAction->isChecked());
    }

    void hiddenWorkspacePanelsRestoreAfterRestart() {
        QTemporaryDir project;
        QVERIFY(write(project.filePath("README.md"), "panel restart test\n"));

        struct PreviewModeGuard {
            QVariant previous = qApp->property("previewMode");
            ~PreviewModeGuard() { qApp->setProperty("previewMode", previous); }
        } previewModeGuard;
        qApp->setProperty("previewMode", false);

        {
            MainWindow window;
            QVERIFY(window.openProject(project.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* hideCommit = window.findChild<QToolButton*>("hideCommitPanelButton");
            auto* hideHistory = window.findChild<QToolButton*>("hideHistoryPanelButton");
            QVERIFY(hideCommit && hideHistory);
            hideCommit->click();
            hideHistory->click();
            QVERIFY(window.close());
        }

        {
            MainWindow window;
            QVERIFY(window.openProject(project.path()));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
            auto* commitPanel = window.findChild<QWidget*>("commitPanel");
            auto* historyPanel = window.findChild<QTabWidget*>("bottomTabs");
            auto* commitRail = window.findChild<QToolButton*>("commitPanelToggleButton");
            auto* historyRail = window.findChild<QToolButton*>("historyPanelToggleButton");
            auto* editorCommit = window.findChild<QSplitter*>("editorCommitSplitter");
            auto* vertical = window.findChild<QSplitter*>("mainVerticalSplitter");
            QVERIFY(commitPanel && historyPanel && commitRail && historyRail && editorCommit &&
                    vertical);
            QVERIFY(commitPanel->isHidden());
            QVERIFY(historyPanel->isHidden());
            QCOMPARE(commitRail->toolTip(), QString::fromUtf8("展开提交面板"));
            QCOMPARE(historyRail->toolTip(), QString::fromUtf8("展开 Git 日志面板"));
            QTest::mouseClick(commitRail, Qt::LeftButton, Qt::NoModifier,
                              commitRail->rect().center());
            QTest::mouseClick(historyRail, Qt::LeftButton, Qt::NoModifier,
                              historyRail->rect().center());
            QTRY_VERIFY_WITH_TIMEOUT(!commitPanel->isHidden() && !historyPanel->isHidden(), 1000);
            QTRY_VERIFY_WITH_TIMEOUT(editorCommit->sizes().value(1) >= 270, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(vertical->sizes().value(1) >= 100, 5000);
            QVERIFY(window.close());
        }
    }

    void changeGroupCheckBoxesSelectFilesForCommit() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto firstPath = repository.filePath("first.txt");
        const auto secondPath = repository.filePath("second.txt");
        QVERIFY(write(firstPath, "first base\n"));
        QVERIFY(write(secondPath, "second base\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(write(firstPath, "first changed\n"));
        QVERIFY(write(secondPath, "second changed\n"));

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        auto* changes = window.findChild<QTreeWidget*>("changesTree");
        QVERIFY(changes);
        auto findGroup = [changes](const QString& kind) {
            for (int i = 0; i < changes->topLevelItemCount(); ++i) {
                auto* item = changes->topLevelItem(i);
                if (item->data(0, Qt::UserRole + 2).toString() == kind)
                    return item;
            }
            return static_cast<QTreeWidgetItem*>(nullptr);
        };

        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(findGroup(QStringLiteral("changes")) &&
                                     findGroup(QStringLiteral("changes"))->childCount() == 2,
                                 10000);
        auto* changesGroup = findGroup(QStringLiteral("changes"));
        QVERIFY(changesGroup);
        QCOMPARE(changesGroup->checkState(0), Qt::Checked);
        changesGroup->setCheckState(0, Qt::Unchecked);
        QCOMPARE(changesGroup->checkState(0), Qt::Unchecked);
        for (int index = 0; index < changesGroup->childCount(); ++index)
            QCOMPARE(changesGroup->child(index)->checkState(0), Qt::Unchecked);
        QCOMPARE(git(repository.path(), {"diff", "--cached"}), QByteArray());
        QVERIFY(git(repository.path(), {"diff"}).contains("+first changed"));
        changesGroup->setCheckState(0, Qt::Checked);
        QCOMPARE(changesGroup->checkState(0), Qt::Checked);
        for (int index = 0; index < changesGroup->childCount(); ++index)
            QCOMPARE(changesGroup->child(index)->checkState(0), Qt::Checked);
    }

    void changeFileGitDiffContextMenuOpensMainAreaDiff() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto trackedPath = repository.filePath("tracked.txt");
        const auto untrackedPath = repository.filePath("new.txt");
        QVERIFY(write(trackedPath, "base version\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(write(trackedPath, "staged version\n"));
        QVERIFY(git(repository.path(), {"add", "--", "tracked.txt"}) != "TEST_GIT_ERROR");
        QVERIFY(write(trackedPath, "working version\n"));
        QVERIFY(write(untrackedPath, "new file content\n"));

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        auto* changes = window.findChild<QTreeWidget*>("changesTree");
        auto* tabs = window.findChild<QTabWidget*>("editorTabs");
        QVERIFY(changes && tabs);
        const auto findGroup = [changes](const QString& kind) {
            for (int i = 0; i < changes->topLevelItemCount(); ++i) {
                auto* group = changes->topLevelItem(i);
                if (group->data(0, Qt::UserRole + 2).toString() == kind)
                    return group;
            }
            return static_cast<QTreeWidgetItem*>(nullptr);
        };
        const auto findFile = [](QTreeWidgetItem* group, const QString& path) {
            if (!group)
                return static_cast<QTreeWidgetItem*>(nullptr);
            for (int i = 0; i < group->childCount(); ++i) {
                auto* item = group->child(i);
                if (item->toolTip(0) == path)
                    return item;
            }
            return static_cast<QTreeWidgetItem*>(nullptr);
        };
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(
            findFile(findGroup(QStringLiteral("changes")), QStringLiteral("tracked.txt")) &&
                findFile(findGroup(QStringLiteral("untracked")), QStringLiteral("new.txt")),
            10000);

        struct ContextMenuState {
            bool actionFound = false;
            bool enabled = false;
        };
        const auto triggerGitDiff = [&window, changes](QTreeWidgetItem* item) {
            auto state = std::make_shared<ContextMenuState>();
            changes->scrollToItem(item);
            const auto rect = changes->visualItemRect(item);
            QTimer::singleShot(0, &window, [state] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                auto* action = menu ? menu->findChild<QAction*>("gitDiffAction") : nullptr;
                if (!action)
                    return;
                state->actionFound = true;
                state->enabled = action->isEnabled();
                if (state->enabled)
                    action->trigger();
                menu->close();
            });
            QMetaObject::invokeMethod(changes, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, rect.center()));
            return state;
        };
        const auto currentPatch = [tabs] {
            auto* current = tabs->currentWidget();
            return current ? current->findChild<QPlainTextEdit*>("fileDiffPatch") : nullptr;
        };

        auto* previous = tabs->currentWidget();
        auto state = triggerGitDiff(
            findFile(findGroup(QStringLiteral("changes")), QStringLiteral("tracked.txt")));
        QVERIFY(state->actionFound && state->enabled);
        QTRY_VERIFY_WITH_TIMEOUT(tabs->currentWidget() != previous && currentPatch(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(currentPatch()->toPlainText().contains("-base version"));
        QVERIFY(currentPatch()->toPlainText().contains("+working version"));
        QVERIFY(!currentPatch()->toPlainText().contains("staged version"));

        previous = tabs->currentWidget();
        state = triggerGitDiff(
            findFile(findGroup(QStringLiteral("untracked")), QStringLiteral("new.txt")));
        QVERIFY(state->actionFound && state->enabled);
        QTRY_VERIFY_WITH_TIMEOUT(tabs->currentWidget() != previous && currentPatch(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(currentPatch()->toPlainText().contains("+new file content"));
    }

    void remoteManagerMasksCredentialsInRemoteUrls() {
        QTemporaryDir repository;
        QVERIFY(repository.isValid());
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"remote", "add", "origin",
                                        "https://alice:private-token@example.invalid/repo.git"}) !=
                "TEST_GIT_ERROR");
        GitClient client(repository.path());
        RemoteManagerDialog dialog(&client, repository.path(), false);
        dialog.show();
        auto* table = dialog.findChild<QTableWidget*>("remotesTable");
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 5000);
        const QString displayedUrl = table->item(0, 1)->text();
        QVERIFY(!displayedUrl.contains(QStringLiteral("private-token")));
        QVERIFY(displayedUrl.contains(QStringLiteral("•••")));
        QVERIFY(dialog.findChild<QPushButton*>("addRemoteButton")->isEnabled());
    }

    void commitAndPushPreviewsCommitsFilesAndMainAreaDiff() {
        QTemporaryDir repository;
        QTemporaryDir remote;
        QVERIFY(repository.isValid() && remote.isValid());
        QVERIFY(git(remote.path(), {"init", "--bare", "--initial-branch=main"}) !=
                "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto path = repository.filePath("app.txt");
        QVERIFY(write(path, "before push\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"remote", "add", "origin", remote.path()}) !=
                "TEST_GIT_ERROR");
        QVERIFY(write(path, "after push\n"));

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* message = window.findChild<QPlainTextEdit*>("commitMessage");
        auto* commitAndPush = window.findChild<QPushButton*>("commitAndPushButton");
        QVERIFY(message && commitAndPush);
        message->setPlainText("Commit for push preview");
        QVERIFY(commitAndPush->isEnabled());
        QTest::mouseClick(commitAndPush, Qt::LeftButton);

        PushDialog* dialog = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((dialog = window.findChild<PushDialog*>("pushDialog")), 10000);
        auto* commits = dialog->findChild<QTableWidget*>("pushCommitTable");
        auto* files = dialog->findChild<QListWidget*>("pushCommitFiles");
        QVERIFY(commits && files);
        QTRY_COMPARE_WITH_TIMEOUT(commits->rowCount(), 2, 15000);
        commits->setCurrentCell(1, 0);
        QTRY_COMPARE_WITH_TIMEOUT(files->count(), 1, 10000);
        QCOMPARE(files->item(0)->data(Qt::UserRole).toString(), QStringLiteral("app.txt"));

        QVERIFY(commits->item(1, 1)->text().contains(QStringLiteral("Commit for push preview")));

        auto* push = dialog->findChild<QPushButton*>("pushNowButton");
        QVERIFY(push && push->isEnabled());
        QString pushError;
        QTimer dismissPushError;
        dismissPushError.setInterval(10);
        connect(&dismissPushError, &QTimer::timeout, qApp, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box)
                return;
            pushError = box->text() + '\n' + box->informativeText();
            box->accept();
        });
        dismissPushError.start();
        QTest::mouseClick(push, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 20000);
        dismissPushError.stop();
        auto* status = dialog->findChild<QLabel*>("pushPreviewStatus");
        QVERIFY(status);
        QVERIFY2(pushError.isEmpty(), qPrintable(pushError));
        QVERIFY2(status->text().contains(QString::fromUtf8("推送完成")),
                 qPrintable(status->text()));
        QCOMPARE(git(remote.path(), {"show", "HEAD:app.txt"}), QByteArray("after push\n"));
    }

    void locateCurrentFileAndEditSourceContextMenus() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto sourcePath = repository.filePath("src/nested/edited.cpp");
        const auto readmePath = repository.filePath("README.md");
        QVERIFY(QDir().mkpath(QFileInfo(sourcePath).absolutePath()));
        QVERIFY(write(sourcePath, "int value = 1;\n"));
        QVERIFY(write(readmePath, "# Context menu test\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(write(sourcePath, "int value = 2;\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Edit source fixture"}) != "TEST_GIT_ERROR");
        QVERIFY(write(sourcePath, "int value = 3;\n"));

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        auto* tree = window.findChild<QTreeView*>("projectTree");
        auto* changes = window.findChild<QTreeWidget*>("changesTree");
        auto* history = window.findChild<QTableWidget*>("historyTable");
        auto* commitFiles = window.findChild<QListWidget*>("commitFiles");
        auto* locate = window.findChild<QToolButton*>("selectCurrentFileButton");
        QVERIFY(tree && changes && history && commitFiles && locate);
        QTRY_COMPARE_WITH_TIMEOUT(history->rowCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(locate->isEnabled());

        auto* proxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
        auto* fileSystem = proxy ? qobject_cast<QFileSystemModel*>(proxy->sourceModel()) : nullptr;
        QVERIFY(proxy && fileSystem);
        QTRY_VERIFY_WITH_TIMEOUT(fileSystem->index(sourcePath).isValid(), 5000);
        window.openFile(sourcePath);
        QCOMPARE(window.currentEditor()->filePath(), QFileInfo(sourcePath).canonicalFilePath());
        tree->setCurrentIndex(QModelIndex{});
        QTest::mouseClick(locate, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->data(tree->currentIndex()).toString(),
                                  QStringLiteral("edited.cpp"), 5000);
        QVERIFY(tree->isExpanded(tree->currentIndex().parent()));
        QVERIFY(tree->isExpanded(tree->currentIndex().parent().parent()));

        struct ContextMenuState {
            bool actionFound = false;
            bool enabled = false;
        };
        auto triggerEditSource = [&](QWidget* view, const QPoint& position) {
            auto state = std::make_shared<ContextMenuState>();
            QTimer::singleShot(0, &window, [state] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                auto* action = menu ? menu->findChild<QAction*>("editSourceAction") : nullptr;
                if (!action)
                    return;
                state->actionFound = true;
                state->enabled = action->isEnabled();
                if (state->enabled)
                    action->trigger();
                menu->close();
            });
            QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, position));
            return state;
        };

        QTreeWidgetItem* changedSource = nullptr;
        const auto findChangedSource = [&] {
            changedSource = nullptr;
            for (int group = 0; group < changes->topLevelItemCount(); ++group) {
                auto* parent = changes->topLevelItem(group);
                for (int child = 0; child < parent->childCount(); ++child) {
                    auto* candidate = parent->child(child);
                    if (candidate->toolTip(0) == QStringLiteral("src/nested/edited.cpp"))
                        changedSource = candidate;
                }
            }
            return changedSource != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findChangedSource(), 10000);
        changes->scrollToItem(changedSource);
        const auto changeRect = changes->visualItemRect(changedSource);
        QVERIFY(changeRect.isValid());
        QCOMPARE(changes->itemAt(changeRect.center()), changedSource);
        window.openFile(readmePath);
        auto menuState = triggerEditSource(changes, changeRect.center());
        QVERIFY(menuState->actionFound);
        QVERIFY(menuState->enabled);
        QTRY_COMPARE_WITH_TIMEOUT(window.currentEditor()->filePath(),
                                  QFileInfo(sourcePath).canonicalFilePath(), 5000);

        history->setCurrentCell(0, 0);
        QTRY_COMPARE_WITH_TIMEOUT(commitFiles->count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(commitFiles->isVisible());
        commitFiles->scrollToItem(commitFiles->item(0));
        const auto commitFileRect = commitFiles->visualItemRect(commitFiles->item(0));
        QVERIFY(commitFileRect.isValid());
        QCOMPARE(commitFiles->itemAt(commitFileRect.center()), commitFiles->item(0));
        window.openFile(readmePath);
        menuState = triggerEditSource(commitFiles, commitFileRect.center());
        QVERIFY(menuState->actionFound);
        QVERIFY(menuState->enabled);
        QTRY_COMPARE_WITH_TIMEOUT(window.currentEditor()->filePath(),
                                  QFileInfo(sourcePath).canonicalFilePath(), 5000);
    }

    void compareWithBranchListsLocalAndRemoteRefs() {
        QTemporaryDir repository;
        QVERIFY(git(repository.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR");
        const auto path = repository.filePath("branch-compare.txt");
        QVERIFY(write(path, "base version\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Base"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"branch", "feature"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"switch", "feature"}) != "TEST_GIT_ERROR");
        QVERIFY(write(path, "feature version\n"));
        QVERIFY(git(repository.path(), {"add", "--all"}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(),
                    {"-c", "user.name=idea4git", "-c", "user.email=idea4git@example.invalid",
                     "commit", "-m", "Feature"}) != "TEST_GIT_ERROR");
        const auto featureHash =
            QString::fromUtf8(git(repository.path(), {"rev-parse", "HEAD"}).trimmed());
        QVERIFY(!featureHash.isEmpty());
        QVERIFY(git(repository.path(), {"update-ref", "refs/remotes/origin/feature",
                                        featureHash}) != "TEST_GIT_ERROR");
        QVERIFY(git(repository.path(), {"switch", "main"}) != "TEST_GIT_ERROR");
        QVERIFY(write(path, "working tree version\n"));

        MainWindow window;
        QVERIFY(window.openProject(repository.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        window.openFile(path);
        auto* editor = window.currentEditor();
        QVERIFY(editor);

        struct BranchMenuState {
            bool opened = false;
            bool localBranchFound = false;
            bool remoteBranchFound = false;
            bool selectedRemoteBranch = false;
        } state;
        QTimer::singleShot(0, &window, [&] {
            auto* contextMenu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!contextMenu)
                return;
            auto* branchMenu = contextMenu->findChild<QMenu*>("editorCompareBranchMenu");
            if (!branchMenu)
                return;
            state.opened =
                QMetaObject::invokeMethod(branchMenu, "aboutToShow", Qt::DirectConnection);
            if (!state.opened)
                return;

            auto* poll = new QTimer(&window);
            poll->setInterval(20);
            QPointer<QMenu> contextGuard(contextMenu);
            QPointer<QMenu> branchGuard(branchMenu);
            QPointer<QTimer> pollGuard(poll);
            connect(poll, &QTimer::timeout, &window, [&, poll, contextGuard, branchGuard] {
                if (!contextGuard || !branchGuard) {
                    poll->stop();
                    poll->deleteLater();
                    return;
                }
                QAction* localFeature = nullptr;
                QAction* remoteFeature = nullptr;
                for (auto* action : branchGuard->actions()) {
                    if (action->objectName() != QStringLiteral("compareBranchAction"))
                        continue;
                    const auto ref = action->data().toString();
                    if (ref == QStringLiteral("refs/heads/feature"))
                        localFeature = action;
                    else if (ref == QStringLiteral("refs/remotes/origin/feature"))
                        remoteFeature = action;
                }
                state.localBranchFound = localFeature != nullptr;
                state.remoteBranchFound = remoteFeature != nullptr;
                if (!localFeature || !remoteFeature)
                    return;
                state.selectedRemoteBranch = true;
                poll->stop();
                remoteFeature->trigger();
                contextGuard->close();
                poll->deleteLater();
            });
            poll->start();
            QTimer::singleShot(10000, &window, [pollGuard, contextGuard] {
                if (pollGuard && pollGuard->isActive()) {
                    pollGuard->stop();
                    if (contextGuard)
                        contextGuard->close();
                    pollGuard->deleteLater();
                }
            });
        });
        QMetaObject::invokeMethod(editor, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, QPoint(8, 8)));
        QVERIFY(state.opened);
        QVERIFY(state.localBranchFound);
        QVERIFY(state.remoteBranchFound);
        QVERIFY(state.selectedRemoteBranch);

        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* diff = window.findChild<FileDiffView*>("fileDiffView");
        QVERIFY(diff && diff->isVisible());
        QCOMPARE(window.findChild<QTabWidget*>("editorTabs")->currentWidget(),
                 static_cast<QWidget*>(diff));
        auto* patch = diff->findChild<QPlainTextEdit*>("fileDiffPatch");
        QVERIFY(patch);
        QVERIFY(patch->toPlainText().contains("-feature version"));
        QVERIFY(patch->toPlainText().contains("+working tree version"));
    }

  private:
    QTemporaryDir m_settings;
};

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
