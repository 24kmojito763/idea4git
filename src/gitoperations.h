#pragma once

#include "gitclient.h"
#include "gitstatus.h"
#include <QPointer>
#include <array>

namespace idea4git {

enum class OperationKind { Merge, Rebase, CherryPick, Revert, Reset };
enum class PendingOperation { None, Merge, Rebase, CherryPick, Revert, Unknown };
enum class MergeMode { Default, NoFastForward, FastForwardOnly, Squash };
enum class ResetMode { Soft, Mixed, Hard };
enum class SequenceAction { Continue, Abort, Skip };
enum class StashAction { Apply, Pop, Drop };

struct GitSnapshot {
    RepositoryStatus status;
    QString head;
    PendingOperation pending = PendingOperation::None;
    bool trackedDirty() const;
    bool conflicts() const;
};

struct OperationRequest {
    OperationKind kind = OperationKind::Merge;
    QString revision;
    MergeMode mergeMode = MergeMode::Default;
    ResetMode resetMode = ResetMode::Mixed;
    int mainline = 0;
    bool rebaseMerges = false;
};

struct OperationPlan {
    OperationRequest request;
    GitSnapshot before;
    QString target;
    QStringList arguments;
};

struct VersionFile {
    QString status;
    QString oldPath;
    QString newPath;
};

struct Comparison {
    QString left;
    QString right;
    QList<VersionFile> files;
};

struct ConflictStage {
    bool present = false;
    QString hash;
    QString mode;
    QByteArray bytes;
};

struct ConflictFile {
    QString path;
    std::array<ConflictStage, 3> stages; // base, ours, theirs: Git index stages 1/2/3.
    QByteArray indexSnapshot;
    QByteArray diskBytes;
    bool diskExists = false;
    bool editable = false;
};

struct StashEntry {
    QString reference;
    QString hash;
    QString message;
};

class GitOperations final : public QObject {
    Q_OBJECT
  public:
    template <typename T> using ValueCallback = std::function<void(const GitResult&, const T&)>;
    explicit GitOperations(GitClient* client, QString root, bool english = false,
                           QObject* parent = nullptr);
    QString root() const { return m_root; }
    bool busy() const { return m_mutating || (m_client && m_client->busy()); }
    void cancel();
    void snapshot(ValueCallback<GitSnapshot> callback);
    void revisions(ValueCallback<QStringList> callback);
    void resolve(const QString& revision, ValueCallback<QString> callback);
    void compare(const QString& left, const QString& right, ValueCallback<Comparison> callback);
    void blob(const QString& commit, const QString& path, GitClient::Callback callback);
    void patch(const QString& left, const QString& right, const VersionFile& file,
               GitClient::Callback callback);
    void plan(const OperationRequest& request, ValueCallback<OperationPlan> callback);
    void execute(const OperationPlan& plan, GitClient::Callback callback);
    void sequence(PendingOperation expected, SequenceAction action, GitClient::Callback callback);
    void conflict(const QString& path, ValueCallback<ConflictFile> callback);
    void saveResolution(const ConflictFile& expected, const QByteArray& bytes, bool remove,
                        ValueCallback<ConflictFile> callback);
    void markResolved(const ConflictFile& expected, GitClient::Callback callback);
    void stashes(ValueCallback<QList<StashEntry>> callback);
    void pushStash(const QString& message, bool untracked, bool keepIndex,
                   GitClient::Callback callback);
    void useStash(StashAction action, const QString& hash, bool restoreIndex,
                  GitClient::Callback callback);
    void stashPatch(const QString& hash, GitClient::Callback callback);
    static bool isText(const QByteArray& bytes);
    static bool hasConflictMarkers(const QByteArray& bytes);

  private:
    void run(const QStringList& args, GitClient::Callback callback, bool write = false);
    GitResult failure(const char* chinese, const char* english) const;
    void readStages(ConflictFile file, int stage, ValueCallback<ConflictFile> callback);
    void verifyConflict(const ConflictFile& expected, GitClient::Callback callback);
    void locateStash(const QString& hash, ValueCallback<StashEntry> callback);
    bool m_english;
    bool m_mutating = false;
    QPointer<GitClient> m_client;
    QString m_root;
};

} // namespace idea4git
