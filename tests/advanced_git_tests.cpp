#include "gitoperations.h"
#include "paths.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace idea4git;

namespace {
GitResult git(const QString& root, const QStringList& args) {
    QProcess process;
    process.setWorkingDirectory(root);
    process.start("git", args);
    GitResult result;
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        result.timedOut = true;
    }
    result.exitCode = process.exitCode();
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    return result;
}
bool write(const QString& root, const QString& name, const QByteArray& bytes) {
    const auto path = repositoryFilePath(root, name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString& root, const QString& name) {
    QFile file(repositoryFilePath(root, name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
template <typename T, typename Start> std::pair<GitResult, T> value(Start start) {
    QEventLoop loop;
    bool done = false;
    std::pair<GitResult, T> answer;
    start([&](const GitResult& result, const T& data) {
        answer = {result, data};
        done = true;
        loop.quit();
    });
    if (!done) {
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!done) {
        answer.first.exitCode = -1;
        answer.first.error = "Test callback timeout";
    }
    return answer;
}
template <typename Start> GitResult action(Start start) {
    QEventLoop loop;
    bool done = false;
    GitResult answer;
    start([&](const GitResult& result) {
        answer = result;
        done = true;
        loop.quit();
    });
    if (!done) {
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!done) {
        answer.exitCode = -1;
        answer.error = "Test callback timeout";
    }
    return answer;
}
struct Repository {
    QTemporaryDir directory;
    GitClient client{directory.path()};
    GitOperations operations{&client, directory.path(), true};
    QString root() const { return directory.path(); }
    bool init() {
        return git(root(), {"init", "--initial-branch=main"}).ok() &&
               write(root(), "file.txt", "base\n") && commit("Base");
    }
    bool commit(const QString& message) {
        return git(root(), {"add", "-A"}).ok() && git(root(), {"commit", "-m", message}).ok();
    }
    QString head() const {
        return QString::fromLatin1(git(root(), {"rev-parse", "HEAD"}).output.trimmed());
    }
    std::pair<GitResult, GitSnapshot> snapshot() {
        return value<GitSnapshot>([&](auto cb) { operations.snapshot(cb); });
    }
    GitResult perform(const OperationRequest& request) {
        auto planned = value<OperationPlan>([&](auto cb) { operations.plan(request, cb); });
        if (!planned.first.ok())
            return planned.first;
        return action([&](auto cb) { operations.execute(planned.second, cb); });
    }
    bool conflictingMerge() {
        if (!init() || !git(root(), {"switch", "-c", "feature"}).ok() ||
            !write(root(), "file.txt", "feature\n") || !commit("Feature"))
            return false;
        if (!git(root(), {"switch", "main"}).ok() || !write(root(), "file.txt", "main\n") ||
            !commit("Main"))
            return false;
        return !perform({OperationKind::Merge, "feature"}).ok() &&
               snapshot().second.pending == PendingOperation::Merge;
    }
};
} // namespace

#define OK(expression)                                                                             \
    do {                                                                                           \
        const auto checked = (expression);                                                         \
        QVERIFY2(checked.ok(), checked.error.constData());                                         \
    } while (false)

class AdvancedGitTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QVERIFY(write(
            m_settings.path(), "gitconfig",
            "[user]\n name = idea4git Tests\n email = tests@example.invalid\n[commit]\n gpgSign = "
            "false\n[core]\n hooksPath = /dev/null\n[init]\n defaultBranch = main\n"));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(m_settings.filePath("gitconfig")));
    }

    void compareArbitraryRevisionsAndRenames() {
        Repository r;
        QVERIFY(r.init());
#ifdef Q_OS_WIN
        const QString old = QString::fromUtf8("中文 old.txt");
        const QString renamed = QString::fromUtf8("中文 new.txt");
#else
        const QString old = QString::fromUtf8(":(glob)中文 old\n.txt");
        const QString renamed = QString::fromUtf8(":(glob)中文 new\n.txt");
#endif
        QVERIFY(write(r.root(), old, "rename me\n"));
        QVERIFY(r.commit("Old filename"));
        OK(git(r.root(), {"tag", "v1"}));
        QVERIFY(QFile::rename(repositoryFilePath(r.root(), old),
                              repositoryFilePath(r.root(), renamed)));
        QVERIFY(write(r.root(), "file.txt", "changed\n"));
        QVERIFY(write(r.root(), "new.txt", "new\n"));
        QVERIFY(r.commit("Next"));
        OK(git(r.root(), {"tag", "-a", "v2", "-m", "Annotated version"}));
        auto compared = value<Comparison>([&](auto cb) { r.operations.compare("v1", "v2", cb); });
        OK(compared.first);
        QCOMPARE(compared.second.files.size(), 3);
        bool sawRename = false;
        for (const auto& file : compared.second.files) {
            if (file.status.startsWith('R')) {
                QCOMPARE(file.oldPath, old);
                QCOMPARE(file.newPath, renamed);
                sawRename = true;
            }
        }
        QVERIFY(sawRename);
        auto blob =
            action([&](auto cb) { r.operations.blob(compared.second.left, "file.txt", cb); });
        OK(blob);
        QCOMPARE(blob.output, QByteArray("base\n"));
        blob = action([&](auto cb) { r.operations.blob(compared.second.right, "file.txt", cb); });
        OK(blob);
        QCOMPARE(blob.output, QByteArray("changed\n"));
        auto invalid = value<QString>([&](auto cb) { r.operations.resolve("--help", cb); });
        QVERIFY(!invalid.first.ok());
        auto same = value<Comparison>([&](auto cb) { r.operations.compare("HEAD", "v2", cb); });
        OK(same.first);
        QVERIFY(same.second.files.isEmpty());
    }

    void mergeFastForwardNoFastForwardAndSquash_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("fast-forward") << int(MergeMode::FastForwardOnly);
        QTest::newRow("merge-commit") << int(MergeMode::NoFastForward);
        QTest::newRow("squash") << int(MergeMode::Squash);
    }
    void mergeFastForwardNoFastForwardAndSquash() {
        QFETCH(int, mode);
        Repository r;
        QVERIFY(r.init());
        const auto base = r.head();
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "feature.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        const auto feature = r.head();
        OK(git(r.root(), {"switch", "main"}));
        OperationRequest request{OperationKind::Merge, "feature"};
        request.mergeMode = MergeMode(mode);
        OK(r.perform(request));
        if (request.mergeMode == MergeMode::FastForwardOnly)
            QCOMPARE(r.head(), feature);
        else if (request.mergeMode == MergeMode::Squash) {
            QCOMPARE(r.head(), base);
            QVERIFY(r.snapshot().second.trackedDirty());
        } else
            QCOMPARE(git(r.root(), {"rev-list", "--parents", "-n", "1", "HEAD"})
                         .output.trimmed()
                         .split(' ')
                         .size(),
                     3);
        QCOMPARE(read(r.root(), "feature.txt"), QByteArray("feature\n"));
    }

    void mergeConflictFourStagesSaveStageContinue() {
        Repository r;
        QVERIFY(r.conflictingMerge());
        auto conflict =
            value<ConflictFile>([&](auto cb) { r.operations.conflict("file.txt", cb); });
        OK(conflict.first);
        QVERIFY(conflict.second.editable);
        QCOMPARE(conflict.second.stages[0].bytes, QByteArray("base\n"));
        QCOMPARE(conflict.second.stages[1].bytes, QByteArray("main\n"));
        QCOMPARE(conflict.second.stages[2].bytes, QByteArray("feature\n"));
        QVERIFY(!action([&](auto cb) { r.operations.markResolved(conflict.second, cb); }).ok());
        auto saved = value<ConflictFile>([&](auto cb) {
            r.operations.saveResolution(conflict.second, "resolved\n", false, cb);
        });
        OK(saved.first);
        QVERIFY(r.snapshot().second.conflicts());
        OK(action([&](auto cb) { r.operations.markResolved(saved.second, cb); }));
        QVERIFY(!r.snapshot().second.conflicts());
        OK(action([&](auto cb) {
            r.operations.sequence(PendingOperation::Merge, SequenceAction::Continue, cb);
        }));
        QCOMPARE(r.snapshot().second.pending, PendingOperation::None);
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("resolved\n"));
        QCOMPARE(git(r.root(), {"rev-list", "--parents", "-n", "1", "HEAD"})
                     .output.trimmed()
                     .split(' ')
                     .size(),
                 3);
    }

    void mergeAbortAndExternalFileProtection() {
        Repository r;
        QVERIFY(r.conflictingMerge());
        const auto original = r.head();
        auto conflict =
            value<ConflictFile>([&](auto cb) { r.operations.conflict("file.txt", cb); });
        OK(conflict.first);
        QVERIFY(write(r.root(), "file.txt", "external change\n"));
        auto saved = value<ConflictFile>(
            [&](auto cb) { r.operations.saveResolution(conflict.second, "lost\n", false, cb); });
        QVERIFY(!saved.first.ok());
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("external change\n"));
        QVERIFY(!action([&](auto cb) {
                     r.operations.sequence(PendingOperation::Rebase, SequenceAction::Abort, cb);
                 }).ok());
        OK(action([&](auto cb) {
            r.operations.sequence(PendingOperation::Merge, SequenceAction::Abort, cb);
        }));
        QCOMPARE(r.head(), original);
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("main\n"));
    }

    void modifyDeleteConflictCanResolveAsDeletion() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "file.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        OK(git(r.root(), {"switch", "main"}));
        QVERIFY(QFile::remove(r.directory.filePath("file.txt")));
        QVERIFY(r.commit("Delete"));
        QVERIFY(!r.perform({OperationKind::Merge, "feature"}).ok());
        auto conflict =
            value<ConflictFile>([&](auto cb) { r.operations.conflict("file.txt", cb); });
        OK(conflict.first);
        QVERIFY(!conflict.second.stages[1].present);
        QVERIFY(conflict.second.stages[2].present);
        auto saved = value<ConflictFile>(
            [&](auto cb) { r.operations.saveResolution(conflict.second, {}, true, cb); });
        OK(saved.first);
        OK(action([&](auto cb) { r.operations.markResolved(saved.second, cb); }));
        OK(action([&](auto cb) {
            r.operations.sequence(PendingOperation::Merge, SequenceAction::Continue, cb);
        }));
        QVERIFY(!QFileInfo::exists(r.directory.filePath("file.txt")));
    }

    void rebaseClean() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "feature.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        const auto original = r.head();
        OK(git(r.root(), {"switch", "main"}));
        QVERIFY(write(r.root(), "main.txt", "main\n"));
        QVERIFY(r.commit("Main"));
        const auto upstream = r.head();
        OK(git(r.root(), {"switch", "feature"}));
        OK(r.perform({OperationKind::Rebase, "main"}));
        QVERIFY(r.head() != original);
        QCOMPARE(QString::fromLatin1(git(r.root(), {"rev-parse", "HEAD^"}).output.trimmed()),
                 upstream);
        QCOMPARE(r.snapshot().second.pending, PendingOperation::None);
    }

    void sequenceConflicts_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("control");
        for (auto kind :
             {OperationKind::Rebase, OperationKind::CherryPick, OperationKind::Revert}) {
            for (auto control :
                 {SequenceAction::Continue, SequenceAction::Abort, SequenceAction::Skip}) {
                const QByteArray name =
                    QByteArray::number(int(kind)) + '-' + QByteArray::number(int(control));
                QTest::newRow(name.constData()) << int(kind) << int(control);
            }
        }
    }
    void sequenceConflicts() {
        QFETCH(int, kind);
        QFETCH(int, control);
        Repository r;
        QVERIFY(r.init());
        const auto operation = OperationKind(kind);
        QString target;
        if (operation == OperationKind::Revert) {
            QVERIFY(write(r.root(), "file.txt", "middle\n"));
            QVERIFY(r.commit("Middle"));
            target = r.head();
            QVERIFY(write(r.root(), "file.txt", "later\n"));
            QVERIFY(r.commit("Later"));
        } else {
            OK(git(r.root(), {"switch", "-c", "feature"}));
            QVERIFY(write(r.root(), "file.txt", "feature\n"));
            QVERIFY(r.commit("Feature"));
            target = r.head();
            OK(git(r.root(), {"switch", "main"}));
            QVERIFY(write(r.root(), "file.txt", "main\n"));
            QVERIFY(r.commit("Main"));
            if (operation == OperationKind::Rebase) {
                OK(git(r.root(), {"switch", "feature"}));
                target = "main";
            }
        }
        const auto original = r.head();
        QVERIFY(!r.perform({operation, target}).ok());
        const auto pending = operation == OperationKind::Rebase       ? PendingOperation::Rebase
                             : operation == OperationKind::CherryPick ? PendingOperation::CherryPick
                                                                      : PendingOperation::Revert;
        QCOMPARE(r.snapshot().second.pending, pending);
        QVERIFY(r.snapshot().second.conflicts());
        if (SequenceAction(control) == SequenceAction::Continue) {
            auto conflict =
                value<ConflictFile>([&](auto cb) { r.operations.conflict("file.txt", cb); });
            OK(conflict.first);
            if (operation == OperationKind::Rebase) {
                QCOMPARE(conflict.second.stages[1].bytes, QByteArray("main\n"));
                QCOMPARE(conflict.second.stages[2].bytes, QByteArray("feature\n"));
            }
            auto saved = value<ConflictFile>([&](auto cb) {
                r.operations.saveResolution(conflict.second, "resolved\n", false, cb);
            });
            OK(saved.first);
            OK(action([&](auto cb) { r.operations.markResolved(saved.second, cb); }));
        }
        OK(action([&](auto cb) { r.operations.sequence(pending, SequenceAction(control), cb); }));
        QCOMPARE(r.snapshot().second.pending, PendingOperation::None);
        QVERIFY(!r.snapshot().second.conflicts());
        if (SequenceAction(control) == SequenceAction::Abort)
            QCOMPARE(r.head(), original);
        if (SequenceAction(control) == SequenceAction::Continue)
            QCOMPARE(read(r.root(), "file.txt"), QByteArray("resolved\n"));
    }

    void cherryPickAndRevertClean() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "file.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        const auto feature = r.head();
        OK(git(r.root(), {"switch", "main"}));
        QVERIFY(write(r.root(), "other.txt", "main\n"));
        QVERIFY(r.commit("Main"));
        OK(r.perform({OperationKind::CherryPick, feature}));
        const auto picked = r.head();
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("feature\n"));
        OK(r.perform({OperationKind::Revert, picked}));
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("base\n"));
        QCOMPARE(git(r.root(), {"rev-list", "--count", "HEAD"}).output.trimmed(), QByteArray("4"));
    }

    void mergeCommitNeedsExplicitMainline() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "feature.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        OK(git(r.root(), {"switch", "main"}));
        OperationRequest merge{OperationKind::Merge, "feature"};
        merge.mergeMode = MergeMode::NoFastForward;
        OK(r.perform(merge));
        OperationRequest revert{OperationKind::Revert, r.head()};
        QVERIFY(!r.perform(revert).ok());
        revert.mainline = 1;
        OK(r.perform(revert));
        QVERIFY(!QFileInfo::exists(r.directory.filePath("feature.txt")));
    }

    void resetModes_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("soft") << int(ResetMode::Soft);
        QTest::newRow("mixed") << int(ResetMode::Mixed);
        QTest::newRow("hard") << int(ResetMode::Hard);
    }
    void resetModes() {
        QFETCH(int, mode);
        Repository r;
        QVERIFY(r.init());
        const auto base = r.head();
        QVERIFY(write(r.root(), "file.txt", "second\n"));
        QVERIFY(r.commit("Second"));
        QVERIFY(write(r.root(), "file.txt", "working\n"));
        QVERIFY(write(r.root(), "added.txt", "index\n"));
        OK(git(r.root(), {"add", "added.txt"}));
        QVERIFY(write(r.root(), "loose.txt", "untracked\n"));
        OperationRequest request{OperationKind::Reset, base};
        request.resetMode = ResetMode(mode);
        OK(r.perform(request));
        QCOMPARE(r.head(), base);
        const auto staged = git(r.root(), {"diff", "--cached"}).output;
        if (request.resetMode == ResetMode::Soft)
            QVERIFY(staged.contains("+second"));
        else
            QVERIFY(staged.isEmpty());
        QCOMPARE(read(r.root(), "file.txt"), request.resetMode == ResetMode::Hard
                                                 ? QByteArray("base\n")
                                                 : QByteArray("working\n"));
        QVERIFY(QFileInfo::exists(r.directory.filePath("loose.txt")));
    }

    void stalePlansAndDirtyWorktreesAreRejected() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "feature.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        OK(git(r.root(), {"switch", "main"}));
        auto plan = value<OperationPlan>(
            [&](auto cb) { r.operations.plan({OperationKind::Merge, "feature"}, cb); });
        OK(plan.first);
        QVERIFY(write(r.root(), "main.txt", "new\n"));
        QVERIFY(r.commit("External"));
        const auto latest = r.head();
        QVERIFY(!action([&](auto cb) { r.operations.execute(plan.second, cb); }).ok());
        QCOMPARE(r.head(), latest);
        QVERIFY(write(r.root(), "file.txt", "dirty\n"));
        QVERIFY(!r.perform({OperationKind::Merge, "feature"}).ok());
    }

    void stashPushApplyPopAndDrop() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.root(), "file.txt", "saved\n"));
        QVERIFY(write(r.root(), "new.txt", "untracked\n"));
        OK(action([&](auto cb) { r.operations.pushStash("A 中文", true, false, cb); }));
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("base\n"));
        QVERIFY(!QFileInfo::exists(r.directory.filePath("new.txt")));
        auto entries = value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); });
        OK(entries.first);
        QCOMPARE(entries.second.size(), 1);
        const auto hash = entries.second.first().hash;
        auto patch = action([&](auto cb) { r.operations.stashPatch(hash, cb); });
        OK(patch);
        QVERIFY(patch.output.contains("+saved"));
        QVERIFY(patch.output.contains("+untracked"));
        OK(action([&](auto cb) { r.operations.useStash(StashAction::Apply, hash, false, cb); }));
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("saved\n"));
        QCOMPARE(value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); }).second.size(),
                 1);
        OK(git(r.root(), {"reset", "--hard", "HEAD"}));
        QVERIFY(QFile::remove(r.directory.filePath("new.txt")));
        OK(action([&](auto cb) { r.operations.useStash(StashAction::Pop, hash, false, cb); }));
        QVERIFY(
            value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); }).second.isEmpty());
        OK(action([&](auto cb) { r.operations.pushStash("B", true, false, cb); }));
        const auto older = value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); })
                               .second.first()
                               .hash;
        QVERIFY(write(r.root(), "file.txt", "second stash\n"));
        OK(action([&](auto cb) { r.operations.pushStash("C", false, false, cb); }));
        OK(action([&](auto cb) { r.operations.useStash(StashAction::Drop, older, false, cb); }));
        entries = value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); });
        QCOMPARE(entries.second.size(), 1);
        QVERIFY(entries.second.first().hash != older);
    }

    void conflictedPopRetainsStash() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.root(), "file.txt", "stash\n"));
        OK(action([&](auto cb) { r.operations.pushStash("Keep on conflict", false, false, cb); }));
        const auto hash = value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); })
                              .second.first()
                              .hash;
        QVERIFY(write(r.root(), "file.txt", "main\n"));
        QVERIFY(r.commit("Main"));
        QVERIFY(!action([&](auto cb) {
                     r.operations.useStash(StashAction::Pop, hash, false, cb);
                 }).ok());
        QCOMPARE(value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); }).second.size(),
                 1);
        const auto snapshot = r.snapshot();
        QVERIFY(snapshot.second.conflicts());
        QCOMPARE(snapshot.second.pending, PendingOperation::None);
    }

    void stashCanRestoreIndex() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.root(), "file.txt", "staged\n"));
        OK(git(r.root(), {"add", "file.txt"}));
        QVERIFY(write(r.root(), "file.txt", "unstaged\n"));
        OK(action([&](auto cb) { r.operations.pushStash("Index", false, false, cb); }));
        const auto hash = value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); })
                              .second.first()
                              .hash;
        OK(action([&](auto cb) { r.operations.useStash(StashAction::Apply, hash, true, cb); }));
        QCOMPARE(git(r.root(), {"show", ":file.txt"}).output, QByteArray("staged\n"));
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("unstaged\n"));
    }

    void stashKeepIndexPreservesStagedChanges() {
        Repository r;
        QVERIFY(r.init());
        QVERIFY(write(r.root(), "file.txt", "staged\n"));
        OK(git(r.root(), {"add", "file.txt"}));
        QVERIFY(write(r.root(), "file.txt", "unstaged\n"));
        OK(action([&](auto cb) { r.operations.pushStash("Keep staging", false, true, cb); }));
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("staged\n"));
        QCOMPARE(git(r.root(), {"show", ":file.txt"}).output, QByteArray("staged\n"));
        QCOMPARE(value<QList<StashEntry>>([&](auto cb) { r.operations.stashes(cb); }).second.size(),
                 1);
    }

    void cancellingQueuedMutationCompletesItsCallback() {
        Repository r;
        QVERIFY(r.init());
        OperationRequest request{OperationKind::Reset, "HEAD"};
        const auto planned = value<OperationPlan>([&](auto cb) { r.operations.plan(request, cb); });
        OK(planned.first);
        bool completed = false, cancelled = false;
        r.client.run({"hash-object", "--stdin"}, [](const GitResult&) {});
        r.operations.execute(planned.second, [&](const GitResult& result) {
            completed = true;
            cancelled = result.cancelled;
        });
        QTimer::singleShot(40, &r.operations, &GitOperations::cancel);
        QTRY_VERIFY_WITH_TIMEOUT(completed && !r.operations.busy(), 5000);
        QVERIFY(cancelled);
        QCOMPARE(read(r.root(), "file.txt"), QByteArray("base\n"));
    }

    void linkedWorktreeHasIndependentOperationState() {
        Repository r;
        QVERIFY(r.init());
        OK(git(r.root(), {"switch", "-c", "feature"}));
        QVERIFY(write(r.root(), "file.txt", "feature\n"));
        QVERIFY(r.commit("Feature"));
        OK(git(r.root(), {"switch", "main"}));
        QVERIFY(write(r.root(), "file.txt", "main\n"));
        QVERIFY(r.commit("Main"));
        QTemporaryDir location;
        const auto linked = location.filePath("linked");
        OK(git(r.root(), {"worktree", "add", "-b", "linked", linked, "main"}));
        GitClient client(linked);
        GitOperations operations(&client, linked, true);
        const auto planned = value<OperationPlan>(
            [&](auto cb) { operations.plan({OperationKind::Merge, "feature"}, cb); });
        OK(planned.first);
        QVERIFY(!action([&](auto cb) { operations.execute(planned.second, cb); }).ok());
        const auto inspected = value<GitSnapshot>([&](auto cb) { operations.snapshot(cb); });
        OK(inspected.first);
        QCOMPARE(inspected.second.pending, PendingOperation::Merge);
        QCOMPARE(r.snapshot().second.pending, PendingOperation::None);
        OK(action([&](auto cb) {
            operations.sequence(PendingOperation::Merge, SequenceAction::Abort, cb);
        }));
    }

    void metadataDiscoveryPreservesUnicodeAndNewlinePaths() {
#ifdef Q_OS_WIN
        const QString pattern = QDir::tempPath() + QString::fromUtf8("/idea4git 中文 space-XXXXXX");
#else
        const QString pattern =
            QDir::tempPath() + QString::fromUtf8("/idea4git 中文\nspace-XXXXXX");
#endif
        QTemporaryDir directory(pattern);
        QVERIFY(directory.isValid());
        OK(git(directory.path(), {"init", "--initial-branch=main"}));
        QVERIFY(write(directory.path(), "file.txt", "base\n"));
        OK(git(directory.path(), {"add", "."}));
        OK(git(directory.path(), {"commit", "-m", "Base"}));
        GitClient client(directory.path());
        GitOperations operations(&client, directory.path(), true);
        const auto inspected = value<GitSnapshot>([&](auto cb) { operations.snapshot(cb); });
        OK(inspected.first);
        QCOMPARE(inspected.second.pending, PendingOperation::None);
        QVERIFY(!inspected.second.head.isEmpty());
    }

  private:
    QTemporaryDir m_settings;
};

QTEST_GUILESS_MAIN(AdvancedGitTests)
#include "advanced_git_tests.moc"
