#include "projectmodel.h"
#include "icons.h"
#include "paths.h"

#include <QDir>
#include <QFileInfo>

namespace idea4git {

ProjectModel::ProjectModel(QObject* parent) : QFileSystemModel(parent) {
    setReadOnly(true);
    setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
}

QVariant ProjectModel::data(const QModelIndex& index, int role) const {
    if (index.column() == 0 && role == Qt::ForegroundRole) {
        const auto path = filePath(index);
        if (m_states.contains(path))
            return stateColor(m_states.value(path));
        for (const auto& directory : m_ignoredDirectories) {
            if (path.startsWith(directory + '/'))
                return stateColor(FileState::Ignored);
        }
        return stateColor(FileState::Clean);
    }
    if (index.column() == 0 && role == Qt::DecorationRole)
        return icon(isDir(index) ? "folder" : "file",
                    isDir(index) ? QColor("#c8a46c") : QColor("#80a5d5"));
    return QFileSystemModel::data(index, role);
}

void ProjectModel::setGitStatus(const QString& repository, const QList<FileChange>& changes) {
    m_states.clear();
    m_ignoredDirectories.clear();
    const auto rank = [](FileState state) {
        switch (state) {
        case FileState::Conflict:
            return 9;
        case FileState::Deleted:
            return 8;
        case FileState::Modified:
            return 7;
        case FileState::Renamed:
            return 6;
        case FileState::Added:
            return 5;
        case FileState::Untracked:
            return 4;
        case FileState::Ignored:
            return 1;
        case FileState::Clean:
            return 0;
        }
        return 0;
    };
    for (const auto& change : changes) {
        QString absolute = repositoryFilePath(repository, change.path);
        m_states.insert(absolute, change.state());
        if (change.state() == FileState::Ignored && change.path.endsWith('/'))
            m_ignoredDirectories.append(absolute);
        while (absolute != repository) {
            const auto parent = QFileInfo(absolute).absolutePath();
            if (parent == absolute || !parent.startsWith(repository + '/'))
                break;
            absolute = parent;
            if (change.state() != FileState::Ignored &&
                rank(change.state()) > rank(m_states.value(absolute, FileState::Clean)))
                m_states.insert(absolute, change.state());
        }
    }
    // Only role data changed. A structural layout signal invalidates proxy indexes
    // and must not be emitted without the matching model-layout protocol.
    QList<QModelIndex> parents = {QModelIndex()};
    while (!parents.isEmpty()) {
        const auto parent = parents.takeLast();
        const int count = rowCount(parent);
        if (count == 0)
            continue;
        emit dataChanged(index(0, 0, parent), index(count - 1, 0, parent), {Qt::ForegroundRole});
        for (int row = 0; row < count; ++row) {
            const auto child = index(row, 0, parent);
            if (rowCount(child) > 0)
                parents.append(child);
        }
    }
}

bool ProjectFilter::filterAcceptsRow(int row, const QModelIndex& parent) const {
    const auto index = sourceModel()->index(row, 0, parent);
    const auto name = sourceModel()->data(index, Qt::DisplayRole).toString();
    return name != ".git" && name != ".codegraph";
}

} // namespace idea4git
