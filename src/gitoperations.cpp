#include "gitoperations.h"
#include "paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>

namespace idea4git {
namespace {
bool oid(const QString& value) {
    static const QRegularExpression pattern("^(?:[0-9a-f]{40}|[0-9a-f]{64})$");
    return pattern.match(value).hasMatch();
}

QString outputLine(QByteArray bytes) {
    if (bytes.endsWith('\n'))
        bytes.chop(1);
    return QString::fromUtf8(bytes);
}

PendingOperation pendingOperation(const QString& gitDirectory) {
    const QDir git(gitDirectory);
    if (QFileInfo(git.filePath("rebase-apply/applying")).isFile())
        return PendingOperation::Unknown; // git am is not a rebase; never abort it as one.
    if (QFileInfo(git.filePath("rebase-merge")).isDir() ||
        QFileInfo(git.filePath("rebase-apply")).isDir())
        return PendingOperation::Rebase;
    if (QFileInfo::exists(git.filePath("MERGE_HEAD")))
        return PendingOperation::Merge;
    if (QFileInfo::exists(git.filePath("CHERRY_PICK_HEAD")))
        return PendingOperation::CherryPick;
    if (QFileInfo::exists(git.filePath("REVERT_HEAD")))
        return PendingOperation::Revert;
    if (QFileInfo(git.filePath("sequencer")).isDir()) {
        QFile todo(git.filePath("sequencer/todo"));
        if (todo.open(QIODevice::ReadOnly)) {
            const auto first = todo.read(4096).trimmed();
            if (first.startsWith("pick "))
                return PendingOperation::CherryPick;
            if (first.startsWith("revert "))
                return PendingOperation::Revert;
        }
        return PendingOperation::Unknown;
    }
    return PendingOperation::None;
}

QString sequenceCommand(PendingOperation pending) {
    switch (pending) {
    case PendingOperation::Merge:
        return "merge";
    case PendingOperation::Rebase:
        return "rebase";
    case PendingOperation::CherryPick:
        return "cherry-pick";
    case PendingOperation::Revert:
        return "revert";
    default:
        return {};
    }
}

bool safeDiskPath(const QString& root, const QString& relative) {
    const auto path = repositoryFilePath(root, relative);
    if (!path.startsWith(root + '/') || QFileInfo(path).isSymLink())
        return false;
    auto parent = QFileInfo(path).absolutePath();
    while (!QFileInfo::exists(parent)) {
        const auto next = QFileInfo(parent).absolutePath();
        if (next == parent)
            return false;
        parent = next;
    }
    const auto canonical = QFileInfo(parent).canonicalFilePath();
    return canonical == root || canonical.startsWith(root + '/');
}

bool readDisk(const QString& path, QByteArray* bytes, bool* exists) {
    const QFileInfo info(path);
    *exists = info.exists();
    if (!*exists) {
        bytes->clear();
        return !info.isSymLink();
    }
    if (!info.isFile() || info.isSymLink() || info.size() > 5 * 1024 * 1024)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *bytes = file.read(5 * 1024 * 1024 + 1);
    return file.error() == QFile::NoError && bytes->size() <= 5 * 1024 * 1024;
}
} // namespace

bool GitSnapshot::trackedDirty() const {
    for (const auto& file : status.files)
        if (file.state() != FileState::Untracked && file.state() != FileState::Ignored)
            return true;
    return false;
}

bool GitSnapshot::conflicts() const {
    for (const auto& file : status.files)
        if (file.conflict())
            return true;
    return false;
}

GitOperations::GitOperations(GitClient* client, QString root, bool english, QObject* parent)
    : QObject(parent), m_english(english), m_client(client), m_root(std::move(root)) {}

GitResult GitOperations::failure(const char* chinese, const char* english) const {
    GitResult result;
    result.error = QByteArray(m_english ? english : chinese);
    return result;
}

void GitOperations::cancel() {
    if (m_client)
        m_client->cancelAll();
}

void GitOperations::run(const QStringList& args, GitClient::Callback callback, bool write) {
    if (!m_client) {
        callback(failure("Git 进程已关闭。", "Git client is closed."));
        return;
    }
    QStringList command = {"-C", m_root};
    command.append(args);
    const QPointer<GitOperations> guard(this);
    // Native Git uses the existing commit message when continuing a sequence;
    // suppress terminal editors for this job only, without changing Git config.
    m_client->runWithEnvironment(
        command, write ? QMap<QString, QString>{{"GIT_EDITOR", "true"}} : QMap<QString, QString>{},
        [guard, callback = std::move(callback)](const GitResult& result) {
            if (guard)
                callback(result);
        },
        write ? 120000 : 30000);
}

void GitOperations::snapshot(ValueCallback<GitSnapshot> callback) {
    run({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--branch",
         "--untracked-files=all"},
        [this, callback](const GitResult& status) {
            if (!status.ok()) {
                callback(status, {});
                return;
            }
            GitSnapshot value;
            value.status = parseStatus(status.output);
            if (!value.status.valid) {
                callback(failure("状态无效或含非 UTF-8 路径，禁止写入。",
                                 "Invalid status or non-UTF-8 paths; writes disabled."),
                         {});
                return;
            }
            run({"rev-parse", "--absolute-git-dir"},
                [this, callback, value](const GitResult& directory) mutable {
                    if (!directory.ok()) {
                        callback(directory, {});
                        return;
                    }
                    const auto path = outputLine(directory.output);
                    if (!QFileInfo(path).isDir()) {
                        callback(failure("无法识别 Git 元数据目录。",
                                         "Cannot identify Git metadata directory."),
                                 {});
                        return;
                    }
                    value.pending = pendingOperation(path);
                    if (value.status.unborn) {
                        callback(directory, value);
                        return;
                    }
                    run({"rev-parse", "--verify", "HEAD"},
                        [callback, value](const GitResult& head) mutable {
                            if (!head.ok()) {
                                callback(head, {});
                                return;
                            }
                            value.head = outputLine(head.output);
                            callback(head, value);
                        });
                });
        });
}

void GitOperations::revisions(ValueCallback<QStringList> callback) {
    run({"for-each-ref", "--count=1000", "--format=%(refname)", "refs/heads/", "refs/remotes/",
         "refs/tags/"},
        [callback](const GitResult& result) {
            QStringList values = {"HEAD", "HEAD~1"};
            values.append(QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts));
            callback(result, values);
        });
}

