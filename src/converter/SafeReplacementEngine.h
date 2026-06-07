#pragma once

#include <functional>
#include <string>

class SafeReplacementEngine
{
public:
    using LineRewrite = std::function<std::string(const std::string&)>;

    [[nodiscard]] std::string rewriteCodeLines(const std::string& code, const LineRewrite& rewrite) const;
    [[nodiscard]] bool isCodeLine(const std::string& line, bool inBlockComment) const;
    [[nodiscard]] static std::string splitTrailingLineComment(const std::string& line,
                                                              std::string& trailingComment);
};
