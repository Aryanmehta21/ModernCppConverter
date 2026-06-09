#pragma once

#include <QPlainTextEdit>

class CppSyntaxHighlighter;
class LineNumberArea;
class QEvent;
class QPaintEvent;

class CppCodeEditor final : public QPlainTextEdit
{
public:
    explicit CppCodeEditor(QWidget* parent = nullptr);

    [[nodiscard]] int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);
    void setAutoIndentEnabled(bool enabled);
    void setAutoCloseBracketsEnabled(bool enabled);

protected:
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    [[nodiscard]] bool isDarkTheme() const;
    void applyTheme();
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();
    void indentSelection();
    void unindentSelection();
    void insertPairedCharacter(const QString& opener, const QString& closer);
    bool maybeSkipClosingCharacter(const QString& text);
    bool maybeRemoveEmptyPair();
    bool handleReturnKey();
    [[nodiscard]] QString currentLineIndent() const;
    [[nodiscard]] QString indentUnit() const;

    LineNumberArea* lineNumberArea_ = nullptr;
    CppSyntaxHighlighter* highlighter_ = nullptr;
    bool autoIndentEnabled_ = true;
    bool autoCloseBracketsEnabled_ = true;
};