void GitOperations::resolve(const QString& revision, ValueCallback<QString> callback) {
    if (revision.isEmpty()) {
        callback(failure("请选择版本。", "Choose a revision."), {});
        return;
    }
    run({"rev-parse", "--verify", "--end-of-options", revision + "^{commit}"},
        [this, callback](const GitResult& result) {
            const auto hash = outputLine(result.output);
            if (!result.ok())
                callback(result, {});
            else if (!oid(hash))
                callback(
                    failure("版本没有解析为有效提交。", "Revision did not resolve to a commit."),
                    {});
            else
                callback(result, hash);
        });
}

void GitOperations::compare(const QString& left, const QString& right,
                            ValueCallback<Comparison> callback) {
    resolve(left, [this, right, callback](const GitResult& a, const QString& leftHash) {
        if (!a.ok()) {
            callback(a, {});
            return;
        }
        resolve(right, [this, leftHash, callback](const GitResult& b, const QString& rightHash) {
            if (!b.ok()) {
                callback(b, {});
                return;
            }
            run({"diff", "--no-ext-diff", "--no-textconv", "--name-status", "-z", "--find-renames",
                 leftHash, rightHash, "--"},
                [this, leftHash, rightHash, callback](const GitResult& diff) {
                    if (!diff.ok()) {
                        callback(diff, {});
                        return;
                    }
                    Comparison comparison{leftHash, rightHash, {}};
                    const auto fields = diff.output.split('\0');
                    for (int i = 0; i < fields.size() - 1;) {
                        const auto status = fields[i++];
                        const int paths = status.startsWith('R') || status.startsWith('C') ? 2 : 1;
                        if (i + paths > fields.size() - 1) {
                            callback(
                                failure("差异路径输出不完整。", "Incomplete diff path output."),
                                {});
                            return;
                        }
                        VersionFile file;
                        file.status = QString::fromLatin1(status);
                        const auto rawOld = fields[i++];
                        const auto rawNew = paths == 2 ? fields[i++] : rawOld;
                        QStringDecoder oldDecoder(QStringDecoder::Utf8,
                                                  QStringConverter::Flag::Stateless);
                        QStringDecoder newDecoder(QStringDecoder::Utf8,
                                                  QStringConverter::Flag::Stateless);
                        file.oldPath = oldDecoder.decode(rawOld);
                        file.newPath = newDecoder.decode(rawNew);
                        if (oldDecoder.hasError() || newDecoder.hasError()) {
                            callback(failure("暂不支持非 UTF-8 文件名。",
                                             "Non-UTF-8 filenames are not supported."),
                                     {});
                            return;
                        }
                        comparison.files.append(file);
                    }
                    callback(diff, comparison);
                });
        });
    });
}

