#pragma once

#include "gitoperations.h"
#include <QDialog>
#include <QVector>
#include <optional>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace idea4git {
class CodeEditor;

struct DiffHunk {
    int leftStart = 0;
    int leftCount = 0;
    int rightStart = 0;
    int rightCount = 0;
};

class FileDiffView final : public QWidget {
    Q_OBJECT
  public:
    FileDiffView(QString title, QString path, QString leftRevision, QByteArray leftBytes,
                 QString rightRevision, QByteArray rightBytes, QByteArray patch, bool english,
                 QWidget* parent = nullptr);

  private:
    void navigateDiff(int delta);
    void updateDiffNavigation();
    CodeEditor* m_before;
    CodeEditor* m_after;
    QPlainTextEdit* m_patch;
    QToolButton* m_previous;
    QToolButton* m_next;
    QLabel* m_diffPosition;
    QVector<DiffHunk> m_hunks;
    int m_hunkIndex = -1;
};

class GitDialog : public QDialog {
    Q_OBJECT
  public:
    GitDialog(GitOperations* operations, bool english, QWidget* parent = nullptr);
    bool working() const { return m_working; }
    void reject() override;

  protected:
    QString t(const char* chinese, const char* english) const;
    void setWorking(bool working);
    void notice(const QString& message, bool error = false);
    void showResult(const GitResult& result);
    virtual bool canLeave() { return true; }
    virtual void updateControls() {}
    void closeEvent(QCloseEvent* event) override;
    QPointer<GitOperations> m_operations;
    QWidget* m_content;
    QVBoxLayout* m_body;
    bool m_english;
    bool m_working = false;

  private:
    QPlainTextEdit* m_notice;
    QPushButton* m_cancel;
    QPushButton* m_close;
};

class CompareDialog final : public GitDialog {
    Q_OBJECT
  public:
    CompareDialog(GitOperations* operations, bool english, QWidget* parent = nullptr,
                  QString left = "HEAD~1", QString right = "HEAD");

  private:
    void compare();
    void selectFile(int row);
    void navigateDiff(int delta);
    void updateDiffNavigation();
    QComboBox* m_left;
    QComboBox* m_right;
    QListWidget* m_files;
    CodeEditor* m_before;
    CodeEditor* m_after;
    QPlainTextEdit* m_patch;
    QLabel* m_leftTitle;
    QLabel* m_rightTitle;
    QToolButton* m_previous;
    QToolButton* m_next;
    QLabel* m_diffPosition;
    Comparison m_comparison;
    QVector<DiffHunk> m_hunks;
    int m_hunkIndex = -1;
    int m_selection = 0;
};

class OperationDialog final : public GitDialog {
    Q_OBJECT
  public:
    OperationDialog(GitOperations* operations, OperationKind kind, bool english,
                    QWidget* parent = nullptr, QString revision = {});
  signals:
    void repositoryChanged();
    void conflictsRequested();

  private:
    void invalidate();
    void preview();
    void execute();
    void updateControls() override;
    OperationKind m_kind;
    QComboBox* m_revision;
    QComboBox* m_mode;
    QSpinBox* m_mainline;
    QCheckBox* m_rebaseMerges;
    QPlainTextEdit* m_details;
    QCheckBox* m_confirm;
    QLineEdit* m_hardConfirmation;
    QPushButton* m_execute;
    QPushButton* m_resolve;
    std::optional<OperationPlan> m_plan;
};

class ConflictDialog final : public GitDialog {
    Q_OBJECT
  public:
    ConflictDialog(GitOperations* operations, QString path, bool english,
                   QWidget* parent = nullptr);
  signals:
    void repositoryChanged();

  private:
    void load();
    void chooseStage(int stage);
    void save(std::function<void(bool)> complete = {});
    void stage();
    bool canLeave() override;
    void updateControls() override;
    QString m_path;
    std::optional<ConflictFile> m_file;
    std::array<CodeEditor*, 3> m_sources;
    std::array<QPushButton*, 3> m_use;
    CodeEditor* m_result;
    QCheckBox* m_delete;
    QPushButton* m_save;
    QPushButton* m_stage;
    QLabel* m_state;
    bool m_loading = false;
    bool m_dirty = false;
    bool m_bom = false;
    bool m_resolved = false;
};

class StashDialog final : public GitDialog {
    Q_OBJECT
  public:
    StashDialog(GitOperations* operations, bool english, QWidget* parent = nullptr);
  signals:
    void repositoryChanged();
    void conflictsRequested();

  private:
    void refresh();
    void preview();
    void push();
    void use(StashAction action);
    void updateControls() override;
    QTableWidget* m_list;
    QPlainTextEdit* m_patch;
    QLineEdit* m_message;
    QCheckBox* m_untracked;
    QCheckBox* m_keepIndex;
    QCheckBox* m_restoreIndex;
    QPushButton* m_apply;
    QPushButton* m_pop;
    QPushButton* m_drop;
    QPushButton* m_resolve;
    QList<StashEntry> m_entries;
};

class PushDialog final : public QDialog {
    Q_OBJECT
  public:
    PushDialog(GitClient* git, QString repositoryRoot, QString branch, QString tracking,
               bool english, QWidget* parent = nullptr);
    void setPushRunning(bool running);
    void pushFinished(bool success, const QString& message);

  signals:
    void pushRequested(const QString& remote, const QString& branch);
    void diffRequested(const QString& commit, const idea4git::CommitFileChange& file);

  private:
    void loadRemotes();
    void loadBranches();
    void loadOutgoingCommits();
    void loadCommitFiles(int row);
    void updateControls();
    QStringList repositoryArgs(const QStringList& args) const;
    GitClient* m_git;
    QString m_repositoryRoot;
    QString m_branch;
    QString m_tracking;
    bool m_english;
    QComboBox* m_remote;
    QComboBox* m_target;
    QTableWidget* m_commits;
    QListWidget* m_files;
    QLabel* m_status;
    QPushButton* m_push;
    QStringList m_commitHashes;
    int m_loadRequest = 0;
    int m_fileRequest = 0;
    bool m_loading = false;
    bool m_pushRunning = false;
};

class RemoteManagerDialog final : public QDialog {
    Q_OBJECT
  public:
    RemoteManagerDialog(GitClient* git, QString repositoryRoot, bool english,
                        QWidget* parent = nullptr);

  private:
    void refresh();
    void addRemote();
    void editRemote();
    void removeRemote();
    void updateControls();
    QStringList repositoryArgs(const QStringList& args) const;
    GitClient* m_git;
    QString m_repositoryRoot;
    bool m_english;
    QTableWidget* m_remotes;
    QLabel* m_status;
    QPushButton* m_add;
    QPushButton* m_edit;
    QPushButton* m_remove;
    bool m_working = false;
};

class GitCredentialDialog final : public QDialog {
    Q_OBJECT
  public:
    GitCredentialDialog(GitClient* git, bool english, QWidget* parent = nullptr);

  private:
    void runManager(const QStringList& args, int timeoutMs = 30000);
    void storeToken();
    void eraseCredential();
    void updateControls();
    GitClient* m_git;
    bool m_english;
    QLineEdit* m_host;
    QLineEdit* m_username;
    QLineEdit* m_token;
    QLineEdit* m_eraseUsername;
    QLabel* m_status;
    QPlainTextEdit* m_accounts;
    QPushButton* m_browserLogin;
    QPushButton* m_storeToken;
    QPushButton* m_erase;
    QPushButton* m_logout;
    bool m_working = false;
};

QString operationName(OperationKind kind, bool english);
QString pendingName(PendingOperation pending, bool english);

} // namespace idea4git
