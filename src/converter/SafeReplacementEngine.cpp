#include "converter/SafeReplacementEngine.h"

#include <sstream>

namespace
{
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool endsWithLineContinuation(const std::string& line)
{
    std::size_t cursor = line.size();
    while (cursor > 0 && (line[cursor - 1] == ' ' || line[cursor - 1] == '\t' || line[cursor - 1] == '\r')) {
        --cursor;
    }
    return cursor > 0 && line[cursor - 1] == '\\';
}
} // namespace

bool SafeReplacementEngine::isCodeLine(const std::string& line, bool inBlockComment) const
{
    const std::string stripped = trim(line);
    if (stripped.empty() || inBlockComment) {
        return false;
    }
    if (startsWith(stripped, "//") || startsWith(stripped, "/*") || startsWith(stripped, "*") || startsWith(stripped, "#")) {
        return false;
    }
    return true;
}

std::string SafeReplacementEngine::splitTrailingLineComment(const std::string& line, std::string& trailingComment)
{
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;

    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        const char current = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (!inString && !inCharacter && current == '/' && line[index + 1] == '/') {
            trailingComment = line.substr(index);
            return line.substr(0, index);
        }
    }

    trailingComment.clear();
    return line;
}

std::string SafeReplacementEngine::rewriteCodeLines(const std::string& code, const LineRewrite& rewrite) const
{
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool firstLine = true;
    bool inBlockComment = false;
    bool inPreprocessorContinuation = false;

    while (std::getline(input, line)) {
        const bool wasInBlockComment = inBlockComment;
        const bool wasInPreprocessorContinuation = inPreprocessorContinuation;
        const std::string stripped = trim(line);
        const std::size_t blockStart = line.find("/*");
        const std::size_t blockEnd = line.find("*/");
        if (blockStart != std::string::npos && (blockEnd == std::string::npos || blockEnd < blockStart)) {
            inBlockComment = true;
        }

        std::string rewritten = line;
        if (!wasInPreprocessorContinuation && isCodeLine(line, wasInBlockComment)) {
            rewritten = rewrite(line);
        }

        if (blockEnd != std::string::npos) {
            inBlockComment = false;
        }
        if (startsWith(stripped, "#") || wasInPreprocessorContinuation) {
            inPreprocessorContinuation = endsWithLineContinuation(line);
        } else {
            inPreprocessorContinuation = false;
        }

        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << rewritten;
    }

    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }
    return output.str();
}
