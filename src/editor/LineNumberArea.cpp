#include "editor/LineNumberArea.h"

#include "editor/CppCodeEditor.h"

LineNumberArea::LineNumberArea(CppCodeEditor* editor)
    : QWidget(editor)
    , editor_(editor)
{
}

QSize LineNumberArea::sizeHint() const
{
    return {editor_ != nullptr ? editor_->lineNumberAreaWidth() : 0, 0};
}

void LineNumberArea::paintEvent(QPaintEvent* event)
{
    if (editor_ != nullptr) {
        editor_->lineNumberAreaPaintEvent(event);
    }
}
