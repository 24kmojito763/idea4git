#include "gitdialogs.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace idea4git {
namespace {
QString localText(bool english, const char* chinese, const char* englishText) {
    return QString::fromUtf8(english ? englishText : chinese);
}

QStringList inRepository(const QString& root, const QStringList& arguments) {
    QStringList result{QStringLiteral("-C"), root};
    result.append(arguments);
    return result;
}

QString safeDisplayUrl(const QString& value) {
    QUrl url(value, QUrl::StrictMode);
    if (url.isValid() && !url.userInfo().isEmpty()) {
        url.setUserName(QStringLiteral("•••"));
        url.setPassword(QStringLiteral("•••"));
        return url.toDisplayString(QUrl::PrettyDecoded);
    }
    return value;
}

bool safeRemoteUrl(const QString& value) {
    const QString urlText = value.trimmed();
    if (urlText.isEmpty() || urlText.contains('\n') || urlText.contains('\r'))
        return false;
    if (urlText.contains(QStringLiteral("://"))) {
        const QUrl url(urlText, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty())
            return false;
        if (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))
            return url.userInfo().isEmpty() && url.query().isEmpty() && url.fragment().isEmpty();
        return url.password().isEmpty() && url.query().isEmpty() && url.fragment().isEmpty();
    }
    if (QDir::isAbsolutePath(urlText) || urlText.startsWith(QStringLiteral("./")) ||
        urlText.startsWith(QStringLiteral("../")))
        return true;
    static const QRegularExpression scpUrl(QStringLiteral("^[^\\s@]+@[^\\s:]+:.+$"));
    return scpUrl.match(urlText).hasMatch();
}

