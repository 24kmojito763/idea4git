#include "mainwindow.h"
#include "codeeditor.h"
#include "gitdialogs.h"
#include "icons.h"
#include "paths.h"
#include "projectmodel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResource>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStringDecoder>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <memory>

static void initializeIdea4gitResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(idea4git);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace idea4git {
namespace {

QString projectSettingsId(const QString& projectRoot) {
    const auto normalizedRoot = QDir::cleanPath(projectRoot).toUtf8();
    const auto projectId =
        QCryptographicHash::hash(normalizedRoot, QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(projectId);
}

QString projectTreeExpansionSettingsKey(const QString& projectRoot) {
    return QStringLiteral("projectTree/%1/expandedDirectories").arg(projectSettingsId(projectRoot));
}

QString projectEditorSessionSettingsGroup(const QString& projectRoot) {
    return QStringLiteral("projectEditors/%1").arg(projectSettingsId(projectRoot));
}

QString normalizedProjectRelativePath(const QString& path) {
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (normalized.isEmpty() || normalized == QStringLiteral(".") ||
        normalized == QStringLiteral("..") || normalized.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(normalized))
        return {};
    return normalized;
}

class ProjectTreeView final : public QTreeView {
  public:
    using QTreeView::QTreeView;

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        m_disclosurePressed = false;
        if (event->button() == Qt::LeftButton) {
            const auto index = indexAt(event->pos());
            if (index.isValid() && model()->hasChildren(index)) {
                const auto row = visualRect(index);
                const QRect disclosure(row.left() - indentation(), row.top(), indentation(),
                                       row.height());
                if (disclosure.contains(event->pos())) {
                    setExpanded(index, !isExpanded(index));
                    m_disclosurePressed = true;
                    event->accept();
                    return;
                }
            }
        }
        QTreeView::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_disclosurePressed) {
            m_disclosurePressed = false;
            event->accept();
            return;
        }
        QTreeView::mouseReleaseEvent(event);
    }

  private:
    bool m_disclosurePressed = false;
};

class DiffHighlighter final : public QSyntaxHighlighter {
  public:
    explicit DiffHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

  protected:
    void highlightBlock(const QString& line) override {
        QTextCharFormat format;
        if (line.startsWith('+') && !line.startsWith("+++")) {
            format.setForeground(QColor("#87bd85"));
            format.setBackground(QColor("#24352c"));
        } else if (line.startsWith('-') && !line.startsWith("---")) {
            format.setForeground(QColor("#d99191"));
            format.setBackground(QColor("#3b282c"));
        } else if (line.startsWith("@@")) {
            format.setForeground(QColor("#7eaff0"));
            format.setBackground(QColor("#252e40"));
        } else if (line.startsWith("diff ") || line.startsWith("commit ")) {
            format.setForeground(QColor("#d6ba7b"));
            format.setFontWeight(QFont::DemiBold);
        } else {
            return;
        }
        setFormat(0, line.size(), format);
    }
};

QToolButton* toolButton(const QString& glyph, const QString& tooltip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(icon(glyph));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setIconSize(QSize(20, 20));
    button->setAutoRaise(true);
    return button;
}

QString displayPath(QString path) {
    return path.replace('\n', QChar(0x21b5)).replace('\r', QChar(0x240d));
}

QString existingWorkingTreeFile(const QString& repositoryRoot, const QString& path,
                                const QString& alternatePath = {}) {
    if (repositoryRoot.isEmpty())
        return {};
    const QStringList candidates =
        alternatePath.isEmpty() ? QStringList{path} : QStringList{path, alternatePath};
    for (const auto& candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        const auto absolutePath = repositoryFilePath(repositoryRoot, candidate);
        if (QFileInfo(absolutePath).isFile())
            return absolutePath;
    }
    return {};
}

FileState commitFileState(const QString& status) {
    if (status.startsWith(QLatin1Char('A')))
        return FileState::Added;
    if (status.startsWith(QLatin1Char('D')))
        return FileState::Deleted;
    if (status.startsWith(QLatin1Char('R')) || status.startsWith(QLatin1Char('C')))
        return FileState::Renamed;
    if (status.startsWith(QLatin1Char('U')))
        return FileState::Conflict;
    return FileState::Modified;
}

QString authorRegex(const QString& author) {
    QString escaped;
    escaped.reserve(author.size() * 2 + 2);
    escaped += '^';
    for (const auto character : author) {
        if (QStringLiteral("\\.^$|?*+()[]{}").contains(character))
            escaped += '\\';
        escaped += character;
    }
    escaped += " <";
    return escaped;
}

struct GlobalSearchMatch {
    QString path;
    int line = 0;
    int column = 0;
    int endColumn = 0;
    QString snippet;
};

struct GlobalSearchResults {
    QVector<GlobalSearchMatch> matches;
    int totalMatches = 0;
    bool truncated = false;
};

bool searchLine(const QString& line, const QString& query, bool fuzzy, bool caseSensitive,
                int* start, int* end) {
    const auto sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!fuzzy) {
        const int found = line.indexOf(query, 0, sensitivity);
        if (found < 0)
            return false;
        *start = found;
        *end = found + query.size();
        return true;
    }

    int cursor = 0;
    *start = -1;
    for (const auto character : query) {
        const int found = line.indexOf(character, cursor, sensitivity);
        if (found < 0)
            return false;
        if (*start < 0)
            *start = found;
        *end = found + 1;
        cursor = found + 1;
    }
    return *start >= 0;
}

QStringList searchSuffixes(QString filter) {
    QStringList suffixes;
    const auto tokens =
        filter.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (auto suffix : tokens) {
        suffix = suffix.trimmed().toLower();
        if (suffix == QStringLiteral("*") || suffix == QStringLiteral("*.*"))
            return {};
        if (suffix.startsWith(QStringLiteral("*.")))
            suffix.remove(0, 2);
        else if (suffix.startsWith(QLatin1Char('*')))
            suffix.remove(0, 1);
        if (suffix.startsWith(QLatin1Char('.')))
            suffix.remove(0, 1);
        if (!suffix.isEmpty() && !suffix.contains(QLatin1Char('*')) && !suffixes.contains(suffix))
            suffixes.append(suffix);
    }
    return suffixes;
}

bool excludedSearchDirectory(const QString& name) {
    const auto lower = name.toLower();
    return lower == QStringLiteral(".git") || lower == QStringLiteral(".codegraph") ||
           lower == QStringLiteral(".idea") || lower == QStringLiteral("node_modules") ||
           lower == QStringLiteral("build") || lower.startsWith(QStringLiteral("build-"));
}

GlobalSearchResults searchProject(const QString& root, const QString& query, bool fuzzy,
                                  bool caseSensitive, const QString& suffixFilter,
                                  const std::shared_ptr<std::atomic_bool>& cancelled) {
    constexpr qsizetype maxFileSize = 2 * 1024 * 1024;
    constexpr qsizetype maxResults = 1000;
    GlobalSearchResults results;
    const auto suffixes = searchSuffixes(suffixFilter);
    QStringList directories{QDir(root).absolutePath()};

    while (!directories.isEmpty()) {
        if (cancelled->load())
            return results;
        const auto directoryPath = directories.takeLast();
        const QDir directory(directoryPath);
        const auto entries = directory.entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto& entry : entries) {
            if (cancelled->load())
                return results;
            if (entry.isSymLink())
                continue;
            if (entry.isDir()) {
                if (!excludedSearchDirectory(entry.fileName()))
                    directories.append(entry.absoluteFilePath());
                continue;
            }
            if (!entry.isFile() || entry.size() > maxFileSize)
                continue;
            if (!suffixes.isEmpty() && !suffixes.contains(entry.suffix().toLower()))
                continue;

            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const auto bytes = file.readAll();
            if (bytes.contains('\0'))
                continue;
            QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            const QString contents =
                decoder.decode(bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes);
            if (decoder.hasError())
                continue;

            const auto lines = contents.split(QLatin1Char('\n'));
            for (qsizetype index = 0; index < lines.size(); ++index) {
                if (cancelled->load())
                    return results;
                auto line = lines.at(index);
                if (line.endsWith(QLatin1Char('\r')))
                    line.chop(1);
                int start = -1;
                int end = -1;
                if (!searchLine(line, query, fuzzy, caseSensitive, &start, &end))
                    continue;

                const auto trimmed = line.trimmed();
                QString snippet = trimmed;
                if (snippet.size() > 180) {
                    const int offset = qMax(0, start - 50);
                    snippet = QStringLiteral("…") + snippet.mid(offset, 180) +
                              (offset + 180 < trimmed.size() ? QStringLiteral("…") : QString());
                }
                ++results.totalMatches;
                if (results.matches.size() < maxResults) {
                    results.matches.append({entry.absoluteFilePath(), static_cast<int>(index) + 1,
                                            start, end, snippet});
                } else {
                    results.truncated = true;
                }
            }
        }
    }
    return results;
}

QString localized(bool english, const char* chinese, const char* englishText) {
    return QString::fromUtf8(english ? englishText : chinese);
}

QString fuzzyScintillaPattern(const QString& query) {
    const QString special = QStringLiteral("\\.^$|?*+()[]{}");
    QString pattern;
    for (qsizetype index = 0; index < query.size(); ++index) {
        const auto character = query.at(index);
        if (special.contains(character))
            pattern += QLatin1Char('\\');
        pattern += character;
        if (index + 1 < query.size())
            pattern += QStringLiteral(".*");
    }
    return pattern;
}

int countEditorMatches(CodeEditor* editor, const QString& pattern, bool regularExpression,
                       bool caseSensitive, int currentLine, int currentIndex, int* currentMatch) {
    if (!editor || !currentMatch)
        return 0;

    const bool hadSelection = editor->hasSelectedText();
    int savedFromLine = 0;
    int savedFromIndex = 0;
    int savedToLine = 0;
    int savedToIndex = 0;
    if (hadSelection) {
        editor->getSelection(&savedFromLine, &savedFromIndex, &savedToLine, &savedToIndex);
    } else {
        editor->getCursorPosition(&savedFromLine, &savedFromIndex);
    }

    *currentMatch = 0;
    int count = 0;
    editor->setCursorPosition(0, 0);
    while (editor->findFirst(pattern, regularExpression, caseSensitive, false, false, true)) {
        int fromLine = 0;
        int fromIndex = 0;
        int toLine = 0;
        int toIndex = 0;
        editor->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        if (fromLine < 0 || fromIndex < 0 || (fromLine == toLine && fromIndex == toIndex))
            break;
        if (fromLine == currentLine && fromIndex == currentIndex)
            *currentMatch = count + 1;
        ++count;
        editor->setCursorPosition(toLine, toIndex);
    }

    if (hadSelection)
        editor->setSelection(savedFromLine, savedFromIndex, savedToLine, savedToIndex);
    else
        editor->setCursorPosition(savedFromLine, savedFromIndex);
    return count;
}

class GlobalSearchDialog final : public QDialog {
  public:
    using OpenMatch = std::function<void(const GlobalSearchMatch&)>;
    using SelectMatch = std::function<void(const QString&)>;

