#include "editor/CppCodeEditor.h"

#include "editor/CppSyntaxHighlighter.h"
#include "editor/LineNumberArea.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QTextBlock>

namespace
{
constexpr int spacesPerIndent = 4;

QString leadingWhitespace(const QString& text)
{
    int index = 0;
    while (index < text.size() && (text[index] == QLatin1Char(' ') || text[index] == QLatin1Char('\t'))) {
        ++index;
    }
    return text.left(index);
}

bool isOpeningBracket(const QString& text)
{
    return text == "(" || text == "{" || text == "[" || text == "\"" || text == "'";
}

QString matchingClosingBracket(const QString& text)
{
    if (text == "(") {
        return ")";
    }
    if (text == "{") {
        return "}";
    }
    if (text == "[") {
        return "]";
    }
    if (text == "\"") {
        return "\"";
    }
    if (text == "'") {
        return "'";
    }
    return {};
}

bool isClosingBracket(const QString& text)
{
    return text == ")" || text == "}" || text == "]" || text == "\"" || text == "'";
}

bool isPair(const QChar& left, const QChar& right)
{
    return (left == QLatin1Char('(') && right == QLatin1Char(')'))
        || (left == QLatin1Char('{') && right == QLatin1Char('}'))
        || (left == QLatin1Char('[') && right == QLatin1Char(']'))
        || (left == QLatin1Char('"') && right == QLatin1Char('"'))
        || (left == QLatin1Char('\'') && right == QLatin1Char('\''));
}
} // namespace

CppCodeEditor::CppCodeEditor(QWidget* parent)
    : QPlainTextEdit(parent)
    , lineNumberArea_(new LineNumberArea(this))
    , highlighter_(new CppSyntaxHighlighter(document()))
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::StyledPanel);

    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixedFont.setStyleHint(QFont::Monospace);
    setFont(fixedFont);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * spacesPerIndent);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CppCodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CppCodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CppCodeEditor::highlightCurrentLine);

    applyTheme();
    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

int CppCodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int maximum = qMax(1, blockCount());
    while (maximum >= 10) {
        maximum /= 10;
        ++digits;
    }

    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CppCodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberArea_);
    const bool darkTheme = isDarkTheme();
    const QColor background = darkTheme ? QColor(31, 36, 46) : QColor(245, 247, 250);
    const QColor foreground = darkTheme ? QColor(156, 166, 178) : QColor(115, 125, 140);
    painter.fillRect(event->rect(), background);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(foreground);
            painter.drawText(0,
                             top,
                             lineNumberArea_->width() - 6,
                             fontMetrics().height(),
                             Qt::AlignRight,
                             number);
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CppCodeEditor::setAutoIndentEnabled(bool enabled)
{
    autoIndentEnabled_ = enabled;
}

void CppCodeEditor::setAutoCloseBracketsEnabled(bool enabled)
{
    autoCloseBracketsEnabled_ = enabled;
}

bool CppCodeEditor::isDarkTheme() const
{
    const QColor baseColor = palette().color(QPalette::Base);
    return baseColor.lightness() < 128;
}

void CppCodeEditor::applyTheme()
{
    const bool darkTheme = isDarkTheme();
    if (highlighter_ != nullptr) {
        highlighter_->setDarkTheme(darkTheme);
    }
    lineNumberArea_->update();
    highlightCurrentLine();
}

void CppCodeEditor::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        applyTheme();
    }
}

void CppCodeEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier) {
        indentSelection();
        return;
    }

    if (event->key() == Qt::Key_Backtab
        || (event->key() == Qt::Key_Tab && event->modifiers().testFlag(Qt::ShiftModifier))) {
        unindentSelection();
        return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && autoIndentEnabled_) {
        if (handleReturnKey()) {
            return;
        }
    }

    if (event->key() == Qt::Key_Backspace && autoCloseBracketsEnabled_ && maybeRemoveEmptyPair()) {
        return;
    }

    const QString text = event->text();
    if (autoCloseBracketsEnabled_ && text.size() == 1) {
        if (isClosingBracket(text) && maybeSkipClosingCharacter(text)) {
            return;
        }
        if (isOpeningBracket(text)) {
            insertPairedCharacter(text, matchingClosingBracket(text));
            return;
        }
    }

    QPlainTextEdit::keyPressEvent(event);
}

