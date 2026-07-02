#include "converter/MakeUniqueModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

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

bool startsWithPreprocessorDirective(const std::string& line)
{
    const std::string stripped = trim(line);
    return !stripped.empty() && stripped.front() == '#';
}

void trimTrailingWhitespace(std::string& line)
{
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.pop_back();
    }
}

bool endsWithMacroContinuation(const std::string& line)
{
    std::string stripped = line;
    trimTrailingWhitespace(stripped);
    return !stripped.empty() && stripped.back() == '\\';
}

std::size_t findLineCommentStart(const std::string& line)
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
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
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
        if (!inString && !inCharacter && current == '/' && line[index + 1] == '/') {
            return index;
        }
    }
    return std::string::npos;
}

std::size_t findMatching(const std::string& text,
                         const std::size_t openPosition,
                         const char openCharacter,
                         const char closeCharacter)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openPosition; index < text.size(); ++index) {
        const char current = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
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
        if (current == openCharacter) {
            ++depth;
        } else if (current == closeCharacter) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

bool hasTopLevelComma(const std::string& text)
{
    int angleDepth = 0;
    int parenDepth = 0;
    for (const char current : text) {
        if (current == '<') {
            ++angleDepth;
        } else if (current == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (current == '(') {
            ++parenDepth;
        } else if (current == ')' && parenDepth > 0) {
            --parenDepth;
        } else if (current == ',' && angleDepth == 0 && parenDepth == 0) {
            return true;
        }
    }
    return false;
}

std::string collapseTypeSpacing(std::string type)
{
    type.erase(std::remove_if(type.begin(), type.end(), [](const unsigned char character) {
                   return std::isspace(character) != 0;
               }),
               type.end());
    return type;
}

bool isIdentifierStart(const char character)
{
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool isIdentifierBody(const char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

void addSkippedChange(std::vector<ConversionChange>& changes,
                      const std::string& before,
                      const std::string& reason)
{
    changes.push_back(ConversionChange{
        "unique_ptr raw-new construction to make_unique",
        before,
        before,
        reason,
        false,
        true,
    });
}

bool rewriteLine(const std::string& line,
                 std::string& rewritten,
                 std::vector<ConversionChange>& changes)
{
    const std::size_t commentStart = findLineCommentStart(line);
    const std::string codePart = commentStart == std::string::npos ? line : line.substr(0, commentStart);
    const std::string commentPart = commentStart == std::string::npos ? std::string{} : line.substr(commentStart);
    const std::size_t indentEnd = codePart.find_first_not_of(" \t");
    if (indentEnd == std::string::npos) {
        return false;
    }

    std::size_t cursor = indentEnd;
    constexpr const char* uniquePtrToken = "std::unique_ptr";
    constexpr std::size_t uniquePtrTokenLength = 15;
    if (codePart.compare(cursor, uniquePtrTokenLength, uniquePtrToken) != 0) {
        return false;
    }
    cursor += uniquePtrTokenLength;
    while (cursor < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= codePart.size() || codePart[cursor] != '<') {
        return false;
    }

    const std::size_t templateClose = findMatching(codePart, cursor, '<', '>');
    if (templateClose == std::string::npos) {
        return false;
    }

    const std::string declaredType = trim(codePart.substr(cursor + 1, templateClose - cursor - 1));
    if (declaredType.empty()) {
        return false;
    }
    if (hasTopLevelComma(declaredType)) {
        addSkippedChange(changes, line, "Skipped unique_ptr raw-new construction with custom deleter.");
        return false;
    }
    if (declaredType.find("[]") != std::string::npos) {
        addSkippedChange(changes, line, "Skipped unique_ptr array raw-new construction.");
        return false;
    }

    cursor = templateClose + 1;
    while (cursor < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= codePart.size() || !isIdentifierStart(codePart[cursor])) {
        return false;
    }
    const std::size_t variableStart = cursor;
    ++cursor;
    while (cursor < codePart.size() && isIdentifierBody(codePart[cursor])) {
        ++cursor;
    }
    const std::string variableName = codePart.substr(variableStart, cursor - variableStart);
    while (cursor < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= codePart.size() || codePart[cursor] != '(') {
        return false;
    }
    const std::size_t initializerClose = findMatching(codePart, cursor, '(', ')');
    if (initializerClose == std::string::npos) {
        return false;
    }
    const std::string initializer = trim(codePart.substr(cursor + 1, initializerClose - cursor - 1));
    std::size_t afterInitializer = initializerClose + 1;
    while (afterInitializer < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[afterInitializer])) != 0) {
        ++afterInitializer;
    }
    if (afterInitializer >= codePart.size() || codePart[afterInitializer] != ';'
        || trim(codePart.substr(afterInitializer + 1)).size() != 0U) {
        return false;
    }

    std::size_t newCursor = initializer.find_first_not_of(" \t");
    if (newCursor == std::string::npos || initializer.compare(newCursor, 3, "new") != 0) {
        return false;
    }
    newCursor += 3;
    if (newCursor < initializer.size() && isIdentifierBody(initializer[newCursor])) {
        return false;
    }
    while (newCursor < initializer.size() && std::isspace(static_cast<unsigned char>(initializer[newCursor])) != 0) {
        ++newCursor;
    }

    int angleDepth = 0;
    std::size_t typeEnd = std::string::npos;
    for (std::size_t index = newCursor; index < initializer.size(); ++index) {
        const char current = initializer[index];
        if (current == '<') {
            ++angleDepth;
        } else if (current == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (current == '(' && angleDepth == 0) {
            typeEnd = index;
            break;
        } else if (current == '[' && angleDepth == 0) {
            addSkippedChange(changes, line, "Skipped unique_ptr array allocation.");
            return false;
        }
    }
    if (typeEnd == std::string::npos) {
        return false;
    }

    const std::string allocatedType = trim(initializer.substr(newCursor, typeEnd - newCursor));
    if (allocatedType.empty() || allocatedType.find("[]") != std::string::npos) {
        addSkippedChange(changes, line, "Skipped unique_ptr array raw-new construction.");
        return false;
    }
    const std::size_t allocationArgsClose = findMatching(initializer, typeEnd, '(', ')');
    if (allocationArgsClose == std::string::npos
        || !trim(initializer.substr(allocationArgsClose + 1)).empty()) {
        return false;
    }
    const std::string arguments = trim(initializer.substr(typeEnd + 1, allocationArgsClose - typeEnd - 1));

    const std::string indentation = codePart.substr(0, indentEnd);
    const bool preserveDeclaredType = collapseTypeSpacing(declaredType) != collapseTypeSpacing(allocatedType);
    rewritten = indentation
        + (preserveDeclaredType ? "std::unique_ptr<" + declaredType + "> " + variableName : "auto " + variableName)
        + " = std::make_unique<" + allocatedType + ">(" + arguments + ");";
    if (!commentPart.empty()) {
        rewritten += " " + trim(commentPart);
    }

    changes.push_back(ConversionChange{
        "unique_ptr raw-new construction to make_unique",
        line,
        rewritten,
        "Replaced direct std::unique_ptr construction from raw new with std::make_unique before compile verification.",
        true,
        false,
    });
    return true;
}
} // namespace

std::string MakeUniqueModernizationPass::rewrite(const std::string& code,
                                                 std::vector<ConversionChange>& changes) const
{
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool first = true;
    bool inMacroContinuation = false;

    while (std::getline(input, line)) {
        std::string rewritten = line;
        const bool macroLine = inMacroContinuation || startsWithPreprocessorDirective(line);
        inMacroContinuation = macroLine && endsWithMacroContinuation(line);
        if (!macroLine) {
            (void)rewriteLine(line, rewritten, changes);
        }

        if (!first) {
            output << '\n';
        }
        first = false;
        output << rewritten;
    }

    std::string rewritten = output.str();
    if (!code.empty() && code.back() == '\n' && (rewritten.empty() || rewritten.back() != '\n')) {
        rewritten.push_back('\n');
    }
    return rewritten;
}
