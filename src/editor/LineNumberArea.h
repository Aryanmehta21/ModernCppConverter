#pragma once

#include <QWidget>

class CppCodeEditor;

class LineNumberArea final : public QWidget
{
public:
    explicit LineNumberArea(CppCodeEditor* editor);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    CppCodeEditor* editor_ = nullptr;
};