void GitOperations::blob(const QString& commit, const QString& path, GitClient::Callback callback) {
    if (!oid(commit)) {
        callback(failure("无效提交哈希。", "Invalid commit hash."));
        return;
    }
    run({"cat-file", "blob", commit + ':' + path}, std::move(callback));
}

void GitOperations::patch(const QString& left, const QString& right, const VersionFile& file,
                          GitClient::Callback callback) {
    if (!oid(left) || !oid(right)) {
        callback(failure("无效提交哈希。", "Invalid commit hash."));
        return;
    }
    run({"diff", "--no-ext-diff", "--no-textconv", "--find-renames", "--unified=3", left, right,
         "--", file.oldPath, file.newPath},
        std::move(callback));
}

void GitOperations::plan(const OperationRequest& request, ValueCallback<OperationPlan> callback) {
    snapshot([this, request, callback](const GitResult& result, const GitSnapshot& before) {
        if (!result.ok()) {
            callback(result, {});
            return;
        }
        if (before.head.isEmpty() || before.pending != PendingOperation::None ||
            before.conflicts()) {
            callback(failure("请先完成当前操作并解决冲突；仓库还需至少一次提交。",
                             "Finish the current operation and resolve conflicts first; a first "
                             "commit is also required."),
                     {});
            return;
        }
        if (request.kind != OperationKind::Reset && before.trackedDirty()) {
            callback(failure("工作区或暂存区有更改。请先提交或 stash，不会自动隐藏更改。",
                             "Tracked changes exist. Commit or stash them first; changes are never "
                             "auto-stashed."),
                     {});
            return;
        }
        resolve(request.revision, [this, request, before, callback](const GitResult& resolved,
                                                                    const QString& hash) {
            if (!resolved.ok()) {
                callback(resolved, {});
                return;
            }
            run({"rev-list", "--parents", "-n", "1", hash}, [this, request, before, hash,
                                                             callback](const GitResult& parents) {
                if (!parents.ok()) {
                    callback(parents, {});
                    return;
                }
                const int count = parents.output.trimmed().split(' ').size() - 1;
                if ((request.kind == OperationKind::CherryPick ||
                     request.kind == OperationKind::Revert) &&
                    ((count > 1 && (request.mainline < 1 || request.mainline > count)) ||
                     (count <= 1 && request.mainline != 0))) {
                    callback(failure("合并提交必须选择有效主线父提交；普通提交请选择“不指定主线”。",
                                     "Merge commits require a valid mainline parent; ordinary "
                                     "commits require mainline 0."),
                             {});
                    return;
                }
                OperationPlan value{request, before, hash, {}};
                auto& args = value.arguments;
                switch (request.kind) {
                case OperationKind::Merge:
                    args = {"merge", "--no-edit", "--no-autostash"};
                    if (request.mergeMode == MergeMode::NoFastForward)
                        args.append("--no-ff");
                    if (request.mergeMode == MergeMode::FastForwardOnly)
                        args.append("--ff-only");
                    if (request.mergeMode == MergeMode::Squash)
                        args.append("--squash");
                    break;
                case OperationKind::Rebase:
                    args = {"rebase", "--no-autostash", "--no-update-refs"};
                    if (request.rebaseMerges)
                        args.append("--rebase-merges");
                    break;
                case OperationKind::CherryPick:
                    args = {"cherry-pick"};
                    break;
                case OperationKind::Revert:
                    args = {"revert", "--no-edit"};
                    break;
                case OperationKind::Reset:
                    args = {"reset", request.resetMode == ResetMode::Soft   ? "--soft"
                                     : request.resetMode == ResetMode::Hard ? "--hard"
                                                                            : "--mixed"};
                    break;
                }
                if ((request.kind == OperationKind::CherryPick ||
                     request.kind == OperationKind::Revert) &&
                    request.mainline)
                    args.append({"--mainline", QString::number(request.mainline)});
                args.append(hash);
                callback(parents, value);
            });
        });
    });
}

