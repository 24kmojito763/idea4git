#pragma once

#include "gitclient.h"
#include "gitoperations.h"
#include "gitstatus.h"
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QToolButton;
class QToolBar;
class QMenu;
class QModelIndex;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace idea4git {
class CodeEditor;
class ProjectModel;
class ProjectFilter;
class PushDialog;

class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(bool english = false, QWidget* parent = nullptr);
    bool openProject(const QString& directory);
    void openFile(const QString& path);
    void refreshRepository();
    CodeEditor* currentEditor() const;
    bool gitBusy() const;
    QString projectRoot() const { return m_projectRoot; }

  protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    QString text(const char* chinese, const char* english) const;
    void buildMenus();
    void buildAdvancedActions(QMenu* gitMenu);
    void buildWorkflowBar();
    void inspectWorkflow();
    void updateAdvancedActions();
    void showComparison(QString left = {}, QString right = {});
    void showAdvancedOperation(OperationKind kind, QString revision = {});
    void showConflict(QString path = {});
    void showStashes();
    void showRemotes();
    void showCredentials();
    void showPushPreview();
    void pushToRemote(const QString& remote, const QString& branch);
    void sequenceControl(SequenceAction action);
    bool prepareWorktreeChange();
    void reloadUnmodifiedEditors();
    void buildWorkspace();
    void applyTheme();
    void setProjectPanelVisible(bool visible);
    void setCommitPanelVisible(bool visible);
    void setHistoryPanelVisible(bool visible);
    void syncChangeGroupCheckState(QTreeWidgetItem* group);
    void chooseProject();
    void chooseFile();
    void showSettings();
    void showBranches();
    void loadHistoryBranches();
    void loadHistory(bool append = false);
    void loadCommitFiles(int row);
    void showCommitFileDiff(const QString& commit, const CommitFileChange& file);
    void showChangeFileDiff(const FileChange& file);
    void showWorkingTreeDiff(CodeEditor* editor, const QString& path, const FileChange& file);
    void showBranchDiff(CodeEditor* editor, const QString& path, const QString& branchName,
                        const QString& branchRef);
    void showCommittedFileDiff(const QString& path);
    void openFileDiff(const QString& title, const QString& path, const QString& leftRevision,
                      const QByteArray& left, const QString& rightRevision, const QByteArray& right,
                      const QByteArray& patch);
    void renderChanges();
    void showDiff(QTreeWidgetItem* item);
    void showCommit(int row);
    void commitChanges(bool pushAfter = false);
    void remoteOperation(const QString& operation);
    void runOperation(const QStringList& args, std::function<void()> success = {});
    QStringList repositoryArgs(const QStringList& args) const;
    void reportError(const QString& title, const GitResult& result);
    bool saveEditor(CodeEditor* editor);
    bool closeEditor(int index);
    bool closeEditors();
    void persistEditorSession();
    bool restoreEditorSession();
    void persistProjectTreeState();
    void trackProjectTreeExpansion(const QModelIndex& index, bool expanded);
    void restorePendingProjectTreeExpansions();
    void updateActions();
    void updateEditorStatus();
    void findText(bool forward = true, bool fromStart = false);
    void showGlobalSearch();
    void selectProjectPath(const QString& path);

    bool m_english;
    QString m_projectRoot;
    QString m_repositoryRoot;
    QStringList m_expandedProjectTreePaths;
    QStringList m_pendingTreeExpansions;
    QSet<QString> m_loadedProjectTreeDirectories;
    QSet<QString> m_excludedCommitPaths;
    bool m_projectTreeStateKnown = false;
    bool m_commitSelectionInitialized = false;
    bool m_lastKnownMaximized = false;
    bool m_pendingNormalAfterMaximize = false;
    bool m_restoreMaximizedOnActivation = false;
    GitClient* m_git = nullptr;
    GitOperations* m_operations = nullptr;
    PendingOperation m_pendingOperation = PendingOperation::None;
    bool m_refreshPending = false;
    bool m_reloadRequested = false;
    QList<QAction*> m_advancedActions;
    QAction* m_compareAction = nullptr;
    QAction* m_resolveAction = nullptr;
    QAction* m_stashAction = nullptr;
    QAction* m_continueAction = nullptr;
    QAction* m_abortAction = nullptr;
    QAction* m_skipAction = nullptr;
    QToolBar* m_workflowBar = nullptr;
    QLabel* m_workflowLabel = nullptr;
    RepositoryStatus m_status;
    QList<Commit> m_commits;
    QList<CommitFileChange> m_commitFileChanges;
    QString m_selectedCommitHash;
    bool m_refreshing = false;
    bool m_hasHead = false;
    bool m_operationRunning = false;
    int m_diffRequest = 0;
    int m_historyOffset = 0;
    int m_historyRequest = 0;
    int m_historyBranchRequest = 0;
    int m_commitFilesRequest = 0;
    int m_generation = 0;
    int m_projectRevealRequest = 0;

    ProjectModel* m_projectModel = nullptr;
    ProjectFilter* m_projectFilter = nullptr;
    QWidget* m_projectPanel = nullptr;
    QWidget* m_commitPanel = nullptr;
    QTreeView* m_tree = nullptr;
    QTabWidget* m_editors = nullptr;
    QTabWidget* m_bottom = nullptr;
    QTreeWidget* m_changes = nullptr;
    QTableWidget* m_history = nullptr;
    QComboBox* m_historyBranch = nullptr;
    QComboBox* m_historyAuthor = nullptr;
    QListWidget* m_commitFiles = nullptr;
    QTimer* m_historyAuthorTimer = nullptr;
    QPlainTextEdit* m_diff = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QPlainTextEdit* m_message = nullptr;
    QCheckBox* m_amend = nullptr;
    QCheckBox* m_findCaseSensitive = nullptr;
    QToolButton* m_branch = nullptr;
    QToolButton* m_projectButton = nullptr;
    QToolButton* m_revealCurrentFileButton = nullptr;
    QToolButton* m_projectPanelRailButton = nullptr;
    QToolButton* m_commitPanelRailButton = nullptr;
    QToolButton* m_historyPanelRailButton = nullptr;
    QPushButton* m_commit = nullptr;
    QPushButton* m_commitAndPush = nullptr;
    QPushButton* m_loadMore = nullptr;
    QLabel* m_repoState = nullptr;
    QLabel* m_changeCount = nullptr;
    QLabel* m_breadcrumb = nullptr;
    QLabel* m_position = nullptr;
    QLabel* m_encoding = nullptr;
    QLabel* m_activity = nullptr;
    QLabel* m_findCount = nullptr;
    QWidget* m_findBar = nullptr;
    QLineEdit* m_find = nullptr;
    QLineEdit* m_treeFilter = nullptr;
    QComboBox* m_findMode = nullptr;
    QSplitter* m_horizontal = nullptr;
    QSplitter* m_vertical = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_refreshAction = nullptr;
    QAction* m_fetchAction = nullptr;
    QAction* m_pullAction = nullptr;
    QAction* m_pushAction = nullptr;
    QAction* m_cancelAction = nullptr;
    QAction* m_findAction = nullptr;
    QAction* m_globalFindAction = nullptr;
    QAction* m_projectPanelAction = nullptr;
    QAction* m_commitPanelAction = nullptr;
    QAction* m_historyPanelAction = nullptr;
    QDialog* m_globalSearchDialog = nullptr;
    QPointer<PushDialog> m_pushDialog;
};

} // namespace idea4git