bool editRemoteForm(QWidget* parent, bool english, bool editing, QString* name, QString* url) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("remoteEditDialog"));
    dialog.setWindowTitle(localText(english, editing ? "编辑远程" : "添加远程",
                                    editing ? "Edit Remote" : "Add Remote"));
    dialog.setMinimumWidth(470);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* nameInput = new QLineEdit(*name, &dialog);
    nameInput->setObjectName(QStringLiteral("remoteNameInput"));
    nameInput->setReadOnly(editing);
    auto* urlInput = new QLineEdit(*url, &dialog);
    urlInput->setObjectName(QStringLiteral("remoteUrlInput"));
    urlInput->setPlaceholderText(QStringLiteral("https://host/owner/repository.git"));
    form->addRow(localText(english, "远程名称", "Remote name"), nameInput);
    form->addRow(localText(english, "Git 地址", "Git URL"), urlInput);
    layout->addLayout(form);
    auto* hint = new QLabel(
        localText(english, "不要把 access token 或密码写入 URL；请使用 Git 凭据管理器或 SSH。",
                  "Do not put an access token or password in the URL; use Git Credential Manager "
                  "or SSH."),
        &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(localText(english, "保存", "Save"));
    buttons->button(QDialogButtonBox::Cancel)->setText(localText(english, "取消", "Cancel"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    const QString proposedName = nameInput->text().trimmed();
    const QString proposedUrl = urlInput->text().trimmed();
    if (proposedName.isEmpty() || proposedName.startsWith('-') ||
        proposedName.contains(QRegularExpression(QStringLiteral("[\\s\\r\\n]")))) {
        QMessageBox::warning(&dialog, localText(english, "远程名称无效", "Invalid remote name"),
                             localText(english, "名称不能为空、不能以 - 开头或包含空白。",
                                       "Name cannot be empty, start with '-', or contain spaces."));
        return false;
    }
    if (!safeRemoteUrl(proposedUrl)) {
        QMessageBox::warning(
            &dialog, localText(english, "Git 地址无效", "Invalid Git URL"),
            localText(english,
                      "请输入有效的 HTTPS、SSH 或本地仓库地址；HTTP(S) 地址不能包含账号、密码、"
                      "查询参数或 token。",
                      "Enter a valid HTTPS, SSH, or local repository URL. HTTP(S) URLs must not "
                      "contain user info, passwords, query parameters, or tokens."));
        return false;
    }
    *name = proposedName;
    *url = proposedUrl;
    return true;
}

QList<CommitFileChange> parsePushFileList(const QByteArray& bytes, bool* valid) {
    QList<CommitFileChange> files;
    const auto tokens = bytes.split('\0');
    *valid = true;
    for (qsizetype index = 0; index < tokens.size();) {
        const auto status = tokens.at(index++);
        if (status.isEmpty())
            continue;
        const bool rename = status.startsWith('R') || status.startsWith('C');
        const qsizetype required = rename ? 2 : 1;
        if (index + required > tokens.size()) {
            *valid = false;
            return {};
        }
        CommitFileChange file;
        file.status = QString::fromLatin1(status);
        if (rename) {
            file.originalPath = QString::fromUtf8(tokens.at(index++));
            file.path = QString::fromUtf8(tokens.at(index++));
        } else {
            file.path = QString::fromUtf8(tokens.at(index++));
        }
        if (file.path.isEmpty() || (rename && file.originalPath.isEmpty())) {
            *valid = false;
            return {};
        }
        files.append(file);
    }
    return files;
}

QString pushFileColor(const QString& status) {
    if (status.startsWith('A') || status.startsWith('C'))
        return QStringLiteral("#87bd85");
    if (status.startsWith('D'))
        return QStringLiteral("#d99191");
    if (status.startsWith('R'))
        return QStringLiteral("#d6ba7b");
    return QStringLiteral("#8baff0");
}

QByteArray credentialInput(const QString& host, const QString& username,
                           const QString& password = {}) {
    QByteArray input = "protocol=https\nhost=" + host.toUtf8() + '\n';
    if (!username.isEmpty())
        input += "username=" + username.toUtf8() + '\n';
    if (!password.isEmpty())
        input += "password=" + password.toUtf8() + '\n';
    input += '\n';
    return input;
}
} // namespace

PushDialog::PushDialog(GitClient* git, QString repositoryRoot, QString branch, QString tracking,
                       bool english, QWidget* parent)
    : QDialog(parent), m_git(git), m_repositoryRoot(std::move(repositoryRoot)),
      m_branch(std::move(branch)), m_tracking(std::move(tracking)), m_english(english) {
    setObjectName(QStringLiteral("pushDialog"));
    setWindowTitle(localText(m_english, "推送提交", "Push Commits"));
    resize(760, 560);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_remote = new QComboBox(this);
    m_remote->setObjectName(QStringLiteral("pushRemoteCombo"));
    m_target = new QComboBox(this);
    m_target->setObjectName(QStringLiteral("pushTargetBranch"));
    m_target->setEditable(true);
    m_target->setInsertPolicy(QComboBox::NoInsert);
    form->addRow(localText(m_english, "远程", "Remote"), m_remote);
    form->addRow(localText(m_english, "目标分支", "Target branch"), m_target);
    layout->addLayout(form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("pushPreviewStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("pushPreviewSplitter"));
    m_commits = new QTableWidget(0, 2, split);
    m_commits->setObjectName(QStringLiteral("pushCommitTable"));
    m_commits->setHorizontalHeaderLabels(
        {localText(m_english, "提交", "Commit"), localText(m_english, "说明", "Message")});
    m_commits->verticalHeader()->hide();
    m_commits->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commits->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commits->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_commits->horizontalHeader()->setStretchLastSection(true);
    m_files = new QListWidget(split);
    m_files->setObjectName(QStringLiteral("pushCommitFiles"));
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    m_files->setToolTip(localText(m_english, "右键文件可在主编辑区查看彩色差异。",
                                  "Right-click a file to view its colored diff in the editor."));
    split->setSizes({300, 220});
    layout->addWidget(split, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    m_push = new QPushButton(localText(m_english, "推送", "Push"), this);
    m_push->setObjectName(QStringLiteral("pushNowButton"));
    m_push->setProperty("primary", true);
    auto* close = new QPushButton(localText(m_english, "关闭", "Close"), this);
    buttons->addWidget(m_push);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_remote, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_loading)
            loadBranches();
    });
    connect(m_target, &QComboBox::currentTextChanged, this, [this] {
        if (!m_loading)
            loadOutgoingCommits();
    });
    connect(m_commits, &QTableWidget::currentCellChanged, this,
            [this](int row) { loadCommitFiles(row); });
    connect(m_files, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = m_files->itemAt(point);
        const int row = m_commits->currentRow();
        if (!item || row < 0 || row >= m_commitHashes.size())
            return;
        QMenu menu(this);
        auto* diff = menu.addAction(localText(m_english, "对比差异", "Compare Diff"));
        diff->setObjectName(QStringLiteral("pushFileDiffAction"));
        if (menu.exec(m_files->viewport()->mapToGlobal(point)) != diff)
            return;
        CommitFileChange file;
        file.path = item->data(Qt::UserRole).toString();
        file.originalPath = item->data(Qt::UserRole + 1).toString();
        file.status = item->data(Qt::UserRole + 2).toString();
        emit diffRequested(m_commitHashes.at(row), file);
    });
    connect(m_files, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int row = m_commits->currentRow();
        if (!item || row < 0 || row >= m_commitHashes.size())
            return;
        CommitFileChange file;
        file.path = item->data(Qt::UserRole).toString();
        file.originalPath = item->data(Qt::UserRole + 1).toString();
        file.status = item->data(Qt::UserRole + 2).toString();
        emit diffRequested(m_commitHashes.at(row), file);
    });
    connect(m_push, &QPushButton::clicked, this, [this] {
        const QString remote = m_remote->currentText();
        const QString branch = m_target->currentText().trimmed();
        if (!remote.isEmpty() && !branch.isEmpty())
            emit pushRequested(remote, branch);
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    loadRemotes();
}

QStringList PushDialog::repositoryArgs(const QStringList& args) const {
    return inRepository(m_repositoryRoot, args);
}

void PushDialog::loadRemotes() {
    if (!m_git)
        return;
    m_loading = true;
    m_status->setText(localText(m_english, "正在读取远程仓库…", "Loading remotes…"));
    const QPointer<PushDialog> guard(this);
    m_git->run(repositoryArgs({"remote"}), [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_remote->clear();
        if (!result.ok()) {
            guard->m_status->setText(
                localText(guard->m_english, "读取远程失败。", "Could not read remotes."));
            guard->m_loading = false;
            guard->updateControls();
            return;
        }
        const auto remotes = QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts);
        for (const auto& remote : remotes)
            guard->m_remote->addItem(remote);
        int selected = -1;
        const int slash = guard->m_tracking.indexOf('/');
        const QString trackedRemote = slash > 0 ? guard->m_tracking.left(slash) : QString();
        selected = guard->m_remote->findText(trackedRemote);
        if (selected < 0 && !remotes.isEmpty())
            selected = 0;
        if (selected >= 0)
            guard->m_remote->setCurrentIndex(selected);
        guard->m_loading = false;
        if (selected >= 0)
            guard->loadBranches();
        else
            guard->m_status->setText(
                localText(guard->m_english, "尚未配置远程仓库。", "No remote is configured."));
        guard->updateControls();
    });
}

