#include "gitstatus.h"
#include <QStringDecoder>

namespace idea4git {

bool FileChange::conflict() const {
    const QByteArray xy = QByteArray(1, index) + worktree;
    return xy == "DD" || xy == "AU" || xy == "UD" || xy == "UA" || xy == "DU" || xy == "AA" ||
           xy == "UU";
}

bool FileChange::staged() const {
    return !conflict() && index != ' ' && index != '?' && index != '!';
}

bool FileChange::unstaged() const { return index != '!' && (worktree != ' ' || index == '?'); }

FileState FileChange::state() const {
    if (index == ' ' && worktree == ' ')
        return FileState::Clean;
    if (conflict())
        return FileState::Conflict;
    if (index == '!')
        return FileState::Ignored;
    if (index == '?')
        return FileState::Untracked;
    if (index == 'D' || worktree == 'D')
        return FileState::Deleted;
    if (index == 'R' || worktree == 'R' || index == 'C')
        return FileState::Renamed;
    if (index == 'A')
        return FileState::Added;
    return FileState::Modified;
}

RepositoryStatus parseStatus(const QByteArray& output) {
    RepositoryStatus status;
    if (!output.isEmpty() && !output.endsWith('\0'))
        status.valid = false;
    const auto records = output.split('\0');
    for (int i = 0; i < records.size(); ++i) {
        const QByteArray& record = records[i];
        if (record.isEmpty())
            continue;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString decoded = decoder.decode(record);
        if (decoder.hasError()) {
            status.valid = false;
            continue;
        }
        if (record.startsWith("## ")) {
            QString header = decoded.mid(3);
            for (const auto& prefix :
                 {QStringLiteral("No commits yet on "), QStringLiteral("Initial commit on ")}) {
                if (header.startsWith(prefix)) {
                    status.unborn = true;
                    header.remove(0, prefix.size());
                }
            }
            const auto position = header.indexOf("...");
            status.branch = position < 0 ? header : header.left(position);
            status.tracking = position < 0 ? QString() : header.mid(position + 3);
            continue;
        }
        if (record.size() < 4 || record[2] != ' ') {
            status.valid = false;
            continue;
        }
        FileChange change;
        change.index = record[0];
        change.worktree = record[1];
        change.path = decoded.mid(3);
        if (change.index == 'R' || change.index == 'C' || change.worktree == 'R' ||
            change.worktree == 'C') {
            if (++i >= records.size() || records[i].isEmpty()) {
                status.valid = false;
                break;
            }
            // With -z, the destination comes first; the source is a separate NUL record.
            QStringDecoder originalDecoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            change.originalPath = originalDecoder.decode(records[i]);
            if (originalDecoder.hasError())
                status.valid = false;
        }
        status.files.append(change);
    }
    if (!status.valid)
        status.files.clear();
    return status;
}

QList<Commit> parseLog(const QByteArray& output) {
    QList<Commit> commits;
    const auto fields = output.split('\0');
    for (int i = 0; i + 5 < fields.size(); i += 6) {
        if (fields[i].isEmpty())
            continue;
        commits.append({QString::fromUtf8(fields[i]), QString::fromUtf8(fields[i + 1]),
                        QString::fromUtf8(fields[i + 2]), QString::fromUtf8(fields[i + 3]),
                        QString::fromUtf8(fields[i + 4]), QString::fromUtf8(fields[i + 5])});
    }
    return commits;
}

QList<CommitFileChange> parseCommitFiles(const QByteArray& output, bool* valid) {
    bool isValid = output.isEmpty() || output.endsWith('\0');
    QList<CommitFileChange> changes;
    const auto fields = output.split('\0');
    for (int i = 0; isValid && i + 1 < fields.size();) {
        const auto status = fields[i++];
        if (status.isEmpty())
            continue;
        const bool renamed = status.startsWith('R') || status.startsWith('C');
        if (i >= fields.size() || fields[i].isEmpty()) {
            isValid = false;
            break;
        }
        const auto firstPath = fields[i++];
        const auto secondPath =
            renamed ? (i < fields.size() ? fields[i++] : QByteArray()) : firstPath;
        if (secondPath.isEmpty()) {
            isValid = false;
            break;
        }

        QStringDecoder firstDecoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        QStringDecoder secondDecoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString originalPath = firstDecoder.decode(firstPath);
        const QString path = secondDecoder.decode(secondPath);
        if (firstDecoder.hasError() || secondDecoder.hasError() ||
            originalPath.toUtf8() != firstPath || path.toUtf8() != secondPath) {
            isValid = false;
            break;
        }
        changes.append({QString::fromLatin1(status), path, renamed ? originalPath : QString()});
    }
    if (!isValid)
        changes.clear();
    if (valid)
        *valid = isValid;
    return changes;
}

QColor stateColor(FileState state) {
    // Darcula-style VCS colors. Staging is orthogonal and shown as an index badge.
    switch (state) {
    case FileState::Clean:
        return QColor("#bcbec4");
    case FileState::Untracked:
        return QColor("#d1675a");
    case FileState::Modified:
        return QColor("#6897bb");
    case FileState::Added:
        return QColor("#629755");
    case FileState::Deleted:
        return QColor("#6c6c6c");
    case FileState::Renamed:
        return QColor("#32a2a8");
    case FileState::Conflict:
        return QColor("#f15858");
    case FileState::Ignored:
        return QColor("#848504");
    }
    return QColor("#bcbec4");
}

QString stateCode(FileState state) {
    switch (state) {
    case FileState::Clean:
        return QStringLiteral(" ");
    case FileState::Untracked:
        return QStringLiteral("?");
    case FileState::Modified:
        return QStringLiteral("M");
    case FileState::Added:
        return QStringLiteral("A");
    case FileState::Deleted:
        return QStringLiteral("D");
    case FileState::Renamed:
        return QStringLiteral("R");
    case FileState::Conflict:
        return QStringLiteral("!");
    case FileState::Ignored:
        return QStringLiteral("I");
    }
    return {};
}

} // namespace idea4git