void CppCodeEditor::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);

    const QRect contents = contentsRect();
    lineNumberArea_->setGeometry(QRect(contents.left(),
                                       contents.top(),
                                       lineNumberAreaWidth(),
                                       contents.height()));
}

void CppCodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CppCodeEditor::updateLineNumberArea(const QRect& rect, int dy)
{
    if (dy != 0) {
        lineNumberArea_->scroll(0, dy);
    } else {
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void CppCodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(isDarkTheme() ? QColor(37, 48, 64) : QColor(232, 242, 255));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    selections.append(selection);

    setExtraSelections(selections);
}

void CppCodeEditor::indentSelection()
{
    QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) {
        cursor.insertText(indentUnit());
        return;
    }

    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    QTextBlock block = document()->findBlock(start);
    cursor.beginEditBlock();
    while (block.isValid() && block.position() <= end) {
        QTextBlock nextBlock = block.next();
        QTextCursor lineCursor(block);
        lineCursor.insertText(indentUnit());
        block = nextBlock;
    }
    cursor.endEditBlock();
}

void CppCodeEditor::unindentSelection()
{
    QTextCursor cursor = textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : cursor.block().position();
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : cursor.block().position();
    QTextBlock block = document()->findBlock(start);

    cursor.beginEditBlock();
    while (block.isValid() && block.position() <= end) {
        QTextBlock nextBlock = block.next();
        const QString text = block.text();
        int removeCount = 0;
        if (text.startsWith(QLatin1Char('\t'))) {
            removeCount = 1;
        } else {
            while (removeCount < spacesPerIndent
                   && removeCount < text.size()
                   && text[removeCount] == QLatin1Char(' ')) {
                ++removeCount;
            }
        }

        if (removeCount > 0) {
            QTextCursor lineCursor(block);
            lineCursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, removeCount);
            lineCursor.removeSelectedText();
        }

        block = nextBlock;
    }
    cursor.endEditBlock();
}

void CppCodeEditor::insertPairedCharacter(const QString& opener, const QString& closer)
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        const QString selected = cursor.selectedText();
        cursor.insertText(opener + selected + closer);
        return;
    }

    cursor.insertText(opener + closer);
    cursor.movePosition(QTextCursor::Left);
    setTextCursor(cursor);
}

bool CppCodeEditor::maybeSkipClosingCharacter(const QString& text)
{
    QTextCursor cursor = textCursor();
    const QString blockText = cursor.block().text();
    const int position = cursor.positionInBlock();
    if (position < blockText.size() && blockText.mid(position, 1) == text) {
        cursor.movePosition(QTextCursor::Right);
        setTextCursor(cursor);
        return true;
    }
    return false;
}

bool CppCodeEditor::maybeRemoveEmptyPair()
{
    QTextCursor cursor = textCursor();
    const QString blockText = cursor.block().text();
    const int position = cursor.positionInBlock();
    if (position <= 0 || position >= blockText.size()) {
        return false;
    }

    if (!isPair(blockText[position - 1], blockText[position])) {
        return false;
    }

    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    setTextCursor(cursor);
    return true;
}

bool CppCodeEditor::handleReturnKey()
{
    QTextCursor cursor = textCursor();
    const QString blockText = cursor.block().text();
    const int position = cursor.positionInBlock();
    const QString beforeCursor = blockText.left(position);
    const QString afterCursor = blockText.mid(position);
    const QString indent = leadingWhitespace(beforeCursor);
    const QString extraIndent = beforeCursor.trimmed().endsWith(QLatin1Char('{')) ? indentUnit() : QString{};

    if (beforeCursor.trimmed().endsWith(QLatin1Char('{')) && afterCursor.trimmed().startsWith(QLatin1Char('}'))) {
        cursor.insertText("\n" + indent + extraIndent + "\n" + indent);
        cursor.movePosition(QTextCursor::Up);
        cursor.movePosition(QTextCursor::EndOfLine);
        setTextCursor(cursor);
        return true;
    }

    cursor.insertText("\n" + indent + extraIndent);
    return true;
}

QString CppCodeEditor::currentLineIndent() const
{
    return leadingWhitespace(textCursor().block().text());
}

QString CppCodeEditor::indentUnit() const
{
    return QString(spacesPerIndent, QLatin1Char(' '));
}