void PushDialog::loadBranches() {
    const QString remote = m_remote->currentText();
    if (!m_git || remote.isEmpty())
        return;
    const QString previousTarget = m_target->currentText();
    m_loading = true;
    const int request = ++m_loadRequest;
    m_status->setText(
        localText(m_english, "正在读取远程跟踪分支…", "Loading remote-tracking branches…"));
    const QPointer<PushDialog> guard(this);
    m_git->run(repositoryArgs({"for-each-ref", "--format=%(refname:short)", "refs/remotes/"}),
               [guard, request, remote, previousTarget](const GitResult& result) {
                   if (!guard || request != guard->m_loadRequest)
                       return;
                   QStringList branches;
                   if (result.ok()) {
                       const QString prefix = remote + '/';
                       for (const auto& reference :
                            QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts)) {
                           if (!reference.startsWith(prefix))
                               continue;
                           const auto branch = reference.mid(prefix.size());
                           if (branch != QStringLiteral("HEAD") && !branches.contains(branch))
                               branches.append(branch);
                       }
                   }
                   const int slash = guard->m_tracking.indexOf('/');
                   const QString trackedBranch =
                       slash > 0 && guard->m_tracking.left(slash) == remote
                           ? guard->m_tracking.mid(slash + 1)
                           : guard->m_branch;
                   if (!trackedBranch.isEmpty() && !branches.contains(trackedBranch))
                       branches.prepend(trackedBranch);
                   if (!guard->m_branch.isEmpty() && !branches.contains(guard->m_branch))
                       branches.append(guard->m_branch);
                   const QSignalBlocker block(guard->m_target);
                   guard->m_target->clear();
                   guard->m_target->addItems(branches);
                   const QString target =
                       !previousTarget.isEmpty() && branches.contains(previousTarget)
                           ? previousTarget
                           : trackedBranch;
                   guard->m_target->setEditText(target);
                   guard->m_loading = false;
                   guard->loadOutgoingCommits();
               });
}

