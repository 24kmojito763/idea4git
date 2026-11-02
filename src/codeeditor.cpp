#include "codeeditor.h"

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollBar>
#include <QStringDecoder>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QUrl>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerxml.h>

namespace idea4git {
namespace {

bool isMarkdownPath(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown") ||
           suffix == QStringLiteral("mdown") || suffix == QStringLiteral("mkd");
}

} // namespace

CodeEditor::CodeEditor(QWidget* parent) : QsciScintilla(parent) {
    setObjectName(QStringLiteral("codeEditor"));
    setUtf8(true);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(11);
    setFont(font);
    setMarginsFont(font);
    setPaper(QColor("#1e1f22"));
    setColor(QColor("#bcbec4"));
    setMarginsBackgroundColor(QColor("#1e1f22"));
    setMarginsForegroundColor(QColor("#6f737a"));
    setMarginType(0, NumberMargin);
    setMarginWidth(0, QStringLiteral("00000"));
    setMarginWidth(1, 0);
    setMarginWidth(2, 14);
    setFolding(BoxedTreeFoldStyle, 2);
    setFoldMarginColors(QColor("#1e1f22"), QColor("#1e1f22"));
    setCaretForegroundColor(QColor("#dfe1e5"));
    setCaretLineVisible(true);
    setCaretLineBackgroundColor(QColor("#26282e"));
    setSelectionBackgroundColor(QColor("#355580"));
    setSelectionForegroundColor(QColor("#dfe1e5"));
    setIndentationGuides(true);
    setIndentationGuidesBackgroundColor(QColor("#303238"));
    setIndentationGuidesForegroundColor(QColor("#42454e"));
    setIndentationsUseTabs(false);
    setTabWidth(4);
    setAutoIndent(true);
    setBraceMatching(SloppyBraceMatch);
    setMatchedBraceBackgroundColor(QColor("#3b514d"));
    setMatchedBraceForegroundColor(QColor("#b5d5c5"));
    setEolMode(EolUnix);
    setWrapMode(WrapNone);
    setScrollWidth(1);
    setScrollWidthTracking(true);
    setFrameShape(QFrame::NoFrame);

    m_markdownPreview = new QTextBrowser(this);
    m_markdownPreview->setObjectName(QStringLiteral("markdownPreview"));
    m_markdownPreview->setFrameShape(QFrame::NoFrame);
    m_markdownPreview->setOpenLinks(false);
    m_markdownPreview->setOpenExternalLinks(true);
    m_markdownPreview->setContextMenuPolicy(Qt::NoContextMenu);
    m_markdownPreview->setStyleSheet(
        QStringLiteral("QTextBrowser { background: #1e1f22; color: #bcbec4; border: none; }"));
    m_markdownPreview->document()->setDocumentMargin(24.0);
    m_markdownPreview->document()->setDefaultStyleSheet(
        QStringLiteral("body { color: #bcbec4; background: #1e1f22; }"
                       "h1, h2, h3, h4 { color: #dfe1e5; border-bottom: 1px solid #3c3f41; }"
                       "a { color: #589df6; }"
                       "pre, code { background: #2b2d30; color: #cf8e6d; }"
                       "blockquote { color: #9da0a8; border-left: 3px solid #555963; }"
                       "table { border-collapse: collapse; }"
                       "th, td { border: 1px solid #45474f; padding: 5px 9px; }"
                       "hr { color: #45474f; }"));
    m_markdownPreview->hide();

    m_markdownToggle = new QToolButton(this);
    m_markdownToggle->setObjectName(QStringLiteral("markdownModeToggle"));
    m_markdownToggle->setAutoRaise(false);
    m_markdownToggle->setStyleSheet(QStringLiteral(
        "QToolButton { color: #dfe1e5; background: #35373d; border: 1px solid #4b4e56; "
        "border-radius: 4px; padding: 5px 10px; }"
        "QToolButton:hover { background: #454851; }"));
    m_markdownToggle->hide();
    connect(m_markdownToggle, &QToolButton::clicked, this, [this] {
        setMarkdownPreviewEnabled(!m_markdownPreviewEnabled);
        focusCurrentView();
    });
}

bool CodeEditor::loadFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.size() > 5 * 1024 * 1024) {
        *error = tr("This preview supports text files up to 5 MiB.");
        return false;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFile::NoError) {
        *error = file.errorString();
        return false;
    }
    if (bytes.contains('\0')) {
        *error = tr("Binary files cannot be edited.");
        return false;
    }
    m_bom = bytes.startsWith("\xef\xbb\xbf");
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString contents = decoder.decode(m_bom ? bytes.mid(3) : bytes);
    if (decoder.hasError()) {
        *error = tr("Only UTF-8 files can be edited safely in this version.");
        return false;
    }
    m_path = QFileInfo(path).canonicalFilePath();
    m_savedBytes = bytes;
    m_isMarkdown = isMarkdownPath(m_path);
    configureLexer();
    setReadOnly(false);
    setText(contents);
    setEolMode(bytes.contains("\r\n") ? EolWindows : EolUnix);
    setCursorPosition(0, 0);
    setModified(false);
    setMarkdownPreviewEnabled(m_isMarkdown);
    return true;
}

