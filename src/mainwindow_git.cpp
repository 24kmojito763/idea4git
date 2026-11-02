#include "codeeditor.h"
#include "gitdialogs.h"
#include "icons.h"
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace idea4git {
namespace {
QString visiblePath(QString path) {
    return path.replace('\n', QChar(0x21b5)).replace('\r', QChar(0x240d));
}

QByteArray addedFilePatch(const QString& path, const QByteArray& bytes) {
    if (!GitOperations::isText(bytes))
        return QByteArray("Binary file differs\n");
    QStringList lines = QString::fromUtf8(bytes).split('\n');
    if (bytes.endsWith('\n') && !lines.isEmpty())
        lines.removeLast();
    QByteArray patch = "--- /dev/null\n+++ b/" + visiblePath(path).toUtf8() + "\n";
    patch += "@@ -0,0 +1," + QByteArray::number(lines.size()) + " @@\n";
    for (const auto& line : lines)
        patch += '+' + line.toUtf8() + '\n';
    return patch;
}
} // namespace

void MainWindow::buildAdvancedActions(QMenu* menu) {
    m_compareAction =
        menu->addAction(icon("diff"), text("比较任意两个版本…", "Compare revisions…"));
    m_compareAction->setObjectName("compareRevisionsAction");
    connect(m_compareAction, &QAction::triggered, this, [this] { showComparison(); });
    m_resolveAction = menu->addAction(text("三方冲突解决…", "Resolve conflicts…"));
    m_resolveAction->setObjectName("resolveConflictsAction");
    connect(m_resolveAction, &QAction::triggered, this, [this] { showConflict(); });
    menu->addSeparator();
    for (auto kind : {OperationKind::Merge, OperationKind::Rebase, OperationKind::CherryPick,
                      OperationKind::Revert, OperationKind::Reset}) {
        auto* action = menu->addAction(operationName(kind, m_english) + QStringLiteral("…"));
        action->setObjectName(QStringLiteral("advancedOperation%1").arg(int(kind)));
        m_advancedActions.append(action);
        connect(action, &QAction::triggered, this, [this, kind] { showAdvancedOperation(kind); });
    }
    m_stashAction = menu->addAction(text("Stash 管理…", "Manage stashes…"));
    m_stashAction->setObjectName("stashManagerAction");
    connect(m_stashAction, &QAction::triggered, this, &MainWindow::showStashes);
    menu->addSeparator();
    m_continueAction = menu->addAction(text("继续当前 Git 操作", "Continue Git operation"));
    m_continueAction->setObjectName("continueOperationAction");
    m_abortAction = menu->addAction(text("中止当前 Git 操作…", "Abort Git operation…"));
    m_abortAction->setObjectName("abortOperationAction");
    m_skipAction = menu->addAction(text("跳过当前提交…", "Skip current commit…"));
    m_skipAction->setObjectName("skipOperationAction");
    connect(m_continueAction, &QAction::triggered, this,
            [this] { sequenceControl(SequenceAction::Continue); });
    connect(m_abortAction, &QAction::triggered, this,
            [this] { sequenceControl(SequenceAction::Abort); });
    connect(m_skipAction, &QAction::triggered, this,
            [this] { sequenceControl(SequenceAction::Skip); });
}

void MainWindow::buildWorkflowBar() {
    addToolBarBreak();
    m_workflowBar = addToolBar(text("进行中的 Git 操作", "Pending Git operation"));
    m_workflowBar->setObjectName("workflowToolbar");
    m_workflowBar->setMovable(false);
    m_workflowLabel = new QLabel(m_workflowBar);
    m_workflowLabel->setTextFormat(Qt::PlainText);
    m_workflowBar->addWidget(m_workflowLabel);
    auto* spacer = new QWidget(m_workflowBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_workflowBar->addWidget(spacer);
    m_workflowBar->addAction(m_resolveAction);
    m_workflowBar->addAction(m_continueAction);
    m_workflowBar->addAction(m_skipAction);
    m_workflowBar->addAction(m_abortAction);
    m_workflowBar->hide();
}

void MainWindow::inspectWorkflow() {
    if (!m_operations)
        return;
    const int generation = m_generation;
    m_operations->snapshot([this, generation](const GitResult& result, const GitSnapshot& state) {
        if (generation != m_generation)
            return;
        m_pendingOperation = result.ok() ? state.pending : PendingOperation::Unknown;
        updateActions();
    });
}

void MainWindow::updateAdvancedActions() {
    const bool available = m_operations && !m_repositoryRoot.isEmpty() && m_status.valid &&
                           !gitBusy() && !m_operationRunning;
    bool conflicts = false;
    for (const auto& file : m_status.files)
        conflicts |= file.conflict();
    const bool active = m_pendingOperation != PendingOperation::None;
    const bool known = active && m_pendingOperation != PendingOperation::Unknown;
    m_compareAction->setEnabled(available && m_hasHead);
    m_resolveAction->setEnabled(available && conflicts);
    for (auto* action : m_advancedActions)
        action->setEnabled(available && m_hasHead && !active && !conflicts);
    m_stashAction->setEnabled(available && m_hasHead && !active && !conflicts);
    m_branch->setEnabled(available && !active && !conflicts);
    m_pullAction->setEnabled(available && !active && !conflicts);
    m_pushAction->setEnabled(available && !active && !conflicts);
    m_amend->setEnabled(m_hasHead && !active);
    m_continueAction->setEnabled(available && known && !conflicts);
    m_abortAction->setEnabled(available && known);
    m_skipAction->setEnabled(available && known && m_pendingOperation != PendingOperation::Merge);
    m_workflowBar->setVisible(!m_repositoryRoot.isEmpty() && (active || conflicts));
    m_workflowLabel->setText(
        active
            ? text("进行中：", "In progress: ") + pendingName(m_pendingOperation, m_english) +
                  (conflicts ? text(" · 先保存并暂存冲突解决结果",
                                    " · save and stage conflict resolutions first")
                             : text(" · 可继续或中止", " · continue or abort"))
            : text("工作区存在未合并文件（如 stash 冲突）。解决后手动提交；没有专用继续/中止操作。",
                   "Unmerged worktree files (e.g. stash conflict). Resolve and commit normally; no "
                   "dedicated continue/abort."));
}

bool MainWindow::prepareWorktreeChange() {
    for (int i = 0; i < m_editors->count(); ++i) {
        auto* editor = qobject_cast<CodeEditor*>(m_editors->widget(i));
        if (editor && editor->isModified()) {
            m_editors->setCurrentIndex(i);
            QMessageBox::information(
                this, text("请先处理未保存文件", "Handle unsaved files first"),
                editor->filePath() + '\n' +
                    text("请先保存或关闭未保存的编辑器标签，再执行可能修改工作区的 Git 操作。",
                         "Save or close unsaved editor tabs before a Git operation that can change "
                         "the worktree."));
            return false;
        }
    }
    return true;
}

void MainWindow::showComparison(QString left, QString right) {
    if (!m_operations || gitBusy())
        return;
    if (left.isEmpty() || right.isEmpty()) {
        auto rows = m_history->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(),
                  [](const QModelIndex& a, const QModelIndex& b) { return a.row() > b.row(); });
        if (rows.size() == 2 && rows.first().row() < m_commits.size() &&
            rows.last().row() < m_commits.size()) {
            left = m_commits[rows.first().row()].hash;
            right = m_commits[rows.last().row()].hash;
        } else {
            left = "HEAD~1";
            right = "HEAD";
        }
    }
    CompareDialog dialog(m_operations, m_english, this, left, right);
    dialog.exec();
    if (m_refreshPending)
        refreshRepository();
}

void MainWindow::openFileDiff(const QString& title, const QString& path,
                              const QString& leftRevision, const QByteArray& left,
                              const QString& rightRevision, const QByteArray& right,
                              const QByteArray& patch) {
    auto* view = new FileDiffView(title, path, leftRevision, left, rightRevision, right, patch,
                                  m_english, m_editors);
    QString fileName = QFileInfo(path).fileName();
    if (fileName.isEmpty())
        fileName = visiblePath(path);
    const int index = m_editors->addTab(view, icon("diff"),
                                        fileName + QStringLiteral(" — ") + text("差异", "Diff"));
    m_editors->setTabToolTip(index, title + QStringLiteral("\n") + visiblePath(path));
    auto* close = new QToolButton(m_editors);
    close->setObjectName(QStringLiteral("closeDiffTabButton"));
    close->setIcon(icon("close"));
    close->setToolTip(text("关闭差异", "Close diff"));
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(14, 14));
    close->setAutoRaise(true);
    m_editors->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    connect(close, &QToolButton::clicked, this, [this, view] {
        const int tab = m_editors->indexOf(view);
        if (tab >= 0)
            closeEditor(tab);
    });
    m_editors->setCurrentIndex(index);
}

