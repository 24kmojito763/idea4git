#pragma once

#include <QMap>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <functional>

namespace idea4git {

struct GitResult {
    int exitCode = -1;
    QByteArray output;
    QByteArray error;
    bool cancelled = false;
    bool timedOut = false;
    bool outputLimitExceeded = false;
    bool ok() const { return exitCode == 0 && !cancelled && !timedOut && !outputLimitExceeded; }
};

class GitClient final : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const GitResult&)>;
    explicit GitClient(QString directory, QObject* parent = nullptr);
    ~GitClient() override;
    void run(const QStringList& arguments, Callback callback, int timeoutMs = 30000);
    void runWithEnvironment(const QStringList& arguments, const QMap<QString, QString>& environment,
                            Callback callback, int timeoutMs = 30000);
    void cancelAll();
    bool busy() const;
    QString directory() const { return m_directory; }

  signals:
    void activityChanged(bool busy, const QString& command);
    void commandCompleted(const QStringList& arguments, const idea4git::GitResult& result);

  private:
    struct Job {
        QStringList arguments;
        Callback callback;
        int timeoutMs;
        QMap<QString, QString> environment;
    };
    void startNext();
    void readOutput();
    void complete(int exitCode);

    QString m_directory;
    QString m_git;
    QProcess m_process;
    QTimer m_timeout;
    QQueue<Job> m_queue;
    Job m_current;
    GitResult m_result;
    bool m_running = false;
    bool m_destroying = false;
};

} // namespace idea4git
Q_DECLARE_METATYPE(idea4git::GitResult)