bool CodeEditor::changedOnDisk() const {
    QFile file(m_path);
    return !file.open(QIODevice::ReadOnly) || file.size() != m_savedBytes.size() ||
           file.readAll() != m_savedBytes;
}

bool CodeEditor::showPreview(const QString& name, const QByteArray& bytes, QString* error) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const bool bom = bytes.startsWith("\xef\xbb\xbf");
    const QString contents = decoder.decode(bom ? bytes.mid(3) : bytes);
    if (bytes.size() > 5 * 1024 * 1024 || bytes.contains('\0') || decoder.hasError()) {
        *error = tr("Only UTF-8 files can be edited safely in this version.");
        return false;
    }
    m_path = name;
    m_bom = bom;
    m_savedBytes = bytes;
    m_isMarkdown = false;
    configureLexer();
    setMarkdownPreviewEnabled(false);
    setReadOnly(false);
    setText(contents);
    setEolMode(bytes.contains("\r\n") ? EolWindows : EolUnix);
    setCursorPosition(0, 0);
    setModified(false);
    setReadOnly(true);
    return true;
}

bool CodeEditor::saveFile(QString* error, bool overwriteExternal) {
    if (isReadOnly()) {
        *error = tr("Binary files cannot be edited.");
        return false;
    }
    if (!overwriteExternal && changedOnDisk()) {
        *error = tr("File changed on disk. Reload it or explicitly confirm overwriting.");
        return false;
    }
    QByteArray bytes = text().toUtf8();
    if (m_bom)
        bytes.prepend("\xef\xbb\xbf");
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    m_savedBytes = bytes;
    setModified(false);
    return true;
}

