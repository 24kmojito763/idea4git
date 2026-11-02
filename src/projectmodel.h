#pragma once

#include "gitstatus.h"
#include <QFileSystemModel>
#include <QHash>
#include <QSortFilterProxyModel>

namespace idea4git {

class ProjectModel final : public QFileSystemModel {
    Q_OBJECT
  public:
    explicit ProjectModel(QObject* parent = nullptr);
    QVariant data(const QModelIndex& index, int role) const override;
    void setGitStatus(const QString& repository, const QList<FileChange>& changes);

  private:
    QHash<QString, FileState> m_states;
    QStringList m_ignoredDirectories;
};

class ProjectFilter final : public QSortFilterProxyModel {
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

  protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
};

} // namespace idea4git