void GitOperations::execute(const OperationPlan& plan, GitClient::Callback callback) {
    if (m_mutating) {
        callback(failure("已有写操作正在运行。", "A mutation is already running."));
        return;
    }
    m_mutating = true;
    snapshot([this, plan, callback](const GitResult& result, const GitSnapshot& now) {
        const auto finish = [this, callback](const GitResult& value) {
            m_mutating = false;
            callback(value);
        };
        if (!result.ok()) {
            finish(result);
            return;
        }
        if (now.head != plan.before.head || now.status.branch != plan.before.status.branch ||
            now.pending != PendingOperation::None || now.conflicts() ||
            (plan.request.kind != OperationKind::Reset && now.trackedDirty())) {
            finish(failure("仓库状态已改变，请重新预览并确认。",
                           "Repository state changed; preview and confirm again."));
            return;
        }
        // Rebuild the validated command from the typed request, never trust a UI command string.
        auto request = plan.request;
        request.revision = plan.target;
        this->plan(request, [this, plan, finish](const GitResult& checked,
                                                 const OperationPlan& validated) {
            if (!checked.ok()) {
                finish(checked);
                return;
            }
            if (validated.before.head != plan.before.head ||
                validated.before.status.branch != plan.before.status.branch) {
                finish(failure("HEAD 已改变，请重新确认。", "HEAD changed; confirm again."));
                return;
            }
            run(validated.arguments, finish, true);
        });
    });
}

void GitOperations::sequence(PendingOperation expected, SequenceAction action,
                             GitClient::Callback callback) {
    if (m_mutating) {
        callback(failure("已有写操作正在运行。", "A mutation is already running."));
        return;
    }
    m_mutating = true;
    snapshot([this, expected, action, callback](const GitResult& result, const GitSnapshot& now) {
        const auto finish = [this, callback](const GitResult& value) {
            m_mutating = false;
            callback(value);
        };
        if (!result.ok()) {
            finish(result);
            return;
        }
        const auto command = sequenceCommand(now.pending);
        if (now.pending != expected || command.isEmpty() ||
            (action == SequenceAction::Skip && expected == PendingOperation::Merge)) {
            finish(
                failure("进行中的操作已改变，或不支持此操作。请刷新。",
                        "The pending operation changed or does not support this action. Refresh."));
            return;
        }
        if (action == SequenceAction::Continue && now.conflicts()) {
            finish(failure("仍有未解决冲突，请保存并暂存所有解决结果。",
                           "Unresolved conflicts remain; save and stage all resolutions."));
            return;
        }
        run({command, action == SequenceAction::Abort  ? "--abort"
                      : action == SequenceAction::Skip ? "--skip"
                                                       : "--continue"},
            finish, true);
    });
}