    GlobalSearchDialog(QString root, bool english, OpenMatch openMatch, SelectMatch selectMatch,
                       QWidget* parent)
        : QDialog(parent), m_root(std::move(root)), m_english(english),
          m_openMatch(std::move(openMatch)), m_selectMatch(std::move(selectMatch)) {
        setObjectName(QStringLiteral("globalSearchDialog"));
        setWindowTitle(localized(m_english, "全局搜索", "Find in Files"));
        resize(850, 560);

        auto* layout = new QVBoxLayout(this);
        auto* queryRow = new QHBoxLayout;
        m_query = new QLineEdit(this);
        m_query->setObjectName(QStringLiteral("globalSearchQuery"));
        m_query->setPlaceholderText(localized(m_english, "输入要查找的内容", "Search text"));
        queryRow->addWidget(m_query, 1);
        m_searchButton = new QPushButton(localized(m_english, "搜索", "Search"), this);
        m_searchButton->setObjectName(QStringLiteral("globalSearchButton"));
        queryRow->addWidget(m_searchButton);
        layout->addLayout(queryRow);

        auto* options = new QHBoxLayout;
        auto* modeLabel = new QLabel(localized(m_english, "匹配：", "Match:"), this);
        options->addWidget(modeLabel);
        m_mode = new QComboBox(this);
        m_mode->setObjectName(QStringLiteral("globalSearchMode"));
        m_mode->addItem(localized(m_english, "精确", "Exact"), QStringLiteral("exact"));
        m_mode->addItem(localized(m_english, "模糊", "Fuzzy"), QStringLiteral("fuzzy"));
        options->addWidget(m_mode);
        m_caseSensitive = new QCheckBox(localized(m_english, "区分大小写", "Match case"), this);
        m_caseSensitive->setObjectName(QStringLiteral("globalSearchCaseSensitive"));
        options->addWidget(m_caseSensitive);
        options->addSpacing(8);
        options->addWidget(new QLabel(localized(m_english, "文件后缀：", "File types:"), this));
        m_suffix = new QLineEdit(this);
        m_suffix->setObjectName(QStringLiteral("globalSearchSuffix"));
        m_suffix->setPlaceholderText(
            localized(m_english, "全部文件，例如 cpp, h", "All files, e.g. cpp, h"));
        m_suffix->setClearButtonEnabled(true);
        options->addWidget(m_suffix, 1);
        layout->addLayout(options);

        m_status = new QLabel(localized(m_english, "输入内容开始搜索项目文件。",
                                        "Enter text to search project files."),
                              this);
        m_status->setObjectName(QStringLiteral("globalSearchStatus"));
        layout->addWidget(m_status);
        auto* content = new QSplitter(Qt::Horizontal, this);
        content->setObjectName(QStringLiteral("globalSearchSplitter"));
        content->setHandleWidth(8);
        content->setChildrenCollapsible(false);
        m_results = new QListWidget(content);
        m_results->setObjectName(QStringLiteral("globalSearchResults"));
        m_results->setMinimumWidth(220);
        m_results->setSelectionMode(QAbstractItemView::SingleSelection);
        auto* previewPanel = new QWidget(content);
        previewPanel->setMinimumWidth(280);
        auto* previewLayout = new QVBoxLayout(previewPanel);
        previewLayout->setContentsMargins(5, 0, 0, 0);
        previewLayout->setSpacing(4);
        m_previewPath = new QLabel(localized(m_english, "选择结果查看附近代码。",
                                             "Select a result to preview nearby code."),
                                   previewPanel);
        m_previewPath->setObjectName(QStringLiteral("globalSearchPreviewPath"));
        m_previewPath->setTextFormat(Qt::PlainText);
        m_previewPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
        previewLayout->addWidget(m_previewPath);
        m_preview = new QPlainTextEdit(previewPanel);
        m_preview->setObjectName(QStringLiteral("globalSearchPreview"));
        m_preview->setReadOnly(true);
        m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_preview->setPlaceholderText(localized(m_english, "代码预览", "Code preview"));
        previewLayout->addWidget(m_preview, 1);
        content->setSizes({460, 390});
        layout->addWidget(content, 1);
        auto* bottom = new QHBoxLayout;
        bottom->addStretch();
        auto* close = new QPushButton(localized(m_english, "关闭", "Close"), this);
        bottom->addWidget(close);
        layout->addLayout(bottom);

        m_searchDebounce.setSingleShot(true);
        m_searchDebounce.setInterval(220);
        connect(&m_searchDebounce, &QTimer::timeout, this, [this] { runSearch(); });
        connect(m_searchButton, &QPushButton::clicked, this, [this] { runSearch(); });
        connect(m_query, &QLineEdit::returnPressed, this, [this] { runSearch(); });
        connect(m_query, &QLineEdit::textChanged, this, [this] { scheduleSearch(); });
        connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { scheduleSearch(); });
        connect(m_caseSensitive, &QCheckBox::toggled, this, [this] { scheduleSearch(); });
        connect(m_suffix, &QLineEdit::textChanged, this, [this] { scheduleSearch(); });
        connect(m_results, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem* current) {
                    previewResult(current);
                    updateMatchStatus();
                    if (current)
                        m_selectMatch(current->data(Qt::UserRole).toString());
                });
        connect(m_results, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* item) { openResult(item); });
        connect(close, &QPushButton::clicked, this, &QDialog::close);
    }

    void setRoot(const QString& root) {
        if (m_root == root)
            return;
        m_root = root;
        scheduleSearch();
    }
    void setInitialQuery(const QString& query) {
        if (!query.isEmpty())
            m_query->setText(query);
    }
    QLineEdit* queryInput() const { return m_query; }

  private:
    void scheduleSearch() {
        ++m_searchRequest;
        if (m_cancelSearch)
            m_cancelSearch->store(true);
        m_totalMatches = 0;
        m_truncated = false;
        m_results->clear();
        previewResult(nullptr);
        m_searchButton->setEnabled(true);
        m_status->setText(m_query->text().isEmpty()
                              ? localized(m_english, "输入内容开始搜索项目文件。",
                                          "Enter text to search project files.")
                              : localized(m_english, "正在搜索…", "Searching…"));
        m_searchDebounce.start();
    }

    void runSearch() {
        m_searchDebounce.stop();
        const int request = ++m_searchRequest;
        if (m_cancelSearch)
            m_cancelSearch->store(true);
        m_cancelSearch.reset();
        const auto query = m_query->text();
        if (query.isEmpty()) {
            m_results->clear();
            previewResult(nullptr);
            m_status->setText(localized(m_english, "输入内容开始搜索项目文件。",
                                        "Enter text to search project files."));
            m_searchButton->setEnabled(true);
            return;
        }
        if (m_root.isEmpty() || !QFileInfo(m_root).isDir()) {
            m_status->setText(localized(m_english, "请先打开一个项目。", "Open a project first."));
            m_searchButton->setEnabled(true);
            return;
        }

        m_totalMatches = 0;
        m_truncated = false;
        m_results->clear();
        previewResult(nullptr);
        m_status->setText(localized(m_english, "正在搜索…", "Searching…"));
        m_searchButton->setEnabled(false);
        const auto root = m_root;
        const bool fuzzy = m_mode->currentData().toString() == QStringLiteral("fuzzy");
        const bool caseSensitive = m_caseSensitive->isChecked();
        const auto suffix = m_suffix->text();
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        m_cancelSearch = cancelled;
        auto* watcher = new QFutureWatcher<GlobalSearchResults>(this);
        connect(watcher, &QFutureWatcher<GlobalSearchResults>::finished, this,
                [this, watcher, request] {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    if (request != m_searchRequest)
                        return;
                    m_cancelSearch.reset();
                    m_searchButton->setEnabled(true);
                    m_totalMatches = result.totalMatches;
                    m_truncated = result.truncated;
                    QDir rootDirectory(m_root);
                    for (const auto& match : result.matches) {
                        const auto relative =
                            displayPath(rootDirectory.relativeFilePath(match.path));
                        auto* item = new QListWidgetItem(QStringLiteral("%1:%2    %3")
                                                             .arg(relative)
                                                             .arg(match.line)
                                                             .arg(match.snippet),
                                                         m_results);
                        item->setData(Qt::UserRole, match.path);
                        item->setData(Qt::UserRole + 1, match.line);
                        item->setData(Qt::UserRole + 2, match.column);
                        item->setData(Qt::UserRole + 3, match.endColumn);
                        item->setToolTip(match.path + QStringLiteral(":%1").arg(match.line));
                    }
                    if (result.matches.isEmpty())
                        m_status->setText(
                            localized(m_english, "未找到匹配内容。", "No matches found."));
                    if (!result.matches.isEmpty()) {
                        m_results->setCurrentRow(0);
                        updateMatchStatus();
                    }
                });
        watcher->setFuture(
            QtConcurrent::run([root, query, fuzzy, caseSensitive, suffix, cancelled] {
                return searchProject(root, query, fuzzy, caseSensitive, suffix, cancelled);
            }));
    }

    void updateMatchStatus() {
        const int row = m_results->currentRow();
        if (row < 0 || m_totalMatches <= 0)
            return;
        QString status = QStringLiteral("%1/%2").arg(row + 1).arg(m_totalMatches);
        status += m_truncated ? localized(m_english, " 个匹配结果（列表仅显示前 1000 条）",
                                          " matches (showing the first 1,000)")
                              : localized(m_english, " 个匹配结果", " matches");
        m_status->setText(status);
    }

    void previewResult(QListWidgetItem* item) {
        if (!item) {
            m_previewPath->clear();
            m_preview->clear();
            return;
        }

        const auto path = item->data(Qt::UserRole).toString();
        const int matchLine = item->data(Qt::UserRole + 1).toInt() - 1;
        m_previewPath->setText(displayPath(QDir(m_root).relativeFilePath(path)) +
                               QStringLiteral(":%1").arg(matchLine + 1));
        m_previewPath->setToolTip(path);

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 5 * 1024 * 1024) {
            m_preview->setPlainText(localized(m_english, "无法读取文件或文件超过 5 MiB。",
                                              "File is unreadable or larger than 5 MiB."));
            return;
        }
        const auto bytes = file.readAll();
        if (bytes.contains('\0')) {
            m_preview->setPlainText(
                localized(m_english, "二进制文件无法预览。", "Binary files cannot be previewed."));
            return;
        }
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString contents =
            decoder.decode(bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes);
        if (decoder.hasError()) {
            m_preview->setPlainText(localized(m_english, "文件不是有效的 UTF-8 文本。",
                                              "File is not valid UTF-8 text."));
            return;
        }

        auto lines = contents.split(QLatin1Char('\n'));
        for (auto& line : lines)
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
        if (lines.isEmpty())
            lines.append(QString());
        const int targetLine = qBound(0, matchLine, static_cast<int>(lines.size()) - 1);
        const int firstLine = qMax(0, targetLine - 5);
        const int lastLine = qMin(static_cast<int>(lines.size()) - 1, targetLine + 5);
        const int lineNumberWidth = QString::number(lastLine + 1).size();
        QStringList previewLines;
        for (int line = firstLine; line <= lastLine; ++line) {
            previewLines.append(
                QStringLiteral("%1 │ %2").arg(line + 1, lineNumberWidth).arg(lines.at(line)));
        }
        m_preview->setPlainText(previewLines.join(QLatin1Char('\n')));

        QTextCursor cursor(m_preview->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor,
                            targetLine - firstLine);
        cursor.select(QTextCursor::LineUnderCursor);
        QTextCharFormat format;
        format.setBackground(QColor("#344d72"));
        format.setForeground(QColor("#dfe1e5"));
        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;
        selection.format = format;
        m_preview->setExtraSelections({selection});
        m_preview->setTextCursor(cursor);
    }

    void openResult(QListWidgetItem* item) {
        if (!item)
            return;
        GlobalSearchMatch match;
        match.path = item->data(Qt::UserRole).toString();
        match.line = item->data(Qt::UserRole + 1).toInt();
        match.column = item->data(Qt::UserRole + 2).toInt();
        match.endColumn = item->data(Qt::UserRole + 3).toInt();
        m_openMatch(match);
    }

    QString m_root;
    bool m_english;
    OpenMatch m_openMatch;
    SelectMatch m_selectMatch;
    QTimer m_searchDebounce;
    std::shared_ptr<std::atomic_bool> m_cancelSearch;
    int m_searchRequest = 0;
    int m_totalMatches = 0;
    bool m_truncated = false;
    QLineEdit* m_query = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_caseSensitive = nullptr;
    QLineEdit* m_suffix = nullptr;
    QPushButton* m_searchButton = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_previewPath = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QListWidget* m_results = nullptr;
};

} // namespace

MainWindow::MainWindow(bool english, QWidget* parent) : QMainWindow(parent), m_english(english) {
    initializeIdea4gitResources();
    setObjectName(QStringLiteral("idea4gitWindow"));
    resize(1500, 940);
    setMinimumSize(1080, 700);
    setWindowTitle(QStringLiteral(IDEA4GIT_APP_NAME));
    setWindowIcon(QIcon(QStringLiteral(":/icons/idea4git-icon.png")));
    buildMenus();
    buildWorkflowBar();
    buildWorkspace();
    applyTheme();
    QSettings settings;
    if (settings.contains("window/geometry"))
        restoreGeometry(settings.value("window/geometry").toByteArray());
    if (settings.contains("window/horizontal"))
        m_horizontal->restoreState(settings.value("window/horizontal").toByteArray());
    if (settings.contains("window/vertical"))
        m_vertical->restoreState(settings.value("window/vertical").toByteArray());
    setProjectPanelVisible(settings.value("window/projectPanelVisible", true).toBool());
    setCommitPanelVisible(settings.value("window/commitPanelVisible", true).toBool());
    setHistoryPanelVisible(settings.value("window/historyPanelVisible", true).toBool());
    updateActions();
}