void MainWindow::showCommitFileDiff(const QString& commit, const CommitFileChange& file) {
    if (!m_git || m_repositoryRoot.isEmpty())
        return;
    const int generation = m_generation;
    m_git->run(repositoryArgs({"rev-list", "--parents", "-n", "1", commit}), [this, generation,
                                                                              commit, file](
                                                                                 const GitResult&
                                                                                     parentResult) {
        if (generation != m_generation)
            return;
        if (!parentResult.ok()) {
            reportError(text("读取提交父版本失败", "Could not read commit parent"), parentResult);
            return;
        }
        const auto fields =
            QString::fromUtf8(parentResult.output).trimmed().split(' ', Qt::SkipEmptyParts);
        const QString parent = fields.size() > 1 ? fields[1] : QString();
        const QString oldPath = file.originalPath.isEmpty() ? file.path : file.originalPath;
        const bool oldExists = !parent.isEmpty() && !file.status.startsWith('A');
        const bool newExists = !file.status.startsWith('D');
        struct LoadedDiff {
            int remaining = 0;
            QByteArray left;
            QByteArray right;
            QByteArray patch;
            QString error;
        };
        auto loaded = std::make_shared<LoadedDiff>();
        loaded->remaining = (oldExists ? 1 : 0) + (newExists ? 1 : 0) + 1;
        const auto complete = [this, generation, loaded, commit, file, parent] {
            if (--loaded->remaining != 0 || generation != m_generation)
                return;
            if (!loaded->error.isEmpty()) {
                m_activity->setText(loaded->error);
                return;
            }
            const QString oldRevision = parent.isEmpty()
                                            ? text("空文件（初始提交）", "Empty (root commit)")
                                            : parent.left(12);
            openFileDiff(text("已提交差异", "Committed diff"), file.path, oldRevision, loaded->left,
                         commit.left(12), loaded->right, loaded->patch);
        };

        if (oldExists) {
            m_git->run(repositoryArgs({"cat-file", "blob", parent + ':' + oldPath}),
                       [loaded, complete](const GitResult& result) {
                           if (!result.ok())
                               loaded->error = QString::fromUtf8(result.error);
                           else
                               loaded->left = result.output;
                           complete();
                       });
        }
        if (newExists) {
            m_git->run(repositoryArgs({"cat-file", "blob", commit + ':' + file.path}),
                       [loaded, complete](const GitResult& result) {
                           if (!result.ok())
                               loaded->error = QString::fromUtf8(result.error);
                           else
                               loaded->right = result.output;
                           complete();
                       });
        }
        QStringList diffArgs;
        if (parent.isEmpty()) {
            diffArgs = {
                "diff-tree",     "--root",         "-r",          "--no-commit-id", "--no-ext-diff",
                "--no-textconv", "--find-renames", "--unified=0", commit,           "--",
                oldPath,         file.path};
        } else {
            diffArgs = {"diff",        "--no-ext-diff", "--no-textconv", "--find-renames",
                        "--unified=0", parent,          commit,          "--",
                        oldPath,       file.path};
        }
        m_git->run(repositoryArgs(diffArgs), [loaded, complete](const GitResult& result) {
            if (!result.ok())
                loaded->error = QString::fromUtf8(result.error);
            else
                loaded->patch = result.output;
            complete();
        });
    });
}