void GitOperations::conflict(const QString& path, ValueCallback<ConflictFile> callback) {
    run({"ls-files", "--unmerged", "-z", "--", path}, [this, path,
                                                       callback](const GitResult& result) {
        if (!result.ok()) {
            callback(result, {});
            return;
        }
        if (result.output.isEmpty()) {
            callback(failure("文件已不再处于冲突状态，请刷新。",
                             "File is no longer conflicted; refresh."),
                     {});
            return;
        }
        ConflictFile file;
        file.path = path;
        file.indexSnapshot = result.output;
        for (const auto& record : result.output.split('\0')) {
            if (record.isEmpty())
                continue;
            const int tab = record.indexOf('\t');
            const auto fields = record.left(tab).split(' ');
            if (tab < 0 || fields.size() != 3 || QString::fromUtf8(record.mid(tab + 1)) != path) {
                callback(failure("无法解析冲突阶段。", "Cannot parse conflict index stages."), {});
                return;
            }
            const int stage = fields[2].toInt();
            if (stage < 1 || stage > 3 || !oid(QString::fromLatin1(fields[1]))) {
                callback(failure("无效冲突阶段。", "Invalid conflict stage."), {});
                return;
            }
            file.stages[stage - 1] = {
                true, QString::fromLatin1(fields[1]), QString::fromLatin1(fields[0]), {}};
        }
        file.editable = safeDiskPath(m_root, path) && readDisk(repositoryFilePath(m_root, path),
                                                               &file.diskBytes, &file.diskExists);
        readStages(file, 0, callback);
    });
}

void GitOperations::readStages(ConflictFile file, int stage, ValueCallback<ConflictFile> callback) {
    if (stage == 3) {
        for (const auto& item : file.stages)
            if (item.present &&
                ((item.mode != "100644" && item.mode != "100755") || !isText(item.bytes)))
                file.editable = false;
        if (!isText(file.diskBytes))
            file.editable = false;
        GitResult success;
        success.exitCode = 0;
        callback(success, file);
        return;
    }
    if (!file.stages[stage].present) {
        readStages(file, stage + 1, callback);
        return;
    }
    // A gitlink stage names a commit, not a blob. Keep it visible but read-only.
    if (file.stages[stage].mode == "160000") {
        file.editable = false;
        file.stages[stage].bytes = file.stages[stage].hash.toUtf8();
        readStages(file, stage + 1, callback);
        return;
    }
    run({"cat-file", "blob", file.stages[stage].hash},
        [this, file, stage, callback](const GitResult& result) mutable {
            if (!result.ok()) {
                callback(result, {});
                return;
            }
            file.stages[stage].bytes = result.output;
            readStages(file, stage + 1, callback);
        });
}

void GitOperations::verifyConflict(const ConflictFile& expected, GitClient::Callback callback) {
    run({"ls-files", "--unmerged", "-z", "--", expected.path},
        [this, expected, callback](const GitResult& result) {
            if (!result.ok()) {
                callback(result);
                return;
            }
            if (result.output.isEmpty() || result.output != expected.indexSnapshot ||
                !expected.editable || !safeDiskPath(m_root, expected.path)) {
                callback(failure(
                    "冲突索引已改变或文件类型不支持，请重新打开冲突解决器。",
                    "Conflict index changed or file type is unsupported; reopen the resolver."));
                return;
            }
            QByteArray bytes;
            bool exists = false;
            if (!readDisk(repositoryFilePath(m_root, expected.path), &bytes, &exists) ||
                exists != expected.diskExists || bytes != expected.diskBytes) {
                callback(failure("文件已在外部改变，请重新加载，不会覆盖。",
                                 "File changed externally; reload instead of overwriting."));
                return;
            }
            callback(result);
        });
}

