#pragma once

#include <Qsci/qsciscintilla.h>

class QResizeEvent;
class QTextBrowser;
class QToolButton;

namespace idea4git {

class CodeEditor final : public QsciScintilla {
    Q_OBJECT
  public:
    explicit CodeEditor(QWidget* parent = nullptr);
    bool loadFile(const QString& path, QString* error);
    bool showPreview(const QString& name, const QByteArray& bytes, QString* error);
    void setEnglish(bool english);
    void showMarkdownSource();
    void showMarkdownPreview();
    void focusCurrentView();
    bool isMarkdownPreviewVisible() const { return m_markdownPreviewEnabled; }
    bool saveFile(QString* error, bool overwriteExternal = false);
    bool changedOnDisk() const;
    QString filePath() const { return m_path; }
    QString encodingLabel() const;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void configureLexer();
    void updateMarkdownPreview();
    void updateMarkdownToggle();
    void setMarkdownPreviewEnabled(bool enabled);
    QString m_path;
    QByteArray m_savedBytes;
    bool m_bom = false;
    bool m_isMarkdown = false;
    bool m_markdownPreviewEnabled = false;
    bool m_english = false;
    QTextBrowser* m_markdownPreview = nullptr;
    QToolButton* m_markdownToggle = nullptr;
};

} // namespace idea4git