void PushDialog::loadOutgoingCommits() {
    const QString remote = m_remote->currentText();
    const QString branch = m_target->currentText().trimmed();
    const int request = ++m_loadRequest;
    ++m_fileRequest;
    m_commitHashes.clear();
    m_commits->setRowCount(0);
    m_files->clear();
    if (!m_git || remote.isEmpty() || branch.isEmpty() || branch.startsWith('-')) {
        m_loading = false;
        m_status->setText(localText(m_english, "选择远程和有效的目标分支以查看待推送提交。",
                                    "Choose a remote and valid target branch to preview commits."));
        updateControls();
        return;
    }
    m_loading = true;
    m_status->setText(localText(m_english, "正在计算待推送提交…", "Calculating outgoing commits…"));
    const QPointer<PushDialog> guard(this);
    m_git->run(
        repositoryArgs({"check-ref-format", "--branch", branch}),
        [guard, request, remote, branch](const GitResult& valid) {
            if (!guard || request != guard->m_loadRequest)
                return;
            if (!valid.ok()) {
                guard->m_loading = false;
                guard->m_status->setText(localText(guard->m_english, "目标分支名称无效。",
                                                   "Invalid target branch name."));
                guard->updateControls();
                return;
            }
            const QString trackingRef = QStringLiteral("refs/remotes/") + remote + '/' + branch;
            guard->m_git->run(
                guard->repositoryArgs({"rev-parse", "--verify", "--quiet", "--end-of-options",
                                       trackingRef + "^{commit}"}),
                [guard, request, remote, trackingRef](const GitResult& base) {
                    if (!guard || request != guard->m_loadRequest)
                        return;
                    QStringList args{"log", "--reverse", "--date-order", "--max-count=501",
                                     "--format=%H%x09%h%x09%s"};
                    if (base.ok())
                        args.append(QString::fromLatin1(base.output.trimmed()) + "..HEAD");
                    else
                        args.append({"HEAD", "--not", "--remotes=" + remote});
                    guard->m_git->run(guard->repositoryArgs(args), [guard, request,
                                                                    baseOk = base.ok()](
                                                                       const GitResult& log) {
                        if (!guard || request != guard->m_loadRequest)
                            return;
                        guard->m_loading = false;
                        if (!log.ok()) {
                            guard->m_status->setText(localText(guard->m_english,
                                                               "读取待推送提交失败。",
                                                               "Could not load outgoing commits."));
                            guard->updateControls();
                            return;
                        }
                        auto lines = QString::fromUtf8(log.output).split('\n', Qt::SkipEmptyParts);
                        const bool truncated = lines.size() > 500;
                        if (truncated)
                            lines = lines.mid(0, 500);
                        guard->m_commitHashes.clear();
                        guard->m_commits->setRowCount(lines.size());
                        for (int row = 0; row < lines.size(); ++row) {
                            const auto fields = lines.at(row).split('\t');
                            if (fields.size() < 3)
                                continue;
                            guard->m_commitHashes.append(fields.at(0));
                            auto* hash = new QTableWidgetItem(fields.at(1));
                            hash->setForeground(QColor("#8baff0"));
                            hash->setToolTip(fields.at(0));
                            guard->m_commits->setItem(row, 0, hash);
                            guard->m_commits->setItem(
                                row, 1, new QTableWidgetItem(fields.mid(2).join('\t')));
                        }
                        const QString basis =
                            baseOk ? localText(guard->m_english, "本地远程跟踪分支",
                                               "local remote-tracking ref")
                                   : localText(guard->m_english,
                                               "本地已知远程分支（尚未获取时预览可能不完整）",
                                               "locally known remote refs (preview may be "
                                               "incomplete until fetched)");
                        guard->m_status->setText(
                            localText(guard->m_english, "%1 个待推送提交 · 基于%2%3",
                                      "%1 outgoing commits · based on %2%3")
                                .arg(guard->m_commitHashes.size())
                                .arg(basis)
                                .arg(truncated ? localText(guard->m_english, " · 仅显示前 500 条",
                                                           " · first 500 shown")
                                               : QString()));
                        if (!guard->m_commitHashes.isEmpty()) {
                            guard->m_commits->setCurrentCell(0, 0);
                        }
                        guard->updateControls();
                    });
                });
        });
    updateControls();
}

