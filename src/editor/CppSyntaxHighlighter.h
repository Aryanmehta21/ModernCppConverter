#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <vector>

class QTextDocument;

class CppSyntaxHighlighter final : public QSyntaxHighlighter
{
public:
    explicit CppSyntaxHighlighter(QTextDocument* parent);
    void setDarkTheme(bool darkTheme);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct HighlightRule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    void rebuildRules();

    std::vector<HighlightRule> rules_;
    QRegularExpression commentStartExpression_;
    QRegularExpression commentEndExpression_;
    QTextCharFormat multiLineCommentFormat_;
    bool darkTheme_ = false;
};
