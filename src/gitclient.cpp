#include "gitclient.h"

#include <QProcessEnvironment>
#include <QStandardPaths>

namespace idea4git {

GitClient::GitClient(QString directory, QObject* parent)
    : QObject(parent), m_directory(std::move(directory)),
      m_git(QStandardPaths::findExecutable(QStringLiteral("git"))) {
    m_process.setWorkingDirectory(m_directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    m_process.setProcessEnvironment(environment);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_result.timedOut = true;
        m_process.kill();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &GitClient::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &GitClient::readOutput);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                complete(status == QProcess::NormalExit ? code : -1);
            });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_result.error = m_process.errorString().toUtf8();
            complete(-1);
        }
    });
}

GitClient::~GitClient() {
    m_destroying = true;
    m_queue.clear();
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

bool GitClient::busy() const { return m_running || !m_queue.isEmpty(); }

void GitClient::run(const QStringList& arguments, Callback callback, int timeoutMs) {
    runWithEnvironment(arguments, {}, std::move(callback), timeoutMs);
}

void GitClient::runWithEnvironment(const QStringList& arguments,
                                   const QMap<QString, QString>& environment, Callback callback,
                                   int timeoutMs) {
    m_queue.enqueue({arguments, std::move(callback), timeoutMs, environment});
    if (!m_running)
        QTimer::singleShot(0, this, &GitClient::startNext);
}

void GitClient::startNext() {
    if (m_running || m_queue.isEmpty() || m_destroying)
        return;
    m_current = m_queue.dequeue();
    m_result = {};
    m_running = true;
    emit activityChanged(true, m_current.arguments.join(' '));
    if (m_git.isEmpty()) {
        m_result.error = "Git executable not found in PATH.";
        complete(-1);
        return;
    }
    QStringList args = {QStringLiteral("--no-pager"), QStringLiteral("-c"),
                        QStringLiteral("color.ui=false"), QStringLiteral("-c"),
                        QStringLiteral("core.quotepath=false")};
    args.append(m_current.arguments);
    auto environment = QProcessEnvironment::systemEnvironment();
    // Restrict literal pathspecs to explicit file selections. Applying this to
    // compound commands (notably stash -u) breaks Git's own internal :/ pathspec.
    QString command;
    for (int i = 0; i < m_current.arguments.size(); ++i) {
        const auto argument = m_current.arguments[i];
        if (argument == "-C" || argument == "-c") {
            ++i;
            continue;
        }
        if (argument.startsWith('-'))
            continue;
        command = argument;
        break;
    }
    const int separator = m_current.arguments.indexOf("--");
    const bool literal =
        QStringList{"add", "rm", "restore", "diff", "diff-tree", "log", "ls-files", "ls-tree"}
            .contains(command) &&
        separator >= 0 && separator + 1 < m_current.arguments.size();
    environment.remove("GIT_LITERAL_PATHSPECS");
    if (literal) {
        environment.remove("GIT_GLOB_PATHSPECS");
        environment.remove("GIT_NOGLOB_PATHSPECS");
        environment.remove("GIT_ICASE_PATHSPECS");
        environment.insert("GIT_LITERAL_PATHSPECS", "1");
    }
    environment.insert("GIT_TERMINAL_PROMPT", "0");
    for (auto i = m_current.environment.cbegin(); i != m_current.environment.cend(); ++i)
        environment.insert(i.key(), i.value());
    m_process.setProcessEnvironment(environment);
    m_process.start(m_git, args);
    m_timeout.start(m_current.timeoutMs);
}

void GitClient::readOutput() {
    m_result.output += m_process.readAllStandardOutput();
    m_result.error += m_process.readAllStandardError();
    constexpr qsizetype limit = 16 * 1024 * 1024;
    if (m_result.output.size() + m_result.error.size() > limit) {
        m_result.output = m_result.output.left(limit / 2);
        m_result.error = m_result.error.left(limit / 2);
        m_result.outputLimitExceeded = true;
        m_process.kill();
    }
}

void GitClient::complete(int exitCode) {
    if (!m_running || m_destroying)
        return;
    m_timeout.stop();
    readOutput();
    m_result.exitCode = exitCode;
    auto callback = std::move(m_current.callback);
    const auto args = m_current.arguments;
    const auto result = m_result;
    m_running = false;
    emit commandCompleted(args, result);
    if (callback)
        callback(result);
    if (m_queue.isEmpty())
        emit activityChanged(false, {});
    else
        QTimer::singleShot(0, this, &GitClient::startNext);
}

void GitClient::cancelAll() {
    // Queued mutations must never run after the user presses Cancel.
    const auto dropped = std::move(m_queue);
    m_queue.clear();
    GitResult cancelled;
    cancelled.cancelled = true;
    for (const auto& job : dropped)
        if (job.callback)
            job.callback(cancelled);
    if (!m_running) {
        emit activityChanged(false, {});
        return;
    }
    m_result.cancelled = true;
    m_process.terminate();
    QTimer::singleShot(1000, this, [this] {
        if (m_running && m_result.cancelled)
            m_process.kill();
    });
}

} // namespace idea4git