QString MainWindow::text(const char* chinese, const char* english) const {
    return QString::fromUtf8(m_english ? english : chinese);
}

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(text("文件", "File"));
    auto* open = file->addAction(icon("open"), text("打开项目…", "Open project…"));
    open->setShortcut(QKeySequence("Ctrl+O"));
    connect(open, &QAction::triggered, this, &MainWindow::chooseProject);
    auto* openFile =
        file->addAction(icon("file", QColor("#7eabdf")), text("打开文件…", "Open file…"));
    openFile->setObjectName(QStringLiteral("openFileAction"));
    openFile->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(openFile, &QAction::triggered, this, &MainWindow::chooseFile);
    m_saveAction = file->addAction(icon("save"), text("保存", "Save"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, [this] {
        if (currentEditor())
            saveEditor(currentEditor());
    });
    file->addSeparator();
    auto* quit = file->addAction(text("退出", "Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(text("编辑", "Edit"));
    const auto editorAction = [this, edit](const QString& label, const QKeySequence& shortcut,
                                           void (QsciScintilla::*method)()) {
        auto* action = edit->addAction(label);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, method] {
            if (currentEditor())
                (currentEditor()->*method)();
        });
    };
    editorAction(text("撤销", "Undo"), QKeySequence::Undo, &QsciScintilla::undo);
    editorAction(text("重做", "Redo"), QKeySequence::Redo, &QsciScintilla::redo);
    edit->addSeparator();
    m_findAction = edit->addAction(icon("search"), text("查找…", "Find…"));
    m_findAction->setObjectName(QStringLiteral("findAction"));
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, [this] {
        m_findBar->show();
        if (currentEditor() && !currentEditor()->selectedText().isEmpty() &&
            m_find->text().isEmpty())
            m_find->setText(currentEditor()->selectedText());
        m_find->setFocus();
        m_find->selectAll();
    });
    m_globalFindAction = edit->addAction(icon("search"), text("在项目中查找…", "Find in Files…"));
    m_globalFindAction->setObjectName(QStringLiteral("globalFindAction"));
    m_globalFindAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(m_globalFindAction, &QAction::triggered, this, &MainWindow::showGlobalSearch);
    auto* view = menuBar()->addMenu(text("视图", "View"));
    auto* log = view->addAction(text("Git 日志", "Git log"));
    connect(log, &QAction::triggered, this, [this] { m_bottom->setCurrentIndex(0); });
    view->addSeparator();
    m_projectPanelAction = view->addAction(text("项目面板", "Project panel"));
    m_projectPanelAction->setObjectName(QStringLiteral("toggleProjectPanelAction"));
    m_projectPanelAction->setCheckable(true);
    m_projectPanelAction->setChecked(true);
    connect(m_projectPanelAction, &QAction::toggled, this, &MainWindow::setProjectPanelVisible);
    m_commitPanelAction = view->addAction(text("提交面板", "Commit panel"));
    m_commitPanelAction->setObjectName(QStringLiteral("toggleCommitPanelAction"));
    m_commitPanelAction->setCheckable(true);
    m_commitPanelAction->setChecked(true);
    connect(m_commitPanelAction, &QAction::toggled, this, &MainWindow::setCommitPanelVisible);
    m_historyPanelAction = view->addAction(text("Git 日志面板", "Git log panel"));
    m_historyPanelAction->setObjectName(QStringLiteral("toggleHistoryPanelAction"));
    m_historyPanelAction->setCheckable(true);
    m_historyPanelAction->setChecked(true);
    connect(m_historyPanelAction, &QAction::toggled, this, &MainWindow::setHistoryPanelVisible);
    auto* git = menuBar()->addMenu(QStringLiteral("Git"));
    buildAdvancedActions(git);
    git->addSeparator();
    auto* remoteManager = git->addAction(text("管理远程…", "Manage Remotes…"));
    remoteManager->setObjectName(QStringLiteral("manageRemotesAction"));
    connect(remoteManager, &QAction::triggered, this, &MainWindow::showRemotes);
    auto* credentialManager = git->addAction(text("Git 凭据…", "Git Credentials…"));
    credentialManager->setObjectName(QStringLiteral("gitCredentialsAction"));
    connect(credentialManager, &QAction::triggered, this, &MainWindow::showCredentials);
    git->addSeparator();
    m_refreshAction = git->addAction(icon("refresh"), text("刷新状态", "Refresh status"));
    m_refreshAction->setShortcut(QKeySequence("F5"));
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshRepository);
    m_fetchAction = git->addAction(icon("fetch"), text("获取", "Fetch"));
    m_pullAction = git->addAction(icon("pull"), text("拉取（仅快进）", "Pull (fast-forward only)"));
    m_pushAction = git->addAction(icon("push"), text("推送", "Push"));
    connect(m_fetchAction, &QAction::triggered, this, [this] { remoteOperation("fetch"); });
    connect(m_pullAction, &QAction::triggered, this, [this] { remoteOperation("pull"); });
    connect(m_pushAction, &QAction::triggered, this, &MainWindow::showPushPreview);
    git->addSeparator();
    m_cancelAction = git->addAction(text("取消 Git 操作", "Cancel Git operation"));
    connect(m_cancelAction, &QAction::triggered, this, [this] {
        if (m_git) {
            m_git->cancelAll();
            m_refreshing = false;
            m_activity->setText(text("已请求取消；请刷新以确认仓库状态",
                                     "Cancellation requested; refresh to verify state"));
        }
    });
    auto* settings = menuBar()->addMenu(text("设置", "Settings"));
    auto* languageSettings = settings->addAction(text("界面语言…", "Interface language…"));
    languageSettings->setObjectName(QStringLiteral("languageSettingsAction"));
    connect(languageSettings, &QAction::triggered, this, &MainWindow::showSettings);
    auto* help = menuBar()->addMenu(text("帮助", "Help"));
    connect(
        help->addAction(text("关于 idea4git", "About idea4git")), &QAction::triggered, this,
        [this] {
            QMessageBox::about(
                this, text("关于 idea4git", "About idea4git"),
                text(
                    "idea4git %1 · 跨平台桌面版\nC++17 / Qt 6 Widgets / QScintilla / Git CLI\n\n"
                    "支持版本比较、三方冲突解决与 Git 高级工作流。\n"
                    "本项目按 GPL-3.0-only 发布；QScintilla 开源版受 GPLv3 约束。",
                    "idea4git %1 · Cross-platform desktop app\nC++17 / Qt 6 Widgets / QScintilla / "
                    "Git CLI\n\n"
                    "Revision comparison, three-way conflict resolution and advanced Git "
                    "workflows.\n"
                    "This project is GPL-3.0-only; open-source QScintilla is licensed under "
                    "GPLv3.")
                    .arg(QCoreApplication::applicationVersion()));
        });
    auto* toolbar = addToolBar(QStringLiteral("Workspace"));
    toolbar->setObjectName(QStringLiteral("workspaceToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    auto* brand = new QLabel(QStringLiteral("  GL  "), toolbar);
    brand->setObjectName(QStringLiteral("brand"));
    toolbar->addWidget(brand);
    auto* name = new QLabel(QStringLiteral("  idea4git  "), toolbar);
    name->setObjectName(QStringLiteral("brandName"));
    toolbar->addWidget(name);
    toolbar->addSeparator();
    m_projectButton = new QToolButton(toolbar);
    m_projectButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_projectButton->setIcon(icon("folder", QColor("#c8a46c")));
    m_projectButton->setText(text("打开项目", "Open project"));
    connect(m_projectButton, &QToolButton::clicked, this, &MainWindow::chooseProject);
    toolbar->addWidget(m_projectButton);
    auto* fileButton = new QToolButton(toolbar);
    fileButton->setObjectName(QStringLiteral("openFileButton"));
    fileButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    fileButton->setIcon(icon("file", QColor("#7eabdf")));
    fileButton->setText(text("打开文件", "Open file"));
    fileButton->setToolTip(text("打开单个文件（如 Markdown 文档）",
                                "Open a single file, such as a Markdown document"));
    connect(fileButton, &QToolButton::clicked, this, &MainWindow::chooseFile);
    toolbar->addWidget(fileButton);
    toolbar->addSeparator();
    m_branch = new QToolButton(toolbar);
    m_branch->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_branch->setIcon(icon("branch", QColor("#a3b8e7")));
    m_branch->setText(text("无仓库", "No repository"));
    connect(m_branch, &QToolButton::clicked, this, &MainWindow::showBranches);
    toolbar->addWidget(m_branch);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addAction(m_saveAction);
    toolbar->addAction(m_refreshAction);
    toolbar->addSeparator();
    toolbar->addAction(m_fetchAction);
    toolbar->addAction(m_pullAction);
    toolbar->addAction(m_pushAction);
    toolbar->addSeparator();
    auto* language = new QToolButton(toolbar);
    language->setIcon(icon("settings"));
    language->setToolTip(text("设置", "Settings"));
    connect(language, &QToolButton::clicked, this, &MainWindow::showSettings);
    toolbar->addWidget(language);
}

void MainWindow::buildWorkspace() {
    auto* workspace = new QWidget(this);
    auto* outer = new QHBoxLayout(workspace);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* rail = new QWidget(workspace);
    rail->setObjectName(QStringLiteral("activityRail"));
    rail->setFixedWidth(64);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(4, 12, 4, 10);
    railLayout->setSpacing(12);
    m_projectPanelRailButton = toolButton("folder", text("项目", "Project"), rail);
    m_projectPanelRailButton->setObjectName(QStringLiteral("projectPanelToggleButton"));
    m_projectPanelRailButton->setText(text("项目", "Project"));
    m_projectPanelRailButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_projectPanelRailButton->setCheckable(true);
    m_projectPanelRailButton->setChecked(true);
    m_projectPanelRailButton->setProperty("active", true);
    auto* projectTool = m_projectPanelRailButton;
    railLayout->addWidget(projectTool);
    connect(projectTool, &QToolButton::clicked, this, [this](bool visible) {
        setProjectPanelVisible(visible);
        if (visible)
            m_tree->setFocus();
    });
    m_commitPanelRailButton = toolButton("git", text("提交", "Commit"), rail);
    m_commitPanelRailButton->setObjectName(QStringLiteral("commitPanelToggleButton"));
    m_commitPanelRailButton->setText(text("提交", "Commit"));
    m_commitPanelRailButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_commitPanelRailButton->setCheckable(true);
    m_commitPanelRailButton->setChecked(true);
    m_commitPanelRailButton->setProperty("active", true);
    auto* gitTool = m_commitPanelRailButton;
    railLayout->addWidget(gitTool);
    connect(gitTool, &QToolButton::clicked, this, [this](bool visible) {
        setCommitPanelVisible(visible);
        if (visible)
            m_changes->setFocus();
    });
    m_historyPanelRailButton = toolButton("history", text("Git 日志", "Git log"), rail);
    m_historyPanelRailButton->setObjectName(QStringLiteral("historyPanelToggleButton"));
    m_historyPanelRailButton->setText(text("日志", "Log"));
    m_historyPanelRailButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_historyPanelRailButton->setCheckable(true);
    m_historyPanelRailButton->setChecked(true);
    m_historyPanelRailButton->setProperty("active", true);
    auto* logTool = m_historyPanelRailButton;
    railLayout->addWidget(logTool);
    connect(logTool, &QToolButton::clicked, this, [this](bool visible) {
        setHistoryPanelVisible(visible);
        if (visible)
            m_bottom->setCurrentIndex(0);
    });
    railLayout->addStretch();
    auto* settingsTool = toolButton("settings", text("设置", "Settings"), rail);
    connect(settingsTool, &QToolButton::clicked, this, &MainWindow::showSettings);
    railLayout->addWidget(settingsTool);
    outer->addWidget(rail);

    m_horizontal = new QSplitter(Qt::Horizontal, workspace);
    m_horizontal->setObjectName(QStringLiteral("mainHorizontalSplitter"));
    auto* project = new QWidget(m_horizontal);
    m_projectPanel = project;
    project->setObjectName(QStringLiteral("projectPanel"));
    project->setMinimumWidth(180);
    auto* projectLayout = new QVBoxLayout(project);
    projectLayout->setContentsMargins(0, 0, 0, 0);
    projectLayout->setSpacing(0);
    auto* projectHeader = new QWidget(project);
    projectHeader->setObjectName(QStringLiteral("projectTreeHeader"));
    auto* projectHeaderLayout = new QHBoxLayout(projectHeader);
    projectHeaderLayout->setContentsMargins(0, 0, 6, 0);
    projectHeaderLayout->setSpacing(2);
    auto* projectTitle = new QLabel(text("项目", "Project"), projectHeader);
    projectTitle->setObjectName(QStringLiteral("panelTitle"));
    projectHeaderLayout->addWidget(projectTitle, 1);
    auto* hideProjectPanel =
        toolButton("hideLeft", text("隐藏项目面板", "Hide Project panel"), projectHeader);
    hideProjectPanel->setObjectName(QStringLiteral("hideProjectPanelButton"));
    hideProjectPanel->setFixedSize(26, 28);
    hideProjectPanel->setIconSize(QSize(16, 16));
    projectHeaderLayout->addWidget(hideProjectPanel);
    connect(hideProjectPanel, &QToolButton::clicked, this,
            [this] { setProjectPanelVisible(false); });
    m_revealCurrentFileButton =
        toolButton("locate", text("在项目树中定位当前文件", "Select current file in Project view"),
                   projectHeader);
    m_revealCurrentFileButton->setObjectName(QStringLiteral("selectCurrentFileButton"));
    m_revealCurrentFileButton->setFixedSize(28, 28);
    m_revealCurrentFileButton->setIconSize(QSize(16, 16));
    m_revealCurrentFileButton->setEnabled(false);
    projectHeaderLayout->addWidget(m_revealCurrentFileButton);
    connect(m_revealCurrentFileButton, &QToolButton::clicked, this, [this] {
        if (auto* editor = currentEditor())
            selectProjectPath(editor->filePath());
    });
    projectLayout->addWidget(projectHeader);
    auto* filter = new QLineEdit(project);
    filter->setObjectName(QStringLiteral("projectTreeFilter"));
    m_treeFilter = filter;
    filter->setPlaceholderText(text("按文件名筛选…", "Filter filenames…"));
    filter->addAction(icon("search"), QLineEdit::LeadingPosition);
    filter->setClearButtonEnabled(true);
    projectLayout->addWidget(filter);
    m_projectModel = new ProjectModel(this);
    m_projectFilter = new ProjectFilter(this);
    m_projectFilter->setSourceModel(m_projectModel);
    m_tree = new ProjectTreeView(project);
    m_tree->setObjectName(QStringLiteral("projectTree"));
    m_tree->setModel(m_projectFilter);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(false);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(18);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int column = 1; column < 4; ++column)
        m_tree->hideColumn(column);
    connect(m_tree, &QTreeView::expanded, this,
            [this](const QModelIndex& index) { trackProjectTreeExpansion(index, true); });
    connect(m_tree, &QTreeView::collapsed, this,
            [this](const QModelIndex& index) { trackProjectTreeExpansion(index, false); });
    connect(m_projectModel, &QFileSystemModel::directoryLoaded, this, [this](const QString& path) {
        const auto loadedPath = QDir::cleanPath(QDir::fromNativeSeparators(path));
        m_loadedProjectTreeDirectories.insert(loadedPath);
        restorePendingProjectTreeExpansions();
    });
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);
    projectLayout->addWidget(m_tree, 1);
    auto* treeHint =
        new QLabel(text("单击文件夹展开/折叠，双击文件打开",
                        "Click folders to expand/collapse; double-click files to open"),
                   project);
    treeHint->setObjectName(QStringLiteral("subtleHint"));
    projectLayout->addWidget(treeHint);
    connect(filter, &QLineEdit::textChanged, this, [this](const QString& pattern) {
        m_projectModel->setNameFilters(pattern.isEmpty() ? QStringList{}
                                                         : QStringList{"*" + pattern + "*"});
        m_projectModel->setNameFilterDisables(false);
    });
    connect(m_tree, &QTreeView::clicked, this, [this](const QModelIndex& proxyIndex) {
        const auto sourceIndex = m_projectFilter->mapToSource(proxyIndex);
        if (!sourceIndex.isValid() || !m_projectModel->isDir(sourceIndex))
            return;
        m_tree->setExpanded(proxyIndex, !m_tree->isExpanded(proxyIndex));
    });
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& proxyIndex) {
        const auto index = m_projectFilter->mapToSource(proxyIndex);
        if (!m_projectModel->isDir(index)) {
            openFile(m_projectModel->filePath(index));
            return;
        }

        const auto path = m_projectModel->filePath(index);
        QTimer::singleShot(0, this, [this, path] {
            const auto sourceIndex = m_projectModel->index(path);
            const auto folderIndex = m_projectFilter->mapFromSource(sourceIndex);
            if (folderIndex.isValid())
                m_tree->setExpanded(folderIndex, true);
        });
    });

    m_vertical = new QSplitter(Qt::Vertical, m_horizontal);
    m_vertical->setObjectName(QStringLiteral("mainVerticalSplitter"));
    auto* top = new QSplitter(Qt::Horizontal, m_vertical);
    top->setObjectName(QStringLiteral("editorCommitSplitter"));
    auto* editorPanel = new QWidget(top);
    auto* editorLayout = new QVBoxLayout(editorPanel);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    m_editors = new QTabWidget(editorPanel);
    m_editors->setObjectName(QStringLiteral("editorTabs"));
    m_editors->setTabsClosable(true);
    m_editors->setMovable(true);
    m_editors->setDocumentMode(true);
    connect(m_editors, &QTabWidget::tabCloseRequested, this, &MainWindow::closeEditor);
    connect(m_editors, &QTabWidget::currentChanged, this, [this] {
        if (auto* editor = currentEditor())
            editor->focusCurrentView();
        updateEditorStatus();
        updateActions();
        if (m_revealCurrentFileButton)
            m_revealCurrentFileButton->setEnabled(currentEditor() != nullptr);
        if (m_findBar && m_findBar->isVisible() && m_find && !m_find->text().isEmpty())
            findText(true, true);
    });
    m_findBar = new QWidget(editorPanel);
    m_findBar->setObjectName(QStringLiteral("currentFindBar"));
    auto* findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(10, 5, 6, 5);
    m_find = new QLineEdit(m_findBar);
    m_find->setObjectName(QStringLiteral("currentFindInput"));
    m_find->setPlaceholderText(text("在当前文件中查找", "Find in current file"));
    findLayout->addWidget(m_find, 1);
    m_findMode = new QComboBox(m_findBar);
    m_findMode->setObjectName(QStringLiteral("currentSearchMode"));
    m_findMode->addItem(text("精确", "Exact"), QStringLiteral("exact"));
    m_findMode->addItem(text("模糊", "Fuzzy"), QStringLiteral("fuzzy"));
    m_findMode->setToolTip(text("精确匹配或按字符顺序模糊匹配", "Exact or ordered fuzzy matching"));
    findLayout->addWidget(m_findMode);
    m_findCaseSensitive = new QCheckBox(text("区分大小写", "Match case"), m_findBar);
    m_findCaseSensitive->setObjectName(QStringLiteral("currentSearchCaseSensitive"));
    m_findCaseSensitive->setToolTip(text("区分大小写", "Match case"));
    findLayout->addWidget(m_findCaseSensitive);
    m_findCount = new QLabel(QStringLiteral("0/0"), m_findBar);
    m_findCount->setObjectName(QStringLiteral("currentFindCount"));
    m_findCount->setMinimumWidth(38);
    m_findCount->setAlignment(Qt::AlignCenter);
    m_findCount->setToolTip(text("当前匹配序号 / 匹配总数", "Current match / total matches"));
    findLayout->addWidget(m_findCount);
    auto* previous = new QPushButton(text("上一个", "Previous"), m_findBar);
    auto* next = new QPushButton(text("下一个", "Next"), m_findBar);
    previous->setObjectName(QStringLiteral("findPreviousButton"));
    next->setObjectName(QStringLiteral("findNextButton"));
    auto* hide = toolButton("close", text("关闭查找", "Close find"), m_findBar);
    findLayout->addWidget(previous);
    findLayout->addWidget(next);
    findLayout->addWidget(hide);
    connect(previous, &QPushButton::clicked, this, [this] { findText(false); });
    connect(next, &QPushButton::clicked, this, [this] { findText(); });
    connect(m_find, &QLineEdit::textChanged, this, [this] { findText(true, true); });
    connect(m_findMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { findText(true, true); });
    connect(m_findCaseSensitive, &QCheckBox::toggled, this, [this] { findText(true, true); });
    connect(hide, &QToolButton::clicked, this, [this] {
        m_findBar->hide();
        if (currentEditor())
            currentEditor()->setFocus();
    });
    editorLayout->addWidget(m_findBar);
    m_findBar->hide();
    editorLayout->addWidget(m_editors, 1);
    m_breadcrumb =
        new QLabel(text("打开项目，开始浏览代码", "Open a project to browse code"), editorPanel);
    m_breadcrumb->setTextFormat(Qt::PlainText);
    m_breadcrumb->setObjectName(QStringLiteral("breadcrumb"));
    editorLayout->addWidget(m_breadcrumb);

    auto* commitPanel = new QWidget(top);
    m_commitPanel = commitPanel;
    commitPanel->setObjectName(QStringLiteral("commitPanel"));
    commitPanel->setMinimumWidth(270);
    commitPanel->setMaximumWidth(460);
    auto* commitLayout = new QVBoxLayout(commitPanel);
    commitLayout->setContentsMargins(0, 0, 0, 12);
    commitLayout->setSpacing(0);
    auto* changesHeader = new QWidget(commitPanel);
    auto* changesHeaderLayout = new QHBoxLayout(changesHeader);
    changesHeaderLayout->setContentsMargins(14, 10, 8, 10);
    auto* changesTitle = new QLabel(text("提交", "Commit"), changesHeader);
    changesTitle->setObjectName(QStringLiteral("sectionName"));
    changesHeaderLayout->addWidget(changesTitle);
    m_changeCount = new QLabel(QStringLiteral("0"), changesHeader);
    m_changeCount->setObjectName(QStringLiteral("countBadge"));
    changesHeaderLayout->addWidget(m_changeCount);
    changesHeaderLayout->addStretch();
    auto* hideCommitPanel =
        toolButton("hideRight", text("隐藏提交面板", "Hide Commit panel"), changesHeader);
    hideCommitPanel->setObjectName(QStringLiteral("hideCommitPanelButton"));
    hideCommitPanel->setFixedSize(26, 28);
    hideCommitPanel->setIconSize(QSize(16, 16));
    changesHeaderLayout->addWidget(hideCommitPanel);
    connect(hideCommitPanel, &QToolButton::clicked, this, [this] { setCommitPanelVisible(false); });
    commitLayout->addWidget(changesHeader);
    m_repoState = new QLabel(text("正在读取仓库…", "Reading repository…"), commitPanel);
    m_repoState->setObjectName(QStringLiteral("repoState"));
    m_repoState->setWordWrap(true);
    commitLayout->addWidget(m_repoState);
    m_changes = new QTreeWidget(commitPanel);
    m_changes->setObjectName(QStringLiteral("changesTree"));
    m_changes->setHeaderHidden(true);
    m_changes->setRootIsDecorated(true);
    m_changes->setIndentation(14);
    m_changes->setUniformRowHeights(true);
    m_changes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_changes->setToolTip(text("勾选要包含在本次提交中的文件；不会改变 Git 暂存区。",
                               "Check the files to include in this commit; this does not change "
                               "Git's index."));
    commitLayout->addWidget(m_changes, 1);
    connect(m_changes, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item) { showDiff(item); });
    m_changes->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_changes, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        auto* item = m_changes->itemAt(position);
        const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
        if (index < 0 || index >= m_status.files.size())
            return;
        m_changes->setCurrentItem(item);
        const auto& file = m_status.files[index];
        QMenu menu(this);
        auto* gitDiff = menu.addAction(text("查看 Git Diff", "Git Diff"));
        gitDiff->setObjectName(QStringLiteral("gitDiffAction"));
        gitDiff->setEnabled(!file.conflict());
        if (file.conflict())
            gitDiff->setToolTip(text("冲突文件请使用冲突解决器查看和处理。",
                                     "Use the conflict resolver to inspect conflicted files."));
        connect(gitDiff, &QAction::triggered, this, [this, file] { showChangeFileDiff(file); });
        menu.addSeparator();
        const auto sourcePath =
            existingWorkingTreeFile(m_repositoryRoot, file.path, file.originalPath);
        auto* editSource = menu.addAction(text("编辑源代码", "Edit Source"));
        editSource->setObjectName(QStringLiteral("editSourceAction"));
        editSource->setEnabled(!sourcePath.isEmpty());
        connect(editSource, &QAction::triggered, this,
                [this, sourcePath] { openFile(sourcePath); });
        menu.exec(m_changes->viewport()->mapToGlobal(position));
    });
    connect(m_changes, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateActions);
    connect(m_changes, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0 || !item)
            return;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0) {
            if (item->childCount() == 0 || item->data(0, Qt::UserRole + 2).toString().isEmpty())
                return;
            const bool selected = item->checkState(0) == Qt::Checked;
            const QSignalBlocker block(m_changes);
            for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
                auto* child = item->child(childIndex);
                const int fileIndex = child->data(0, Qt::UserRole).toInt();
                if (fileIndex < 0 || fileIndex >= m_status.files.size())
                    continue;
                const auto& file = m_status.files[fileIndex];
                const bool include = selected && !file.conflict();
                child->setCheckState(0, include ? Qt::Checked : Qt::Unchecked);
                if (include)
                    m_excludedCommitPaths.remove(file.path);
                else
                    m_excludedCommitPaths.insert(file.path);
            }
            syncChangeGroupCheckState(item);
            updateActions();
            return;
        }
        if (index >= m_status.files.size())
            return;
        const auto& file = m_status.files[index];
        if (file.conflict()) {
            const QSignalBlocker block(m_changes);
            item->setCheckState(0, Qt::Unchecked);
            return;
        }
        if (item->checkState(0) == Qt::Checked)
            m_excludedCommitPaths.remove(file.path);
        else
            m_excludedCommitPaths.insert(file.path);
        syncChangeGroupCheckState(item->parent());
        updateActions();
    });
    connect(m_changes, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const int i = item->data(0, Qt::UserRole).toInt();
        if (i >= 0 && i < m_status.files.size())
            openFile(repositoryFilePath(m_repositoryRoot, m_status.files[i].path));
    });
    auto* messageLabel = new QLabel(text("提交信息", "Commit message"), commitPanel);
    messageLabel->setObjectName(QStringLiteral("messageLabel"));
    commitLayout->addWidget(messageLabel);
    m_message = new QPlainTextEdit(commitPanel);
    m_message->setObjectName(QStringLiteral("commitMessage"));
    m_message->setPlaceholderText(text("描述这次更改…", "Describe these changes…"));
    m_message->setFixedHeight(88);
    commitLayout->addWidget(m_message);
    connect(m_message, &QPlainTextEdit::textChanged, this, &MainWindow::updateActions);
    m_amend = new QCheckBox(text("修订上一次提交（amend）", "Amend last commit"), commitPanel);
    m_amend->setObjectName(QStringLiteral("amendCheckbox"));
    commitLayout->addWidget(m_amend);
    m_commit = new QPushButton(text("提交所选文件", "Commit selected files"), commitPanel);
    m_commit->setObjectName(QStringLiteral("commitButton"));
    m_commit->setProperty("primary", true);
    m_commit->setMinimumHeight(34);
    m_commit->setShortcut(QKeySequence("Ctrl+Return"));
    m_commitAndPush = new QPushButton(text("提交并推送", "Commit and Push"), commitPanel);
    m_commitAndPush->setObjectName(QStringLiteral("commitAndPushButton"));
    m_commitAndPush->setMinimumHeight(34);
    auto* commitButtons = new QHBoxLayout;
    commitButtons->setContentsMargins(0, 0, 0, 0);
    commitButtons->setSpacing(6);
    commitButtons->addWidget(m_commit, 1);
    commitButtons->addWidget(m_commitAndPush, 1);
    commitLayout->addLayout(commitButtons);
    connect(m_commit, &QPushButton::clicked, this, [this] { commitChanges(false); });
    connect(m_commitAndPush, &QPushButton::clicked, this, [this] { commitChanges(true); });
    auto* commitHint = new QLabel(
        text("勾选要提交的文件 · Ctrl+Enter", "Select files to commit · Ctrl+Enter"), commitPanel);
    commitHint->setObjectName(QStringLiteral("commitHint"));
    commitLayout->addWidget(commitHint);
    top->setSizes({780, 316});
    top->setStretchFactor(0, 1);
    top->setStretchFactor(1, 0);

    m_bottom = new QTabWidget(m_vertical);
    m_bottom->setObjectName(QStringLiteral("bottomTabs"));
    m_bottom->setDocumentMode(true);
    auto* hideHistoryPanel =
        toolButton("hideDown", text("隐藏 Git 日志面板", "Hide Git log panel"), m_bottom);
    hideHistoryPanel->setObjectName(QStringLiteral("hideHistoryPanelButton"));
    hideHistoryPanel->setFixedSize(26, 24);
    hideHistoryPanel->setIconSize(QSize(16, 16));
    m_bottom->setCornerWidget(hideHistoryPanel, Qt::TopRightCorner);
    connect(hideHistoryPanel, &QToolButton::clicked, this,
            [this] { setHistoryPanelVisible(false); });
    auto* historyPanel = new QWidget(m_bottom);
    auto* historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(0);
    auto* historyFilters = new QHBoxLayout;
    historyFilters->setContentsMargins(8, 5, 8, 5);
    historyFilters->addWidget(new QLabel(text("分支：", "Branch:"), historyPanel));
    m_historyBranch = new QComboBox(historyPanel);
    m_historyBranch->setObjectName("historyBranchFilter");
    m_historyBranch->setMinimumWidth(170);
    historyFilters->addWidget(m_historyBranch);
    historyFilters->addSpacing(8);
    historyFilters->addWidget(new QLabel(text("用户：", "Author:"), historyPanel));
    m_historyAuthor = new QComboBox(historyPanel);
    m_historyAuthor->setObjectName("historyAuthorFilter");
    m_historyAuthor->setEditable(true);
    m_historyAuthor->setInsertPolicy(QComboBox::NoInsert);
    m_historyAuthor->setMinimumWidth(170);
    m_historyAuthor->addItem(text("全部用户", "All authors"), QString());
    historyFilters->addWidget(m_historyAuthor);
    historyFilters->addStretch();
    historyLayout->addLayout(historyFilters);

    m_historyAuthorTimer = new QTimer(this);
    m_historyAuthorTimer->setSingleShot(true);
    m_historyAuthorTimer->setInterval(350);
    connect(m_historyAuthorTimer, &QTimer::timeout, this, [this] { loadHistory(false); });
    connect(m_historyBranch, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { loadHistory(false); });
    connect(m_historyAuthor, &QComboBox::activated, this, [this] {
        m_historyAuthorTimer->stop();
        loadHistory(false);
    });
    connect(m_historyAuthor, &QComboBox::editTextChanged, this,
            [this] { m_historyAuthorTimer->start(); });
    connect(m_historyAuthor->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        m_historyAuthorTimer->stop();
        loadHistory(false);
    });

    auto* historySplit = new QSplitter(Qt::Horizontal, historyPanel);
    m_history = new QTableWidget(0, 4, historySplit);
    m_history->setObjectName(QStringLiteral("historyTable"));
    m_history->setHorizontalHeaderLabels({text("提交", "Commit"),
                                          text("说明 / 引用", "Message / refs"),
                                          text("作者", "Author"), text("时间", "Date")});
    m_history->verticalHeader()->hide();
    m_history->verticalHeader()->setDefaultSectionSize(31);
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_history->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_history, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        if (gitBusy() || !m_operations)
            return;
        QMenu menu(this);
        auto* compare =
            menu.addAction(text("比较所选的两个提交…", "Compare two selected commits…"));
        compare->setEnabled(m_history->selectionModel()->selectedRows().size() == 2);
        connect(compare, &QAction::triggered, this,
                [this] { QTimer::singleShot(0, this, [this] { showComparison(); }); });
        const int row = m_history->currentRow();
        if (row >= 0 && row < m_commits.size()) {
            const auto hash = m_commits[row].hash;
            for (auto kind :
                 {OperationKind::CherryPick, OperationKind::Revert, OperationKind::Reset}) {
                auto* action = menu.addAction(operationName(kind, m_english) + QStringLiteral("…"));
                action->setEnabled(m_pendingOperation == PendingOperation::None);
                connect(action, &QAction::triggered, this, [this, kind, hash] {
                    QTimer::singleShot(0, this,
                                       [this, kind, hash] { showAdvancedOperation(kind, hash); });
                });
            }
        }
        menu.exec(m_history->viewport()->mapToGlobal(point));
    });
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->setShowGrid(false);
    m_history->setAlternatingRowColors(false);
    m_history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_history->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_history->setColumnWidth(0, 110);
    m_history->setColumnWidth(2, 145);
    m_history->setColumnWidth(3, 145);
    auto* filesPanel = new QWidget(historySplit);
    auto* filesLayout = new QVBoxLayout(filesPanel);
    filesLayout->setContentsMargins(6, 4, 6, 4);
    filesLayout->setSpacing(4);
    auto* filesHeading = new QLabel(text("变更文件", "Changed files"), filesPanel);
    filesHeading->setObjectName("commitFilesHeading");
    filesLayout->addWidget(filesHeading);
    m_commitFiles = new QListWidget(filesPanel);
    m_commitFiles->setObjectName("commitFiles");
    m_commitFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commitFiles->setToolTip(
        text("双击查看提交差异，右键编辑当前源文件",
             "Double-click for commit diff; right-click to edit the current source"));
    m_commitFiles->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_commitFiles, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                auto* item = m_commitFiles->itemAt(position);
                if (!item || !item->data(Qt::UserRole).isValid())
                    return;
                const int index = item->data(Qt::UserRole).toInt();
                if (index < 0 || index >= m_commitFileChanges.size())
                    return;
                m_commitFiles->setCurrentItem(item);
                const auto& file = m_commitFileChanges[index];
                const auto sourcePath =
                    existingWorkingTreeFile(m_repositoryRoot, file.path, file.originalPath);
                QMenu menu(this);
                auto* editSource = menu.addAction(text("编辑源代码", "Edit Source"));
                editSource->setObjectName(QStringLiteral("editSourceAction"));
                editSource->setEnabled(!sourcePath.isEmpty());
                connect(editSource, &QAction::triggered, this,
                        [this, sourcePath] { openFile(sourcePath); });
                menu.exec(m_commitFiles->viewport()->mapToGlobal(position));
            });
    filesLayout->addWidget(m_commitFiles, 1);
    historySplit->setSizes({790, 330});
    historySplit->setStretchFactor(0, 1);
    historySplit->setStretchFactor(1, 0);
    historyLayout->addWidget(historySplit, 1);
    m_loadMore =
        new QPushButton(text("加载更早的 100 条提交", "Load 100 older commits"), historyPanel);
    m_loadMore->setObjectName(QStringLiteral("loadMoreButton"));
    historyLayout->addWidget(m_loadMore);
    connect(m_loadMore, &QPushButton::clicked, this, [this] { loadHistory(true); });
    connect(m_history, &QTableWidget::cellDoubleClicked, this,
            [this](int row) { showCommit(row); });
    connect(m_history, &QTableWidget::currentCellChanged, this,
            [this](int row) { loadCommitFiles(row); });
    connect(m_commitFiles, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int index = item ? item->data(Qt::UserRole).toInt() : -1;
        if (index >= 0 && index < m_commitFileChanges.size() && !m_selectedCommitHash.isEmpty())
            showCommitFileDiff(m_selectedCommitHash, m_commitFileChanges[index]);
    });
    m_bottom->addTab(historyPanel, icon("history"), text("Git 日志", "Git log"));
    m_diff = new QPlainTextEdit(m_bottom);
    m_diff->setObjectName(QStringLiteral("diffView"));
    m_diff->setReadOnly(true);
    m_diff->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_diff->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_diff->setPlaceholderText(text("选择更改文件查看差异；双击提交查看详情",
                                    "Select a changed file, or double-click a commit"));
    new DiffHighlighter(m_diff->document());
    m_bottom->addTab(m_diff, icon("diff"), text("差异", "Diff"));
    m_output = new QPlainTextEdit(m_bottom);
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(2000);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_bottom->addTab(m_output, icon("terminal"), text("Git 输出", "Git output"));
    m_vertical->setSizes({570, 245});
    m_vertical->setStretchFactor(0, 1);
    m_vertical->setStretchFactor(1, 0);
    m_horizontal->setSizes({240, 1216});
    m_horizontal->setStretchFactor(0, 0);
    m_horizontal->setStretchFactor(1, 1);
    outer->addWidget(m_horizontal, 1);
    setCentralWidget(workspace);

    m_activity = new QLabel(text("就绪", "Ready"), this);
    m_activity->setObjectName("activityStatus");
    statusBar()->addWidget(m_activity, 1);
    m_position = new QLabel(this);
    statusBar()->addPermanentWidget(m_position);
    m_encoding = new QLabel(QStringLiteral("UTF-8    LF"), this);
    statusBar()->addPermanentWidget(m_encoding);
    auto* native = new QLabel(QStringLiteral("  Qt 6 / C++  "), this);
    native->setObjectName(QStringLiteral("nativeBadge"));
    statusBar()->addPermanentWidget(native);
}

