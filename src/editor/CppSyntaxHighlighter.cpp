#include "editor/CppSyntaxHighlighter.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QRegularExpression>
#include <QTextDocument>

namespace
{
QTextCharFormat formatWithColor(const QColor& color, bool bold = false, bool italic = false)
{
    QTextCharFormat format;
    format.setForeground(color);
    if (bold) {
        format.setFontWeight(QFont::Bold);
    }
    format.setFontItalic(italic);
    return format;
}
} // namespace

CppSyntaxHighlighter::CppSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
    , commentStartExpression_(R"(/\*)")
    , commentEndExpression_(R"(\*/)")
{
    rebuildRules();
}

void CppSyntaxHighlighter::setDarkTheme(bool darkTheme)
{
    if (darkTheme_ == darkTheme) {
        return;
    }

    darkTheme_ = darkTheme;
    rebuildRules();
    rehighlight();
}

void CppSyntaxHighlighter::rebuildRules()
{
    rules_.clear();

    const QColor keywordColor = darkTheme_ ? QColor(130, 170, 255) : QColor(0, 92, 197);
    const QColor typeColor = darkTheme_ ? QColor(78, 201, 176) : QColor(0, 128, 128);
    const QColor preprocessorColor = darkTheme_ ? QColor(198, 134, 190) : QColor(128, 0, 128);
    const QColor stringColor = darkTheme_ ? QColor(206, 145, 120) : QColor(163, 21, 21);
    const QColor numberColor = darkTheme_ ? QColor(181, 206, 168) : QColor(128, 64, 0);
    const QColor commentColor = darkTheme_ ? QColor(139, 148, 158) : QColor(106, 115, 125);

    const QTextCharFormat keywordFormat = formatWithColor(keywordColor, true);
    const QStringList keywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "class", "co_await", "co_return", "co_yield", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "final", "for", "friend", "goto", "if", "inline", "mutable",
        "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
        "or_eq", "override", "private", "protected", "public", "reflexpr", "register",
        "reinterpret_cast", "requires", "return", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "synchronized", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "using", "virtual", "void", "volatile", "while", "xor", "xor_eq",
    };
    for (const QString& keyword : keywords) {
        rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(keyword)), keywordFormat});
    }

    const QTextCharFormat typeFormat = formatWithColor(typeColor, true);
    const QStringList types = {
        "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long",
        "short", "signed", "unsigned", "wchar_t", "size_t", "std::array", "std::deque",
        "std::map", "std::optional", "std::pair", "std::shared_ptr", "std::span",
        "std::string", "std::string_view", "std::tuple", "std::unique_ptr",
        "std::variant", "std::vector",
    };
    for (const QString& type : types) {
        rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(type))), typeFormat});
    }

    rules_.push_back({QRegularExpression(R"(#[ \t]*[A-Za-z_]\w*)"), formatWithColor(preprocessorColor, true)});
    rules_.push_back({QRegularExpression(R"((?:"(?:\\.|[^"\\])*"))"), formatWithColor(stringColor)});
    rules_.push_back({QRegularExpression(R"((?:'(?:\\.|[^'\\])*'))"), formatWithColor(stringColor)});
    rules_.push_back({QRegularExpression(R"(\b(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)(?:[uUlLfF]*)\b)"), formatWithColor(numberColor)});
    rules_.push_back({QRegularExpression(R"(//[^\n]*)"), formatWithColor(commentColor, false, true)});
    multiLineCommentFormat_ = formatWithColor(commentColor, false, true);
}

void CppSyntaxHighlighter::highlightBlock(const QString& text)
{
    for (const HighlightRule& rule : rules_) {
        QRegularExpressionMatchIterator iterator = rule.pattern.globalMatch(text);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), rule.format);
        }
    }

    setCurrentBlockState(0);

    int startIndex = 0;
    if (previousBlockState() != 1) {
        startIndex = static_cast<int>(text.indexOf(commentStartExpression_));
    }

    while (startIndex >= 0) {
        const QRegularExpressionMatch endMatch = commentEndExpression_.match(text, startIndex);
        int commentLength = 0;
        if (!endMatch.hasMatch()) {
            setCurrentBlockState(1);
            commentLength = text.length() - startIndex;
        } else {
            commentLength = static_cast<int>(endMatch.capturedEnd() - startIndex);
        }
        setFormat(startIndex, commentLength, multiLineCommentFormat_);
        startIndex = static_cast<int>(text.indexOf(commentStartExpression_, startIndex + commentLength));
    }
}
