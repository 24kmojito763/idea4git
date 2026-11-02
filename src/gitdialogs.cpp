#include "gitdialogs.h"
#include "codeeditor.h"
#include "icons.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <memory>

namespace idea4git {
namespace {
QString visiblePath(QString path) {
    return path.replace('\n', QChar(0x21b5)).replace('\r', QChar(0x240d));
}
QLabel* label(const QString& text, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
class PatchHighlighter final : public QSyntaxHighlighter {
  public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    void highlightBlock(const QString& line) override {
        QTextCharFormat style;
        if (line.startsWith('+') && !line.startsWith("+++")) {
            style.setForeground(QColor("#87bd85"));
            style.setBackground(QColor("#24352c"));
        } else if (line.startsWith('-') && !line.startsWith("---")) {
            style.setForeground(QColor("#d99191"));
            style.setBackground(QColor("#3b282c"));
        } else if (line.startsWith("@@"))
            style.setForeground(QColor("#7eaff0"));
        else if (line.startsWith("diff "))
            style.setForeground(QColor("#d6ba7b"));
        else
            return;
        setFormat(0, line.size(), style);
    }
};
QPlainTextEdit* patchView(QWidget* parent) {
    auto* view = new QPlainTextEdit(parent);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    view->setFont(font);
    new PatchHighlighter(view->document());
    return view;
}
void showBytes(CodeEditor* view, const QString& path, const QByteArray& bytes,
               const QString& unavailable) {
    QString error;
    if (!view->showPreview(path, bytes, &error)) {
        view->setReadOnly(false);
        view->setText(unavailable);
        view->setReadOnly(true);
    }
}
void markPatch(CodeEditor* left, CodeEditor* right, const QByteArray& bytes) {
    left->markerDeleteAll(8);
    right->markerDeleteAll(9);
    left->markerDefine(QsciScintilla::Background, 8);
    left->setMarkerBackgroundColor(QColor("#3b282c"), 8);
    right->markerDefine(QsciScintilla::Background, 9);
    right->setMarkerBackgroundColor(QColor("#24352c"), 9);
    static const QRegularExpression header("^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@");
    int a = 0, b = 0;
    bool hunk = false;
    for (const auto& line : QString::fromUtf8(bytes).split('\n')) {
        const auto match = header.match(line);
        if (match.hasMatch()) {
            a = match.captured(1).toInt() - 1;
            b = match.captured(2).toInt() - 1;
            hunk = true;
        } else if (hunk && line.startsWith('-'))
            left->markerAdd(a++, 8);
        else if (hunk && line.startsWith('+'))
            right->markerAdd(b++, 9);
        else if (hunk && line.startsWith(' ')) {
            ++a;
            ++b;
        }
    }
}
QVector<DiffHunk> parseDiffHunks(const QByteArray& bytes) {
    static const QRegularExpression header("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@");
    QVector<DiffHunk> hunks;
    for (const auto& line : QString::fromUtf8(bytes).split('\n')) {
        const auto match = header.match(line);
        if (!match.hasMatch())
            continue;
        hunks.append({match.captured(1).toInt(),
                      match.captured(2).isEmpty() ? 1 : match.captured(2).toInt(),
                      match.captured(3).toInt(),
                      match.captured(4).isEmpty() ? 1 : match.captured(4).toInt()});
    }
    return hunks;
}
void placeAtHunk(CodeEditor* left, CodeEditor* right, const DiffHunk& hunk) {
    const int leftLine = qBound(0, hunk.leftStart - 1, qMax(0, left->lines() - 1));
    const int rightLine = qBound(0, hunk.rightStart - 1, qMax(0, right->lines() - 1));
    left->setCursorPosition(leftLine, 0);
    right->setCursorPosition(rightLine, 0);
    left->ensureLineVisible(leftLine);
    right->ensureLineVisible(rightLine);
}
} // namespace

FileDiffView::FileDiffView(QString title, QString path, QString leftRevision, QByteArray leftBytes,
                           QString rightRevision, QByteArray rightBytes, QByteArray patch,
                           bool english, QWidget* parent)
    : QWidget(parent) {
    setObjectName("fileDiffView");
    setAccessibleName(title + QStringLiteral(" — ") + visiblePath(path));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    auto* navigation = new QHBoxLayout;
    m_previous = new QToolButton(this);
    m_previous->setObjectName("previousDifferenceButton");
    m_previous->setText(QStringLiteral("↑"));
    m_previous->setToolTip(english ? "Previous difference" : "上一个差异");
    m_previous->setAccessibleName(m_previous->toolTip());
    m_previous->setAutoRaise(true);
    m_next = new QToolButton(this);
    m_next->setObjectName("nextDifferenceButton");
    m_next->setText(QStringLiteral("↓"));
    m_next->setToolTip(english ? "Next difference" : "下一个差异");
    m_next->setAccessibleName(m_next->toolTip());
    m_next->setAutoRaise(true);
    m_diffPosition = new QLabel(this);
    m_diffPosition->setObjectName("differencePosition");
    navigation->addWidget(m_previous);
    navigation->addWidget(m_next);
    navigation->addWidget(m_diffPosition);
    navigation->addStretch();
    layout->addLayout(navigation);

    auto* tabs = new QTabWidget(this);
    auto* pair = new QSplitter(Qt::Horizontal, tabs);
    const auto pane = [pair, english](const QString& revision, const QString& filePath,
                                      const QByteArray& bytes, const QString& objectName) {
        auto* panel = new QWidget(pair);
        auto* panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(0, 0, 0, 0);
        auto* heading = label(revision + QStringLiteral("   ") + visiblePath(filePath), panel);
        panelLayout->addWidget(heading);
        auto* editor = new CodeEditor(panel);
        editor->setObjectName(objectName);
        editor->setReadOnly(true);
        showBytes(editor, filePath, bytes,
                  QString::fromUtf8(english ? "Binary or unsupported text; use Unified diff."
                                            : "二进制或不支持的文本；请查看统一差异。"));
        panelLayout->addWidget(editor, 1);
        return editor;
    };
    m_before = pane(leftRevision, path, leftBytes, "fileDiffBefore");
    m_after = pane(rightRevision, path, rightBytes, "fileDiffAfter");
    connect(m_before->verticalScrollBar(), &QScrollBar::valueChanged, m_after->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(m_after->verticalScrollBar(), &QScrollBar::valueChanged, m_before->verticalScrollBar(),
            &QScrollBar::setValue);
    tabs->addTab(pair, english ? "Side by side" : "左右对照");
    m_patch = patchView(tabs);
    m_patch->setObjectName("fileDiffPatch");
    m_patch->setPlainText(QString::fromUtf8(patch));
    tabs->addTab(m_patch, english ? "Unified diff" : "统一差异");
    layout->addWidget(tabs, 1);

    connect(m_previous, &QToolButton::clicked, this, [this] { navigateDiff(-1); });
    connect(m_next, &QToolButton::clicked, this, [this] { navigateDiff(1); });
    m_hunks = parseDiffHunks(patch);
    markPatch(m_before, m_after, patch);
    updateDiffNavigation();
    if (!m_hunks.isEmpty())
        navigateDiff(1);
}

void FileDiffView::navigateDiff(int delta) {
    if (m_hunks.isEmpty())
        return;
    const int nextIndex = m_hunkIndex < 0 && delta > 0 ? 0 : m_hunkIndex + delta;
    if (nextIndex < 0 || nextIndex >= m_hunks.size())
        return;
    m_hunkIndex = nextIndex;
    placeAtHunk(m_before, m_after, m_hunks[m_hunkIndex]);
    updateDiffNavigation();
}

void FileDiffView::updateDiffNavigation() {
    m_previous->setEnabled(m_hunkIndex > 0);
    m_next->setEnabled(m_hunkIndex + 1 < m_hunks.size());
    m_diffPosition->setText(
        QStringLiteral("%1/%2").arg(m_hunkIndex < 0 ? 0 : m_hunkIndex + 1).arg(m_hunks.size()));
}

QString operationName(OperationKind kind, bool english) {
    switch (kind) {
    case OperationKind::Merge:
        return english ? "Merge" : "合并（merge）";
    case OperationKind::Rebase:
        return english ? "Rebase" : "变基（rebase）";
    case OperationKind::CherryPick:
        return "Cherry-pick";
    case OperationKind::Revert:
        return "Revert";
    case OperationKind::Reset:
        return english ? "Reset" : "重置（reset）";
    }
    return {};
}
QString pendingName(PendingOperation pending, bool english) {
    switch (pending) {
    case PendingOperation::Merge:
        return operationName(OperationKind::Merge, english);
    case PendingOperation::Rebase:
        return operationName(OperationKind::Rebase, english);
    case PendingOperation::CherryPick:
        return "Cherry-pick";
    case PendingOperation::Revert:
        return "Revert";
    case PendingOperation::Unknown:
        return english ? "Unknown sequence" : "未知序列操作";
    default:
        return english ? "No pending operation" : "无进行中的操作";
    }
}

GitDialog::GitDialog(GitOperations* operations, bool english, QWidget* parent)
    : QDialog(parent), m_operations(operations), m_english(english) {
    resize(1180, 820);
    setMinimumSize(850, 600);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);
    m_content = new QWidget(this);
    m_body = new QVBoxLayout(m_content);
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(10);
    layout->addWidget(m_content, 1);
    m_notice = new QPlainTextEdit(this);
    m_notice->setObjectName("operationNotice");
    m_notice->setReadOnly(true);
    m_notice->setFixedHeight(76);
    layout->addWidget(m_notice);
    auto* buttons = new QHBoxLayout;
    m_cancel = new QPushButton(t("取消 Git 命令", "Cancel Git command"), this);
    m_cancel->setEnabled(false);
    m_cancel->setObjectName("dialogCancelGit");
    buttons->addWidget(m_cancel);
    buttons->addStretch();
    m_close = new QPushButton(t("关闭", "Close"), this);
    m_close->setAutoDefault(false);
    m_cancel->setAutoDefault(false);
    m_close->setObjectName("dialogClose");
    buttons->addWidget(m_close);
    layout->addLayout(buttons);
    connect(m_close, &QPushButton::clicked, this, &GitDialog::reject);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_operations)
            m_operations->cancel();
        notice(t("已请求取消；已生效的操作不会自动回滚。",
                 "Cancellation requested; completed changes are not rolled back."));
    });
}
QString GitDialog::t(const char* chinese, const char* english) const {
    return QString::fromUtf8(m_english ? english : chinese);
}
void GitDialog::setWorking(bool working) {
    m_working = working;
    m_content->setEnabled(!working);
    m_cancel->setEnabled(working);
    m_close->setEnabled(!working);
    updateControls();
}
void GitDialog::notice(const QString& message, bool error) {
    m_notice->setPlainText(message);
    m_notice->setStyleSheet(error ? "color: #e2a1a1; background: #30252a; padding: 6px;"
                                  : "color: #a8b8d1; background: #252930; padding: 6px;");
}
void GitDialog::showResult(const GitResult& result) {
    QString details = QString::fromUtf8(result.output + result.error);
    if (result.cancelled)
        details.prepend(
            t("命令已取消，请刷新实际状态。\n", "Command cancelled; refresh the actual state.\n"));
    if (result.timedOut)
        details.prepend(t("命令超时。\n", "Command timed out.\n"));
    if (result.outputLimitExceeded)
        details.prepend(t("输出超过 16 MiB 上限。\n", "Output exceeded the 16 MiB limit.\n"));
    notice(details.isEmpty() ? t("操作完成。", "Operation completed.") : details.left(12000),
           !result.ok());
}
void GitDialog::reject() {
    if (m_working) {
        notice(t("请先等待或取消 Git 命令。", "Wait for or cancel the Git command first."));
        return;
    }
    if (canLeave())
        QDialog::reject();
}
void GitDialog::closeEvent(QCloseEvent* event) {
    if (m_working || !canLeave()) {
        event->ignore();
        return;
    }
    QDialog::reject();
    event->accept();
}