void GitOperations::saveResolution(const ConflictFile& expected, const QByteArray& bytes,
                                   bool remove, ValueCallback<ConflictFile> callback) {
    if (m_mutating || (!remove && !isText(bytes))) {
        callback(failure("写操作正忙，或结果不是受支持的 UTF-8 文本。",
                         "A mutation is busy, or the result is not supported UTF-8 text."),
                 {});
        return;
    }
    m_mutating = true;
    verifyConflict(expected, [this, expected, bytes, remove,
                              callback](const GitResult& checked) mutable {
        m_mutating = false;
        if (!checked.ok()) {
            callback(checked, {});
            return;
        }
        const auto path = repositoryFilePath(m_root, expected.path);
        auto updated = expected;
        if (remove) {
            if (expected.diskExists && !QFile::remove(path)) {
                callback(failure("无法删除结果文件。", "Cannot delete result file."), {});
                return;
            }
            updated.diskExists = false;
            updated.diskBytes.clear();
        } else {
            if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
                !safeDiskPath(m_root, expected.path)) {
                callback(failure("结果路径不可安全写入。", "Result path cannot be written safely."),
                         {});
                return;
            }
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
                !file.commit()) {
                auto error = checked;
                error.exitCode = -1;
                error.error = file.errorString().toUtf8();
                callback(error, {});
                return;
            }
            if (!expected.diskExists) {
                const auto& mode =
                    expected.stages[1].present ? expected.stages[1].mode : expected.stages[2].mode;
                if (mode == "100755")
                    QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner |
                                                    QFile::ExeGroup | QFile::ExeOther);
            }
            updated.diskExists = true;
            updated.diskBytes = bytes;
        }
        callback(checked, updated);
    });
}

void GitOperations::markResolved(const ConflictFile& expected, GitClient::Callback callback) {
    if (m_mutating || (expected.diskExists && hasConflictMarkers(expected.diskBytes))) {
        callback(failure(
            "仍有冲突标记，或已有写操作。请先编辑并保存结果。",
            "Conflict markers remain or a mutation is busy. Edit and save the result first."));
        return;
    }
    m_mutating = true;
    verifyConflict(expected, [this, expected, callback](const GitResult& checked) {
        const auto finish = [this, callback](const GitResult& value) {
            m_mutating = false;
            callback(value);
        };
        if (!checked.ok()) {
            finish(checked);
            return;
        }
        run({"add", "-A", "--", expected.path}, finish, true);
    });
}

void GitOperations::stashes(ValueCallback<QList<StashEntry>> callback) {
    run({"stash", "list", "-z", "--format=%gd%x00%H%x00%gs"},
        [this, callback](const GitResult& result) {
            QList<StashEntry> entries;
            if (!result.ok()) {
                callback(result, entries);
                return;
            }
            const auto fields = result.output.split('\0');
            if ((fields.size() - 1) % 3 != 0) {
                callback(failure("stash 列表输出不完整。", "Incomplete stash list output."), {});
                return;
            }
            for (int i = 0; i + 2 < fields.size(); i += 3) {
                if (!oid(QString::fromLatin1(fields[i + 1]))) {
                    callback(failure("stash 哈希无效。", "Invalid stash hash."), {});
                    return;
                }
                entries.append({QString::fromUtf8(fields[i]), QString::fromLatin1(fields[i + 1]),
                                QString::fromUtf8(fields[i + 2])});
            }
            callback(result, entries);
        });
}

void GitOperations::pushStash(const QString& message, bool untracked, bool keepIndex,
                              GitClient::Callback callback) {
    if (m_mutating) {
        callback(failure("已有写操作正在运行。", "A mutation is already running."));
        return;
    }
    m_mutating = true;
    snapshot([this, message, untracked, keepIndex, callback](const GitResult& result,
                                                             const GitSnapshot& now) {
        const auto finish = [this, callback](const GitResult& value) {
            m_mutating = false;
            callback(value);
        };
        if (!result.ok()) {
            finish(result);
            return;
        }
        if (now.pending != PendingOperation::None || now.conflicts() || now.head.isEmpty()) {
            finish(failure(
                "请先完成当前操作；stash 需要已有提交且没有冲突。",
                "Finish the current operation first; stash requires a commit and no conflicts."));
            return;
        }
        QStringList args = {"stash", "push"};
        if (untracked)
            args.append("--include-untracked");
        if (keepIndex)
            args.append("--keep-index");
        args.append({"-m", message.isEmpty() ? "idea4git stash" : message, "--"});
        run(args, finish, true);
    });
}