void MainWindow::showWorkingTreeDiff(CodeEditor* editor, const QString& path,
                                     const FileChange& file) {
    if (!m_git || !editor || path.isEmpty())
        return;
    QByteArray right;
    if (file.state() != FileState::Deleted) {
        QFile source(editor->filePath());
        if (!source.open(QIODevice::ReadOnly) || source.size() > 16 * 1024 * 1024) {
            QMessageBox::warning(this, text("无法读取文件", "Could not read file"),
                                 text("工作区文件无法读取或超过 16 MiB。",
                                      "The working-tree file is unreadable or exceeds 16 MiB."));
            return;
        }
        right = source.readAll();
    }

    const auto state = file.state();
    const bool untracked = state == FileState::Untracked;
    const bool newlyAdded = state == FileState::Added;
    const bool oldExists = m_hasHead && !untracked && !newlyAdded;
    const QString oldPath = file.originalPath.isEmpty() ? path : file.originalPath;
    if (!m_hasHead || untracked) {
        openFileDiff(text("未提交差异（未保存的编辑不包含在 Git diff 中）",
                          "Working-tree diff (unsaved editor text is not included)"),
                     path, text("空文件", "Empty"), {}, text("工作区", "Worktree"), right,
                     addedFilePatch(path, right));
        return;
    }

    struct LoadedDiff {
        int remaining = 2;
        QByteArray left;
        QByteArray patch;
        QString error;
    };
    auto loaded = std::make_shared<LoadedDiff>();
    const int generation = m_generation;
    const auto complete = [this, generation, loaded, path, right] {
        if (--loaded->remaining != 0 || generation != m_generation)
            return;
        if (!loaded->error.isEmpty()) {
            m_activity->setText(loaded->error);
            return;
        }
        openFileDiff(text("未提交差异（未保存的编辑不包含在 Git diff 中）",
                          "Working-tree diff (unsaved editor text is not included)"),
                     path, "HEAD", loaded->left, text("工作区", "Worktree"), right, loaded->patch);
    };
    if (oldExists) {
        m_git->run(repositoryArgs({"cat-file", "blob", "HEAD:" + oldPath}),
                   [loaded, complete](const GitResult& result) {
                       if (!result.ok())
                           loaded->error = QString::fromUtf8(result.error);
                       else
                           loaded->left = result.output;
                       complete();
                   });
    } else {
        --loaded->remaining;
    }

    QStringList diffArgs = {"diff", "--no-ext-diff", "--no-textconv", "--find-renames",
                            "--unified=0"};
    diffArgs.append({"HEAD", "--"});
    if (!file.originalPath.isEmpty() && file.originalPath != path)
        diffArgs.append(file.originalPath);
    diffArgs.append(path);
    m_git->run(repositoryArgs(diffArgs), [loaded, complete](const GitResult& result) {
        if (!result.ok())
            loaded->error = QString::fromUtf8(result.error);
        else
            loaded->patch = result.output;
        complete();
    });
}