CompareDialog::CompareDialog(GitOperations* operations, bool english, QWidget* parent, QString left,
                             QString right)
    : GitDialog(operations, english, parent) {
    setObjectName("compareDialog");
    setWindowTitle(t("比较任意两个版本", "Compare two revisions"));
    m_body->addWidget(label(t("分支、标签、提交哈希及 HEAD~N 均可；只读取提交快照，不改动工作区。",
                              "Use branches, tags, hashes or HEAD~N. Reads commit snapshots "
                              "without changing the worktree."),
                            m_content));
    auto* form = new QHBoxLayout;
    m_left = new QComboBox(m_content);
    m_left->setEditable(true);
    m_left->setObjectName("compareLeft");
    m_left->setCurrentText(left);
    m_right = new QComboBox(m_content);
    m_right->setEditable(true);
    m_right->setObjectName("compareRight");
    m_right->setCurrentText(right);
    auto* swap = new QPushButton(t("交换", "Swap"), m_content);
    auto* compareButton = new QPushButton(t("比较", "Compare"), m_content);
    compareButton->setObjectName("compareButton");
    compareButton->setProperty("primary", true);
    form->addWidget(m_left, 1);
    form->addWidget(swap);
    form->addWidget(m_right, 1);
    form->addWidget(compareButton);
    m_body->addLayout(form);
    connect(swap, &QPushButton::clicked, this, [this] {
        const auto a = m_left->currentText();
        m_left->setCurrentText(m_right->currentText());
        m_right->setCurrentText(a);
    });
    connect(compareButton, &QPushButton::clicked, this, &CompareDialog::compare);
    auto* navigation = new QHBoxLayout;
    m_previous = new QToolButton(m_content);
    m_previous->setObjectName("previousDifferenceButton");
    m_previous->setText(QStringLiteral("↑"));
    m_previous->setToolTip(t("上一个差异", "Previous difference"));
    m_previous->setAutoRaise(true);
    m_next = new QToolButton(m_content);
    m_next->setObjectName("nextDifferenceButton");
    m_next->setText(QStringLiteral("↓"));
    m_next->setToolTip(t("下一个差异", "Next difference"));
    m_next->setAutoRaise(true);
    m_diffPosition = new QLabel(m_content);
    m_diffPosition->setObjectName("differencePosition");
    navigation->addWidget(m_previous);
    navigation->addWidget(m_next);
    navigation->addWidget(m_diffPosition);
    navigation->addStretch();
    m_body->addLayout(navigation);
    connect(m_previous, &QToolButton::clicked, this, [this] { navigateDiff(-1); });
    connect(m_next, &QToolButton::clicked, this, [this] { navigateDiff(1); });
    auto* splitter = new QSplitter(Qt::Horizontal, m_content);
    m_files = new QListWidget(splitter);
    m_files->setObjectName("comparisonFiles");
    m_files->setMinimumWidth(190);
    auto* tabs = new QTabWidget(splitter);
    auto* pair = new QSplitter(Qt::Horizontal, tabs);
    const auto pane = [pair](QLabel** title, CodeEditor** editor) {
        auto* panel = new QWidget(pair);
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(0, 0, 0, 0);
        *title = label({}, panel);
        layout->addWidget(*title);
        *editor = new CodeEditor(panel);
        (*editor)->setReadOnly(true);
        layout->addWidget(*editor, 1);
    };
    pane(&m_leftTitle, &m_before);
    pane(&m_rightTitle, &m_after);
    m_before->setObjectName("comparisonBefore");
    m_after->setObjectName("comparisonAfter");
    connect(m_before->verticalScrollBar(), &QScrollBar::valueChanged, m_after->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(m_after->verticalScrollBar(), &QScrollBar::valueChanged, m_before->verticalScrollBar(),
            &QScrollBar::setValue);
    tabs->addTab(pair, t("左右对照", "Side by side"));
    m_patch = patchView(tabs);
    m_patch->setObjectName("comparisonPatch");
    tabs->addTab(m_patch, t("统一差异", "Unified diff"));
    splitter->setSizes({250, 900});
    m_body->addWidget(splitter, 1);
    connect(m_files, &QListWidget::currentRowChanged, this, &CompareDialog::selectFile);
    setWorking(true);
    const QPointer<CompareDialog> guard(this);
    operations->revisions([guard](const GitResult& result, const QStringList& refs) {
        if (!guard)
            return;
        const auto a = guard->m_left->currentText(), b = guard->m_right->currentText();
        guard->m_left->addItems(refs);
        guard->m_right->addItems(refs);
        guard->m_left->setCurrentText(a);
        guard->m_right->setCurrentText(b);
        guard->setWorking(false);
        if (!result.ok())
            guard->showResult(result);
        else
            guard->compare();
    });
}

void CompareDialog::compare() {
    if (m_working || !m_operations)
        return;
    setWorking(true);
    ++m_selection;
    m_hunks.clear();
    m_hunkIndex = -1;
    updateDiffNavigation();
    const QPointer<CompareDialog> guard(this);
    m_operations->compare(
        m_left->currentText(), m_right->currentText(),
        [guard](const GitResult& result, const Comparison& comparison) {
            if (!guard)
                return;
            guard->setWorking(false);
            if (!result.ok()) {
                guard->showResult(result);
                return;
            }
            guard->m_comparison = comparison;
            {
                const QSignalBlocker block(guard->m_files);
                guard->m_files->clear();
                for (const auto& file : comparison.files.mid(0, 5000))
                    guard->m_files->addItem(file.status + "   " + visiblePath(file.newPath));
            }
            guard->notice(guard->t("左侧：", "Left: ") + comparison.left +
                          guard->t("\n右侧：", "\nRight: ") + comparison.right +
                          guard->t("\n变更文件：", "\nChanged files: ") +
                          QString::number(comparison.files.size()) +
                          (comparison.files.size() > 5000
                               ? guard->t("（仅显示前 5000 项）", " (first 5000 shown)")
                               : ""));
            if (!comparison.files.isEmpty()) {
                guard->m_files->setCurrentRow(0);
            } else {
                guard->m_before->setReadOnly(false);
                guard->m_before->clear();
                guard->m_before->setReadOnly(true);
                guard->m_after->setReadOnly(false);
                guard->m_after->clear();
                guard->m_after->setReadOnly(true);
                guard->m_patch->clear();
            }
        });
}

void CompareDialog::selectFile(int row) {
    if (row < 0 || row >= m_comparison.files.size() || !m_operations)
        return;
    const int request = ++m_selection;
    m_hunks.clear();
    m_hunkIndex = -1;
    updateDiffNavigation();
    const auto file = m_comparison.files[row];
    m_leftTitle->setText(m_comparison.left.left(12) + "   " + visiblePath(file.oldPath));
    m_rightTitle->setText(m_comparison.right.left(12) + "   " + visiblePath(file.newPath));
    setWorking(true);
    const QPointer<CompareDialog> guard(this);
    auto remaining = std::make_shared<int>(3);
    const auto done = [guard, request, remaining] {
        if (guard && request == guard->m_selection && --*remaining == 0)
            guard->setWorking(false);
    };
    const auto content = [guard, request, file, done](bool left, const GitResult& result,
                                                      bool absent) {
        if (guard && request == guard->m_selection) {
            auto* view = left ? guard->m_before : guard->m_after;
            const QString message =
                absent ? guard->t("此版本不存在该文件。", "File does not exist in this revision.")
                       : guard->t("二进制、非 UTF-8 或大于 5 MiB；请查看统一差异。",
                                  "Binary, non-UTF-8 or over 5 MiB; see the unified diff.");
            showBytes(view, left ? file.oldPath : file.newPath,
                      absent || !result.ok() ? message.toUtf8() : result.output, message);
            if (!result.ok() && !absent)
                guard->showResult(result);
        }
        done();
    };
    GitResult empty;
    empty.exitCode = 0;
    if (file.status.startsWith('A'))
        content(true, empty, true);
    else
        m_operations->blob(m_comparison.left, file.oldPath,
                           [content](const GitResult& r) { content(true, r, false); });
    if (file.status.startsWith('D'))
        content(false, empty, true);
    else
        m_operations->blob(m_comparison.right, file.newPath,
                           [content](const GitResult& r) { content(false, r, false); });
    m_operations->patch(m_comparison.left, m_comparison.right, file,
                        [guard, request, done](const GitResult& result) {
                            if (guard && request == guard->m_selection) {
                                guard->m_patch->setPlainText(
                                    QString::fromUtf8(result.ok() ? result.output : result.error));
                                if (result.ok()) {
                                    markPatch(guard->m_before, guard->m_after, result.output);
                                    guard->m_hunks = parseDiffHunks(result.output);
                                    if (!guard->m_hunks.isEmpty())
                                        guard->navigateDiff(1);
                                    else
                                        guard->updateDiffNavigation();
                                }
                            }
                            done();
                        });
}

void CompareDialog::navigateDiff(int delta) {
    if (m_hunks.isEmpty())
        return;
    const int nextIndex = m_hunkIndex < 0 && delta > 0 ? 0 : m_hunkIndex + delta;
    if (nextIndex < 0 || nextIndex >= m_hunks.size())
        return;
    m_hunkIndex = nextIndex;
    placeAtHunk(m_before, m_after, m_hunks[m_hunkIndex]);
    updateDiffNavigation();
}

void CompareDialog::updateDiffNavigation() {
    m_previous->setEnabled(m_hunkIndex > 0);
    m_next->setEnabled(m_hunkIndex + 1 < m_hunks.size());
    m_diffPosition->setText(
        QStringLiteral("%1/%2").arg(m_hunkIndex < 0 ? 0 : m_hunkIndex + 1).arg(m_hunks.size()));
}

OperationDialog::OperationDialog(GitOperations* operations, OperationKind kind, bool english,
                                 QWidget* parent, QString revision)
    : GitDialog(operations, english, parent), m_kind(kind) {
    setObjectName("gitOperationDialog");
    setWindowTitle(operationName(kind, english));
    resize(900, 740);
    auto* form = new QFormLayout;
    m_revision = new QComboBox(m_content);
    m_revision->setEditable(true);
    m_revision->setObjectName("operationRevision");
    m_revision->setCurrentText(
        revision.isEmpty()
            ? (kind == OperationKind::Merge || kind == OperationKind::Rebase ? "main" : "HEAD~1")
            : revision);
    form->addRow(t("目标版本", "Target revision"), m_revision);
    m_mode = new QComboBox(m_content);
    m_mode->setObjectName("operationMode");
    if (kind == OperationKind::Merge) {
        m_mode->addItems(
            {t("按 Git 配置合并", "Default merge"),
             t("总是创建合并提交（--no-ff）", "Always create merge commit (--no-ff)"),
             t("仅快进（--ff-only）", "Fast-forward only (--ff-only)"),
             t("压缩到暂存区（--squash），之后手动提交", "Squash into index; commit separately")});
        form->addRow(t("合并方式", "Merge mode"), m_mode);
    } else if (kind == OperationKind::Reset) {
        m_mode->addItems(
            {t("Soft：保留暂存区和工作区", "Soft: preserve index and worktree"),
             t("Mixed：重置暂存区，保留工作区", "Mixed: reset index, preserve worktree"),
             t("Hard：丢弃暂存区和受影响的工作区内容",
               "Hard: discard index and affected worktree content")});
        m_mode->setCurrentIndex(1);
        form->addRow(t("重置方式", "Reset mode"), m_mode);
    } else
        m_mode->hide();
    m_mainline = new QSpinBox(m_content);
    m_mainline->setRange(0, 128);
    m_mainline->setSpecialValueText(t("不指定主线（普通提交）", "No mainline (ordinary commit)"));
    m_mainline->setObjectName("operationMainline");
    if (kind == OperationKind::CherryPick || kind == OperationKind::Revert)
        form->addRow(t("合并提交的主线父编号", "Mainline parent for merge commits"), m_mainline);
    else
        m_mainline->hide();
    m_rebaseMerges = new QCheckBox(
        t("重建合并结构（--rebase-merges）", "Recreate merge structure (--rebase-merges)"),
        m_content);
    if (kind == OperationKind::Rebase)
        form->addRow(m_rebaseMerges);
    else
        m_rebaseMerges->hide();
    m_body->addLayout(form);
    auto* previewButton =
        new QPushButton(t("预览目标与影响", "Preview target and consequences"), m_content);
    previewButton->setObjectName("operationPreview");
    m_body->addWidget(previewButton);
    m_details = patchView(m_content);
    m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_details->setObjectName("operationDetails");
    m_body->addWidget(m_details, 1);
    m_confirm =
        new QCheckBox(t("我已确认仓库、当前分支、目标提交和操作后果",
                        "I verified the repository, branch, target commit and consequences"),
                      m_content);
    m_confirm->setObjectName("operationConfirmed");
    m_body->addWidget(m_confirm);
    m_hardConfirmation = new QLineEdit(m_content);
    m_hardConfirmation->setObjectName("hardResetConfirmation");
    m_hardConfirmation->setPlaceholderText(
        t("Hard reset 需输入 RESET 后才能执行", "Type RESET to enable hard reset"));
    m_body->addWidget(m_hardConfirmation);
    m_execute = new QPushButton(t("执行已预览的操作", "Execute previewed operation"), m_content);
    m_execute->setObjectName("operationExecute");
    m_execute->setProperty("primary", true);
    m_body->addWidget(m_execute);
    m_resolve =
        new QPushButton(t("打开三方冲突解决器", "Open three-way conflict resolver"), m_content);
    m_resolve->setObjectName("operationResolve");
    m_resolve->hide();
    m_body->addWidget(m_resolve);
    connect(m_resolve, &QPushButton::clicked, this, [this] {
        emit conflictsRequested();
        accept();
    });
    connect(previewButton, &QPushButton::clicked, this, &OperationDialog::preview);
    connect(m_execute, &QPushButton::clicked, this, &OperationDialog::execute);
    connect(m_revision, &QComboBox::currentTextChanged, this, &OperationDialog::invalidate);
    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &OperationDialog::invalidate);
    connect(m_mainline, qOverload<int>(&QSpinBox::valueChanged), this,
            &OperationDialog::invalidate);
    connect(m_rebaseMerges, &QCheckBox::toggled, this, &OperationDialog::invalidate);
    connect(m_confirm, &QCheckBox::toggled, this, &OperationDialog::updateControls);
    connect(m_hardConfirmation, &QLineEdit::textChanged, this, &OperationDialog::updateControls);
    notice(t("先预览，再确认。merge/rebase/cherry-pick/revert 要求已跟踪文件干净；不会自动 "
             "stash，也不会强制推送。",
             "Preview, then confirm. Merge/rebase/cherry-pick/revert require clean tracked files; "
             "no automatic stash or force-push."));
    updateControls();
    setWorking(true);
    const QPointer<OperationDialog> guard(this);
    operations->revisions([guard](const GitResult& result, const QStringList& refs) {
        if (!guard)
            return;
        const auto current = guard->m_revision->currentText();
        guard->m_revision->addItems(refs);
        guard->m_revision->setCurrentText(current);
        guard->setWorking(false);
        if (!result.ok())
            guard->showResult(result);
    });
}