void GitOperations::locateStash(const QString& hash, ValueCallback<StashEntry> callback) {
    if (!oid(hash)) {
        callback(failure("无效 stash 哈希。", "Invalid stash hash."), {});
        return;
    }
    stashes([this, hash, callback](const GitResult& result, const QList<StashEntry>& entries) {
        if (!result.ok()) {
            callback(result, {});
            return;
        }
        QList<StashEntry> matches;
        for (const auto& entry : entries)
            if (entry.hash == hash)
                matches.append(entry);
        if (matches.size() != 1) {
            callback(
                failure("stash 已删除或存在重复引用，请刷新后重新选择。",
                        "Stash is missing or has duplicate references; refresh and select again."),
                {});
            return;
        }
        callback(result, matches.first());
    });
}

void GitOperations::useStash(StashAction action, const QString& hash, bool restoreIndex,
                             GitClient::Callback callback) {
    if (m_mutating) {
        callback(failure("已有写操作正在运行。", "A mutation is already running."));
        return;
    }
    m_mutating = true;
    snapshot([this, action, hash, restoreIndex, callback](const GitResult& result,
                                                          const GitSnapshot& now) {
        const auto finish = [this, callback](const GitResult& value) {
            m_mutating = false;
            callback(value);
        };
        if (!result.ok()) {
            finish(result);
            return;
        }
        if (now.pending != PendingOperation::None || now.conflicts()) {
            finish(failure("请先完成进行中的操作并解决冲突。",
                           "Finish the pending operation and resolve conflicts first."));
            return;
        }
        locateStash(hash, [this, action, hash, restoreIndex, finish](const GitResult& located,
                                                                     const StashEntry& entry) {
            if (!located.ok()) {
                finish(located);
                return;
            }
            if (action == StashAction::Drop) {
                run({"stash", "drop", entry.reference}, finish, true);
                return;
            }
            QStringList args = {"stash", "apply"};
            if (restoreIndex)
                args.append("--index");
            args.append(hash);
            run(
                args,
                [this, action, hash, finish](const GitResult& applied) {
                    if (!applied.ok() || action == StashAction::Apply) {
                        finish(applied);
                        return;
                    }
                    // Pop by immutable object first; only remove the still-identifiable
                    // entry after a successful apply. Conflicts always retain the stash.
                    locateStash(hash, [this, applied, finish](const GitResult& found,
                                                              const StashEntry& current) {
                        if (!found.ok()) {
                            finish(found);
                            return;
                        }
                        run(
                            {"stash", "drop", current.reference},
                            [applied, finish](const GitResult& dropped) mutable {
                                auto result = dropped;
                                result.output.prepend(applied.output);
                                finish(result);
                            },
                            true);
                    });
                },
                true);
        });
    });
}

void GitOperations::stashPatch(const QString& hash, GitClient::Callback callback) {
    if (!oid(hash)) {
        callback(failure("无效 stash 哈希。", "Invalid stash hash."));
        return;
    }
    run({"stash", "show", "--include-untracked", "--patch", "--no-ext-diff", "--no-textconv", hash},
        std::move(callback));
}

bool GitOperations::isText(const QByteArray& bytes) {
    if (bytes.size() > 5 * 1024 * 1024 || bytes.contains('\0'))
        return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString ignored = decoder.decode(bytes);
    return !decoder.hasError();
}

bool GitOperations::hasConflictMarkers(const QByteArray& bytes) {
    static const QRegularExpression marker("(?m)^(?:<{7,} |>{7,} |\\|{7,} |={7,}\\r?$)");
    return marker.match(QString::fromUtf8(bytes)).hasMatch();
}

} // namespace idea4git
