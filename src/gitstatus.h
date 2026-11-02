#pragma once

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QString>

namespace idea4git {

enum class FileState { Clean, Untracked, Modified, Added, Deleted, Renamed, Conflict, Ignored };

struct FileChange {
    QString path;
    QString originalPath;
    char index = ' ';
    char worktree = ' ';

    bool staged() const;
    bool unstaged() const;
    bool conflict() const;
    FileState state() const;
};

struct RepositoryStatus {
    QString branch;
    QString tracking;
    QList<FileChange> files;
    bool valid = true;
    bool unborn = false;
};

struct Commit {
    QString hash;
    QString shortHash;
    QString author;
    QString date;
    QString refs;
    QString subject;
};

struct CommitFileChange {
    QString status;
    QString path;
    QString originalPath;
};

RepositoryStatus parseStatus(const QByteArray& output);
QList<Commit> parseLog(const QByteArray& output);
QList<CommitFileChange> parseCommitFiles(const QByteArray& output, bool* valid = nullptr);
QColor stateColor(FileState state);
QString stateCode(FileState state);

} // namespace idea4git