void PushDialog::loadCommitFiles(int row) {
    const int request = ++m_fileRequest;
    m_files->clear();
    if (!m_git || row < 0 || row >= m_commitHashes.size())
        return;
    const QString commit = m_commitHashes.at(row);
    m_status->setText(localText(m_english, "正在读取提交文件…", "Loading commit files…"));
    const QPointer<PushDialog> guard(this);
    m_git->run(
        repositoryArgs({"rev-list", "--parents", "-n", "1", commit}),
        [guard, request, commit](const GitResult& parents) {
            if (!guard || request != guard->m_fileRequest)
                return;
            if (!parents.ok()) {
                guard->m_status->setText(localText(guard->m_english, "读取提交父版本失败。",
                                                   "Could not read commit parent."));
                return;
            }
            const auto fields =
                QString::fromUtf8(parents.output).trimmed().split(' ', Qt::SkipEmptyParts);
            QStringList args;
            if (fields.size() > 1) {
                args = {"diff", "--no-ext-diff", "--no-textconv", "--find-renames", "--name-status",
                        "-z",   fields.at(1),    commit};
            } else {
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
            }
            guard->m_git->run(guard->repositoryArgs(args), [guard, request,
                                                            commit](const GitResult& result) {
                if (!guard || request != guard->m_fileRequest)
                    return;
                if (!result.ok()) {
                    guard->m_status->setText(localText(guard->m_english, "读取变更文件失败。",
                                                       "Could not read changed files."));
                    return;
                }
                bool valid = false;
                const auto files = parsePushFileList(result.output, &valid);
                if (!valid) {
                    guard->m_status->setText(localText(guard->m_english,
                                                       "Git 返回的文件列表无法安全解析。",
                                                       "Could not safely parse Git's file list."));
                    return;
                }
                for (const auto& file : files) {
                    QString display = file.status.left(1) + QStringLiteral("   ") + file.path;
                    if (!file.originalPath.isEmpty())
                        display += QStringLiteral("  ← ") + file.originalPath;
                    display.replace('\n', QChar(0x21b5)).replace('\r', QChar(0x240d));
                    auto* item = new QListWidgetItem(display, guard->m_files);
                    item->setForeground(QColor(pushFileColor(file.status)));
                    item->setData(Qt::UserRole, file.path);
                    item->setData(Qt::UserRole + 1, file.originalPath);
                    item->setData(Qt::UserRole + 2, file.status);
                    item->setToolTip(file.path +
                                     (file.originalPath.isEmpty()
                                          ? QString()
                                          : QStringLiteral("\n← ") + file.originalPath));
                }
                guard->m_status->setText(
                    localText(guard->m_english, "%1 个提交文件。右键可查看彩色差异。",
                              "%1 changed files. Right-click to view a colored diff.")
                        .arg(files.size()));
            });
        });
}

void PushDialog::setPushRunning(bool running) {
    m_pushRunning = running;
    updateControls();
}

void PushDialog::pushFinished(bool success, const QString& message) {
    m_pushRunning = false;
    m_status->setText(message);
    if (success)
        m_status->setStyleSheet(QStringLiteral("color: #87bd85"));
    else
        m_status->setStyleSheet(QStringLiteral("color: #d99191"));
    updateControls();
}

void PushDialog::updateControls() {
    const bool remoteSelected = m_remote && m_remote->currentIndex() >= 0;
    const bool branchSelected = m_target && !m_target->currentText().trimmed().isEmpty() &&
                                !m_target->currentText().trimmed().startsWith('-');
    m_push->setEnabled(remoteSelected && branchSelected && !m_commitHashes.isEmpty() &&
                       !m_loading && !m_pushRunning);
}

RemoteManagerDialog::RemoteManagerDialog(GitClient* git, QString repositoryRoot, bool english,
                                         QWidget* parent)
    : QDialog(parent), m_git(git), m_repositoryRoot(std::move(repositoryRoot)), m_english(english) {
    setObjectName(QStringLiteral("remoteManagerDialog"));
    setWindowTitle(localText(m_english, "管理远程仓库", "Manage Remotes"));
    resize(720, 360);
    auto* layout = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("remoteManagerStatus"));
    layout->addWidget(m_status);
    m_remotes = new QTableWidget(0, 2, this);
    m_remotes->setObjectName(QStringLiteral("remotesTable"));
    m_remotes->setHorizontalHeaderLabels(
        {localText(m_english, "名称", "Name"), localText(m_english, "Git 地址", "Git URL")});
    m_remotes->verticalHeader()->hide();
    m_remotes->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remotes->setSelectionMode(QAbstractItemView::SingleSelection);
    m_remotes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remotes->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_remotes, 1);
    auto* buttons = new QHBoxLayout;
    m_add = new QPushButton(localText(m_english, "添加…", "Add…"), this);
    m_add->setObjectName(QStringLiteral("addRemoteButton"));
    m_edit = new QPushButton(localText(m_english, "编辑地址…", "Edit URL…"), this);
    m_edit->setObjectName(QStringLiteral("editRemoteButton"));
    m_remove = new QPushButton(localText(m_english, "移除", "Remove"), this);
    m_remove->setObjectName(QStringLiteral("removeRemoteButton"));
    auto* close = new QPushButton(localText(m_english, "关闭", "Close"), this);
    buttons->addWidget(m_add);
    buttons->addWidget(m_edit);
    buttons->addWidget(m_remove);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(m_add, &QPushButton::clicked, this, &RemoteManagerDialog::addRemote);
    connect(m_edit, &QPushButton::clicked, this, &RemoteManagerDialog::editRemote);
    connect(m_remove, &QPushButton::clicked, this, &RemoteManagerDialog::removeRemote);
    connect(m_remotes, &QTableWidget::itemSelectionChanged, this,
            &RemoteManagerDialog::updateControls);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    refresh();
}