void MainWindow::showBranchDiff(CodeEditor* editor, const QString& path, const QString& branchName,
                                const QString& branchRef) {
    if (!m_git || !editor || path.isEmpty() || branchRef.isEmpty())
        return;

    QByteArray right;
    QFile source(editor->filePath());
    if (source.exists()) {
        if (!source.open(QIODevice::ReadOnly) || source.size() > 16 * 1024 * 1024) {
            QMessageBox::warning(this, text("无法读取文件", "Could not read file"),
                                 text("工作区文件无法读取或超过 16 MiB。",
                                      "The working-tree file is unreadable or exceeds 16 MiB."));
            return;
        }
        right = source.readAll();
    }

    const int generation = m_generation;
    m_activity->setText(text("正在读取分支文件…", "Reading branch file…"));
    m_git->run(
        repositoryArgs({"cat-file", "blob", branchRef + ':' + path}),
        [this, generation, path, branchName, right](const GitResult& branchResult) {
            if (generation != m_generation)
                return;
            if (branchResult.cancelled || branchResult.timedOut ||
                branchResult.outputLimitExceeded) {
                reportError(text("读取分支文件失败", "Could not read branch file"), branchResult);
                return;
            }

            const QByteArray left = branchResult.ok() ? branchResult.output : QByteArray();
            const auto temporary = std::make_shared<QTemporaryDir>();
            if (!temporary->isValid()) {
                m_activity->setText(
                    text("无法创建差异临时文件。", "Could not create temporary diff files."));
                return;
            }
            const QString leftPath = temporary->filePath(QStringLiteral("branch-version"));
            const QString rightPath = temporary->filePath(QStringLiteral("working-tree-version"));
            const auto writeTemporary = [](const QString& filePath, const QByteArray& bytes,
                                           QString* error) {
                QFile file(filePath);
                if (!file.open(QIODevice::WriteOnly)) {
                    *error = file.errorString();
                    return false;
                }
                if (file.write(bytes) != bytes.size() || !file.flush()) {
                    *error = file.errorString();
                    return false;
                }
                return true;
            };
            QString writeError;
            if (!writeTemporary(leftPath, left, &writeError) ||
                !writeTemporary(rightPath, right, &writeError)) {
                m_activity->setText(writeError);
                return;
            }

            const QString leftLabel = visiblePath(branchName + ':' + path);
            const QString rightLabel = visiblePath(text("工作区:", "Working tree:") + path);
            const auto diffArgs =
                repositoryArgs({"--literal-pathspecs", "diff", "--no-index", "--no-ext-diff",
                                "--no-textconv", "--unified=0", "--", leftPath, rightPath});
            m_git->run(diffArgs, [this, generation, path, branchName, left, right, leftLabel,
                                  rightLabel, temporary](const GitResult& diff) {
                if (generation != m_generation)
                    return;
                const bool hasDiff = diff.exitCode == 1 && !diff.cancelled && !diff.timedOut &&
                                     !diff.outputLimitExceeded;
                if (!diff.ok() && !hasDiff) {
                    reportError(text("比较分支文件失败", "Could not compare branch file"), diff);
                    return;
                }

                QByteArray patch = diff.output;
                auto lines = patch.split('\n');
                bool oldHeaderReplaced = false;
                bool newHeaderReplaced = false;
                for (auto& line : lines) {
                    if (line.startsWith("diff --git ")) {
                        line = "diff --git a/" + leftLabel.toUtf8() + " b/" + rightLabel.toUtf8();
                    } else if (line.startsWith("Binary files ")) {
                        line = "Binary files a/" + leftLabel.toUtf8() + " and b/" +
                               rightLabel.toUtf8() + " differ";
                    } else if (!oldHeaderReplaced && line.startsWith("--- ")) {
                        line = "--- a/" + leftLabel.toUtf8();
                        oldHeaderReplaced = true;
                    } else if (!newHeaderReplaced && line.startsWith("+++ ")) {
                        line = "+++ b/" + rightLabel.toUtf8();
                        newHeaderReplaced = true;
                    }
                }
                patch = lines.join('\n');
                const QString title =
                    text("与分支比较", "Compare with branch") + QStringLiteral(" — ") + branchName;
                openFileDiff(title, path, branchName, left, text("工作区", "Working tree"), right,
                             patch);
                m_activity->setText(title);
            });
        });
}

