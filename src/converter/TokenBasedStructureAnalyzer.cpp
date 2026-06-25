#include "converter/TokenBasedStructureAnalyzer.h"

#include <cctype>
#include <regex>
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

std::vector<std::string> splitLines(const std::string& code)
{
    std::vector<std::string> lines;
    std::stringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::size_t findMatchingBrace(const std::string& code, std::size_t openBrace)
{
    int depth = 1;
    bool inString = false;
    bool inCharacter = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inPreprocessorLine = false;
    bool escaped = false;
    bool atLineStart = false;
    for (std::size_t position = openBrace + 1; position < code.size(); ++position) {
        const char current = code[position];
        const char next = position + 1 < code.size() ? code[position + 1] : '\0';

        if (inPreprocessorLine) {
            if (current == '\n') {
                inPreprocessorLine = false;
                atLineStart = true;
            }
            continue;
        }
        if (inLineComment) {
            if (current == '\n') {
                inLineComment = false;
                atLineStart = true;
            }
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                inBlockComment = false;
                ++position;
            }
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (current == '\n') {
            atLineStart = true;
            continue;
        }
        if (atLineStart && std::isspace(static_cast<unsigned char>(current)) != 0) {
            continue;
        }
        if (atLineStart && current == '#') {
            inPreprocessorLine = true;
            continue;
        }
        atLineStart = false;

        if (!inString && !inCharacter && current == '/' && next == '/') {
            inLineComment = true;
            ++position;
            continue;
        }
        if (!inString && !inCharacter && current == '/' && next == '*') {
            inBlockComment = true;
            ++position;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }

        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return position;
            }
        }
    }
    return std::string::npos;
}

std::vector<ClassBlock> analyzeClasses(const std::string& code)
{
    static const std::regex classHeader(R"(\b(class|struct)\s+([A-Za-z_]\w*)[^;{]*\{)");
    std::vector<ClassBlock> classes;
    std::string remaining = code;
    std::size_t offset = 0;
    std::smatch match;

    while (std::regex_search(remaining, match, classHeader)) {
        const std::size_t start = offset + static_cast<std::size_t>(match.position());
        const std::size_t headerEnd = start + static_cast<std::size_t>(match.length());
        const std::size_t openBrace = code.rfind('{', headerEnd - 1);
        if (openBrace == std::string::npos) {
            break;
        }

        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        std::size_t end = closeBrace + 1;
        while (end < code.size() && std::isspace(static_cast<unsigned char>(code[end]))) {
            ++end;
        }
        if (end < code.size() && code[end] == ';') {
            ++end;
        }

        classes.push_back(ClassBlock{
            start,
            openBrace,
            closeBrace,
            end,
            match[1].str(),
            match[2].str(),
            code.substr(start, end - start),
        });

        offset = end;
        remaining = code.substr(offset);
    }

    return classes;
}

std::vector<StructTypedefDeclaration> analyzeTypedefStructs(const std::string& code)
{
    static const std::regex typedefHeader(R"(\btypedef\s+struct(?:\s+([A-Za-z_]\w*))?\s*\{)");
    std::vector<StructTypedefDeclaration> declarations;
    std::string remaining = code;
    std::size_t offset = 0;
    std::smatch match;

    while (std::regex_search(remaining, match, typedefHeader)) {
        const std::size_t start = offset + static_cast<std::size_t>(match.position());
        const std::size_t headerEnd = start + static_cast<std::size_t>(match.length());
        const std::size_t openBrace = code.rfind('{', headerEnd - 1);
        if (openBrace == std::string::npos) {
            break;
        }

        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        std::size_t aliasStart = closeBrace + 1;
        while (aliasStart < code.size() && std::isspace(static_cast<unsigned char>(code[aliasStart]))) {
            ++aliasStart;
        }
        std::size_t aliasEnd = aliasStart;
        while (aliasEnd < code.size()
               && (std::isalnum(static_cast<unsigned char>(code[aliasEnd])) || code[aliasEnd] == '_')) {
            ++aliasEnd;
        }
        std::size_t semicolon = aliasEnd;
        while (semicolon < code.size() && std::isspace(static_cast<unsigned char>(code[semicolon]))) {
            ++semicolon;
        }
        if (aliasStart == aliasEnd || semicolon >= code.size() || code[semicolon] != ';') {
            offset = closeBrace + 1;
            remaining = code.substr(offset);
            continue;
        }

        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        declarations.push_back(StructTypedefDeclaration{
            start,
            semicolon + 1,
            code.substr(start, semicolon + 1 - start),
            match[1].matched ? match[1].str() : std::string{},
            code.substr(aliasStart, aliasEnd - aliasStart),
            body,
            body.find("(*") != std::string::npos,
        });

        offset = semicolon + 1;
        remaining = code.substr(offset);
    }

    return declarations;
}