QStringList RemoteManagerDialog::repositoryArgs(const QStringList& args) const {
    return inRepository(m_repositoryRoot, args);
}

void RemoteManagerDialog::refresh() {
    if (!m_git)
        return;
    m_working = true;
    m_status->setText(localText(m_english, "正在读取远程配置…", "Loading remote configuration…"));
    updateControls();
    const QPointer<RemoteManagerDialog> guard(this);
    m_git->run(repositoryArgs({"remote", "-v"}), [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_remotes->setRowCount(0);
        if (!result.ok()) {
            guard->m_status->setText(localText(guard->m_english, "读取远程配置失败。",
                                               "Could not read remote configuration."));
            guard->m_working = false;
            guard->updateControls();
            return;
        }
        QMap<QString, QString> remotes;
        const auto lines = QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            const int tab = line.indexOf('\t');
            if (tab < 0 || !line.endsWith(QStringLiteral(" (fetch)")))
                continue;
            const QString name = line.left(tab);
            const QString url = line.mid(tab + 1, line.size() - tab - 1 - 8);
            remotes.insert(name, url);
        }
        for (auto it = remotes.cbegin(); it != remotes.cend(); ++it) {
            const int row = guard->m_remotes->rowCount();
            guard->m_remotes->insertRow(row);
            guard->m_remotes->setItem(row, 0, new QTableWidgetItem(it.key()));
            auto* url = new QTableWidgetItem(safeDisplayUrl(it.value()));
            url->setData(Qt::UserRole, it.value());
            guard->m_remotes->setItem(row, 1, url);
        }
        guard->m_working = false;
        guard->m_status->setText(
            remotes.isEmpty()
                ? localText(guard->m_english, "尚未配置远程。", "No remotes configured.")
                : localText(guard->m_english, "%1 个远程仓库。", "%1 remotes configured.")
                      .arg(remotes.size()));
        guard->updateControls();
    });
}

void RemoteManagerDialog::addRemote() {
    QString name;
    QString url;
    if (!editRemoteForm(this, m_english, false, &name, &url) || !m_git)
        return;
    m_working = true;
    updateControls();
    const QPointer<RemoteManagerDialog> guard(this);
    m_git->run(repositoryArgs({"remote", "add", name, url}), [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_working = false;
        if (!result.ok())
            guard->m_status->setText(localText(guard->m_english, "添加远程失败，请检查名称和地址。",
                                               "Could not add remote; check its name and URL."));
        else
            guard->refresh();
        guard->updateControls();
    });
}

void RemoteManagerDialog::editRemote() {
    const int row = m_remotes->currentRow();
    if (row < 0 || !m_git)
        return;
    QString name = m_remotes->item(row, 0)->text();
    const QString rawUrl = m_remotes->item(row, 1)->data(Qt::UserRole).toString();
    QString url = safeDisplayUrl(rawUrl);
    if (url.contains(QStringLiteral("•••")))
        url.clear();
    if (!editRemoteForm(this, m_english, true, &name, &url))
        return;
    m_working = true;
    updateControls();
    const QPointer<RemoteManagerDialog> guard(this);
    m_git->run(repositoryArgs({"remote", "set-url", name, url}), [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_working = false;
        if (!result.ok())
            guard->m_status->setText(
                localText(guard->m_english, "更新远程地址失败。", "Could not update remote URL."));
        else
            guard->refresh();
        guard->updateControls();
    });
}