void OperationDialog::invalidate() {
    m_plan.reset();
    m_confirm->setChecked(false);
    m_hardConfirmation->clear();
    m_details->clear();
    updateControls();
}
void OperationDialog::updateControls() {
    const bool hard =
        m_kind == OperationKind::Reset && m_mode->currentIndex() == int(ResetMode::Hard);
    m_hardConfirmation->setVisible(hard);
    m_execute->setEnabled(!m_working && m_plan.has_value() && m_confirm->isChecked() &&
                          (!hard || m_hardConfirmation->text() == "RESET"));
}
void OperationDialog::preview() {
    if (m_working || !m_operations)
        return;
    OperationRequest request;
    request.kind = m_kind;
    request.revision = m_revision->currentText();
    request.mainline = m_mainline->value();
    request.mergeMode = MergeMode(m_mode->currentIndex());
    request.resetMode = ResetMode(m_mode->currentIndex());
    request.rebaseMerges = m_rebaseMerges->isChecked();
    m_plan.reset();
    m_confirm->setChecked(false);
    setWorking(true);
    const QPointer<OperationDialog> guard(this);
    m_operations->plan(request, [guard](const GitResult& result, const OperationPlan& plan) {
        if (!guard)
            return;
        if (!result.ok()) {
            guard->setWorking(false);
            guard->showResult(result);
            return;
        }
        guard->m_plan = plan;
        QString consequences;
        if (plan.request.kind == OperationKind::Reset) {
            if (plan.request.resetMode == ResetMode::Soft)
                consequences =
                    guard->t("移动当前分支/HEAD；暂存区与工作区保持不变。",
                             "Moves the current branch/HEAD; preserves index and worktree.");
            if (plan.request.resetMode == ResetMode::Mixed)
                consequences =
                    guard->t("移动当前分支/HEAD 并重置暂存区；工作区文件保留，暂存选择会丢失。",
                             "Moves branch/HEAD and resets the index; worktree files remain, "
                             "staging choices are lost.");
            if (plan.request.resetMode == ResetMode::Hard)
                consequences = guard->t(
                    "危险：移动当前分支/"
                    "HEAD，丢弃已暂存和受影响的未提交修改；妨碍写入目标版本的未跟踪文件/"
                    "目录也可能被 Git 删除。未提交内容无法依靠 reflog 恢复。",
                    "DANGER: moves branch/HEAD and discards staged and affected uncommitted "
                    "changes. Untracked files/directories obstructing checkout may also be "
                    "deleted. Reflog cannot recover uncommitted content.");
        } else if (plan.request.kind == OperationKind::Rebase)
            consequences = guard->t(
                "重放当前分支提交到目标之上，提交哈希会改变。不会自动 "
                "stash，不更新其他分支引用；已发布提交会影响协作者。此处不是交互式 rebase。",
                "Replays current branch commits onto the target, rewriting hashes. No auto-stash "
                "or update of other branch refs. Published history affects collaborators. This is "
                "not interactive rebase.");
        else if (plan.request.kind == OperationKind::Revert)
            consequences = guard->t("创建新的反向提交，不删除原提交；可能产生冲突。",
                                    "Creates a new inverse commit without removing the original; "
                                    "conflicts are possible.");
        else if (plan.request.kind == OperationKind::CherryPick)
            consequences = guard->t("将目标提交的改动复制到当前分支并创建提交；可能产生冲突。",
                                    "Copies the target commit's changes onto the current branch; "
                                    "conflicts are possible.");
        else
            consequences = guard->t(
                "将目标合并到当前分支；可能快进、创建合并提交或产生冲突。Squash "
                "只准备暂存区，需另行提交。",
                "Merges the target into the current branch; may fast-forward, create a merge "
                "commit or conflict. Squash only prepares the index and needs a separate commit.");
        guard->m_details->setPlainText(
            guard->t("仓库：", "Repository: ") + guard->m_operations->root() +
            guard->t("\n当前分支：", "\nCurrent branch: ") + plan.before.status.branch +
            "\nHEAD: " + plan.before.head + guard->t("\n目标：", "\nTarget: ") + plan.target +
            "\n\ngit " + plan.arguments.join(' ') + "\n\n" + consequences);
        guard->notice(guard->t("已解析为固定提交哈希；执行前会再次核验 HEAD 和进行中的操作。",
                               "Target pinned to a commit hash; HEAD and pending operations are "
                               "checked again before execution."));
        guard->setWorking(false);
    });
}
void OperationDialog::execute() {
    if (!m_execute->isEnabled() || !m_plan || !m_operations)
        return;
    const auto plan = *m_plan;
    setWorking(true);
    const QPointer<OperationDialog> guard(this);
    m_operations->execute(plan, [guard](const GitResult& result) {
        if (!guard)
            return;
        guard->m_plan.reset();
        guard->showResult(result);
        emit guard->repositoryChanged();
        guard->m_operations->snapshot(
            [guard](const GitResult& inspected, const GitSnapshot& state) {
                if (!guard)
                    return;
                guard->m_resolve->setVisible(inspected.ok() && state.conflicts());
                if (inspected.ok() && state.conflicts())
                    guard->m_details->appendPlainText(
                        guard->t("\n操作停在冲突处。请打开三方解决器，保存并暂存结果，然后在主窗口"
                                 "继续/中止操作。",
                                 "\nOperation stopped at conflicts. Resolve, save and stage them, "
                                 "then continue/abort from the main window."));
                guard->setWorking(false);
            });
    });
}