void MainWindow::showCommittedFileDiff(const QString& path) {
    if (!m_git || path.isEmpty())
        return;
    const int generation = m_generation;
    m_git->run(
        repositoryArgs({"log", "--follow", "-1", "--format=%H", "--", path}),
        [this, generation, path](const GitResult& result) {
            if (generation != m_generation)
                return;
            const auto commit = QString::fromUtf8(result.output).trimmed();
            if (!result.ok() || commit.isEmpty()) {
                if (!result.ok())
                    reportError(text("读取文件历史失败", "Could not read file history"), result);
                else
                    m_activity->setText(
                        text("该文件没有已提交的历史。", "This file has no committed history."));
                return;
            }
            m_git->run(
                repositoryArgs({"rev-list", "--parents", "-n", "1", commit}),
                [this, generation, path, commit](const GitResult& parents) {
                    if (generation != m_generation)
                        return;
                    if (!parents.ok()) {
                        reportError(text("读取提交父版本失败", "Could not read commit parent"),
                                    parents);
                        return;
                    }
                    const auto fields =
                        QString::fromUtf8(parents.output).trimmed().split(' ', Qt::SkipEmptyParts);
                    const QString parent = fields.size() > 1 ? fields[1] : QString();
                    QStringList args;
                    if (parent.isEmpty()) {
                        args = {"diff-tree",
                                "--root",
                                "-r",
                                "--no-commit-id",
                                "--no-ext-diff",
                                "--no-textconv",
                                "--find-renames",
                                "--name-status",
                                "-z",
                                commit};
                    } else {
                        args = {"diff",           "--no-ext-diff", "--no-textconv",
                                "--find-renames", "--name-status", "-z",
                                parent,           commit};
                    }
                    m_git->run(repositoryArgs(args), [this, generation, path,
                                                      commit](const GitResult& files) {
                        if (generation != m_generation)
                            return;
                        if (!files.ok()) {
                            reportError(text("读取已提交差异失败", "Could not read committed diff"),
                                        files);
                            return;
                        }
                        bool valid = false;
                        const auto changes = parseCommitFiles(files.output, &valid);
                        if (!valid) {
                            m_activity->setText(text("Git 文件列表无法安全解析。",
                                                     "Could not safely parse Git's file list."));
                            return;
                        }
                        for (const auto& change : changes)
                            if (change.path == path || change.originalPath == path) {
                                showCommitFileDiff(commit, change);
                                return;
                            }
                        m_activity->setText(text("没有找到该文件对应的提交差异。",
                                                 "No committed change was found for this file."));
                    });
                });
        });
}