void RemoteManagerDialog::removeRemote() {
    const int row = m_remotes->currentRow();
    if (row < 0 || !m_git)
        return;
    const QString name = m_remotes->item(row, 0)->text();
    if (QMessageBox::question(
            this, localText(m_english, "移除远程仓库", "Remove remote"),
            localText(m_english, "将从此仓库配置中移除“%1”。不会删除服务器上的仓库。",
                      "Remove '%1' from this repository's configuration. The server repository "
                      "will not be deleted.")
                .arg(name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    m_working = true;
    updateControls();
    const QPointer<RemoteManagerDialog> guard(this);
    m_git->run(repositoryArgs({"remote", "remove", name}), [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_working = false;
        if (!result.ok())
            guard->m_status->setText(
                localText(guard->m_english, "移除远程失败。", "Could not remove remote."));
        else
            guard->refresh();
        guard->updateControls();
    });
}

void RemoteManagerDialog::updateControls() {
    const bool selected = m_remotes && m_remotes->currentRow() >= 0;
    m_add->setEnabled(!m_working);
    m_edit->setEnabled(!m_working && selected);
    m_remove->setEnabled(!m_working && selected);
}

GitCredentialDialog::GitCredentialDialog(GitClient* git, bool english, QWidget* parent)
    : QDialog(parent), m_git(git), m_english(english) {
    setObjectName(QStringLiteral("gitCredentialDialog"));
    setWindowTitle(localText(m_english, "Git 凭据", "Git Credentials"));
    resize(620, 500);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(
        localText(m_english,
                  "登录由 Git Credential Manager 处理。个人访问令牌只通过标准输入交给凭据管理器，"
                  "不会写入命令参数、应用设置或远程 URL。",
                  "Sign-in is handled by Git Credential Manager. A personal access token is sent "
                  "only on stdin; it is not written to command arguments, app settings, or remote "
                  "URLs."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("credentialStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* accountButtons = new QHBoxLayout;
    m_browserLogin = new QPushButton(
        localText(m_english, "使用 GitHub 浏览器登录…", "Sign in to GitHub in browser…"), this);
    m_browserLogin->setObjectName(QStringLiteral("credentialBrowserLogin"));
    auto* refreshAccounts =
        new QPushButton(localText(m_english, "刷新账号", "Refresh accounts"), this);
    refreshAccounts->setObjectName(QStringLiteral("credentialRefreshAccounts"));
    accountButtons->addWidget(m_browserLogin);
    accountButtons->addWidget(refreshAccounts);
    accountButtons->addStretch();
    layout->addLayout(accountButtons);
    m_accounts = new QPlainTextEdit(this);
    m_accounts->setObjectName(QStringLiteral("credentialAccounts"));
    m_accounts->setReadOnly(true);
    m_accounts->setMaximumHeight(110);
    m_accounts->setPlaceholderText(localText(m_english, "GitHub 账号列表", "GitHub accounts"));
    layout->addWidget(m_accounts);

    auto* patTitle =
        new QLabel(localText(m_english, "保存个人访问令牌", "Store a personal access token"), this);
    layout->addWidget(patTitle);
    auto* form = new QFormLayout;
    m_host = new QLineEdit(QStringLiteral("github.com"), this);
    m_host->setObjectName(QStringLiteral("credentialHost"));
    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("credentialUsername"));
    m_token = new QLineEdit(this);
    m_token->setObjectName(QStringLiteral("credentialToken"));
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(
        localText(m_english, "令牌仅在本次操作中保留在内存", "Used only for this operation"));
    form->addRow(localText(m_english, "服务器", "Host"), m_host);
    form->addRow(localText(m_english, "用户名", "Username"), m_username);
    form->addRow(localText(m_english, "访问令牌", "Access token"), m_token);
    layout->addLayout(form);
    m_storeToken = new QPushButton(
        localText(m_english, "保存到 Git 凭据管理器", "Store in Git Credential Manager"), this);
    m_storeToken->setObjectName(QStringLiteral("credentialStoreToken"));
    layout->addWidget(m_storeToken, 0, Qt::AlignLeft);

    auto* eraseRow = new QHBoxLayout;
    m_eraseUsername = new QLineEdit(this);
    m_eraseUsername->setObjectName(QStringLiteral("credentialEraseUsername"));
    m_eraseUsername->setPlaceholderText(
        localText(m_english, "要移除的账号（可留空）", "Account to remove (optional)"));
    eraseRow->addWidget(m_eraseUsername, 1);
    m_erase =
        new QPushButton(localText(m_english, "移除服务器凭据", "Erase host credential"), this);
    m_erase->setObjectName(QStringLiteral("credentialErase"));
    eraseRow->addWidget(m_erase);
    m_logout = new QPushButton(localText(m_english, "退出 GitHub 登录", "Log out of GitHub"), this);
    m_logout->setObjectName(QStringLiteral("credentialLogout"));
    eraseRow->addWidget(m_logout);
    layout->addLayout(eraseRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(localText(m_english, "关闭", "Close"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refreshAccounts, &QPushButton::clicked, this,
            [this] { runManager({"credential-manager", "github", "list"}); });
    connect(m_browserLogin, &QPushButton::clicked, this,
            [this] { runManager({"credential-manager", "github", "login", "--browser"}, 300000); });
    connect(m_storeToken, &QPushButton::clicked, this, &GitCredentialDialog::storeToken);
    connect(m_erase, &QPushButton::clicked, this, &GitCredentialDialog::eraseCredential);
    connect(m_logout, &QPushButton::clicked, this, [this] {
        const QString username = m_eraseUsername->text().trimmed();
        if (username.isEmpty()) {
            m_status->setText(localText(m_english, "请先填写要退出的 GitHub 账号。",
                                        "Enter the GitHub account to log out."));
            return;
        }
        runManager({"credential-manager", "github", "logout", username});
    });
    updateControls();
    runManager({"credential-manager", "github", "list"});
}

void GitCredentialDialog::runManager(const QStringList& args, int timeoutMs) {
    if (!m_git || m_working)
        return;
    m_working = true;
    m_status->setText(localText(m_english, "正在与 Git Credential Manager 通信…",
                                "Contacting Git Credential Manager…"));
    updateControls();
    const QPointer<GitCredentialDialog> guard(this);
    m_git->run(
        args,
        [guard, args](const GitResult& result) {
            if (!guard)
                return;
            guard->m_working = false;
            if (args.size() >= 3 && args.at(1) == QStringLiteral("github") &&
                args.at(2) == QStringLiteral("list")) {
                guard->m_accounts->setPlainText(
                    result.ok() ? QString::fromUtf8(result.output).trimmed()
                                : localText(guard->m_english,
                                            "账号列表不可用；请确认已安装 Git Credential Manager。",
                                            "Account list unavailable; verify Git Credential "
                                            "Manager is installed."));
            }
            if (result.ok())
                guard->m_status->setText(
                    localText(guard->m_english, "操作完成。", "Operation completed."));
            else
                guard->m_status->setText(localText(
                    guard->m_english,
                    "操作失败。请确认已安装 Git Credential Manager，并检查网络或凭据后重试。",
                    "Operation failed. Verify Git Credential Manager is installed, then check "
                    "network "
                    "or credentials and retry."));
            guard->updateControls();
        },
        timeoutMs);
}

void GitCredentialDialog::storeToken() {
    const QString host = m_host->text().trimmed();
    const QString username = m_username->text().trimmed();
    QString secret = m_token->text();
    static const QRegularExpression hostPattern(QStringLiteral("^[A-Za-z0-9.-]+(?::[0-9]+)?$"));
    if (!hostPattern.match(host).hasMatch() || username.isEmpty() || secret.isEmpty() ||
        username.contains('\n') || username.contains('\r') || secret.contains('\n') ||
        secret.contains('\r')) {
        secret.fill(QChar('\0'));
        m_status->setText(localText(m_english, "请填写有效服务器、用户名和访问令牌。",
                                    "Enter a valid host, username, and access token."));
        return;
    }
    QByteArray input = credentialInput(host, username, secret);
    secret.fill(QChar('\0'));
    m_token->clear();
    m_working = true;
    m_status->setText(localText(m_english, "正在安全保存凭据…", "Storing credential securely…"));
    updateControls();
    const QPointer<GitCredentialDialog> guard(this);
    m_git->runWithInput(
        {"credential-manager", "store"}, std::move(input),
        [guard](const GitResult& result) {
            if (!guard)
                return;
            guard->m_working = false;
            guard->m_status->setText(
                result.ok()
                    ? localText(guard->m_english, "凭据已交给 Git Credential Manager 保存。",
                                "Credential passed to Git Credential Manager.")
                    : localText(guard->m_english,
                                "保存失败。请确认 Git Credential Manager 的安全存储已配置。",
                                "Could not store credential. Check Git Credential Manager's "
                                "secure storage configuration."));
            guard->updateControls();
        },
        30000);
}

void GitCredentialDialog::eraseCredential() {
    const QString host = m_host->text().trimmed();
    const QString username = m_eraseUsername->text().trimmed();
    static const QRegularExpression hostPattern(QStringLiteral("^[A-Za-z0-9.-]+(?::[0-9]+)?$"));
    if (!hostPattern.match(host).hasMatch() || username.contains('\n') || username.contains('\r')) {
        m_status->setText(
            localText(m_english, "服务器或用户名无效。", "Invalid host or username."));
        return;
    }
    m_working = true;
    m_status->setText(localText(m_english, "正在移除凭据…", "Erasing credential…"));
    updateControls();
    const QPointer<GitCredentialDialog> guard(this);
    m_git->runWithInput(
        {"credential-manager", "erase"}, credentialInput(host, username),
        [guard](const GitResult& result) {
            if (!guard)
                return;
            guard->m_working = false;
            guard->m_status->setText(
                result.ok() ? localText(guard->m_english, "已请求移除该凭据。",
                                        "Credential removal requested.")
                            : localText(guard->m_english,
                                        "移除失败。请确认 Git Credential Manager 已安装。",
                                        "Could not erase credential. Verify Git Credential Manager "
                                        "is installed."));
            guard->updateControls();
        },
        30000);
}

void GitCredentialDialog::updateControls() {
    const bool idle = !m_working;
    m_browserLogin->setEnabled(idle);
    m_storeToken->setEnabled(idle);
    m_erase->setEnabled(idle);
    m_logout->setEnabled(idle);
}

} // namespace idea4git