ConflictDialog::ConflictDialog(GitOperations* operations, QString path, bool english,
                               QWidget* parent)
    : GitDialog(operations, english, parent), m_path(std::move(path)) {
    setObjectName("conflictDialog");
    setWindowTitle(t("三方冲突解决：", "Three-way merge: ") + visiblePath(m_path));
    resize(1450, 940);
    m_body->addWidget(
        label(t("上方按 Git 索引阶段展示 ours / base / "
                "theirs；下方编辑最终结果。保存只写工作区，暂存后才标记为已解决。",
                "Top: Git index stages ours / base / theirs. Edit the final result below. Saving "
                "only writes the worktree; staging marks the conflict resolved."),
              m_content));
    auto* vertical = new QSplitter(Qt::Vertical, m_content);
    auto* sources = new QSplitter(Qt::Horizontal, vertical);
    for (int position = 0; position < 3; ++position) {
        const int stage = position == 0 ? 1 : position == 1 ? 0 : 2;
        auto* panel = new QWidget(sources);
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(0, 0, 0, 0);
        const QString title =
            stage == 0 ? t("Base · 共同基础（阶段 1）", "Base · common ancestor (stage 1)")
            : stage == 1 ? t("Ours · 当前侧（阶段 2）", "Ours · current side (stage 2)")
                         : t("Theirs · 传入侧（阶段 3）", "Theirs · incoming side (stage 3)");
        layout->addWidget(label(title, panel));
        m_sources[stage] = new CodeEditor(panel);
        m_sources[stage]->setReadOnly(true);
        m_sources[stage]->setObjectName(stage == 0   ? "conflictBase"
                                        : stage == 1 ? "conflictOurs"
                                                     : "conflictTheirs");
        layout->addWidget(m_sources[stage], 1);
        m_use[stage] = new QPushButton(stage == 0   ? t("使用 Base 全文", "Use entire base")
                                       : stage == 1 ? t("使用 Ours 全文", "Use entire ours")
                                                    : t("使用 Theirs 全文", "Use entire theirs"),
                                       panel);
        m_use[stage]->setObjectName(stage == 0 ? "useBase" : stage == 1 ? "useOurs" : "useTheirs");
        layout->addWidget(m_use[stage]);
        connect(m_use[stage], &QPushButton::clicked, this, [this, stage] { chooseStage(stage); });
    }
    auto* resultPanel = new QWidget(vertical);
    auto* resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    auto* resultHeader = new QHBoxLayout;
    resultHeader->addWidget(label(t("结果（可编辑）", "Result (editable)"), resultPanel));
    resultHeader->addStretch();
    m_delete = new QCheckBox(t("结果为删除文件", "Resolve by deleting this file"), resultPanel);
    m_delete->setObjectName("conflictDelete");
    resultHeader->addWidget(m_delete);
    resultLayout->addLayout(resultHeader);
    m_result = new CodeEditor(resultPanel);
    m_result->setObjectName("conflictResult");
    resultLayout->addWidget(m_result, 1);
    m_body->addWidget(vertical, 1);
    vertical->setSizes({370, 330});
    m_state = label({}, m_content);
    m_state->setObjectName("conflictState");
    m_body->addWidget(m_state);
    auto* actions = new QHBoxLayout;
    auto* reload = new QPushButton(t("重新加载", "Reload"), m_content);
    reload->setObjectName("conflictReload");
    actions->addWidget(reload);
    actions->addStretch();
    m_save = new QPushButton(t("保存结果", "Save result"), m_content);
    m_save->setObjectName("conflictSave");
    actions->addWidget(m_save);
    m_stage = new QPushButton(t("暂存并标记已解决", "Stage and mark resolved"), m_content);
    m_stage->setObjectName("conflictStage");
    m_stage->setProperty("primary", true);
    actions->addWidget(m_stage);
    m_body->addLayout(actions);
    connect(m_result, &QsciScintilla::textChanged, this, [this] {
        if (!m_loading) {
            m_dirty = true;
            updateControls();
        }
    });
    connect(m_delete, &QCheckBox::toggled, this, [this](bool remove) {
        m_result->setReadOnly(remove || !m_file || !m_file->editable);
        if (!m_loading) {
            m_dirty = true;
            updateControls();
        }
    });
    connect(m_save, &QPushButton::clicked, this, [this] { save(); });
    connect(m_stage, &QPushButton::clicked, this, &ConflictDialog::stage);
    connect(reload, &QPushButton::clicked, this, [this] {
        if (m_dirty &&
            QMessageBox::question(this, t("放弃未保存的解决结果？", "Discard unsaved resolution?"),
                                  t("重新加载会丢弃结果区未保存的编辑。",
                                    "Reloading discards unsaved edits in the result pane."),
                                  QMessageBox::Discard | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Discard)
            return;
        load();
    });
    load();
}

void ConflictDialog::load() {
    if (!m_operations || m_working)
        return;
    setWorking(true);
    const QPointer<ConflictDialog> guard(this);
    m_operations->snapshot([guard](const GitResult& result, const GitSnapshot& snapshot) {
        if (!guard)
            return;
        if (!result.ok()) {
            guard->setWorking(false);
            guard->showResult(result);
            return;
        }
        guard->notice(
            snapshot.pending == PendingOperation::Rebase
                ? guard->t("变基注意：Ours 是已经变基到的上游结果，Theirs 是当前正在重放的提交；以 "
                           "Git 阶段 2/3 为准。",
                           "Rebase: Ours is the rebased/upstream result; Theirs is the commit "
                           "currently being replayed. These are Git stages 2/3.")
                : guard->t("删除/"
                           "新增冲突可能缺少某个阶段。选择缺失的一侧表示删除文件；二进制、符号链接"
                           "和子模块保持只读。",
                           "Add/delete conflicts may lack a stage. Choosing an absent side deletes "
                           "the file. Binary files, symlinks and submodules remain read-only."));
        guard->m_operations->conflict(
            guard->m_path, [guard](const GitResult& loaded, const ConflictFile& file) {
                if (!guard)
                    return;
                if (!loaded.ok()) {
                    guard->setWorking(false);
                    guard->showResult(loaded);
                    return;
                }
                guard->m_loading = true;
                guard->m_file = file;
                guard->m_resolved = false;
                for (int i = 0; i < 3; ++i) {
                    const auto& source = file.stages[i];
                    const auto missing =
                        guard->t("此阶段不存在该文件（可选择删除）。",
                                 "File absent in this stage (can resolve as deletion).");
                    showBytes(guard->m_sources[i], file.path,
                              source.present ? source.bytes : missing.toUtf8(),
                              guard->t("此阶段不是可编辑的 UTF-8 文本。",
                                       "This stage is not editable UTF-8 text."));
                }
                showBytes(guard->m_result, file.path, file.diskBytes,
                          guard->t("此文件类型不支持内置解决；请使用外部工具。",
                                   "Use an external tool for this file type."));
                guard->m_bom = file.diskBytes.startsWith("\xef\xbb\xbf");
                guard->m_delete->setChecked(!file.diskExists);
                guard->m_result->setReadOnly(!file.editable || !file.diskExists);
                guard->m_dirty = false;
                guard->m_loading = false;
                guard->setWorking(false);
            });
    });
}
void ConflictDialog::chooseStage(int stage) {
    if (!m_file || !m_file->editable || m_working)
        return;
    const auto& source = m_file->stages[stage];
    m_loading = true;
    QString error;
    m_result->showPreview(m_path, source.bytes, &error);
    m_result->setReadOnly(!source.present);
    m_delete->setChecked(!source.present);
    m_bom = source.bytes.startsWith("\xef\xbb\xbf");
    m_loading = false;
    m_dirty = true;
    updateControls();
}
void ConflictDialog::updateControls() {
    const bool editable = m_file && m_file->editable && !m_working && !m_resolved;
    m_result->setReadOnly(!editable || m_delete->isChecked());
    for (auto* button : m_use)
        button->setEnabled(editable);
    m_delete->setEnabled(editable);
    m_save->setEnabled(editable && m_dirty);
    m_stage->setEnabled(
        editable && !m_dirty &&
        (!m_file->diskExists || !GitOperations::hasConflictMarkers(m_file->diskBytes)));
    m_state->setText(
        m_resolved ? t("已暂存，冲突已标记为解决。关闭后可继续 Git 操作。",
                       "Staged and marked resolved. Close to continue the Git operation.")
        : !m_file  ? t("正在加载冲突…", "Loading conflict…")
        : !m_file->editable
            ? t("此冲突只读：二进制/非 UTF-8、符号链接、子模块、超大文件或不安全路径。",
                "Read-only conflict: binary/non-UTF-8, symlink, submodule, oversized file or "
                "unsafe path.")
        : m_dirty ? t("结果尚未保存；请先保存，再暂存。", "Result not saved; save before staging.")
                  : t("结果已与工作区同步；清除冲突标记后才能暂存。",
                      "Result matches the worktree; remove conflict markers before staging."));
}
void ConflictDialog::save(std::function<void(bool)> complete) {
    if (!m_file || m_working || !m_dirty || !m_operations) {
        if (complete)
            complete(false);
        return;
    }
    const bool remove = m_delete->isChecked();
    if (remove && m_file->diskExists &&
        QMessageBox::warning(
            this, t("删除冲突文件？", "Delete conflicted file?"),
            m_operations->root() + '\n' + visiblePath(m_path) + '\n' +
                t("保存结果会删除工作区中的此文件，尚不会更新暂存区。",
                  "Saving deletes this worktree file, without updating the index yet."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        if (complete)
            complete(false);
        return;
    }
    QByteArray bytes = m_result->text().toUtf8();
    if (m_bom)
        bytes.prepend("\xef\xbb\xbf");
    setWorking(true);
    const QPointer<ConflictDialog> guard(this);
    m_operations->saveResolution(
        *m_file, bytes, remove,
        [guard, complete](const GitResult& result, const ConflictFile& updated) {
            if (!guard)
                return;
            if (result.ok()) {
                guard->m_file = updated;
                guard->m_dirty = false;
                emit guard->repositoryChanged();
                guard->notice(guard->t("结果已保存到工作区；尚未暂存。",
                                       "Result saved to worktree; not staged yet."));
            } else
                guard->showResult(result);
            guard->setWorking(false);
            if (complete)
                complete(result.ok());
        });
}
void ConflictDialog::stage() {
    if (!m_file || !m_stage->isEnabled() || !m_operations)
        return;
    setWorking(true);
    const QPointer<ConflictDialog> guard(this);
    m_operations->markResolved(*m_file, [guard](const GitResult& result) {
        if (!guard)
            return;
        if (result.ok()) {
            guard->m_resolved = true;
            guard->notice(guard->t("已暂存并标记已解决；不会自动提交或继续。",
                                   "Staged and marked resolved; no automatic commit or continue."));
            emit guard->repositoryChanged();
        } else
            guard->showResult(result);
        guard->setWorking(false);
    });
}
bool ConflictDialog::canLeave() {
    if (!m_dirty)
        return true;
    const auto choice = QMessageBox::warning(
        this, t("保存解决结果？", "Save resolution?"),
        t("结果区还有未保存的编辑。", "The result pane has unsaved edits."),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice == QMessageBox::Discard) {
        m_dirty = false;
        return true;
    }
    if (choice == QMessageBox::Save) {
        const QPointer<ConflictDialog> guard(this);
        save([guard](bool success) {
            if (guard && success)
                guard->QDialog::reject();
        });
    }
    return false;
}

StashDialog::StashDialog(GitOperations* operations, bool english, QWidget* parent)
    : GitDialog(operations, english, parent) {
    setObjectName("stashDialog");
    setWindowTitle(t("Stash 管理", "Stash manager"));
    m_body->addWidget(label(
        t("创建、预览、应用、弹出或删除 stash。Pop 只有应用成功后才移除记录；冲突会保留 stash。",
          "Create, preview, apply, pop or drop stashes. Pop removes an entry only after successful "
          "application; conflicts retain the stash."),
        m_content));
    auto* create = new QHBoxLayout;
    m_message = new QLineEdit(m_content);
    m_message->setObjectName("stashMessage");
    m_message->setPlaceholderText(t("Stash 说明", "Stash message"));
    create->addWidget(m_message, 1);
    m_untracked = new QCheckBox(
        t("包含未跟踪文件（不含忽略文件）", "Include untracked (not ignored)"), m_content);
    m_untracked->setObjectName("stashUntracked");
    create->addWidget(m_untracked);
    m_keepIndex = new QCheckBox(t("保留暂存区", "Keep index"), m_content);
    m_keepIndex->setObjectName("stashKeepIndex");
    create->addWidget(m_keepIndex);
    auto* push = new QPushButton(t("创建 Stash", "Create stash"), m_content);
    push->setObjectName("stashPush");
    create->addWidget(push);
    m_body->addLayout(create);
    auto* split = new QSplitter(Qt::Vertical, m_content);
    m_list = new QTableWidget(0, 3, split);
    m_list->setObjectName("stashList");
    m_list->setHorizontalHeaderLabels(
        {t("引用", "Ref"), t("提交", "Commit"), t("说明", "Message")});
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->verticalHeader()->hide();
    m_list->setColumnWidth(0, 110);
    m_list->setColumnWidth(1, 120);
    m_list->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_list->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_patch = patchView(split);
    m_patch->setObjectName("stashPatch");
    split->setSizes({230, 350});
    m_body->addWidget(split, 1);
    auto* actions = new QHBoxLayout;
    auto* reload = new QPushButton(t("刷新", "Refresh"), m_content);
    actions->addWidget(reload);
    m_restoreIndex =
        new QCheckBox(t("同时恢复暂存状态（--index）", "Restore staging too (--index)"), m_content);
    m_restoreIndex->setObjectName("stashRestoreIndex");
    actions->addWidget(m_restoreIndex);
    actions->addStretch();
    m_apply = new QPushButton(t("应用（保留记录）", "Apply (keep entry)"), m_content);
    m_apply->setObjectName("stashApply");
    actions->addWidget(m_apply);
    m_pop = new QPushButton(t("弹出（Pop）", "Pop"), m_content);
    m_pop->setObjectName("stashPop");
    actions->addWidget(m_pop);
    m_drop = new QPushButton(t("删除记录…", "Drop entry…"), m_content);
    m_drop->setObjectName("stashDrop");
    actions->addWidget(m_drop);
    m_body->addLayout(actions);
    m_resolve = new QPushButton(t("处理工作区冲突", "Resolve worktree conflicts"), m_content);
    m_resolve->hide();
    m_body->addWidget(m_resolve);
    connect(m_resolve, &QPushButton::clicked, this, [this] {
        emit conflictsRequested();
        accept();
    });
    connect(push, &QPushButton::clicked, this, &StashDialog::push);
    connect(reload, &QPushButton::clicked, this, &StashDialog::refresh);
    connect(m_list, &QTableWidget::itemSelectionChanged, this, &StashDialog::preview);
    connect(m_apply, &QPushButton::clicked, this, [this] { use(StashAction::Apply); });
    connect(m_pop, &QPushButton::clicked, this, [this] { use(StashAction::Pop); });
    connect(m_drop, &QPushButton::clicked, this, [this] { use(StashAction::Drop); });
    refresh();
}
void StashDialog::updateControls() {
    const int row = m_list->currentRow();
    const bool selected = !m_working && row >= 0 && row < m_entries.size();
    m_apply->setEnabled(selected);
    m_pop->setEnabled(selected);
    m_drop->setEnabled(selected);
}
void StashDialog::refresh() {
    if (m_working || !m_operations)
        return;
    setWorking(true);
    const QPointer<StashDialog> guard(this);
    m_operations->stashes([guard](const GitResult& result, const QList<StashEntry>& entries) {
        if (!guard)
            return;
        if (!result.ok()) {
            guard->setWorking(false);
            guard->showResult(result);
            return;
        }
        guard->m_entries = entries;
        {
            const QSignalBlocker block(guard->m_list);
            guard->m_list->clearContents();
            guard->m_list->setRowCount(qMin(5000, entries.size()));
            for (int row = 0; row < guard->m_list->rowCount(); ++row) {
                const auto& entry = entries[row];
                guard->m_list->setItem(row, 0, new QTableWidgetItem(entry.reference));
                guard->m_list->setItem(row, 1, new QTableWidgetItem(entry.hash.left(12)));
                guard->m_list->setItem(row, 2, new QTableWidgetItem(entry.message));
            }
        }
        guard->setWorking(false);
        if (entries.isEmpty()) {
            guard->m_patch->clear();
            guard->notice(guard->t("暂无 stash。可填写说明后创建。",
                                   "No stashes. Enter a message to create one."));
        } else
            guard->m_list->selectRow(0);
    });
}
void StashDialog::preview() {
    updateControls();
    const int row = m_list->currentRow();
    if (m_working || row < 0 || row >= m_entries.size() || !m_operations)
        return;
    const auto entry = m_entries[row];
    setWorking(true);
    const QPointer<StashDialog> guard(this);
    m_operations->stashPatch(entry.hash, [guard, entry](const GitResult& result) {
        if (!guard)
            return;
        guard->m_patch->setPlainText(QString::fromUtf8(result.ok() ? result.output : result.error));
        guard->setWorking(false);
        if (result.ok())
            guard->notice(entry.reference + "\n" + entry.hash + "\n" + entry.message);
        else
            guard->showResult(result);
    });
}
void StashDialog::push() {
    if (m_working || !m_operations)
        return;
    const auto details =
        m_operations->root() + '\n' +
        t("将保存并移走工作区修改。", "Save and remove worktree changes.") +
        (m_untracked->isChecked()
             ? t("\n包含未跟踪文件，但不包含忽略文件。",
                 "\nIncludes untracked, but not ignored files.")
             : t("\n未跟踪文件保留在原处。", "\nUntracked files remain in place."));
    if (QMessageBox::question(this, t("创建 Stash？", "Create stash?"), details,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setWorking(true);
    const QPointer<StashDialog> guard(this);
    m_operations->pushStash(m_message->text(), m_untracked->isChecked(), m_keepIndex->isChecked(),
                            [guard](const GitResult& result) {
                                if (!guard)
                                    return;
                                guard->setWorking(false);
                                guard->showResult(result);
                                emit guard->repositoryChanged();
                                if (result.ok()) {
                                    guard->m_message->clear();
                                    guard->refresh();
                                }
                            });
}
void StashDialog::use(StashAction action) {
    const int row = m_list->currentRow();
    if (m_working || row < 0 || row >= m_entries.size() || !m_operations)
        return;
    const auto entry = m_entries[row];
    const auto summary =
        m_operations->root() + "\n" + entry.reference + "\n" + entry.hash + "\n" + entry.message;
    if (action == StashAction::Drop) {
        bool accepted = false;
        const auto confirmation = QInputDialog::getText(
            this, t("删除 Stash 记录", "Drop stash entry"),
            summary + '\n' +
                t("记录删除后无法通过本界面恢复。输入 DROP 确认：",
                  "This UI cannot restore a dropped entry. Type DROP to confirm:"),
            QLineEdit::Normal, {}, &accepted);
        if (!accepted || confirmation != "DROP")
            return;
    } else if (QMessageBox::question(
                   this, t("确认应用 Stash", "Confirm stash application"),
                   summary + '\n' +
                       t("将修改当前工作区，可能发生冲突。Pop 仅在成功后删除记录。",
                         "This modifies the worktree and may conflict. Pop drops the entry only on "
                         "success."),
                   QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setWorking(true);
    const QPointer<StashDialog> guard(this);
    m_operations->useStash(
        action, entry.hash, m_restoreIndex->isChecked(), [guard](const GitResult& result) {
            if (!guard)
                return;
            guard->showResult(result);
            emit guard->repositoryChanged();
            guard->m_operations->snapshot([guard, result](const GitResult& inspected,
                                                          const GitSnapshot& snapshot) {
                if (!guard)
                    return;
                guard->m_resolve->setVisible(inspected.ok() && snapshot.conflicts());
                guard->setWorking(false);
                if (result.ok())
                    guard->refresh();
                else if (inspected.ok() && snapshot.conflicts())
                    guard->notice(
                        guard->t(
                            "应用发生冲突，原 stash 保留。请使用三方解决器；stash 本身没有 "
                            "--continue/--abort，解决后按正常流程提交。",
                            "Apply conflicted; the original stash is retained. Use the resolver. "
                            "Stash has no --continue/--abort; resolve and commit normally."),
                        true);
            });
        });
}

} // namespace idea4git