void MainWindow::showAdvancedOperation(OperationKind kind, QString revision) {
    if (!m_operations || gitBusy() || !prepareWorktreeChange())
        return;
    if (revision.isEmpty() && (kind == OperationKind::CherryPick || kind == OperationKind::Revert ||
                               kind == OperationKind::Reset)) {
        const int row = m_history->currentRow();
        if (row >= 0 && row < m_commits.size())
            revision = m_commits[row].hash;
    }
    bool resolve = false;
    OperationDialog dialog(m_operations, kind, m_english, this, revision);
    connect(&dialog, &OperationDialog::repositoryChanged, this,
            [this] { m_reloadRequested = true; });
    connect(&dialog, &OperationDialog::conflictsRequested, this, [&resolve] { resolve = true; });
    dialog.exec();
    refreshRepository();
    if (resolve) {
        // The queued refresh must finish before the next modal loads current conflict paths.
        auto* timer = new QTimer(this);
        timer->setInterval(50);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            if (!gitBusy()) {
                timer->stop();
                timer->deleteLater();
                showConflict();
            }
        });
        timer->start();
    }
}

void MainWindow::showConflict(QString path) {
    if (!m_operations || gitBusy() || !prepareWorktreeChange())
        return;
    QStringList paths;
    for (const auto& file : m_status.files)
        if (file.conflict())
            paths.append(file.path);
    if (paths.isEmpty())
        return;
    if (path.isEmpty() || !paths.contains(path)) {
        const auto* selected = m_changes->currentItem();
        const int index = selected ? selected->data(0, Qt::UserRole).toInt() : -1;
        if (index >= 0 && index < m_status.files.size() && m_status.files[index].conflict())
            path = m_status.files[index].path;
        else if (paths.size() == 1)
            path = paths.first();
        else {
            QDialog picker(this);
            picker.setWindowTitle(text("选择冲突文件", "Select conflicted file"));
            picker.setMinimumWidth(500);
            auto* layout = new QVBoxLayout(&picker);
            auto* combo = new QComboBox(&picker);
            for (const auto& item : paths) {
                QString display = item;
                display.replace('\n', QChar(0x21b5));
                combo->addItem(display, item);
            }
            layout->addWidget(combo);
            auto* buttons =
                new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &picker);
            buttons->button(QDialogButtonBox::Open)->setText(text("打开", "Open"));
            buttons->button(QDialogButtonBox::Cancel)->setText(text("取消", "Cancel"));
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
            if (picker.exec() != QDialog::Accepted)
                return;
            path = combo->currentData().toString();
        }
    }
    ConflictDialog dialog(m_operations, path, m_english, this);
    connect(&dialog, &ConflictDialog::repositoryChanged, this,
            [this] { m_reloadRequested = true; });
    dialog.exec();
    refreshRepository();
}