void MainWindow::applyTheme() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#2b2d30"));
    palette.setColor(QPalette::WindowText, QColor("#bcbec4"));
    palette.setColor(QPalette::Base, QColor("#1e1f22"));
    palette.setColor(QPalette::AlternateBase, QColor("#25262a"));
    palette.setColor(QPalette::Text, QColor("#bcbec4"));
    palette.setColor(QPalette::Button, QColor("#36383e"));
    palette.setColor(QPalette::ButtonText, QColor("#bcbec4"));
    palette.setColor(QPalette::Light, QColor("#43454a"));
    palette.setColor(QPalette::Mid, QColor("#32343a"));
    palette.setColor(QPalette::Dark, QColor("#17181b"));
    palette.setColor(QPalette::Highlight, QColor("#355580"));
    palette.setColor(QPalette::HighlightedText, QColor("#dfe1e5"));
    palette.setColor(QPalette::PlaceholderText, QColor("#777c87"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#696d76"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#696d76"));
    setPalette(palette);
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: #1e1f22; color: #bcbec4; }
        QWidget { color: #bcbec4; }
        QMenuBar { background: #2b2d30; padding: 2px 6px; font-size: 12px; }
        QMenuBar::item { padding: 4px 10px; background: transparent; }
        QMenuBar::item:selected, QMenu::item:selected { background: #3a4255; }
        QMenu { background: #2b2d30; border: 1px solid #43454a; padding: 5px; }
        QMenu::item { padding: 7px 28px 7px 12px; }
        QMenu::separator { height: 1px; background: #43454a; margin: 4px; }
        QToolBar { background: #2b2d30; border: none; border-bottom: 1px solid #17181b;
                   spacing: 6px; padding: 6px 10px; }
        QToolBar::separator { width: 1px; background: #45474b; margin: 7px 8px; }
        QToolButton { border: none; background: transparent; border-radius: 4px;
                      padding: 5px; min-height: 24px; }
        QToolButton:hover { background: #393b40; }
        QToolButton:pressed, QToolButton:checked, QToolButton[active=true] {
            background: #364765;
        }
        QToolButton:disabled { color: #62656d; }
        #brand { background: #4777df; color: white; font-weight: 700; font-size: 15px;
                 padding: 5px 0; border-radius: 6px; }
        #brandName { font-size: 16px; font-weight: 600; color: #dfe1e5; }
        #activityRail { background: #2b2d30; border-right: 1px solid #17181b; }
        #projectPanel, #commitPanel { background: #25262a; }
        #panelTitle { font-weight: 600; padding: 13px 14px; background: #2b2d30; }
        #sectionName { font-weight: 600; color: #dfe1e5; }
        #countBadge { background: #383b42; border-radius: 7px; padding: 1px 6px; font-size: 11px; }
        QLineEdit { background: #1e1f22; border: 1px solid #43454a; border-radius: 4px;
                    padding: 5px 7px; selection-background-color: #355580; }
        #projectPanel QLineEdit { margin: 7px 10px; }
        QLineEdit:focus, QPlainTextEdit:focus { border-color: #4b79e8; }
        QComboBox, QSpinBox { background: #1e1f22; color: #bcbec4; border: 1px solid #43454a;
                             border-radius: 4px; padding: 5px 7px; min-height: 20px; }
        QComboBox:focus, QSpinBox:focus { border-color: #4b79e8; }
        QComboBox::drop-down { width: 24px; border-left: 1px solid #43454a; }
        QComboBox::down-arrow { image: url(:/icons/down.xpm); width: 8px; height: 5px; }
        QComboBox QAbstractItemView { background: #25262a; color: #bcbec4;
                                      selection-background-color: #2e4369; border: 1px solid #43454a; }
        QListWidget { background: #25262a; color: #bcbec4; border: 1px solid #43454a; outline: none; }
        QListWidget::item { padding: 7px 9px; }
        QListWidget::item:selected { background: #2e4369; }
        QTreeView, QTreeWidget { background: transparent; border: none; outline: none; }
        QTreeView::item { height: 28px; padding-right: 8px; }
        QTreeView::item:hover { background: #2d3038; }
        QTreeView::item:selected { background: #2e4369; }
        QTreeView::branch { background: transparent; width: 18px; }
        QTreeView::branch:closed:has-children { image: url(:/icons/right.xpm); }
        QTreeView::branch:open:has-children { image: url(:/icons/down.xpm); }
        #changesTree { padding: 4px 6px; }
        #repoState { color: #8c9099; padding: 3px 14px 7px; font-size: 11px; }
        #messageLabel { padding: 8px 14px; color: #a8adb8; }
        QPlainTextEdit { background: #1e1f22; border: none; selection-background-color: #355580; }
        #commitMessage { border: 1px solid #43454a; border-radius: 4px; margin: 0 12px; padding: 6px; }
        QCheckBox { spacing: 7px; padding: 10px 14px; font-size: 11px; }
        QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #6d7180;
                               border-radius: 3px; background: #1e1f22; }
        QCheckBox::indicator:checked { background: #4b79e8; border-color: #6a9af7;
                                       image: url(:/icons/check.xpm); }
        QCheckBox::indicator:disabled { background: #303238; border-color: #43454a; }
        QPushButton { background: #36383e; border: 1px solid #4e5159; border-radius: 4px; padding: 5px 12px; }
        QPushButton:hover { background: #42454d; }
        QPushButton:focus, QToolButton:focus { border: 1px solid #6a9af7; }
        QPushButton[primary=true] { background: #416bd5; border: 1px solid #527fe6; color: white; }
        QPushButton[primary=true]:hover { background: #4c79e5; }
        QPushButton:disabled { background: #303238; color: #696d76; border-color: #3c3f45; }
        #commitButton { margin: 0 12px; font-weight: 600; }
        #commitHint, #subtleHint { color: #818590; padding: 9px 12px 2px; font-size: 10px; }
        #breadcrumb { background: #25262a; color: #9297a3; font-size: 11px; padding: 5px 12px; }
        QTabWidget::pane { border: none; }
        QTabBar { background: #2b2d30; }
        QTabBar::tab { background: #2b2d30; color: #9297a3; padding: 9px 14px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { background: #1e1f22; color: #dfe1e5; border-bottom: 2px solid #548af7; }
        QTabBar::tab:hover { background: #33353b; }
        QSplitter::handle { background: #17181b; }
        QSplitter::handle:horizontal { width: 2px; }
        QSplitter::handle:vertical { height: 2px; }
        QTableWidget { background: #1e1f22; border: none; outline: none; }
        QTableWidget::item { padding: 3px 10px; }
        QTableWidget::item:selected { background: #2e4369; }
        QHeaderView::section { background: #25262a; color: #8e939e; border: none;
                               border-bottom: 1px solid #32343a; padding: 7px 10px; text-align: left; }
        #loadMoreButton { border: none; border-radius: 0; background: #25262a; color: #8caff0; font-size: 11px; }
        QStatusBar { background: #2b2d30; border-top: 1px solid #17181b; font-size: 11px; padding: 1px 7px; }
        QStatusBar::item { border: none; }
        QStatusBar QLabel { padding: 2px 9px; }
        #nativeBadge { color: #7b8493; }
        QScrollBar:vertical { background: #1e1f22; width: 11px; margin: 0; }
        QScrollBar:horizontal { background: #1e1f22; height: 11px; margin: 0; }
        QScrollBar::handle { background: #454850; border-radius: 4px; min-height: 28px; min-width: 28px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QToolTip { background: #383a40; color: #dfe1e5; border: 1px solid #555861; padding: 5px; }
    )"));
}

void MainWindow::setProjectPanelVisible(bool visible) {
    if (m_projectPanel)
        m_projectPanel->setVisible(visible);
    if (m_projectPanelAction && m_projectPanelAction->isChecked() != visible)
        m_projectPanelAction->setChecked(visible);
    if (m_projectPanelRailButton) {
        m_projectPanelRailButton->setChecked(visible);
        m_projectPanelRailButton->setProperty("active", visible);
        const auto action = visible ? text("隐藏项目面板", "Hide Project panel")
                                    : text("展开项目面板", "Show Project panel");
        m_projectPanelRailButton->setToolTip(action);
        m_projectPanelRailButton->setAccessibleName(action);
    }
}

void MainWindow::setCommitPanelVisible(bool visible) {
    if (m_commitPanel) {
        m_commitPanel->setVisible(visible);
        if (visible) {
            const auto restorePanelWidth = [this] {
                if (!m_commitPanel || m_commitPanel->isHidden())
                    return;
                auto* splitter = qobject_cast<QSplitter*>(m_commitPanel->parentWidget());
                if (!splitter)
                    return;
                const auto sizes = splitter->sizes();
                if (sizes.size() < 2 || sizes.at(1) >= m_commitPanel->minimumWidth())
                    return;
                const int available = splitter->width() - splitter->handleWidth();
                if (available < m_commitPanel->minimumWidth())
                    return;
                splitter->setSizes(
                    {available - m_commitPanel->minimumWidth(), m_commitPanel->minimumWidth()});
            };
            restorePanelWidth();
            QTimer::singleShot(0, this, restorePanelWidth);
        }
    }
    if (m_commitPanelAction && m_commitPanelAction->isChecked() != visible)
        m_commitPanelAction->setChecked(visible);
    if (m_commitPanelRailButton) {
        m_commitPanelRailButton->setChecked(visible);
        m_commitPanelRailButton->setProperty("active", visible);
        const auto action = visible ? text("隐藏提交面板", "Hide Commit panel")
                                    : text("展开提交面板", "Show Commit panel");
        m_commitPanelRailButton->setToolTip(action);
        m_commitPanelRailButton->setAccessibleName(action);
    }
}

void MainWindow::setHistoryPanelVisible(bool visible) {
    if (m_bottom) {
        m_bottom->setVisible(visible);
        if (visible) {
            const auto restorePanelHeight = [this] {
                constexpr int minimumHistoryHeight = 140;
                if (!m_bottom || m_bottom->isHidden() || !m_vertical)
                    return;
                const auto sizes = m_vertical->sizes();
                if (sizes.size() < 2 || sizes.at(1) >= minimumHistoryHeight)
                    return;
                const int available = m_vertical->height() - m_vertical->handleWidth();
                if (available < minimumHistoryHeight)
                    return;
                m_vertical->setSizes({available - minimumHistoryHeight, minimumHistoryHeight});
            };
            restorePanelHeight();
            QTimer::singleShot(0, this, restorePanelHeight);
        }
    }
    if (m_historyPanelAction && m_historyPanelAction->isChecked() != visible)
        m_historyPanelAction->setChecked(visible);
    if (m_historyPanelRailButton) {
        m_historyPanelRailButton->setChecked(visible);
        m_historyPanelRailButton->setProperty("active", visible);
        const auto action = visible ? text("隐藏 Git 日志面板", "Hide Git log panel")
                                    : text("展开 Git 日志面板", "Show Git log panel");
        m_historyPanelRailButton->setToolTip(action);
        m_historyPanelRailButton->setAccessibleName(action);
    }
}

void MainWindow::syncChangeGroupCheckState(QTreeWidgetItem* group) {
    if (!group || group->childCount() == 0)
        return;
    int checked = 0;
    for (int i = 0; i < group->childCount(); ++i)
        if (group->child(i)->checkState(0) == Qt::Checked)
            ++checked;
    const auto state = checked == 0                     ? Qt::Unchecked
                       : checked == group->childCount() ? Qt::Checked
                                                        : Qt::PartiallyChecked;
    const QSignalBlocker block(m_changes);
    group->setCheckState(0, state);
}

void MainWindow::chooseProject() {
    const auto directory =
        QFileDialog::getExistingDirectory(this, text("打开项目", "Open project"), m_projectRoot);
    if (!directory.isEmpty())
        openProject(directory);
}

void MainWindow::chooseFile() {
    const auto filePath =
        QFileDialog::getOpenFileName(this, text("打开文件", "Open file"),
                                     m_projectRoot.isEmpty() ? QDir::homePath() : m_projectRoot,
                                     text("Markdown 文档 (*.md *.markdown);;所有文件 (*)",
                                          "Markdown files (*.md *.markdown);;All files (*)"));
    if (!filePath.isEmpty())
        openFile(filePath);
}

bool MainWindow::openProject(const QString& directory) {
    const QFileInfo info(directory);
    if (!info.isDir())
        return false;
    if (gitBusy()) {
        QMessageBox::information(
            this, text("Git 正忙", "Git is busy"),
            text("请先等待 Git 操作完成，或取消后再打开其他项目。",
                 "Wait for Git to finish, or cancel before switching projects."));
        return false;
    }
    persistEditorSession();
    if (!closeEditors())
        return false;
    persistProjectTreeState();
    ++m_generation;
    ++m_diffRequest;
    if (m_operations) {
        m_operations->deleteLater();
        m_operations = nullptr;
    }
    m_pendingOperation = PendingOperation::None;
    m_refreshPending = false;
    m_reloadRequested = false;
    if (m_git) {
        m_git->disconnect(this);
        m_git->deleteLater();
    }
    {
        const QSignalBlocker block(m_tree);
        m_tree->collapseAll();
    }
    if (m_treeFilter)
        m_treeFilter->clear();
    m_projectRoot = info.canonicalFilePath();
    m_expandedProjectTreePaths.clear();
    m_pendingTreeExpansions.clear();
    QSettings treeSettings;
    const auto treeSettingsKey = projectTreeExpansionSettingsKey(m_projectRoot);
    m_projectTreeStateKnown = treeSettings.contains(treeSettingsKey);
    if (m_projectTreeStateKnown) {
        for (const auto& path : treeSettings.value(treeSettingsKey).toStringList()) {
            const auto normalized = normalizedProjectRelativePath(path);
            if (!normalized.isEmpty() && !m_expandedProjectTreePaths.contains(normalized))
                m_expandedProjectTreePaths.append(normalized);
        }
    } else {
        m_expandedProjectTreePaths = {QStringLiteral("src"), QStringLiteral("include")};
    }
    m_pendingTreeExpansions = m_expandedProjectTreePaths;
    m_repositoryRoot.clear();
    m_status = {};
    m_hasHead = false;
    m_commits.clear();
    m_commitFileChanges.clear();
    m_selectedCommitHash.clear();
    ++m_commitFilesRequest;
    m_history->setRowCount(0);
    m_commitFiles->clear();
    m_historyBranch->clear();
    {
        const QSignalBlocker block(m_historyAuthor);
        m_historyAuthor->clear();
        m_historyAuthor->addItem(text("全部用户", "All authors"), QString());
    }
    m_historyAuthorTimer->stop();
    m_changes->clear();
    m_diff->clear();
    m_output->clear();
    m_message->clear();
    m_amend->setChecked(false);
    m_projectModel->setGitStatus({}, {});
    m_projectButton->setText(info.fileName());
    m_projectButton->setToolTip(m_projectRoot);
    setWindowTitle(QStringLiteral(IDEA4GIT_APP_NAME) + QStringLiteral(" — ") + info.fileName());
    m_projectModel->setRootPath(m_projectRoot);
    m_tree->setRootIndex(m_projectFilter->mapFromSource(m_projectModel->index(m_projectRoot)));
    QTimer::singleShot(0, this, &MainWindow::restorePendingProjectTreeExpansions);
    m_git = new GitClient(m_projectRoot, this);
    connect(m_git, &GitClient::activityChanged, this, [this](bool busy, const QString& command) {
        m_activity->setText(busy ? QStringLiteral("Git: ") + command.left(100)
                                 : text("就绪", "Ready"));
        updateActions();
        if (!busy && m_refreshPending && !qApp->activeModalWidget())
            QTimer::singleShot(0, this, &MainWindow::refreshRepository);
    });
    connect(m_git, &GitClient::commandCompleted, this,
            [this](const QStringList& args, const GitResult& result) {
                m_output->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") +
                                          QStringLiteral("  git ") + args.join(' '));
                if (!result.error.isEmpty())
                    m_output->appendPlainText(QString::fromUtf8(result.error));
                if (result.cancelled)
                    m_output->appendPlainText(
                        text("操作已取消；更改不一定已回滚。",
                             "Cancelled; changes may already have taken effect."));
                if (result.timedOut)
                    m_output->appendPlainText(text("操作超时。", "Operation timed out."));
            });
    if (!qApp->property("previewMode").toBool())
        QSettings().setValue("project/last", m_projectRoot);
    refreshRepository();
    if (!restoreEditorSession()) {
        for (const auto& candidate :
             {QStringLiteral("src/main.cpp"), QStringLiteral("README.md")}) {
            const auto path = QDir(m_projectRoot).filePath(candidate);
            if (QFileInfo::exists(path)) {
                openFile(path);
                break;
            }
        }
    }
    updateActions();
    return true;
}

void MainWindow::persistProjectTreeState() {
    if (m_projectRoot.isEmpty() || !m_projectTreeStateKnown ||
        qApp->property("previewMode").toBool())
        return;

    QStringList paths;
    for (const auto& path : m_expandedProjectTreePaths) {
        const auto normalized = normalizedProjectRelativePath(path);
        if (!normalized.isEmpty() && !paths.contains(normalized))
            paths.append(normalized);
    }
    paths.sort(Qt::CaseSensitive);
    QSettings settings;
    settings.setValue(projectTreeExpansionSettingsKey(m_projectRoot), paths);
}

void MainWindow::persistEditorSession() {
    if (m_projectRoot.isEmpty() || qApp->property("previewMode").toBool())
        return;

    QStringList paths;
    QString activePath;
    for (int index = 0; index < m_editors->count(); ++index) {
        auto* editor = qobject_cast<CodeEditor*>(m_editors->widget(index));
        if (!editor)
            continue;
        const auto absolutePath = QFileInfo(editor->filePath()).canonicalFilePath();
        if (absolutePath.isEmpty())
            continue;
        const auto relativePath =
            normalizedProjectRelativePath(QDir(m_projectRoot).relativeFilePath(absolutePath));
        if (relativePath.isEmpty())
            continue;
        if (!paths.contains(relativePath))
            paths.append(relativePath);
        if (editor == currentEditor())
            activePath = relativePath;
    }

    const auto group = projectEditorSessionSettingsGroup(m_projectRoot);
    QSettings settings;
    settings.setValue(group + QStringLiteral("/openFiles"), paths);
    settings.setValue(group + QStringLiteral("/activeFile"), activePath);
}

bool MainWindow::restoreEditorSession() {
    if (m_projectRoot.isEmpty() || qApp->property("previewMode").toBool())
        return false;

    const auto group = projectEditorSessionSettingsGroup(m_projectRoot);
    const auto openFilesKey = group + QStringLiteral("/openFiles");
    QSettings settings;
    if (!settings.contains(openFilesKey))
        return false;

    const auto paths = settings.value(openFilesKey).toStringList();
    for (const auto& path : paths) {
        const auto relativePath = normalizedProjectRelativePath(path);
        if (relativePath.isEmpty())
            continue;
        const auto absolutePath = QDir(m_projectRoot).filePath(relativePath);
        if (QFileInfo(absolutePath).isFile())
            openFile(absolutePath);
    }

    const auto activePath = normalizedProjectRelativePath(
        settings.value(group + QStringLiteral("/activeFile")).toString());
    if (!activePath.isEmpty()) {
        const auto activeAbsolutePath =
            QFileInfo(QDir(m_projectRoot).filePath(activePath)).canonicalFilePath();
        for (int index = 0; index < m_editors->count(); ++index) {
            auto* editor = qobject_cast<CodeEditor*>(m_editors->widget(index));
            if (editor && editor->filePath() == activeAbsolutePath) {
                m_editors->setCurrentIndex(index);
                break;
            }
        }
    }
    return true;
}

void MainWindow::trackProjectTreeExpansion(const QModelIndex& index, bool expanded) {
    if (!index.isValid() || m_projectRoot.isEmpty())
        return;
    const auto sourceIndex = m_projectFilter->mapToSource(index);
    if (!sourceIndex.isValid() || !m_projectModel->isDir(sourceIndex))
        return;

    const auto relative =
        QDir(m_projectRoot).relativeFilePath(m_projectModel->filePath(sourceIndex));
    const auto path = normalizedProjectRelativePath(relative);
    if (path.isEmpty())
        return;

    m_projectTreeStateKnown = true;
    if (expanded) {
        if (!m_expandedProjectTreePaths.contains(path))
            m_expandedProjectTreePaths.append(path);
    } else {
        const auto prefix = path + QLatin1Char('/');
        m_expandedProjectTreePaths.erase(
            std::remove_if(m_expandedProjectTreePaths.begin(), m_expandedProjectTreePaths.end(),
                           [&path, &prefix](const QString& candidate) {
                               return candidate == path || candidate.startsWith(prefix);
                           }),
            m_expandedProjectTreePaths.end());
        m_pendingTreeExpansions.erase(
            std::remove_if(m_pendingTreeExpansions.begin(), m_pendingTreeExpansions.end(),
                           [&path, &prefix](const QString& candidate) {
                               return candidate == path || candidate.startsWith(prefix);
                           }),
            m_pendingTreeExpansions.end());
    }
    persistProjectTreeState();
}

void MainWindow::restorePendingProjectTreeExpansions() {
    if (m_projectRoot.isEmpty() || m_pendingTreeExpansions.isEmpty())
        return;

    const auto rootPath = QDir::cleanPath(QDir::fromNativeSeparators(m_projectRoot));
    const auto rootSource = m_projectModel->index(rootPath);
    if (!rootSource.isValid())
        return;

    QStringList stillPending;
    const auto pending = m_pendingTreeExpansions;
    for (const auto& target : pending) {
        QString currentDirectory = rootPath;
        QModelIndex currentSource = rootSource;
        QString relativePrefix;
        bool waitingForLoad = false;
        bool invalidPath = false;

        const auto components = target.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i < components.size(); ++i) {
            if (!m_loadedProjectTreeDirectories.contains(currentDirectory)) {
                if (m_projectModel->canFetchMore(currentSource))
                    m_projectModel->fetchMore(currentSource);
                waitingForLoad = true;
                break;
            }

            const auto childPath = QDir(currentDirectory).filePath(components.at(i));
            const auto childSource = m_projectModel->index(childPath);
            if (!childSource.isValid() || !m_projectModel->isDir(childSource)) {
                invalidPath = true;
                break;
            }

            relativePrefix = relativePrefix.isEmpty()
                                 ? components.at(i)
                                 : relativePrefix + QLatin1Char('/') + components.at(i);
            const auto childProxy = m_projectFilter->mapFromSource(childSource);
            if (!childProxy.isValid()) {
                waitingForLoad = true;
                break;
            }

            if (!m_tree->isExpanded(childProxy)) {
                const QSignalBlocker block(m_tree);
                m_tree->expand(childProxy);
            }
            if (!m_expandedProjectTreePaths.contains(relativePrefix))
                m_expandedProjectTreePaths.append(relativePrefix);
            currentDirectory = QDir::cleanPath(childPath);
            currentSource = childSource;
        }

        if (waitingForLoad) {
            stillPending.append(target);
            if (!m_expandedProjectTreePaths.contains(target))
                m_expandedProjectTreePaths.append(target);
        } else if (invalidPath) {
            const auto prefix = target + QLatin1Char('/');
            m_expandedProjectTreePaths.erase(
                std::remove_if(m_expandedProjectTreePaths.begin(), m_expandedProjectTreePaths.end(),
                               [&target, &prefix](const QString& candidate) {
                                   return candidate == target || candidate.startsWith(prefix);
                               }),
                m_expandedProjectTreePaths.end());
        }
    }

    m_pendingTreeExpansions = stillPending;
    if (m_pendingTreeExpansions.isEmpty())
        persistProjectTreeState();
}

QStringList MainWindow::repositoryArgs(const QStringList& args) const {
    QStringList result = {QStringLiteral("-C"), m_repositoryRoot};
    result.append(args);
    return result;
}

bool MainWindow::gitBusy() const {
    return (m_git && m_git->busy()) || (m_operations && m_operations->busy());
}

void MainWindow::refreshRepository() {
    if (!m_git)
        return;
    if (gitBusy() || m_refreshing) {
        m_refreshPending = true;
        return;
    }
    m_refreshPending = false;
    m_refreshing = true;
    const int generation = m_generation;
    const QString previousRepositoryRoot = m_repositoryRoot;
    // --show-cdup contains only ../ components, never a quoted/non-ASCII root path.
    m_git->run({"rev-parse", "--show-cdup"}, [this, generation,
                                              previousRepositoryRoot](const GitResult& discovery) {
        if (generation != m_generation)
            return;
        if (!discovery.ok()) {
            m_refreshing = false;
            m_repositoryRoot.clear();
            if (!previousRepositoryRoot.isEmpty()) {
                m_excludedCommitPaths.clear();
                m_commitSelectionInitialized = false;
            }
            m_status = {};
            m_hasHead = false;
            m_branch->setText(text("无 Git 仓库", "No Git repository"));
            m_repoState->setText(
                text("当前目录不是 Git 仓库。仍可浏览和编辑文件；请打开已有仓库使用 Git。",
                     "Not a Git repository. You can still edit files; open an existing repository "
                     "to use Git."));
            m_projectModel->setGitStatus({}, {});
            renderChanges();
            updateActions();
            return;
        }
        auto relative = discovery.output;
        if (relative.endsWith('\n'))
            relative.chop(1);
        if (relative.endsWith('\r'))
            relative.chop(1);
        const auto discoveredRoot =
            QDir::cleanPath(QDir(m_projectRoot).absoluteFilePath(QString::fromUtf8(relative)));
        if (discoveredRoot != previousRepositoryRoot) {
            m_excludedCommitPaths.clear();
            m_commitSelectionInitialized = false;
        }
        m_repositoryRoot = discoveredRoot;
        if (!m_operations || m_operations->root() != m_repositoryRoot) {
            if (m_operations)
                m_operations->deleteLater();
            m_operations = new GitOperations(m_git, m_repositoryRoot, m_english, this);
        }
        auto args = repositoryArgs({"status", "--porcelain=v1", "-z", "--branch",
                                    "--untracked-files=all", "--ignored=matching"});
        args.prepend("--no-optional-locks");
        m_git->run(args, [this, generation](const GitResult& result) {
            if (generation != m_generation)
                return;
            m_refreshing = false;
            if (!result.ok()) {
                reportError(text("读取状态失败", "Could not read status"), result);
                updateActions();
                return;
            }
            m_status = parseStatus(result.output);
            if (!m_status.valid) {
                m_repoState->setText(
                    text("状态输出不完整或含非 UTF-8 路径，已禁用 Git 写操作。请在外部处理后刷新。",
                         "Incomplete status or non-UTF-8 paths. Git writes disabled; resolve "
                         "externally and refresh."));
                m_changes->clear();
                m_changeCount->setText("0");
                m_projectModel->setGitStatus({}, {});
                updateActions();
                return;
            }
            m_hasHead = !m_status.unborn;
            m_branch->setText(m_status.branch + QStringLiteral("  ▾"));
            m_branch->setToolTip(m_repositoryRoot + '\n' + m_status.tracking);
            m_repoState->setText(m_status.tracking.isEmpty()
                                     ? text("本地分支 · 未设置上游", "Local branch · no upstream")
                                     : m_status.tracking);
            m_projectModel->setGitStatus(m_repositoryRoot, m_status.files);
            renderChanges();
            loadHistoryBranches();
            inspectWorkflow();
            if (m_reloadRequested && !qApp->activeModalWidget()) {
                m_reloadRequested = false;
                reloadUnmodifiedEditors();
            }
            updateActions();
        });
    });
    updateActions();
}

void MainWindow::renderChanges() {
    const QSignalBlocker block(m_changes);
    m_changes->clear();
    QSet<QString> currentPaths;
    for (const auto& file : m_status.files)
        if (file.state() != FileState::Ignored)
            currentPaths.insert(file.path);
    m_excludedCommitPaths.intersect(currentPaths);
    auto* changed = new QTreeWidgetItem(m_changes, {text("更改", "Changes")});
    auto* untracked = new QTreeWidgetItem(m_changes, {text("未跟踪", "Untracked")});
    changed->setData(0, Qt::UserRole + 2, QStringLiteral("changes"));
    untracked->setData(0, Qt::UserRole + 2, QStringLiteral("untracked"));
    for (auto* group : {changed, untracked}) {
        group->setData(0, Qt::UserRole, -1);
        group->setFlags((group->flags() & ~Qt::ItemIsSelectable) | Qt::ItemIsUserCheckable);
        auto font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        group->setForeground(0, QColor("#a4a9b4"));
        group->setExpanded(true);
        group->setToolTip(0, text("勾选或取消本组文件的提交选择。",
                                  "Select or deselect every file in this group for the commit."));
    }
    int count = 0;
    for (int i = 0; i < m_status.files.size(); ++i) {
        const auto& file = m_status.files[i];
        if (file.state() == FileState::Ignored)
            continue;
        ++count;
        const auto append = [&](QTreeWidgetItem* group) {
            auto* item = new QTreeWidgetItem(
                group, {stateCode(file.state()) + "   " + displayPath(file.path)});
            if (!file.conflict())
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setForeground(0, stateColor(file.state()));
            item->setData(0, Qt::UserRole, i);
            const bool selected = !file.conflict() && !m_excludedCommitPaths.contains(file.path);
            item->setData(0, Qt::UserRole + 1, selected);
            item->setCheckState(0, selected ? Qt::Checked : Qt::Unchecked);
            if (!m_commitSelectionInitialized && !file.conflict())
                item->setCheckState(0, Qt::Checked);
            item->setToolTip(
                0, file.path + (file.originalPath.isEmpty() ? "" : "\n← " + file.originalPath));
        };
        append(file.state() == FileState::Untracked ? untracked : changed);
    }
    m_commitSelectionInitialized = true;
    for (auto* group : {changed, untracked}) {
        syncChangeGroupCheckState(group);
        group->setText(0, group->text(0) + QStringLiteral("  (%1)").arg(group->childCount()));
        group->setHidden(group->childCount() == 0);
    }
    if (!count && !m_repositoryRoot.isEmpty()) {
        auto* empty = new QTreeWidgetItem(
            m_changes, {text("工作区干净，没有待提交更改", "Working tree is clean")});
        empty->setData(0, Qt::UserRole, -1);
        empty->setForeground(0, QColor("#7c9c7c"));
    }
    m_changeCount->setText(QString::number(count));
    m_amend->setEnabled(m_hasHead);
    updateActions();
}

void MainWindow::loadHistory(bool append) {
    if (m_repositoryRoot.isEmpty())
        return;
    if (!append) {
        ++m_historyRequest;
        m_commits.clear();
        m_history->setRowCount(0);
        m_historyOffset = 0;
        m_selectedCommitHash.clear();
        m_commitFileChanges.clear();
        m_commitFiles->clear();
        if (auto* heading = findChild<QLabel*>("commitFilesHeading"))
            heading->setText(text("变更文件", "Changed files"));
        ++m_commitFilesRequest;
    }
    const int request = m_historyRequest;
    const int generation = m_generation;
    if (!m_hasHead) {
        m_loadMore->hide();
        return;
    }
    m_loadMore->setEnabled(false);
    QStringList args = {"log",
                        "--no-show-signature",
                        "--date-order",
                        "-z",
                        "-n",
                        "100",
                        "--skip=" + QString::number(m_historyOffset),
                        "--format=%H%x00%h%x00%an%x00%aI%x00%D%x00%s"};
    const auto branch = m_historyBranch->currentData().toString();
    if (branch == "ALL")
        args.append({"--branches", "--remotes"});
    else
        args.append(branch.isEmpty() ? QStringLiteral("HEAD") : branch);
    QString author = m_historyAuthor->currentText().trimmed();
    if (m_historyAuthor->currentIndex() == 0 && author == m_historyAuthor->itemText(0).trimmed())
        author.clear();
    if (!author.isEmpty())
        args.append("--author=" + authorRegex(author));
    m_git->run(repositoryArgs(args), [this, request, generation](const GitResult& result) {
        if (request != m_historyRequest || generation != m_generation)
            return;
        if (!result.ok()) {
            m_loadMore->setEnabled(true);
            return;
        }
        const auto commits = parseLog(result.output);
        m_historyOffset += commits.size();
        m_commits.append(commits);

        {
            const QSignalBlocker block(m_historyAuthor);
            const QSignalBlocker editBlock(m_historyAuthor->lineEdit());
            const auto currentAuthor = m_historyAuthor->currentText();
            for (const auto& commit : m_commits) {
                bool present = false;
                for (int i = 1; i < m_historyAuthor->count(); ++i)
                    present |= m_historyAuthor->itemText(i) == commit.author;
                if (!present && !commit.author.isEmpty())
                    m_historyAuthor->addItem(commit.author, commit.author);
            }
            m_historyAuthor->setCurrentText(currentAuthor);
        }

        const QString selectedHash = m_selectedCommitHash;
        m_history->setRowCount(m_commits.size());
        for (int row = 0; row < m_commits.size(); ++row) {
            const auto& commit = m_commits[row];
            auto* hash = new QTableWidgetItem(commit.shortHash);
            hash->setForeground(QColor("#8baff0"));
            hash->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            hash->setIcon(icon("git", QColor("#7799d6")));
            m_history->setItem(row, 0, hash);
            const QString refs = commit.refs.isEmpty() ? QString() : "[" + commit.refs + "]  ";
            m_history->setItem(row, 1, new QTableWidgetItem(refs + commit.subject));
            m_history->setItem(row, 2, new QTableWidgetItem(commit.author));
            m_history->setItem(row, 3,
                               new QTableWidgetItem(QDateTime::fromString(commit.date, Qt::ISODate)
                                                        .toLocalTime()
                                                        .toString("MM-dd HH:mm")));
            for (int column = 0; column < 4; ++column)
                m_history->item(row, column)->setToolTip(commit.hash + '\n' + commit.subject);
        }
        int selectedRow = -1;
        if (!selectedHash.isEmpty())
            for (int row = 0; row < m_commits.size(); ++row)
                if (m_commits[row].hash == selectedHash) {
                    selectedRow = row;
                    break;
                }
        if (selectedRow < 0 && !m_commits.isEmpty())
            selectedRow = 0;
        if (selectedRow >= 0) {
            const QSignalBlocker block(m_history);
            m_history->setCurrentCell(selectedRow, 0);
            loadCommitFiles(selectedRow);
        } else {
            m_commitFiles->clear();
            m_selectedCommitHash.clear();
        }
        m_loadMore->setVisible(commits.size() == 100);
        m_loadMore->setEnabled(true);
    });
}

void MainWindow::loadHistoryBranches() {
    if (!m_git || m_repositoryRoot.isEmpty())
        return;
    const int generation = m_generation;
    const int request = ++m_historyBranchRequest;
    const auto previous = m_historyBranch->currentData().toString();
    m_git->run(
        repositoryArgs({"for-each-ref", "--count=1000", "--format=%(refname)%00%(refname:short)",
                        "refs/heads", "refs/remotes"}),
        [this, generation, request, previous](const GitResult& result) {
            if (generation != m_generation || request != m_historyBranchRequest)
                return;
            const QSignalBlocker block(m_historyBranch);
            m_historyBranch->clear();
            m_historyBranch->addItem(text("全部分支", "All branches"), "ALL");
            m_historyBranch->addItem("HEAD", "HEAD");
            if (result.ok()) {
                const auto records = result.output.split('\n');
                for (const auto& record : records) {
                    const int separator = record.indexOf('\0');
                    if (separator <= 0 || separator + 1 >= record.size())
                        continue;
                    const auto fullName = QString::fromUtf8(record.left(separator));
                    const auto shortName = QString::fromUtf8(record.mid(separator + 1));
                    if (!fullName.isEmpty() && !shortName.isEmpty())
                        m_historyBranch->addItem(shortName, fullName);
                }
            }

            QString desired = previous;
            if (desired.isEmpty() || m_historyBranch->findData(desired) < 0) {
                const auto current = m_status.branch.isEmpty()
                                         ? QStringLiteral("HEAD")
                                         : QStringLiteral("refs/heads/") + m_status.branch;
                desired = m_historyBranch->findData(current) >= 0 ? current : "HEAD";
            }
            const int index = m_historyBranch->findData(desired);
            m_historyBranch->setCurrentIndex(index < 0 ? 0 : index);
            loadHistory(false);
        });
}

void MainWindow::loadCommitFiles(int row) {
    if (row < 0 || row >= m_commits.size() || !m_git || m_repositoryRoot.isEmpty())
        return;
    const auto commit = m_commits[row].hash;
    m_selectedCommitHash = commit;
    m_commitFiles->clear();
    m_commitFiles->addItem(text("正在读取变更文件…", "Loading changed files…"));
    if (auto* heading = findChild<QLabel*>("commitFilesHeading"))
        heading->setText(text("变更文件", "Changed files"));
    m_commitFileChanges.clear();
    const int request = ++m_commitFilesRequest;
    const int generation = m_generation;
    m_git->run(
        repositoryArgs({"rev-list", "--parents", "-n", "1", commit}),
        [this, request, generation, commit](const GitResult& parentsResult) {
            if (generation != m_generation || request != m_commitFilesRequest)
                return;
            if (!parentsResult.ok()) {
                m_commitFiles->clear();
                m_commitFiles->addItem(QString::fromUtf8(parentsResult.error));
                return;
            }
            const auto fields =
                QString::fromUtf8(parentsResult.output).trimmed().split(' ', Qt::SkipEmptyParts);
            QStringList args;
            if (fields.size() > 1) {
                args = {"diff", "--no-ext-diff", "--no-textconv", "--find-renames", "--name-status",
                        "-z",   fields[1],       commit};
            } else {
                args = {"diff-tree",
                        "--root",
                        "-r",
                        "--no-commit-id",
                        "--no-ext-diff",
                        "--no-textconv",
                        "--find-renames",
                        "--name-status",
                        "-z",
                        commit};
            }
            m_git->run(repositoryArgs(args), [this, request, generation](const GitResult& result) {
                if (generation != m_generation || request != m_commitFilesRequest)
                    return;
                m_commitFiles->clear();
                if (!result.ok()) {
                    m_commitFiles->addItem(QString::fromUtf8(result.error));
                    return;
                }
                bool valid = false;
                m_commitFileChanges = parseCommitFiles(result.output, &valid);
                if (!valid) {
                    m_commitFiles->addItem(text("Git 返回的文件列表无法安全解析。",
                                                "Could not safely parse Git's file list."));
                    m_commitFileChanges.clear();
                    return;
                }
                const int totalFileChanges = static_cast<int>(m_commitFileChanges.size());
                const int visibleCount = qMin(totalFileChanges, 5000);
                for (int i = 0; i < visibleCount; ++i) {
                    const auto& file = m_commitFileChanges[i];
                    QString label = file.status + QStringLiteral("   ") + displayPath(file.path);
                    if (!file.originalPath.isEmpty())
                        label += QStringLiteral("  ←  ") + displayPath(file.originalPath);
                    auto* item = new QListWidgetItem(label, m_commitFiles);
                    item->setForeground(stateColor(commitFileState(file.status)));
                    item->setData(Qt::UserRole, i);
                    item->setToolTip(file.originalPath.isEmpty()
                                         ? file.path
                                         : file.originalPath + " → " + file.path);
                }
                if (visibleCount < totalFileChanges)
                    m_commitFiles->addItem(
                        text("仅显示前 5000 项。", "Showing first 5,000 items."));
                if (m_commitFileChanges.isEmpty())
                    m_commitFiles->addItem(text("此提交没有可显示的文件差异。",
                                                "This commit has no file changes to display."));
                const auto title = text("变更文件", "Changed files");
                if (auto* heading = findChild<QLabel*>("commitFilesHeading"))
                    heading->setText(title + QStringLiteral(" (%1%2)")
                                                 .arg(totalFileChanges)
                                                 .arg(visibleCount < totalFileChanges
                                                          ? text("，前 5000 项", ", first 5,000")
                                                          : QString()));
            });
        });
}

void MainWindow::openFile(const QString& path) {
    const auto canonical = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < m_editors->count(); ++i) {
        auto* editor = qobject_cast<CodeEditor*>(m_editors->widget(i));
        if (editor && editor->filePath() == canonical && !canonical.isEmpty()) {
            m_editors->setCurrentIndex(i);
            return;
        }
    }
    auto* editor = new CodeEditor(m_editors);
    QString error;
    if (!editor->loadFile(path, &error)) {
        delete editor;
        QMessageBox::warning(this, text("无法打开文件", "Could not open file"),
                             path + "\n\n" + error);
        return;
    }
    editor->setEnglish(m_english);
    const int index =
        m_editors->addTab(editor, icon("file", QColor("#7eabdf")), QFileInfo(path).fileName());
    auto* close = toolButton("close", text("关闭文件", "Close file"), m_editors);
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(14, 14));
    m_editors->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    connect(close, &QToolButton::clicked, this,
            [this, editor] { closeEditor(m_editors->indexOf(editor)); });
    m_editors->setTabToolTip(index, canonical);
    m_editors->setCurrentIndex(index);
    connect(editor, &QsciScintilla::modificationChanged, this, [this, editor](bool modified) {
        const int tab = m_editors->indexOf(editor);
        if (tab >= 0)
            m_editors->setTabText(tab, QFileInfo(editor->filePath()).fileName() +
                                           (modified ? " *" : ""));
        updateActions();
    });
    connect(editor, &QsciScintilla::cursorPositionChanged, this, [this] { updateEditorStatus(); });
    editor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        editor, &QWidget::customContextMenuRequested, this, [this, editor](const QPoint& position) {
            QMenu menu(this);
            menu.addAction(text("撤销", "Undo"), editor, &QsciScintilla::undo);
            menu.addAction(text("重做", "Redo"), editor, &QsciScintilla::redo);
            menu.addSeparator();
            menu.addAction(text("剪切", "Cut"), editor, &QsciScintilla::cut);
            menu.addAction(text("复制", "Copy"), editor, &QsciScintilla::copy);
            menu.addAction(text("粘贴", "Paste"), editor, &QsciScintilla::paste);
            menu.addSeparator();
            menu.addAction(text("全选", "Select all"), editor, [editor] { editor->selectAll(); });
            menu.addSeparator();

            QString relativePath;
            bool withinRepository = false;
            if (!m_repositoryRoot.isEmpty()) {
                relativePath = QDir::cleanPath(
                    QDir(m_repositoryRoot)
                        .relativeFilePath(QFileInfo(editor->filePath()).absoluteFilePath()));
                withinRepository = relativePath != ".." && !relativePath.startsWith("../") &&
                                   !QDir::isAbsolutePath(relativePath);
            }
            bool hasUncommittedChanges = false;
            bool hasCommittedHistory = m_hasHead && withinRepository;
            FileChange currentChange;
            for (const auto& change : m_status.files) {
                if (change.path != relativePath && change.originalPath != relativePath)
                    continue;
                currentChange = change;
                const auto state = change.state();
                hasUncommittedChanges = state != FileState::Ignored && state != FileState::Clean;
                hasCommittedHistory = hasCommittedHistory && state != FileState::Untracked &&
                                      state != FileState::Added && state != FileState::Ignored;
                break;
            }
            auto* workingDiff =
                menu.addAction(text("Git Diff（未提交）", "Git Diff (uncommitted)"));
            workingDiff->setObjectName("editorUncommittedDiffAction");
            workingDiff->setEnabled(withinRepository && hasUncommittedChanges);
            if (!workingDiff->isEnabled())
                workingDiff->setToolTip(text("当前文件没有未提交更改", "No uncommitted changes"));
            connect(workingDiff, &QAction::triggered, this,
                    [this, editor, relativePath, currentChange] {
                        showWorkingTreeDiff(editor, relativePath, currentChange);
                    });
            auto* committedDiff =
                menu.addAction(text("查看最近已提交差异…", "View latest committed diff…"));
            committedDiff->setObjectName("editorCommittedDiffAction");
            committedDiff->setEnabled(hasCommittedHistory);
            connect(committedDiff, &QAction::triggered, this,
                    [this, relativePath] { showCommittedFileDiff(relativePath); });

            auto* compareBranch = menu.addMenu(text("与分支比较", "Compare with Branch"));
            compareBranch->setObjectName(QStringLiteral("editorCompareBranchMenu"));
            compareBranch->setToolTip(text("比较当前文件与本地或远程分支",
                                           "Compare this file with a local or remote branch"));
            compareBranch->setEnabled(withinRepository && m_git);
            if (withinRepository && m_git) {
                auto branchLoadState = std::make_shared<int>(0);
                const int generation = m_generation;
                QPointer<QMenu> branchMenuGuard(compareBranch);
                QPointer<CodeEditor> editorGuard(editor);
                connect(
                    compareBranch, &QMenu::aboutToShow, this,
                    [this, branchMenuGuard, editorGuard, relativePath, generation,
                     branchLoadState] {
                        if (!branchMenuGuard || *branchLoadState != 0)
                            return;
                        *branchLoadState = 1;
                        branchMenuGuard->clear();
                        auto* loading =
                            branchMenuGuard->addAction(text("正在读取分支…", "Loading branches…"));
                        loading->setEnabled(false);
                        m_git->run(
                            repositoryArgs({"for-each-ref",
                                            "--format=%(refname:short)\t%(refname)\t%(symref)",
                                            "refs/heads", "refs/remotes"}),
                            [this, branchMenuGuard, editorGuard, relativePath, generation,
                             branchLoadState](const GitResult& result) {
                                if (!branchMenuGuard)
                                    return;
                                branchMenuGuard->clear();
                                if (generation != m_generation) {
                                    *branchLoadState = 0;
                                    auto* stale = branchMenuGuard->addAction(
                                        text("仓库已切换，请重新打开菜单。",
                                             "Repository changed; reopen this menu."));
                                    stale->setEnabled(false);
                                    return;
                                }
                                if (!result.ok()) {
                                    *branchLoadState = 0;
                                    auto* failed = branchMenuGuard->addAction(
                                        text("读取分支失败", "Could not read branches"));
                                    failed->setEnabled(false);
                                    failed->setToolTip(QString::fromUtf8(result.error));
                                    return;
                                }

                                QList<QPair<QString, QString>> localBranches;
                                QList<QPair<QString, QString>> remoteBranches;
                                const auto lines = QString::fromUtf8(result.output)
                                                       .split('\n', Qt::SkipEmptyParts);
                                for (auto line : lines) {
                                    if (line.endsWith('\r'))
                                        line.chop(1);
                                    const auto fields = line.split('\t');
                                    if (fields.size() < 2 ||
                                        (fields.size() > 2 && !fields[2].isEmpty()))
                                        continue;
                                    const auto& name = fields[0];
                                    const auto& ref = fields[1];
                                    if (ref.startsWith(QStringLiteral("refs/heads/"))) {
                                        localBranches.append({name, ref});
                                    } else if (ref.startsWith(QStringLiteral("refs/remotes/")) &&
                                               !name.endsWith(QStringLiteral("/HEAD"))) {
                                        remoteBranches.append({name, ref});
                                    }
                                }

                                const auto addBranchActions = [this, branchMenuGuard, editorGuard,
                                                               relativePath](
                                                                  const QList<
                                                                      QPair<QString, QString>>&
                                                                      branches) {
                                    for (const auto& branch : branches) {
                                        auto* action = branchMenuGuard->addAction(branch.first);
                                        action->setObjectName(
                                            QStringLiteral("compareBranchAction"));
                                        action->setData(branch.second);
                                        action->setToolTip(text(
                                            "比较当前工作区文件与此分支版本",
                                            "Compare the current worktree file with this branch"));
                                        connect(action, &QAction::triggered, this,
                                                [this, editorGuard, relativePath, branch] {
                                                    if (editorGuard)
                                                        showBranchDiff(editorGuard, relativePath,
                                                                       branch.first, branch.second);
                                                });
                                    }
                                };

                                if (!localBranches.isEmpty()) {
                                    branchMenuGuard->addSection(text("本地分支", "Local branches"));
                                    addBranchActions(localBranches);
                                }
                                if (!remoteBranches.isEmpty()) {
                                    branchMenuGuard->addSection(
                                        text("远程分支", "Remote branches"));
                                    addBranchActions(remoteBranches);
                                }
                                if (localBranches.isEmpty() && remoteBranches.isEmpty()) {
                                    auto* empty = branchMenuGuard->addAction(
                                        text("没有可比较的分支", "No branches to compare"));
                                    empty->setEnabled(false);
                                }
                                *branchLoadState = 2;
                            });
                    });
            }
            menu.exec(editor->mapToGlobal(position));
        });
    editor->focusCurrentView();
    updateEditorStatus();
    updateActions();
}

CodeEditor* MainWindow::currentEditor() const {
    return qobject_cast<CodeEditor*>(m_editors->currentWidget());
}

void MainWindow::updateEditorStatus() {
    auto* editor = currentEditor();
    if (!editor) {
        m_position->clear();
        m_encoding->clear();
        m_breadcrumb->setText(text("从左侧项目树打开文件", "Open a file from the project tree"));
        return;
    }
    int line = 0, column = 0;
    editor->getCursorPosition(&line, &column);
    m_position->setText(QStringLiteral("%1:%2").arg(line + 1).arg(column + 1));
    m_encoding->setText(editor->encodingLabel());
    m_breadcrumb->setText(displayPath(QDir(m_projectRoot).relativeFilePath(editor->filePath())));
    m_breadcrumb->setToolTip(editor->filePath());
}

bool MainWindow::saveEditor(CodeEditor* editor) {
    bool overwrite = false;
    if (editor->changedOnDisk()) {
        const auto reply =
            QMessageBox::warning(this, text("文件已在外部更改", "File changed on disk"),
                                 editor->filePath() + '\n' +
                                     text("覆盖会丢失外部更改。仍要保存吗？",
                                          "Overwriting discards external changes. Save anyway?"),
                                 QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Save)
            return false;
        overwrite = true;
    }
    QString error;
    if (!editor->saveFile(&error, overwrite)) {
        QMessageBox::warning(this, text("保存失败", "Could not save"), error);
        return false;
    }
    m_activity->setText(text("文件已保存", "File saved"));
    refreshRepository();
    return true;
}

bool MainWindow::closeEditor(int index) {
    auto* page = m_editors->widget(index);
    auto* editor = qobject_cast<CodeEditor*>(page);
    if (!editor) {
        if (!qobject_cast<FileDiffView*>(page))
            return true;
        m_editors->removeTab(index);
        page->deleteLater();
        return true;
    }
    if (editor->isModified()) {
        m_editors->setCurrentIndex(index);
        const auto reply = QMessageBox::warning(
            this, text("保存更改？", "Save changes?"), editor->filePath(),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (reply == QMessageBox::Cancel || (reply == QMessageBox::Save && !saveEditor(editor)))
            return false;
    }
    m_editors->removeTab(index);
    editor->deleteLater();
    return true;
}

bool MainWindow::closeEditors() {
    while (m_editors->count() > 0)
        if (!closeEditor(m_editors->count() - 1))
            return false;
    return true;
}

void MainWindow::findText(bool forward, bool fromStart) {
    auto* editor = currentEditor();
    if (!editor)
        return;
    editor->showMarkdownSource();
    if (m_find->text().isEmpty()) {
        m_findCount->setText(QStringLiteral("0/0"));
        m_activity->clear();
        return;
    }
    const bool fuzzy = m_findMode->currentData().toString() == QStringLiteral("fuzzy");
    const auto pattern = fuzzy ? fuzzyScintillaPattern(m_find->text()) : m_find->text();
    if (fromStart) {
        editor->setCursorPosition(0, 0);
    } else if (editor->hasSelectedText()) {
        int fromLine = 0;
        int fromIndex = 0;
        int toLine = 0;
        int toIndex = 0;
        editor->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        editor->setCursorPosition(forward ? toLine : fromLine, forward ? toIndex : fromIndex);
    }
    const bool caseSensitive = m_findCaseSensitive->isChecked();
    const bool found = editor->findFirst(pattern, fuzzy, caseSensitive, false, true, forward);
    int currentMatch = 0;
    int totalMatches = 0;
    if (found) {
        int fromLine = 0;
        int fromIndex = 0;
        int toLine = 0;
        int toIndex = 0;
        editor->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        totalMatches = countEditorMatches(editor, pattern, fuzzy, caseSensitive, fromLine,
                                          fromIndex, &currentMatch);
    }
    m_findCount->setText(QStringLiteral("%1/%2").arg(currentMatch).arg(totalMatches));
    m_activity->setText(found ? text("已找到", "Match found")
                              : text("未找到匹配内容", "No matches found"));
}

void MainWindow::showGlobalSearch() {
    if (!m_globalSearchDialog) {
        auto* dialog = new GlobalSearchDialog(
            m_projectRoot, m_english,
            [this](const GlobalSearchMatch& match) {
                openFile(match.path);
                auto* editor = currentEditor();
                if (!editor || editor->filePath() != QFileInfo(match.path).canonicalFilePath())
                    return;
                editor->showMarkdownSource();
                selectProjectPath(match.path);
                const int line = qMax(0, match.line - 1);
                editor->setSelection(line, match.column, line, match.endColumn);
                editor->ensureLineVisible(line);
                editor->setFocus();
                m_activity->setText(text("已定位到搜索结果", "Opened search result"));
            },
            [this](const QString& path) { selectProjectPath(path); }, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_globalSearchDialog = dialog;
        connect(dialog, &QObject::destroyed, this, [this] { m_globalSearchDialog = nullptr; });
    }

    auto* dialog = static_cast<GlobalSearchDialog*>(m_globalSearchDialog);
    dialog->setRoot(m_projectRoot);
    QString query = m_find->text();
    if (currentEditor() && !currentEditor()->selectedText().isEmpty())
        query = currentEditor()->selectedText();
    dialog->setInitialQuery(query);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    dialog->queryInput()->setFocus();
    dialog->queryInput()->selectAll();
}

void MainWindow::selectProjectPath(const QString& path) {
    if (m_projectRoot.isEmpty() || !m_projectModel || !m_projectFilter || !m_tree)
        return;
    if (m_treeFilter && !m_treeFilter->text().isEmpty())
        m_treeFilter->clear();

    const auto target = QFileInfo(path).canonicalFilePath();
    if (target.isEmpty())
        return;
    const auto relative = QDir::fromNativeSeparators(QDir(m_projectRoot).relativeFilePath(target));
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        relative == QStringLiteral("."))
        return;
    const auto components = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.isEmpty())
        return;

    const int request = ++m_projectRevealRequest;
    auto* timer = new QTimer(this);
    timer->setInterval(25);
    connect(timer, &QTimer::timeout, this,
            [this, timer, request, components, attempt = 0]() mutable {
                const auto finish = [timer] {
                    timer->stop();
                    timer->deleteLater();
                };
                if (request != m_projectRevealRequest) {
                    finish();
                    return;
                }
                if (++attempt > 400) {
                    finish();
                    return;
                }

                QModelIndex parentSource = m_projectModel->index(m_projectRoot);
                QModelIndex parentProxy = m_projectFilter->mapFromSource(parentSource);
                if (!parentSource.isValid() || !parentProxy.isValid())
                    return;
                m_tree->expand(parentProxy);
                QString parentPath = m_projectRoot;

                for (qsizetype part = 0; part < components.size(); ++part) {
                    const auto childPath = QDir(parentPath).filePath(components.at(part));
                    const auto childSource = m_projectModel->index(childPath);
                    if (!childSource.isValid()) {
                        if (m_projectModel->canFetchMore(parentSource))
                            m_projectModel->fetchMore(parentSource);
                        m_tree->expand(parentProxy);
                        return;
                    }
                    const auto childProxy = m_projectFilter->mapFromSource(childSource);
                    if (!childProxy.isValid()) {
                        if (m_projectModel->canFetchMore(parentSource))
                            m_projectModel->fetchMore(parentSource);
                        m_tree->expand(parentProxy);
                        return;
                    }
                    if (part + 1 == components.size()) {
                        m_tree->setCurrentIndex(childProxy);
                        m_tree->scrollTo(childProxy, QAbstractItemView::PositionAtCenter);
                        finish();
                        return;
                    }
                    if (!m_projectModel->isDir(childSource)) {
                        finish();
                        return;
                    }
                    m_tree->expand(childProxy);
                    parentPath = childPath;
                    parentSource = childSource;
                    parentProxy = childProxy;
                }
            });
    timer->start();
}

void MainWindow::showDiff(QTreeWidgetItem* item) {
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= m_status.files.size() || m_repositoryRoot.isEmpty())
        return;
    const auto file = m_status.files[index];
    const int request = ++m_diffRequest;
    m_bottom->setCurrentIndex(1);
    if (file.state() == FileState::Untracked) {
        QFile source(repositoryFilePath(m_repositoryRoot, file.path));
        if (!source.open(QIODevice::ReadOnly) || source.size() > 2 * 1024 * 1024) {
            m_diff->setPlainText(text("文件无法读取或超过 2 MiB 差异预览限制。",
                                      "File unreadable or larger than the 2 MiB preview limit."));
            return;
        }
        const auto bytes = source.readAll();
        if (bytes.contains('\0')) {
            m_diff->setPlainText(text("二进制新文件", "New binary file"));
            return;
        }
        QString preview = text("未跟踪文件：", "Untracked file: ") + displayPath(file.path) + '\n';
        for (const auto& line : QString::fromUtf8(bytes).split('\n'))
            preview += '+' + line + '\n';
        m_diff->setPlainText(preview);
        return;
    }
    m_diff->setPlainText(text("正在加载差异…", "Loading diff…"));
    QStringList args = {"diff", "--no-ext-diff", "--no-textconv", "--unified=3"};
    if (m_hasHead)
        args.append("HEAD");
    args.append({"--", file.path});
    if (!file.originalPath.isEmpty())
        args.append(file.originalPath);
    m_git->run(repositoryArgs(args), [this, request](const GitResult& result) {
        if (request != m_diffRequest)
            return;
        m_diff->setPlainText(
            result.ok() ? (result.output.isEmpty()
                               ? text("没有可显示的文本差异（可能只有权限或二进制变化）。",
                                      "No text diff (possibly mode-only or binary changes).")
                               : QString::fromUtf8(result.output))
                        : QString::fromUtf8(result.error));
    });
}

void MainWindow::showCommit(int row) {
    if (row < 0 || row >= m_commits.size() || gitBusy())
        return;
    const int request = ++m_diffRequest;
    m_bottom->setCurrentIndex(1);
    m_git->run(repositoryArgs({"show", "--no-ext-diff", "--no-textconv", "--format=medium",
                               m_commits[row].hash, "--"}),
               [this, request](const GitResult& result) {
                   if (request == m_diffRequest)
                       m_diff->setPlainText(result.ok() ? QString::fromUtf8(result.output)
                                                        : QString::fromUtf8(result.error));
               });
}

void MainWindow::commitChanges(bool pushAfter) {
    const auto* button = pushAfter ? m_commitAndPush : m_commit;
    if (!button || !button->isEnabled())
        return;
    if (m_amend->isChecked() &&
        QMessageBox::warning(
            this, text("修订上次提交", "Amend last commit"),
            m_repositoryRoot + '\n' + m_status.branch + '\n' +
                text("此操作会替换上一次提交并改变其哈希。如果已经推送，请确认协作影响。是否继续？",
                     "This replaces the last commit and changes its hash. If already pushed, "
                     "consider the impact on collaborators. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QStringList paths;
    for (int groupIndex = 0; groupIndex < m_changes->topLevelItemCount(); ++groupIndex) {
        const auto* group = m_changes->topLevelItem(groupIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            const auto* item = group->child(childIndex);
            if (item->checkState(0) != Qt::Checked)
                continue;
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index < 0 || index >= m_status.files.size() || m_status.files[index].conflict())
                continue;
            const auto& file = m_status.files[index];
            paths.append(file.path);
            if (!file.originalPath.isEmpty())
                paths.append(file.originalPath);
        }
    }
    paths.removeDuplicates();
    if (paths.isEmpty())
        return;

    QStringList args = {"commit", "--only"};
    if (m_amend->isChecked())
        args.append("--amend");
    args.append({"-m", m_message->toPlainText()});
    args.append("--");
    args.append(paths);
    QStringList addArgs{"add", "-A", "--"};
    addArgs.append(paths);
    runOperation(addArgs, [this, args, pushAfter] {
        runOperation(args, [this, pushAfter] {
            m_message->clear();
            m_amend->setChecked(false);
            if (pushAfter)
                showPushPreview();
        });
    });
}

void MainWindow::runOperation(const QStringList& args, std::function<void()> success) {
    if (!m_git || gitBusy() || m_repositoryRoot.isEmpty())
        return;
    m_operationRunning = true;
    updateActions();
    m_git->run(
        repositoryArgs(args),
        [this, success = std::move(success)](const GitResult& result) {
            m_operationRunning = false;
            if (result.ok()) {
                if (success)
                    success();
                m_output->appendPlainText(QString::fromUtf8(result.output));
            } else if (!result.cancelled) {
                reportError(text("Git 操作失败", "Git operation failed"), result);
            }
            // Cancellation is not a rollback: always re-read the actual index and worktree.
            refreshRepository();
            updateActions();
        },
        120000);
    updateActions();
}

void MainWindow::remoteOperation(const QString& operation) {
    if (gitBusy() || m_repositoryRoot.isEmpty())
        return;
    const auto warning =
        text("将使用当前仓库的默认远程配置执行：",
             "Using this repository's default remote configuration:") +
        "\n\n" + m_repositoryRoot + "\n" + m_status.branch + "\ngit " + operation +
        (operation == "pull" ? " --ff-only" : "") + "\n\n" +
        text("本程序不额外添加强制推送参数；实际推送范围与 refspec 遵循已有 Git "
             "配置，请先核验。拉取仅允许快进。终端交互认证不可用，请预先配置凭据助手或 SSH agent。",
             "No extra force-push option is added; existing Git configuration determines the push "
             "scope and refspecs, so review them first. Pull is fast-forward only. Terminal "
             "authentication is unavailable; configure a credential helper or SSH agent first.");
    if (QMessageBox::question(this, text("确认远程操作", "Confirm remote operation"), warning,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    runOperation(operation == "pull" ? QStringList{"pull", "--ff-only"} : QStringList{operation});
}

void MainWindow::showRemotes() {
    if (!m_git || m_repositoryRoot.isEmpty() || gitBusy())
        return;
    RemoteManagerDialog dialog(m_git, m_repositoryRoot, m_english, this);
    dialog.exec();
    refreshRepository();
}

void MainWindow::showCredentials() {
    if (!m_git || gitBusy())
        return;
    GitCredentialDialog dialog(m_git, m_english, this);
    dialog.exec();
}

void MainWindow::showPushPreview() {
    if (!m_git || m_repositoryRoot.isEmpty())
        return;
    if (m_pushDialog) {
        m_pushDialog->raise();
        m_pushDialog->activateWindow();
        return;
    }
    auto* dialog = new PushDialog(m_git, m_repositoryRoot, m_status.branch, m_status.tracking,
                                  m_english, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_pushDialog = dialog;
    connect(dialog, &PushDialog::pushRequested, this, &MainWindow::pushToRemote);
    connect(dialog, &PushDialog::diffRequested, this,
            [this](const QString& commit, const CommitFileChange& file) {
                showCommitFileDiff(commit, file);
            });
    connect(dialog, &QDialog::finished, this, [this] {
        m_pushDialog = nullptr;
        updateActions();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    updateActions();
}

void MainWindow::pushToRemote(const QString& remote, const QString& branch) {
    if (!m_git || !m_pushDialog || m_operationRunning || remote.isEmpty() || branch.isEmpty() ||
        remote.startsWith('-') || branch.startsWith('-'))
        return;
    QPointer<PushDialog> dialog = m_pushDialog;
    dialog->setPushRunning(true);
    m_operationRunning = true;
    updateActions();
    m_git->run(
        repositoryArgs({"check-ref-format", "--branch", branch}),
        [this, dialog, remote, branch](const GitResult& valid) {
            if (!valid.ok()) {
                m_operationRunning = false;
                if (dialog)
                    dialog->pushFinished(
                        false, text("目标分支名称无效。", "The target branch name is invalid."));
                updateActions();
                return;
            }
            const QString refspec = QStringLiteral("HEAD:refs/heads/") + branch;
            m_git->run(
                repositoryArgs({"push", "--porcelain", remote, refspec}),
                [this, dialog](const GitResult& result) {
                    m_operationRunning = false;
                    if (result.ok()) {
                        m_output->appendPlainText(QString::fromUtf8(result.output));
                        if (dialog)
                            dialog->pushFinished(true, text("推送完成。", "Push completed."));
                    } else if (result.cancelled) {
                        if (dialog)
                            dialog->pushFinished(
                                false, text("推送已取消；请刷新确认远程状态。",
                                            "Push cancelled; refresh to verify the remote."));
                    } else {
                        if (dialog)
                            dialog->pushFinished(false, QString::fromUtf8(result.error).left(2000));
                        reportError(text("推送失败", "Push failed"), result);
                    }
                    refreshRepository();
                    updateActions();
                },
                120000);
        });
}

void MainWindow::showBranches() {
    if (gitBusy() || m_repositoryRoot.isEmpty())
        return;
    m_git->run(repositoryArgs({"for-each-ref", "--format=%(refname:short)", "refs/heads/"}),
               [this](const GitResult& result) {
                   if (!result.ok()) {
                       reportError(text("读取分支失败", "Could not read branches"), result);
                       return;
                   }
                   auto* menu = new QMenu(this);
                   connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
                   for (const auto& branch :
                        QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts)) {
                       auto* action = menu->addAction(icon("branch"), branch);
                       action->setCheckable(true);
                       action->setChecked(branch == m_status.branch);
                       connect(action, &QAction::triggered, this, [this, branch] {
                           // QMenu executes inside the read callback; defer until the process has
                           // completed.
                           QTimer::singleShot(
                               0, this, [this, branch] { runOperation({"switch", "--", branch}); });
                       });
                   }
                   menu->addSeparator();
                   auto* create =
                       menu->addAction(text("新建并切换分支…", "Create and switch branch…"));
                   connect(create, &QAction::triggered, this, [this] {
                       QTimer::singleShot(0, this, [this] {
                           bool accepted = false;
                           const auto name = QInputDialog::getText(
                               this, text("新建分支", "New branch"),
                               text("分支名称（Git 会验证名称）", "Branch name (validated by Git)"),
                               QLineEdit::Normal, {}, &accepted);
                           if (accepted && !name.isEmpty())
                               runOperation({"switch", "-c", name});
                       });
                   });
                   menu->popup(m_branch->mapToGlobal(QPoint(0, m_branch->height())));
               });
}

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(text("设置", "Settings"));
    dialog.setMinimumWidth(410);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(text("界面语言", "Interface language"), &dialog);
    layout->addWidget(title);
    auto* chinese = new QRadioButton(QStringLiteral("简体中文"), &dialog);
    auto* english = new QRadioButton(QStringLiteral("English"), &dialog);
    const bool savedEnglish = QSettings().value("ui/language", "zh_CN").toString() == "en";
    chinese->setChecked(!savedEnglish);
    english->setChecked(savedEnglish);
    layout->addWidget(chinese);
    layout->addWidget(english);
    auto* hint =
        new QLabel(text("保存后下次启动生效。仅内置这两种语言。",
                        "Takes effect on next launch. Only these two languages are built in."),
                   &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(text("保存", "Save"));
    buttons->button(QDialogButtonBox::Cancel)->setText(text("取消", "Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted)
        QSettings().setValue("ui/language", english->isChecked() ? "en" : "zh_CN");
}

void MainWindow::reportError(const QString& title, const GitResult& result) {
    QString details = QString::fromUtf8(result.error);
    if (result.timedOut)
        details.prepend(text("操作已超时。\n", "Operation timed out.\n"));
    if (result.outputLimitExceeded)
        details.prepend(text("输出超过 16 MiB 安全上限；已停止命令。\n",
                             "Output exceeded 16 MiB; command stopped.\n"));
    m_output->appendPlainText(details);
    m_bottom->setCurrentIndex(2);
    QMessageBox::warning(this, title, details.left(4000));
}

void MainWindow::updateActions() {
    const bool busy = gitBusy() || m_operationRunning;
    const bool repository = !m_repositoryRoot.isEmpty() && m_status.valid;
    m_saveAction->setEnabled(currentEditor() && currentEditor()->isModified());
    m_refreshAction->setEnabled(!busy && !m_projectRoot.isEmpty());
    m_fetchAction->setEnabled(repository && !busy);
    m_pullAction->setEnabled(repository && !busy);
    m_pushAction->setEnabled(repository && !busy);
    m_branch->setEnabled(repository && !busy);
    m_cancelAction->setEnabled(busy);
    m_projectButton->setEnabled(!busy);
    int selectedCount = 0;
    for (int groupIndex = 0; groupIndex < m_changes->topLevelItemCount(); ++groupIndex) {
        const auto* group = m_changes->topLevelItem(groupIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            const auto* item = group->child(childIndex);
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < m_status.files.size() && item->checkState(0) == Qt::Checked &&
                !m_status.files[index].conflict())
                ++selectedCount;
        }
    }
    const QString commitText =
        selectedCount ? text("提交所选文件（%1）", "Commit selected (%1)").arg(selectedCount)
                      : text("提交所选文件", "Commit selected files");
    const bool canCommit = repository && !busy && !m_pushDialog &&
                           m_pendingOperation == PendingOperation::None && selectedCount > 0 &&
                           !m_message->toPlainText().trimmed().isEmpty();
    m_commit->setText(commitText);
    m_commit->setEnabled(canCommit);
    m_commitAndPush->setEnabled(canCommit);
    m_loadMore->setEnabled(repository && !busy && m_hasHead);
    updateAdvancedActions();
}

void MainWindow::changeEvent(QEvent* event) {
    constexpr int kLateDeactivationGraceMs = 250;
    if (event->type() == QEvent::WindowStateChange) {
        const auto oldState = static_cast<QWindowStateChangeEvent*>(event)->oldState();
        const auto newState = windowState();
        if (newState.testFlag(Qt::WindowMinimized)) {
            m_lastKnownMaximized = oldState.testFlag(Qt::WindowMaximized) || m_lastKnownMaximized ||
                                   m_restoreMaximizedOnActivation;
            m_restoreMaximizedOnActivation = m_lastKnownMaximized;
            m_pendingNormalAfterMaximize = false;
        } else if (newState.testFlag(Qt::WindowMaximized)) {
            m_lastKnownMaximized = true;
            m_restoreMaximizedOnActivation = false;
            m_pendingNormalAfterMaximize = false;
        } else if (oldState.testFlag(Qt::WindowMinimized)) {
            m_restoreMaximizedOnActivation = m_lastKnownMaximized;
        } else if (oldState.testFlag(Qt::WindowMaximized) && m_lastKnownMaximized) {
            if (isActiveWindow()) {
                // Wayland can report max -> normal before the matching deactivation event.
                // Keep the restore intent briefly so a delayed deactivation can claim it;
                // discard it if the window remains active after the grace period.
                m_pendingNormalAfterMaximize = true;
                QTimer::singleShot(kLateDeactivationGraceMs, this, [this] {
                    if (!m_pendingNormalAfterMaximize)
                        return;
                    m_pendingNormalAfterMaximize = false;
                    if (isActiveWindow()) {
                        m_lastKnownMaximized = false;
                        m_restoreMaximizedOnActivation = false;
                    } else {
                        m_restoreMaximizedOnActivation = m_lastKnownMaximized;
                    }
                });
            } else {
                m_restoreMaximizedOnActivation = true;
            }
        }
    }

    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (!isActiveWindow()) {
            if (m_pendingNormalAfterMaximize)
                m_pendingNormalAfterMaximize = false;
            if (m_lastKnownMaximized)
                m_restoreMaximizedOnActivation = true;
        } else {
            if (m_pendingNormalAfterMaximize) {
                m_pendingNormalAfterMaximize = false;
                m_lastKnownMaximized = false;
                m_restoreMaximizedOnActivation = false;
            }
            if (m_restoreMaximizedOnActivation && !isMaximized()) {
                QTimer::singleShot(0, this, [this] {
                    if (!m_restoreMaximizedOnActivation || !isActiveWindow())
                        return;
                    m_restoreMaximizedOnActivation = false;
                    showMaximized();
                });
            }
        }
        if (m_git && !gitBusy())
            QTimer::singleShot(0, this, &MainWindow::refreshRepository);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (gitBusy() &&
        QMessageBox::warning(this, text("Git 操作仍在运行", "Git is still running"),
                             text("退出会终止当前 Git 进程。部分更改可能已经生效，是否仍要退出？",
                                  "Quitting terminates the current Git process. Some changes may "
                                  "already have taken effect. Quit anyway?"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    persistEditorSession();
    if (!closeEditors()) {
        event->ignore();
        return;
    }
    persistProjectTreeState();
    if (!qApp->property("previewMode").toBool()) {
        QSettings settings;
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/horizontal", m_horizontal->saveState());
        settings.setValue("window/vertical", m_vertical->saveState());
        settings.setValue("window/projectPanelVisible", !m_projectPanel->isHidden());
        settings.setValue("window/commitPanelVisible", !m_commitPanel->isHidden());
        settings.setValue("window/historyPanelVisible", !m_bottom->isHidden());
    }
    event->accept();
}

} // namespace idea4git
