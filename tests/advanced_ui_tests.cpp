#include "codeeditor.h"
#include "gitdialogs.h"
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

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
QByteArray git(const QString& root, const QStringList& args) {
    QProcess process;
    process.setWorkingDirectory(root);
    process.start("git", args);
    if (!process.waitForFinished(10000) || process.exitCode() != 0)
        return "TEST_GIT_ERROR";
    return process.readAllStandardOutput();
}
void answer(QMessageBox::StandardButton choice) {
    QTimer::singleShot(0, [choice] {
        auto* box = qobject_cast<QMessageBox*>(qApp->activeModalWidget());
        if (box && box->button(choice))
            box->button(choice)->click();
    });
}
struct Repository {
    QTemporaryDir dir;
    GitClient client{dir.path()};
    GitOperations operations{&client, dir.path()};
    bool init() {
        return git(dir.path(), {"init", "--initial-branch=main"}) != "TEST_GIT_ERROR" &&
               write(dir.filePath("file.txt"), "base\n") && commit("Base");
    }
    bool commit(const QString& message) {
        return git(dir.path(), {"add", "-A"}) != "TEST_GIT_ERROR" &&
               git(dir.path(), {"commit", "-m", message}) != "TEST_GIT_ERROR";
    }
    bool mergeConflict() {
        if (!init() || git(dir.path(), {"switch", "-c", "feature"}) == "TEST_GIT_ERROR")
            return false;
        if (!write(dir.filePath("file.txt"), "feature\n") || !commit("Feature"))
            return false;
        if (git(dir.path(), {"switch", "main"}) == "TEST_GIT_ERROR" ||
            !write(dir.filePath("file.txt"), "main\n") || !commit("Main"))
            return false;
        return git(dir.path(), {"merge", "--no-edit", "feature"}) == "TEST_GIT_ERROR";
    }
};
void capture(QWidget* widget, const QString& name) {
    const auto folder = qEnvironmentVariable("IDEA4GIT_ADVANCED_QA");
    if (!folder.isEmpty()) {
        QDir().mkpath(folder);
        QVERIFY(widget->grab().save(folder + '/' + name));
    }
}
} // namespace

class AdvancedUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("idea4gitAdvancedTests");
        QCoreApplication::setApplicationName("idea4gitAdvancedTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        qApp->setProperty("previewMode", true);
        qApp->setFont(QFont(QStringLiteral("Noto Sans CJK SC"), 10));
        qApp->setStyle("Fusion");
        const auto config = m_settings.filePath("gitconfig");
        QVERIFY(write(config,
                      "[user]\n name = idea4git Tests\n email = tests@example.invalid\n[commit]\n "
                      "gpgSign = false\n[core]\n hooksPath = /dev/null\n"));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(config));
    }

    void comparisonShowsBothSnapshotsAndPatch() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.dir.filePath("file.txt"), "changed\n"));
        QVERIFY(r.commit("Changed"));
        MainWindow host;
        CompareDialog dialog(&r.operations, false, &host);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        auto* list = dialog.findChild<QListWidget*>("comparisonFiles");
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        auto* before = dialog.findChild<CodeEditor*>("comparisonBefore");
        auto* after = dialog.findChild<CodeEditor*>("comparisonAfter");
        QVERIFY(before);
        QVERIFY(after);
        QCOMPARE(before->text(), QStringLiteral("base\n"));
        QCOMPARE(after->text(), QStringLiteral("changed\n"));
        QVERIFY(before->isReadOnly());
        QVERIFY(after->isReadOnly());
        const auto patch = dialog.findChild<QPlainTextEdit*>("comparisonPatch")->toPlainText();
        QVERIFY(patch.contains("-base"));
        QVERIFY(patch.contains("+changed"));
        QVERIFY(before->markersAtLine(0) & (1 << 8));
        QVERIFY(after->markersAtLine(0) & (1 << 9));
        QCOMPARE(read(r.dir.filePath("file.txt")), QByteArray("changed\n"));
        capture(&dialog, "compare-zh.png");
        auto* left = dialog.findChild<QComboBox*>("compareLeft");
        auto* right = dialog.findChild<QComboBox*>("compareRight");
        left->setCurrentText("HEAD");
        right->setCurrentText("HEAD");
        dialog.findChild<QPushButton*>("compareButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(list->count(), 0);
    }

    void conflictHasThreeSourcesAndExplicitSaveStage() {
        Repository r;
        QVERIFY(r.mergeConflict());
        MainWindow host;
        ConflictDialog dialog(&r.operations, "file.txt", false, &host);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(dialog.findChild<CodeEditor*>("conflictBase")->text(), QStringLiteral("base\n"));
        QCOMPARE(dialog.findChild<CodeEditor*>("conflictOurs")->text(), QStringLiteral("main\n"));
        QCOMPARE(dialog.findChild<CodeEditor*>("conflictTheirs")->text(),
                 QStringLiteral("feature\n"));
        auto* save = dialog.findChild<QPushButton*>("conflictSave");
        auto* stage = dialog.findChild<QPushButton*>("conflictStage");
        QVERIFY(!stage->isEnabled());
        dialog.findChild<QPushButton*>("useTheirs")->click();
        auto* result = dialog.findChild<CodeEditor*>("conflictResult");
        QCOMPARE(result->text(), QStringLiteral("feature\n"));
        result->setText("resolved main and feature\n");
        QVERIFY(save->isEnabled());
        QVERIFY(!stage->isEnabled());
        save->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(read(r.dir.filePath("file.txt")), QByteArray("resolved main and feature\n"));
        QVERIFY(!git(r.dir.path(), {"ls-files", "--unmerged"}).isEmpty());
        QVERIFY(stage->isEnabled());
        capture(&dialog, "conflict-zh.png");
        stage->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QVERIFY(git(r.dir.path(), {"ls-files", "--unmerged"}).isEmpty());
        QVERIFY(!stage->isEnabled());
        QVERIFY(result->isReadOnly());
        QVERIFY(QFileInfo::exists(
            r.dir.filePath(".git/MERGE_HEAD"))); // Staging must not auto-continue.
    }

    void conflictUnsavedCloseCanBeCancelled() {
        Repository r;
        QVERIFY(r.mergeConflict());
        MainWindow host;
        ConflictDialog dialog(&r.operations, "file.txt", true, &host);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        dialog.findChild<QPushButton*>("useOurs")->click();
        answer(QMessageBox::Cancel);
        dialog.reject();
        QVERIFY(dialog.isVisible());
        answer(QMessageBox::Discard);
        dialog.reject();
        QVERIFY(!dialog.isVisible());
        QVERIFY(read(r.dir.filePath("file.txt")).contains("<<<<<<<"));
    }

    void operationFormsExecuteRealGit_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("resetMode");
        QTest::newRow("merge") << int(OperationKind::Merge) << 0;
        QTest::newRow("rebase") << int(OperationKind::Rebase) << 0;
        QTest::newRow("cherry-pick") << int(OperationKind::CherryPick) << 0;
        QTest::newRow("revert") << int(OperationKind::Revert) << 0;
        QTest::newRow("reset-soft") << int(OperationKind::Reset) << int(ResetMode::Soft);
        QTest::newRow("reset-mixed") << int(OperationKind::Reset) << int(ResetMode::Mixed);
        QTest::newRow("reset-hard") << int(OperationKind::Reset) << int(ResetMode::Hard);
    }
    void operationFormsExecuteRealGit() {
        QFETCH(int, kind);
        QFETCH(int, resetMode);
        Repository r;
        QVERIFY(r.init());
        const auto base = QString::fromLatin1(git(r.dir.path(), {"rev-parse", "HEAD"}).trimmed());
        QVERIFY(git(r.dir.path(), {"switch", "-c", "feature"}) != "TEST_GIT_ERROR");
        QVERIFY(write(r.dir.filePath("feature.txt"), "feature\n"));
        QVERIFY(r.commit("Feature"));
        const auto feature =
            QString::fromLatin1(git(r.dir.path(), {"rev-parse", "HEAD"}).trimmed());
        QVERIFY(git(r.dir.path(), {"switch", "main"}) != "TEST_GIT_ERROR");
        QVERIFY(write(r.dir.filePath("main.txt"), "main\n"));
        QVERIFY(r.commit("Main"));
        const auto main = QString::fromLatin1(git(r.dir.path(), {"rev-parse", "HEAD"}).trimmed());
        const auto operation = OperationKind(kind);
        QString target = operation == OperationKind::Reset    ? base
                         : operation == OperationKind::Revert ? main
                                                              : feature;
        if (operation == OperationKind::Rebase) {
            QVERIFY(git(r.dir.path(), {"switch", "feature"}) != "TEST_GIT_ERROR");
            target = "main";
        }
        MainWindow host;
        OperationDialog dialog(&r.operations, operation, false, &host, target);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        auto* execute = dialog.findChild<QPushButton*>("operationExecute");
        QVERIFY(!execute->isEnabled());
        if (operation == OperationKind::Reset)
            dialog.findChild<QComboBox*>("operationMode")->setCurrentIndex(resetMode);
        dialog.findChild<QPushButton*>("operationPreview")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QVERIFY(dialog.findChild<QPlainTextEdit*>("operationDetails")
                    ->toPlainText()
                    .contains(r.dir.path()));
        QVERIFY(!execute->isEnabled());
        dialog.findChild<QCheckBox*>("operationConfirmed")->setChecked(true);
        if (operation == OperationKind::Reset && resetMode == int(ResetMode::Hard)) {
            QVERIFY(!execute->isEnabled());
            dialog.findChild<QLineEdit*>("hardResetConfirmation")->setText("WRONG");
            QVERIFY(!execute->isEnabled());
            dialog.findChild<QLineEdit*>("hardResetConfirmation")->setText("RESET");
            capture(&dialog, "reset-confirmation-zh.png");
        }
        QVERIFY(execute->isEnabled());
        execute->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        const auto notice = dialog.findChild<QPlainTextEdit*>("operationNotice")->toPlainText();
        QVERIFY2(!notice.contains("fatal:"), qPrintable(notice));
        if (operation == OperationKind::Merge || operation == OperationKind::CherryPick ||
            operation == OperationKind::Rebase)
            QCOMPARE(read(r.dir.filePath("feature.txt")), QByteArray("feature\n"));
        if (operation == OperationKind::Rebase)
            QCOMPARE(QString::fromLatin1(git(r.dir.path(), {"rev-parse", "HEAD^"}).trimmed()),
                     main);
        if (operation == OperationKind::Revert)
            QVERIFY(!QFileInfo::exists(r.dir.filePath("main.txt")));
        if (operation == OperationKind::Reset)
            QCOMPARE(QString::fromLatin1(git(r.dir.path(), {"rev-parse", "HEAD"}).trimmed()), base);
        QVERIFY(!execute->isEnabled()); // A completed plan cannot be executed twice.
    }

    void stashButtonsPushPreviewApplyPopAndDrop() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.dir.filePath("file.txt"), "stash content\n"));
        QVERIFY(write(r.dir.filePath("untracked.txt"), "new\n"));
        MainWindow host;
        StashDialog dialog(&r.operations, false, &host);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        dialog.findChild<QLineEdit*>("stashMessage")->setText(QString::fromUtf8("保存工作进度"));
        dialog.findChild<QCheckBox*>("stashUntracked")->setChecked(true);
        answer(QMessageBox::Yes);
        dialog.findChild<QPushButton*>("stashPush")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        auto* list = dialog.findChild<QTableWidget*>("stashList");
        QCOMPARE(list->rowCount(), 1);
        QVERIFY(!QFileInfo::exists(r.dir.filePath("untracked.txt")));
        QVERIFY(dialog.findChild<QPlainTextEdit*>("stashPatch")
                    ->toPlainText()
                    .contains("+stash content"));
        capture(&dialog, "stash-zh.png");
        answer(QMessageBox::Yes);
        dialog.findChild<QPushButton*>("stashApply")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(read(r.dir.filePath("file.txt")), QByteArray("stash content\n"));
        QCOMPARE(list->rowCount(), 1);
        QVERIFY(git(r.dir.path(), {"reset", "--hard", "HEAD"}) != "TEST_GIT_ERROR");
        QVERIFY(QFile::remove(r.dir.filePath("untracked.txt")));
        answer(QMessageBox::Yes);
        dialog.findChild<QPushButton*>("stashPop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(list->rowCount(), 0);
        answer(QMessageBox::Yes);
        dialog.findChild<QPushButton*>("stashPush")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(list->rowCount(), 1);
        QTimer::singleShot(0, [] {
            auto* input = qobject_cast<QInputDialog*>(qApp->activeModalWidget());
            if (input) {
                input->setTextValue("WRONG");
                input->accept();
            }
        });
        dialog.findChild<QPushButton*>("stashDrop")->click();
        QCOMPARE(list->rowCount(), 1);
        QTimer::singleShot(0, [] {
            auto* input = qobject_cast<QInputDialog*>(qApp->activeModalWidget());
            if (input) {
                input->setTextValue("DROP");
                input->accept();
            }
        });
        dialog.findChild<QPushButton*>("stashDrop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        QCOMPARE(list->rowCount(), 0);
    }

    void mainWindowDiscoversAndContinuesPendingMerge() {
        Repository r;
        QVERIFY(r.mergeConflict());
        MainWindow window;
        QVERIFY(window.openProject(r.dir.path()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        auto* banner = window.findChild<QToolBar*>("workflowToolbar");
        QVERIFY(banner->isVisible());
        auto* proceed = window.findChild<QAction*>("continueOperationAction");
        QVERIFY(!proceed->isEnabled());
        QVERIFY(window.findChild<QAction*>("abortOperationAction")->isEnabled());
        QVERIFY(window.findChild<QAction*>("resolveConflictsAction")->isEnabled());
        ConflictDialog dialog(&r.operations, "file.txt", false, &window);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        dialog.findChild<QPushButton*>("useTheirs")->click();
        dialog.findChild<QPushButton*>("conflictSave")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        dialog.findChild<QPushButton*>("conflictStage")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.working() && !r.client.busy(), 10000);
        dialog.reject();
        window.refreshRepository();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(proceed->isEnabled());
        capture(&window, "workflow-banner-zh.png");
        answer(QMessageBox::Yes);
        proceed->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.gitBusy(), 10000);
        QVERIFY(!banner->isVisible());
        QVERIFY(!QFileInfo::exists(r.dir.filePath(".git/MERGE_HEAD")));
        QCOMPARE(git(r.dir.path(), {"rev-list", "--parents", "-n", "1", "HEAD"})
                     .trimmed()
                     .split(' ')
                     .size(),
                 3);
        QVERIFY(window.findChild<QAction*>("advancedOperation1")->isEnabled());
    }

    void advancedMenusExistInBothLanguages() {
        MainWindow chinese(false);
        MainWindow english(true);
        for (auto* window : {&chinese, &english}) {
            QVERIFY(window->findChild<QAction*>("compareRevisionsAction"));
            QVERIFY(window->findChild<QAction*>("resolveConflictsAction"));
            QVERIFY(window->findChild<QAction*>("stashManagerAction"));
            for (int kind = 0; kind < 5; ++kind)
                QVERIFY(window->findChild<QAction*>(QString("advancedOperation%1").arg(kind)));
        }
        QCOMPARE(english.findChild<QAction*>("compareRevisionsAction")->text(),
                 QStringLiteral("Compare revisions…"));
    }

  private:
    QTemporaryDir m_settings;
};

QTEST_MAIN(AdvancedUiTests)
#include "advanced_ui_tests.moc"