QString CodeEditor::encodingLabel() const {
    return (m_bom ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8")) +
           QStringLiteral("    ") + (eolMode() == EolWindows ? "CRLF" : "LF");
}

void CodeEditor::setEnglish(bool english) {
    m_english = english;
    updateMarkdownToggle();
}

void CodeEditor::showMarkdownSource() { setMarkdownPreviewEnabled(false); }

void CodeEditor::showMarkdownPreview() { setMarkdownPreviewEnabled(true); }

void CodeEditor::focusCurrentView() {
    if (m_markdownPreviewEnabled)
        m_markdownPreview->setFocus(Qt::OtherFocusReason);
    else
        setFocus(Qt::OtherFocusReason);
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QsciScintilla::resizeEvent(event);
    if (!m_markdownPreview || !m_markdownToggle)
        return;

    m_markdownPreview->setGeometry(rect());
    m_markdownToggle->adjustSize();
    m_markdownToggle->move(width() - m_markdownToggle->width() - 14, 10);
    if (m_markdownPreviewEnabled)
        m_markdownPreview->raise();
    if (m_isMarkdown)
        m_markdownToggle->raise();
}

void CodeEditor::updateMarkdownPreview() {
    if (!m_markdownPreview || !m_isMarkdown)
        return;

    const auto baseUrl = QUrl::fromLocalFile(QFileInfo(m_path).absolutePath() + QLatin1Char('/'));
    m_markdownPreview->document()->setBaseUrl(baseUrl);
    m_markdownPreview->setMarkdown(text());
    m_markdownPreview->document()->setBaseUrl(baseUrl);
    m_markdownPreview->verticalScrollBar()->setValue(0);
}

void CodeEditor::updateMarkdownToggle() {
    if (!m_markdownToggle)
        return;
    if (!m_isMarkdown) {
        m_markdownToggle->hide();
        return;
    }

    m_markdownToggle->setText(
        m_markdownPreviewEnabled
            ? (m_english ? QStringLiteral("Source") : QStringLiteral("源码"))
            : (m_english ? QStringLiteral("Preview") : QStringLiteral("预览")));
    m_markdownToggle->setToolTip(m_markdownPreviewEnabled
                                     ? (m_english ? QStringLiteral("Edit Markdown source")
                                                  : QStringLiteral("编辑 Markdown 源码"))
                                     : (m_english ? QStringLiteral("Preview Markdown")
                                                  : QStringLiteral("预览 Markdown 文档")));
    m_markdownToggle->setAccessibleName(m_markdownToggle->toolTip());
    m_markdownToggle->show();
    m_markdownToggle->raise();
}

void CodeEditor::setMarkdownPreviewEnabled(bool enabled) {
    m_markdownPreviewEnabled = enabled && m_isMarkdown;
    if (m_markdownPreviewEnabled) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        updateMarkdownPreview();
        m_markdownPreview->setGeometry(rect());
        m_markdownPreview->show();
        m_markdownPreview->raise();
    } else if (m_markdownPreview) {
        m_markdownPreview->hide();
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
    updateMarkdownToggle();
}

void CodeEditor::configureLexer() {
    auto* previous = lexer();
    setLexer(nullptr);
    delete previous;
    const QString suffix = QFileInfo(m_path).suffix().toLower();
    QsciLexer* language = nullptr;
    if (QStringList{"cpp", "cc", "c", "h", "hpp", "java", "js", "ts"}.contains(suffix))
        language = new QsciLexerCPP(this);
    else if (suffix == "py")
        language = new QsciLexerPython(this);
    else if (suffix == "json")
        language = new QsciLexerJSON(this);
    else if (QStringList{"md", "markdown", "mdown", "mkd"}.contains(suffix))
        language = new QsciLexerMarkdown(this);
    else if (suffix == "sh")
        language = new QsciLexerBash(this);
    else if (suffix == "css")
        language = new QsciLexerCSS(this);
    else if (suffix == "xml" || suffix == "html" || suffix == "svg")
        language = new QsciLexerXML(this);
    else if (suffix == "cmake" || QFileInfo(m_path).fileName() == "CMakeLists.txt")
        language = new QsciLexerCMake(this);
    if (!language)
        return;
    language->setDefaultFont(font());
    language->setDefaultPaper(QColor("#1e1f22"));
    language->setDefaultColor(QColor("#bcbec4"));
    for (int style = 0; style < 128; ++style) {
        language->setPaper(QColor("#1e1f22"), style);
        language->setFont(font(), style);
        const QColor original = language->defaultColor(style);
        QColor mapped("#bcbec4");
        if (original.green() > original.red() * 1.3 && original.green() > original.blue())
            mapped = QColor("#6aab73");
        else if (original.red() > original.green() * 1.3)
            mapped = QColor("#cf8e6d");
        else if (original.blue() > original.red() * 1.3)
            mapped = QColor("#c77dbb");
        language->setColor(mapped, style);
    }
    if (auto* cpp = qobject_cast<QsciLexerCPP*>(language)) {
        for (int style :
             {QsciLexerCPP::Comment, QsciLexerCPP::CommentLine, QsciLexerCPP::CommentDoc})
            cpp->setColor(QColor("#7a7e85"), style);
        cpp->setColor(QColor("#cf8e6d"), QsciLexerCPP::Keyword);
        cpp->setColor(QColor("#6aab73"), QsciLexerCPP::DoubleQuotedString);
        cpp->setColor(QColor("#6aab73"), QsciLexerCPP::SingleQuotedString);
        cpp->setColor(QColor("#2aacb8"), QsciLexerCPP::Number);
        cpp->setColor(QColor("#bbb529"), QsciLexerCPP::PreProcessor);
    }
    setLexer(language);
}

} // namespace idea4git
