#include "gitclient.h"
#include "gitstatus.h"
#include "paths.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <optional>

using namespace idea4git;

namespace {
QByteArray records(std::initializer_list<QByteArray> entries) {
    QByteArray output;
    for (const auto& entry : entries) {
        output.append(entry);
        output.append('\0');
    }
    return output;
}

bool write(const QString& directory, const QString& path, const QByteArray& bytes) {
    const auto absolute = repositoryFilePath(directory, path);
    QDir().mkpath(QFileInfo(absolute).absolutePath());
    QFile file(absolute);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

GitResult git(const QString& directory, const QStringList& args) {
    GitClient client(directory);
    QEventLoop loop;
    std::optional<GitResult> result;
    client.run(
        args,
        [&](const GitResult& value) {
            result = value;
            loop.quit();
        },
        10000);
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    loop.exec();
    return result.value_or(GitResult{-1, {}, "Test timeout"});
}

GitResult init(const QString& directory) {
    auto result = git(directory, {"init", "--initial-branch=main"});
    if (!result.ok())
        return result;
    return git(directory, {"config", "user.name", "idea4git Tests"});
}

GitResult commit(const QString& directory, const QString& message) {
    return git(directory, {"-c", "user.email=test@example.invalid", "-c", "commit.gpgSign=false",
                           "commit", "-m", message});
}
} // namespace

class GitTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(m_config.isValid());
        QVERIFY(write(m_config.path(), "config", "[core]\n hooksPath = /dev/null\n"));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(m_config.filePath("config")));
    }

    void statusStates_data() {
        QTest::addColumn<QByteArray>("xy");
        QTest::addColumn<int>("state");
        QTest::addColumn<bool>("staged");
        QTest::addColumn<bool>("unstaged");
        QTest::addColumn<QString>("color");
        QTest::newRow("clean") << QByteArray("  ") << int(FileState::Clean) << false << false
                               << "#bcbec4";
        QTest::newRow("untracked")
            << QByteArray("??") << int(FileState::Untracked) << false << true << "#d1675a";
        QTest::newRow("modified") << QByteArray(" M") << int(FileState::Modified) << false << true
                                  << "#6897bb";
        QTest::newRow("staged-modified")
            << QByteArray("M ") << int(FileState::Modified) << true << false << "#6897bb";
        QTest::newRow("partially-staged")
            << QByteArray("MM") << int(FileState::Modified) << true << true << "#6897bb";
        QTest::newRow("added") << QByteArray("A ") << int(FileState::Added) << true << false
                               << "#629755";
        QTest::newRow("deleted") << QByteArray(" D") << int(FileState::Deleted) << false << true
                                 << "#6c6c6c";
        QTest::newRow("staged-deleted")
            << QByteArray("D ") << int(FileState::Deleted) << true << false << "#6c6c6c";
        QTest::newRow("renamed") << QByteArray("R ") << int(FileState::Renamed) << true << false
                                 << "#32a2a8";
        QTest::newRow("conflict") << QByteArray("UU") << int(FileState::Conflict) << false << true
                                  << "#f15858";
        QTest::newRow("both-added")
            << QByteArray("AA") << int(FileState::Conflict) << false << true << "#f15858";
        QTest::newRow("ignored") << QByteArray("!!") << int(FileState::Ignored) << false << false
                                 << "#848504";
    }

    void statusStates() {
        QFETCH(QByteArray, xy);
        QFETCH(int, state);
        QFETCH(bool, staged);
        QFETCH(bool, unstaged);
        QFETCH(QString, color);
        FileChange file;
        file.index = xy[0];
        file.worktree = xy[1];
        QCOMPARE(int(file.state()), state);
        QCOMPARE(file.staged(), staged);
        QCOMPARE(file.unstaged(), unstaged);
        QCOMPARE(stateColor(file.state()).name(), color);
    }

    void nulParsing() {
        const auto status = parseStatus(
            records({"## main...origin/main [ahead 2]", " M src/有 空格\nand-newline.cpp",
                     "R  destination\n.txt", "original name.txt", "!! build/"}));
        QVERIFY(status.valid);
        QCOMPARE(status.branch, "main");
        QCOMPARE(status.tracking, "origin/main [ahead 2]");
        QCOMPARE(status.files.size(), 3);
        QCOMPARE(status.files[0].path, QString::fromUtf8("src/有 空格\nand-newline.cpp"));
        QCOMPARE(status.files[1].path, "destination\n.txt");
        QCOMPARE(status.files[1].originalPath, "original name.txt");
        QVERIFY(parseStatus(records({"## No commits yet on main", "?? a"})).unborn);
        QVERIFY(!parseStatus(records({"R  missing-original"})).valid);
        const auto invalid = parseStatus(records({"?? valid.txt", QByteArray("?? ") + char(0xff)}));
        QVERIFY(!invalid.valid);
        QVERIFY(invalid.files.isEmpty());
        QVERIFY(!parseStatus("?? truncated").valid);
    }

    void commitFileListNulParsing() {
        bool valid = false;
        const auto changes =
            parseCommitFiles(records({"M", "src/有 空格\nand-newline.cpp", "R100", "old name.txt",
                                      "new name.txt", "A", "added.txt", "D", "deleted.txt"}),
                             &valid);
        QVERIFY(valid);
        QCOMPARE(changes.size(), 4);
        QCOMPARE(changes[0].status, "M");
        QCOMPARE(changes[0].path, QString::fromUtf8("src/有 空格\nand-newline.cpp"));
        QCOMPARE(changes[1].status, "R100");
        QCOMPARE(changes[1].originalPath, "old name.txt");
        QCOMPARE(changes[1].path, "new name.txt");
        QCOMPARE(changes[2].path, "added.txt");
        QCOMPARE(changes[3].path, "deleted.txt");
        QVERIFY(parseCommitFiles({}, &valid).isEmpty());
        QVERIFY(valid);
        QVERIFY(parseCommitFiles(records({"R100", "old.txt"}), &valid).isEmpty());
        QVERIFY(!valid);
        QVERIFY(
            parseCommitFiles(records({"M", QByteArray("bad-") + char(0xff)}), &valid).isEmpty());
        QVERIFY(!valid);
    }

    void literalPathsAndUnbornUnstage() {
        QTemporaryDir repository;
        QVERIFY(repository.isValid());
        QVERIFY(init(repository.path()).ok());
#ifdef Q_OS_WIN
        const QString special = QString::fromUtf8("中文 with spaces.txt");
#else
        const QString special = QString::fromUtf8(":(glob)中文[1]\nwith spaces.txt");
#endif
        QCOMPARE(repositoryFilePath(repository.path(), special), repository.path() + '/' + special);
        QVERIFY(write(repository.path(), special, "special\n"));
        QVERIFY(write(repository.path(), "ordinary.txt", "ordinary\n"));
        QVERIFY(git(repository.path(), {"add", "--", special}).ok());
        auto status =
            parseStatus(git(repository.path(), {"status", "--porcelain=v1", "-z", "-b"}).output);
        QVERIFY(status.unborn);
        QCOMPARE(status.files.size(), 2);
        int staged = 0;
        for (const auto& file : status.files) {
            if (file.staged()) {
                QCOMPARE(file.path, special);
                ++staged;
            }
        }
        QCOMPARE(staged, 1);
        QVERIFY(git(repository.path(), {"rm", "--cached", "--ignore-unmatch", "--", special}).ok());
        QVERIFY(QFileInfo::exists(repositoryFilePath(repository.path(), special)));
        status = parseStatus(git(repository.path(), {"status", "--porcelain=v1", "-z"}).output);
        for (const auto& file : status.files)
            QVERIFY(!file.staged());
    }

    void stageCommitDiffAndHistory() {
        QTemporaryDir repository;
        QVERIFY(init(repository.path()).ok());
        QVERIFY(write(repository.path(), "file.txt", "first\n"));
        QVERIFY(git(repository.path(), {"add", "--", "file.txt"}).ok());
        QVERIFY(commit(repository.path(), QString::fromUtf8("初始化项目")).ok());
        QVERIFY(write(repository.path(), "file.txt", "second\n"));
        QVERIFY(git(repository.path(), {"add", "--", "file.txt"}).ok());
        QVERIFY(write(repository.path(), "file.txt", "third\n"));
        const auto status =
            parseStatus(git(repository.path(), {"status", "--porcelain=v1", "-z"}).output);
        QCOMPARE(status.files.size(), 1);
        QVERIFY(status.files[0].staged());
        QVERIFY(status.files[0].unstaged());
        const auto diff =
            git(repository.path(), {"diff", "--cached", "--no-ext-diff", "--", "file.txt"});
        QVERIFY(diff.ok());
        QVERIFY(diff.output.contains("+second"));
        QVERIFY(!diff.output.contains("+third"));
        QVERIFY(commit(repository.path(), "Only staged changes").ok());
        QCOMPARE(git(repository.path(), {"show", "HEAD:file.txt"}).output, QByteArray("second\n"));
        const auto log = git(repository.path(), {"log", "-z", "-n", "100",
                                                 "--format=%H%x00%h%x00%an%x00%aI%x00%D%x00%s"});
        QVERIFY(log.ok());
        const auto commits = parseLog(log.output);
        QCOMPARE(commits.size(), 2);
        QCOMPARE(commits[0].subject, "Only staged changes");
        QCOMPARE(commits[1].subject, QString::fromUtf8("初始化项目"));
        QVERIFY(git(repository.path(), {"add", "--", "file.txt"}).ok());
        QVERIFY(git(repository.path(), {"restore", "--staged", "--", "file.txt"}).ok());
        QVERIFY(git(repository.path(), {"diff", "--cached"}).output.isEmpty());
        QVERIFY(git(repository.path(), {"diff"}).output.contains("+third"));
    }

    void renameDeleteAndIgnore() {
        QTemporaryDir repository;
        QVERIFY(init(repository.path()).ok());
        QVERIFY(write(repository.path(), "old name.txt", "contents\n"));
        QVERIFY(write(repository.path(), ".gitignore", "build/\n"));
        QVERIFY(git(repository.path(), {"add", "."}).ok());
        QVERIFY(commit(repository.path(), "Base").ok());
        QVERIFY(QFile::rename(repository.filePath("old name.txt"),
                              repository.filePath("new name.txt")));
        QVERIFY(git(repository.path(), {"add", "-A"}).ok());
        auto status =
            parseStatus(git(repository.path(), {"status", "--porcelain=v1", "-z"}).output);
        QCOMPARE(status.files.size(), 1);
        QCOMPARE(status.files[0].state(), FileState::Renamed);
        QCOMPARE(status.files[0].path, "new name.txt");
        QCOMPARE(status.files[0].originalPath, "old name.txt");
        QVERIFY(commit(repository.path(), "Rename").ok());
        QVERIFY(QFile::remove(repository.filePath("new name.txt")));
        QVERIFY(write(repository.path(), "build/ignored.txt", "ignored\n"));
        status = parseStatus(
            git(repository.path(), {"status", "--porcelain=v1", "-z", "--ignored=matching"})
                .output);
        QCOMPARE(status.files.size(), 2);
        bool deleted = false, ignored = false;
        for (const auto& file : status.files) {
            deleted |= file.state() == FileState::Deleted;
            ignored |= file.state() == FileState::Ignored;
        }
        QVERIFY(deleted);
        QVERIFY(ignored);
    }

    void conflictsAreNotReportedAsStaged() {
        QTemporaryDir repository;
        QVERIFY(init(repository.path()).ok());
        QVERIFY(write(repository.path(), "conflict.txt", "base\n"));
        QVERIFY(git(repository.path(), {"add", "."}).ok());
        QVERIFY(commit(repository.path(), "Base").ok());
        QVERIFY(git(repository.path(), {"switch", "-c", "feature"}).ok());
        QVERIFY(write(repository.path(), "conflict.txt", "feature\n"));
        QVERIFY(git(repository.path(), {"add", "."}).ok());
        QVERIFY(commit(repository.path(), "Feature").ok());
        QVERIFY(git(repository.path(), {"switch", "main"}).ok());
        QVERIFY(write(repository.path(), "conflict.txt", "main\n"));
        QVERIFY(git(repository.path(), {"add", "."}).ok());
        QVERIFY(commit(repository.path(), "Main").ok());
        const auto merge = git(repository.path(), {"merge", "--no-edit", "feature"});
        const auto statusResult = git(repository.path(), {"status", "--porcelain=v1", "-z"});
        const auto status = parseStatus(statusResult.output);
        const auto details =
            QStringLiteral("merge exit=%1 stderr(hex)=%2; status exit=%3 stderr(hex)=%4 "
                           "output(hex)=%5")
                .arg(merge.exitCode)
                .arg(QString::fromLatin1(merge.error.toHex(' ')))
                .arg(statusResult.exitCode)
                .arg(QString::fromLatin1(statusResult.error.toHex(' ')))
                .arg(QString::fromLatin1(statusResult.output.toHex(' ')));
        QVERIFY2(!merge.ok(), qPrintable(details));
        QVERIFY2(statusResult.ok(), qPrintable(details));
        QVERIFY2(status.valid, qPrintable(details));
        QVERIFY2(status.files.size() == 1, qPrintable(details));
        QVERIFY(status.files[0].conflict());
        QVERIFY(!status.files[0].staged());
    }

    void cancellationDropsQueuedCommands() {
        QTemporaryDir directory;
        GitClient client(directory.path());
        QSignalSpy completed(&client, &GitClient::commandCompleted);
        bool cancelled = false;
        bool queuedCancelled = false;
        client.run({"hash-object", "--stdin"},
                   [&](const GitResult& result) { cancelled = result.cancelled; });
        client.run({"--version"},
                   [&](const GitResult& result) { queuedCancelled = result.cancelled; });
        QTimer::singleShot(40, &client, &GitClient::cancelAll);
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QVERIFY(cancelled);
        QVERIFY(queuedCancelled);
        QCOMPARE(completed.size(), 1);
    }

    void timeoutDoesNotBlockEventLoop() {
        QTemporaryDir directory;
        GitClient client(directory.path());
        bool timedOut = false;
        int ticks = 0;
        QTimer pulse;
        pulse.setInterval(5);
        connect(&pulse, &QTimer::timeout, this, [&] { ++ticks; });
        pulse.start();
        client.run(
            {"hash-object", "--stdin"},
            [&](const GitResult& result) { timedOut = result.timedOut; }, 80);
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QVERIFY(timedOut);
        QVERIFY(ticks > 2);
    }

  private:
    QTemporaryDir m_config;
};

QTEST_GUILESS_MAIN(GitTests)
#include "git_tests.moc"