std::vector<LoopBlock> analyzeLoops(const std::string& code)
{
    static const std::regex loopStartPattern(R"(\bfor\s*\()");
    std::vector<LoopBlock> loops;
    std::string remaining = code;
    std::size_t offset = 0;
    std::smatch match;

    while (std::regex_search(remaining, match, loopStartPattern)) {
        const std::size_t start = offset + static_cast<std::size_t>(match.position());
        const std::size_t openParen = code.find('(', start);
        if (openParen == std::string::npos) {
            break;
        }

        int parenDepth = 1;
        std::size_t closeParen = openParen + 1;
        for (; closeParen < code.size(); ++closeParen) {
            if (code[closeParen] == '(') {
                ++parenDepth;
            } else if (code[closeParen] == ')') {
                --parenDepth;
                if (parenDepth == 0) {
                    break;
                }
            }
        }
        if (closeParen >= code.size()) {
            break;
        }

        std::size_t openBrace = closeParen + 1;
        while (openBrace < code.size() && std::isspace(static_cast<unsigned char>(code[openBrace]))) {
            ++openBrace;
        }
        if (openBrace == std::string::npos) {
            break;
        }
        if (openBrace >= code.size() || code[openBrace] != '{') {
            offset = closeParen + 1;
            remaining = code.substr(offset);
            continue;
        }

        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        loops.push_back(LoopBlock{
            start,
            closeBrace + 1,
            code.substr(start, closeBrace + 1 - start),
            code.substr(start, openBrace - start),
            code.substr(openBrace + 1, closeBrace - openBrace - 1),
        });

        offset = closeBrace + 1;
        remaining = code.substr(offset);
    }

    return loops;
}

std::vector<PreprocessorBlock> analyzePreprocessorBlocks(const std::string& code)
{
    const std::vector<std::string> lines = splitLines(code);
    std::vector<PreprocessorBlock> blocks;
    std::vector<std::size_t> stack;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string stripped = trim(lines[index]);
        if (std::regex_match(stripped, std::regex(R"(^#\s*(if|ifdef|ifndef)\b.*)"))) {
            stack.push_back(index);
        } else if (std::regex_match(stripped, std::regex(R"(^#\s*endif\b.*)"))) {
            if (!stack.empty()) {
                const std::size_t startLine = stack.back();
                stack.pop_back();
                std::vector<std::string> body;
                for (std::size_t bodyLine = startLine + 1; bodyLine < index; ++bodyLine) {
                    body.push_back(lines[bodyLine]);
                }
                blocks.push_back(PreprocessorBlock{startLine, index, lines[startLine], body});
            }
        }
    }

    return blocks;
}
} // namespace

CodeStructure TokenBasedStructureAnalyzer::analyze(const std::string& code) const
{
    CodeStructure structure;
    structure.preprocessorBlocks = analyzePreprocessorBlocks(code);
    structure.typedefStructs = analyzeTypedefStructs(code);
    structure.classes = analyzeClasses(code);
    structure.loops = analyzeLoops(code);
    return structure;
}