void MainWindow::showStashes() {
    if (!m_operations || gitBusy() || !prepareWorktreeChange())
        return;
    bool resolve = false;
    StashDialog dialog(m_operations, m_english, this);
    connect(&dialog, &StashDialog::repositoryChanged, this, [this] { m_reloadRequested = true; });
    connect(&dialog, &StashDialog::conflictsRequested, this, [&resolve] { resolve = true; });
    dialog.exec();
    refreshRepository();
    if (resolve) {
        auto* timer = new QTimer(this);
        timer->setInterval(50);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            if (!gitBusy()) {
                timer->stop();
                timer->deleteLater();
                showConflict();
            }
        });
        timer->start();
    }
}

void MainWindow::sequenceControl(SequenceAction action) {
    if (!m_operations || gitBusy() || !prepareWorktreeChange())
        return;
    const auto target = pendingName(m_pendingOperation, m_english);
    const QString description =
        action == SequenceAction::Abort
            ? text("中止会尝试恢复操作前状态，本次操作中的解决结果和修改可能被覆盖。请先备份需要保"
                   "留的内容。",
                   "Abort attempts to restore the pre-operation state; resolution edits may be "
                   "overwritten. Back up anything to keep.")
        : action == SequenceAction::Skip
            ? text("跳过会丢弃当前正在应用的提交及其冲突解决内容，然后继续后面的提交。",
                   "Skip discards the current commit's changes and resolution edits, then proceeds "
                   "to later commits.")
            : text("继续将使用已暂存的解决结果和 Git 已有提交信息，不启动终端编辑器。",
                   "Continue uses staged resolutions and Git's existing commit message without a "
                   "terminal editor.");
    if (QMessageBox::warning(this, target, m_repositoryRoot + '\n' + target + "\n\n" + description,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    m_operationRunning = true;
    updateActions();
    m_operations->sequence(m_pendingOperation, action, [this](const GitResult& result) {
        m_operationRunning = false;
        m_reloadRequested = true;
        m_output->appendPlainText(QString::fromUtf8(result.output));
        if (!result.ok() && !result.cancelled)
            reportError(text("Git 操作未完成", "Git operation not completed"), result);
        refreshRepository();
        updateActions();
    });
}

void MainWindow::reloadUnmodifiedEditors() {
    for (int i = m_editors->count() - 1; i >= 0; --i) {
        auto* editor = qobject_cast<CodeEditor*>(m_editors->widget(i));
        if (!editor || editor->isModified() || !editor->changedOnDisk())
            continue;
        if (!QFileInfo::exists(editor->filePath())) {
            closeEditor(i);
            continue;
        }
        const auto path = editor->filePath();
        int line, column;
        editor->getCursorPosition(&line, &column);
        const int scroll = editor->verticalScrollBar()->value();
        QString error;
        if (editor->loadFile(path, &error)) {
            editor->setCursorPosition(qMin(line, editor->lines() - 1), column);
            editor->verticalScrollBar()->setValue(scroll);
        } else {
            closeEditor(i);
            m_output->appendPlainText(path + '\n' + error);
        }
    }
    updateEditorStatus();
}

} // namespace idea4git
