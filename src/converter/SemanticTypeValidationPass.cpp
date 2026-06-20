#include "converter/SemanticTypeValidationPass.h"

#include "converter/IncludeManager.h"
#include "converter/NsdmiScopeSafetyPass.h"
#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

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

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string ruleName,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2U);
    for (const char character : text) {
        if (std::string(R"(\.^$|()[]{}*+?)").find(character) != std::string::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string accessExpressionRegex(const std::string& symbolName)
{
    return R"((?:(?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)(?:[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?)*(?:\.|->))?)"
        + escapeRegex(symbolName)
        + R"(\b)";
}

std::set<std::string> collectStringSymbols(const std::string& code)
{
    std::set<std::string> symbols;
    const std::regex declarationPattern(R"(\b(?:const\s+)?std::string\s*(?:[&*]\s*)?([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        const std::string name = (*iterator)[1].str();
        if (name != "operator") {
            symbols.insert(name);
        }
    }
    return symbols;
}

std::size_t findMatchingCloseParen(const std::string& text, const std::size_t openParen)
{
    if (openParen >= text.size() || text[openParen] != '(') {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openParen; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (character == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

std::vector<std::string> splitTopLevelArguments(const std::string& text)
{
    std::vector<std::string> arguments;
    std::size_t start = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    int angleDepth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (character == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '(' || character == '{') {
            ++parenDepth;
        } else if (character == ')' || character == '}') {
            --parenDepth;
        } else if (character == '[') {
            ++bracketDepth;
        } else if (character == ']') {
            --bracketDepth;
        } else if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (character == ',' && parenDepth == 0 && bracketDepth == 0 && angleDepth == 0) {
            arguments.push_back(trim(text.substr(start, index - start)));
            start = index + 1;
        }
    }
    arguments.push_back(trim(text.substr(start)));
    return arguments;
}

bool expressionMatchesStringSymbol(const std::string& expression, const std::string& symbol)
{
    return std::regex_match(trim(expression), std::regex(accessExpressionRegex(symbol)));
}

std::string stripCStringAccessor(std::string expression, const std::set<std::string>& stringSymbols)
{
    expression = trim(std::move(expression));
    const std::regex cstrPattern(R"(^(.+?)\s*\.\s*c_str\s*\(\s*\)\s*$)");
    std::smatch match;
    if (!std::regex_match(expression, match, cstrPattern)) {
        return expression;
    }

    const std::string base = trim(match[1].str());
    for (const std::string& symbol : stringSymbols) {
        if (expressionMatchesStringSymbol(base, symbol)) {
            return base;
        }
    }
    return expression;
}

bool expressionIsKnownString(const std::string& expression, const std::set<std::string>& stringSymbols)
{
    const std::string normalized = stripCStringAccessor(expression, stringSymbols);
    for (const std::string& symbol : stringSymbols) {
        if (expressionMatchesStringSymbol(normalized, symbol)) {
            return true;
        }
    }
    return false;
}

std::string rewriteStringStrcmpComparisonsInLine(std::string line,
                                                 const std::set<std::string>& stringSymbols,
                                                 bool& changed,
                                                 std::vector<ConversionChange>& changes)
{
    std::size_t searchPosition = 0;
    while (searchPosition < line.size()) {
        std::smatch match;
        const std::string suffix = line.substr(searchPosition);
        const std::regex strcmpName(R"((?:std::)?strcmp\s*\()");
        if (!std::regex_search(suffix, match, strcmpName)) {
            break;
        }

        const std::size_t namePosition = searchPosition + static_cast<std::size_t>(match.position());
        const std::size_t openParen = namePosition + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeParen = findMatchingCloseParen(line, openParen);
        if (closeParen == std::string::npos) {
            break;
        }

        std::size_t operatorStart = closeParen + 1;
        while (operatorStart < line.size() && std::isspace(static_cast<unsigned char>(line[operatorStart])) != 0) {
            ++operatorStart;
        }

        std::string comparisonOperator;
        if (line.compare(operatorStart, 2, "==") == 0) {
            comparisonOperator = "==";
        } else if (line.compare(operatorStart, 2, "!=") == 0) {
            comparisonOperator = "!=";
        } else {
            searchPosition = closeParen + 1;
            continue;
        }

        std::size_t zeroStart = operatorStart + 2;
        while (zeroStart < line.size() && std::isspace(static_cast<unsigned char>(line[zeroStart])) != 0) {
            ++zeroStart;
        }
        if (zeroStart >= line.size() || line[zeroStart] != '0') {
            searchPosition = closeParen + 1;
            continue;
        }

        const std::vector<std::string> arguments = splitTopLevelArguments(line.substr(openParen + 1, closeParen - openParen - 1));
        if (arguments.size() != 2) {
            searchPosition = closeParen + 1;
            continue;
        }

        if (!expressionIsKnownString(arguments[0], stringSymbols)
            && !expressionIsKnownString(arguments[1], stringSymbols)) {
            searchPosition = closeParen + 1;
            continue;
        }

        const std::string left = stripCStringAccessor(arguments[0], stringSymbols);
        const std::string right = stripCStringAccessor(arguments[1], stringSymbols);
        const std::string replacement = left + " " + comparisonOperator + " " + right;
        const std::size_t replaceEnd = zeroStart + 1;
        const std::string before = line.substr(namePosition, replaceEnd - namePosition);
        line.replace(namePosition, replaceEnd - namePosition, replacement);
        addAppliedChange(changes,
                         "std::string C API cleanup",
                         trim(before),
                         trim(replacement),
                         "Replaced strcmp involving a converted std::string with ordinary comparison.");
        changed = true;
        searchPosition = namePosition + replacement.size();
    }
    return line;
}

bool simpleFormatStringNeedsCStringAt(const std::string& format, const std::size_t placeholderIndex)
{
    std::size_t currentPlaceholder = 0;
    for (std::size_t index = 0; index < format.size(); ++index) {
        if (format[index] != '%') {
            continue;
        }
        if (index + 1 < format.size() && format[index + 1] == '%') {
            ++index;
            continue;
        }

        std::size_t specifierIndex = index + 1;
        while (specifierIndex < format.size()
               && std::string_view("-+ #0").find(format[specifierIndex]) != std::string_view::npos) {
            ++specifierIndex;
        }
        while (specifierIndex < format.size()
               && std::isdigit(static_cast<unsigned char>(format[specifierIndex])) != 0) {
            ++specifierIndex;
        }
        if (specifierIndex < format.size() && format[specifierIndex] == '.') {
            ++specifierIndex;
            while (specifierIndex < format.size()
                   && std::isdigit(static_cast<unsigned char>(format[specifierIndex])) != 0) {
                ++specifierIndex;
            }
        }
        if (specifierIndex + 1 < format.size()
            && ((format[specifierIndex] == 'l' && format[specifierIndex + 1] == 'l')
                || (format[specifierIndex] == 'h' && format[specifierIndex + 1] == 'h'))) {
            specifierIndex += 2;
        } else if (specifierIndex < format.size()
                   && std::string_view("hlzt").find(format[specifierIndex]) != std::string_view::npos) {
            ++specifierIndex;
        }
        if (specifierIndex >= format.size()) {
            return false;
        }

        if (currentPlaceholder == placeholderIndex) {
            return format[specifierIndex] == 's';
        }
        ++currentPlaceholder;
        index = specifierIndex;
    }
    return false;
}

std::string rewriteFprintfStringArguments(std::string code,
                                          const std::set<std::string>& stringSymbols,
                                          std::vector<ConversionChange>& changes)
{
    if (stringSymbols.empty() || code.find("fprintf") == std::string::npos) {
        return code;
    }

    bool changed = false;
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool firstLine = true;
    const std::regex fprintfPattern(R"(^([ \t]*)(?:std::)?fprintf\s*\((.+)\)\s*;\s*$)");
    while (std::getline(input, line)) {
        std::string updatedLine = line;
        std::smatch match;
        if (std::regex_match(line, match, fprintfPattern)) {
            std::vector<std::string> arguments = splitTopLevelArguments(match[2].str());
            if (arguments.size() >= 2) {
                std::smatch formatMatch;
                const std::regex stringLiteralPattern(R"re(^\s*"((?:\\.|[^"\\])*)"\s*$)re");
                if (std::regex_match(arguments[1], formatMatch, stringLiteralPattern)) {
                    bool lineChanged = false;
                    std::size_t placeholderIndex = 0;
                    for (std::size_t argumentIndex = 2; argumentIndex < arguments.size(); ++argumentIndex) {
                        if (!simpleFormatStringNeedsCStringAt(formatMatch[1].str(), placeholderIndex)) {
                            ++placeholderIndex;
                            continue;
                        }
                        const std::string argument = trim(arguments[argumentIndex]);
                        if (argument.find(".c_str()") == std::string::npos
                            && expressionIsKnownString(argument, stringSymbols)) {
                            arguments[argumentIndex] = argument + ".c_str()";
                            lineChanged = true;
                        }
                        ++placeholderIndex;
                    }
                    if (lineChanged) {
                        std::ostringstream replacement;
                        replacement << match[1].str() << "fprintf(";
                        for (std::size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
                            if (argumentIndex > 0) {
                                replacement << ", ";
                            }
                            replacement << arguments[argumentIndex];
                        }
                        replacement << ");";
                        updatedLine = replacement.str();
                        addAppliedChange(changes,
                                         "fprintf std::string argument compatibility",
                                         trim(line),
                                         trim(updatedLine),
                                         "Added c_str() for std::string values passed to preserved FILE* %s formatting.");
                        changed = true;
                    }
                }
            }
        }

        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << updatedLine;
    }

    if (!changed) {
        return code;
    }
    std::string updated = output.str();
    if (!code.empty() && code.back() == '\n') {
        updated.push_back('\n');
    }
    return updated;
}

bool lineUsesIdentifier(const std::string& line, const std::string& identifier)
{
    return std::regex_search(line, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::string removeCstringIfUnused(const std::string& code)
{
    const IncludeManager includeManager;
    return includeManager.removeIncludeIfUnused(code,
                                                "#include <cstring>",
                                                {"std::strcpy", "std::strncpy", "std::strcat", "std::strcmp", "std::strlen",
                                                 "strcpy(", "strncpy(", "strcat(", "strcmp(", "strlen("});
}

std::string rewriteTemporaryStringAppendBuffers(std::string code,
                                                const std::string& symbol,
                                                std::vector<ConversionChange>& changes)
{
    const std::string targetExpression = accessExpressionRegex(symbol);
    std::vector<std::string> lines;
    {
        std::stringstream input(code);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    bool changed = false;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::string trailingComment;
        const std::string allocationLine = SafeReplacementEngine::splitTrailingLineComment(lines[index], trailingComment);
        std::smatch allocationMatch;
        const std::regex allocationPattern(
            "^([ \\t]*)char\\s*\\*\\s*([A-Za-z_]\\w*)\\s*=\\s*new\\s+char\\s*\\[[^\\]\\n;]*("
                + targetExpression
                + ")(?:\\s*\\.\\s*size\\s*\\(\\s*\\))?[^\\]\\n;]*\\]\\s*;\\s*$",
            std::regex::ECMAScript);
        if (!std::regex_match(allocationLine, allocationMatch, allocationPattern)) {
            continue;
        }

        const std::string indent = allocationMatch[1].str();
        const std::string tempName = allocationMatch[2].str();
        const std::string stringExpression = trim(allocationMatch[3].str());
        const std::string escapedTemp = escapeRegex(tempName);
        const std::string escapedStringExpression = escapeRegex(stringExpression);

        std::optional<std::size_t> copyLine;
        std::optional<std::size_t> appendLine;
        std::optional<std::size_t> assignmentLine;
        std::optional<std::size_t> tempDeleteLine;
        std::optional<std::size_t> convertedStringDeleteLine;
        std::string suffixExpression;
        bool unsafeUse = false;

        const std::size_t scanEnd = std::min(lines.size(), index + 10U);
        for (std::size_t scan = index + 1U; scan < scanEnd; ++scan) {
            std::string scanComment;
            const std::string codePart = trim(SafeReplacementEngine::splitTrailingLineComment(lines[scan], scanComment));
            if (codePart.empty()) {
                continue;
            }

            std::smatch statementMatch;
            if (!copyLine
                && std::regex_match(codePart,
                                    std::regex(R"((?:std::)?strcpy\s*\(\s*)" + escapedTemp
                                                   + R"(\s*,\s*)" + escapedStringExpression + R"(\s*\)\s*;)"))) {
                copyLine = scan;
                continue;
            }
            if (!appendLine
                && std::regex_match(codePart,
                                    statementMatch,
                                    std::regex(R"((?:std::)?strcat\s*\(\s*)" + escapedTemp
                                                   + R"(\s*,\s*([^;]+?)\s*\)\s*;)"))) {
                suffixExpression = trim(statementMatch[1].str());
                appendLine = scan;
                continue;
            }
            if (!assignmentLine
                && std::regex_match(codePart,
                                    std::regex(escapedStringExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;)"))) {
                assignmentLine = scan;
                continue;
            }
            if (!tempDeleteLine
                && std::regex_match(codePart,
                                    std::regex(R"(delete\s*\[\s*\]\s*)" + escapedTemp + R"(\s*;)"))) {
                tempDeleteLine = scan;
                continue;
            }
            if (!convertedStringDeleteLine
                && std::regex_match(codePart,
                                    std::regex(R"(delete\s*\[\s*\]\s*)" + escapedStringExpression + R"(\s*;)"))) {
                convertedStringDeleteLine = scan;
                continue;
            }
            if (lineUsesIdentifier(codePart, tempName)) {
                unsafeUse = true;
                break;
            }
        }

        if (unsafeUse || !copyLine || !appendLine || !assignmentLine || suffixExpression.empty()) {
            continue;
        }

        const std::size_t lastRemoval = std::max({index,
                                                  *copyLine,
                                                  *appendLine,
                                                  *assignmentLine,
                                                  tempDeleteLine.value_or(index),
                                                  convertedStringDeleteLine.value_or(index)});
        std::string before;
        for (std::size_t lineIndex = index; lineIndex <= lastRemoval; ++lineIndex) {
            if (lineUsesIdentifier(lines[lineIndex], tempName)
                || lines[lineIndex].find(stringExpression) != std::string::npos) {
                before += trim(lines[lineIndex]) + "\n";
            }
        }

        lines[index] = indent + stringExpression + " += " + suffixExpression + ";";
        lines[*copyLine].clear();
        lines[*appendLine].clear();
        lines[*assignmentLine].clear();
        if (tempDeleteLine) {
            lines[*tempDeleteLine].clear();
        }
        if (convertedStringDeleteLine) {
            lines[*convertedStringDeleteLine].clear();
        }

        addAppliedChange(changes,
                         "std::string temporary C-buffer append cleanup",
                         trim(before),
                         trim(lines[index]),
                         "Removed a temporary char buffer that only concatenated into a converted std::string.");
        changed = true;
    }

    if (!changed) {
        return code;
    }

    std::ostringstream output;
    bool first = true;
    for (const std::string& line : lines) {
        if (line.empty()) {
            continue;
        }
        if (!first) {
            output << '\n';
        }
        first = false;
        output << line;
    }
    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }
    return output.str();
}

std::string cleanupCStringApiAfterStringModernization(std::string code, std::vector<ConversionChange>& changes)
{
    const std::set<std::string> stringSymbols = collectStringSymbols(code);
    if (stringSymbols.empty()) {
        return code;
    }

    bool changed = false;
    for (const std::string& symbol : stringSymbols) {
        const std::string beforeTempCleanup = code;
        code = rewriteTemporaryStringAppendBuffers(std::move(code), symbol, changes);
        changed = changed || code != beforeTempCleanup;
    }

    const SafeReplacementEngine safeReplacement;
    for (const std::string& symbol : stringSymbols) {
        const std::string targetExpression = accessExpressionRegex(symbol);
        code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
            std::string trailingComment;
            std::string rewritten = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex strncpyPattern("^([ \\t]*)(?:std::)?strncpy\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^,;]+?)\\s*,\\s*[^;]+\\)\\s*;\\s*$");
            if (std::regex_match(rewritten, match, strncpyPattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + " = " + trim(match[3].str()) + ";";
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Replaced strncpy targeting a converted std::string with assignment.");
                changed = true;
                rewritten = replacement;
            }

            const std::regex strcpyPattern("^([ \\t]*)(?:std::)?strcpy\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^;]+?)\\s*\\)\\s*;\\s*$");
            if (std::regex_match(rewritten, match, strcpyPattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + " = " + trim(match[3].str()) + ";";
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Replaced strcpy targeting a converted std::string with assignment.");
                changed = true;
                rewritten = replacement;
            }

            const std::regex strcatPattern("^([ \\t]*)(?:std::)?strcat\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^;]+?)\\s*\\)\\s*;\\s*$");
            if (std::regex_match(rewritten, match, strcatPattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + " += " + trim(match[3].str()) + ";";
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Replaced strcat targeting a converted std::string with append.");
                changed = true;
                rewritten = replacement;
            }

            const std::regex nullTerminationPattern("^[ \\t]*(" + targetExpression + ")\\s*\\[[^;\\n]+\\]\\s*=\\s*'\\\\0'\\s*;\\s*$");
            if (std::regex_match(rewritten, match, nullTerminationPattern)) {
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(rewritten),
                                 "removed",
                                 "Removed manual null termination on a converted std::string.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            rewritten = rewriteStringStrcmpComparisonsInLine(std::move(rewritten), stringSymbols, changed, changes);

            const std::string beforeLengthRewrite = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex("(?:std::)?strlen\\s*\\(\\s*(" + targetExpression + ")\\s*\\)"),
                                           "$1.size()");
            if (rewritten != beforeLengthRewrite) {
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(beforeLengthRewrite),
                                 trim(rewritten),
                                 "Replaced strlen on a converted std::string with size().");
                changed = true;
            }

            const std::string beforeSizeofRewrite = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex("sizeof\\s*\\(\\s*(" + targetExpression + ")\\s*\\)"),
                                           "$1.size()");
            if (rewritten != beforeSizeofRewrite) {
                addAppliedChange(changes,
                                 "std::string C API cleanup",
                                 trim(beforeSizeofRewrite),
                                 trim(rewritten),
                                 "Replaced sizeof on a converted std::string used as a buffer length.");
                changed = true;
            }

            return rewritten + trailingComment;
        });
    }

    if (!changed) {
        return code;
    }

    const IncludeManager includeManager;
    code = includeManager.ensureInclude(std::move(code), "#include <string>");
    return removeCstringIfUnused(code);
}

std::string rollbackStringViewCStr(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex functionPattern(
        R"((^[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+[A-Za-z_]\w*\s*\([^)]*?)std::string_view\s+([A-Za-z_]\w*)([^)]*\)\s*(const\s*)?\{([\s\S]*?)^\s*\}))",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, functionPattern)) {
        const std::string parameterName = match[2].str();
        const std::string body = match[5].str();
        if (body.find(parameterName + ".c_str()") == std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }
        std::string replacement = match[0].str();
        const std::string beforeHeader = trim(replacement.substr(0, replacement.find('{')));
        replacement.replace(replacement.find("std::string_view " + parameterName),
                            std::string("std::string_view " + parameterName).size(),
                            "const std::string& " + parameterName);
        const std::string afterHeader = trim(replacement.substr(0, replacement.find('{')));
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "SemanticTypeValidationPass",
                         beforeHeader,
                         afterHeader,
                         "Repaired invalid string_view/c_str interaction by restoring the owning string reference type.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::set<std::string> collectStringViewSymbols(const std::string& code)
{
    std::set<std::string> symbols;
    const std::regex declarationPattern(R"(\bstd::string_view\s+([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        symbols.insert((*iterator)[1].str());
    }
    return symbols;
}

std::set<std::string> collectConstCharPointerSinkFunctions(const std::string& code)
{
    std::set<std::string> functions;
    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*\s+)+([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?(?:;|\{))",
        std::regex::ECMAScript);
    const std::regex constCharParameter(R"((?:^|,)\s*(?:const\s+)?char\s*\*\s*(?:[A-Za-z_]\w*)?)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        if (std::regex_search((*iterator)[2].str(), constCharParameter)) {
            functions.insert((*iterator)[1].str());
        }
    }
    return functions;
}

std::string repairStringViewArtifacts(std::string code, std::vector<ConversionChange>& changes)
{
    const std::set<std::string> stringViewSymbols = collectStringViewSymbols(code);
    if (stringViewSymbols.empty()) {
        return code;
    }

    const std::set<std::string> constCharSinks = collectConstCharPointerSinkFunctions(code);
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    bool needsStringInclude = false;

    for (const std::string& symbol : stringViewSymbols) {
        const std::string escaped = escapeRegex(symbol);
        code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
            std::string trailingComment;
            std::string rewritten = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);

            const std::string beforeLength = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(\b)" + escaped + R"(\s*\.\s*length\s*\(\s*\))"),
                                           symbol + ".size()");
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(\b(?:std::)?strlen\s*\(\s*)" + escaped + R"(\s*\))"),
                                           symbol + ".size()");
            if (rewritten != beforeLength) {
                addAppliedChange(changes,
                                 "std::string_view semantic cleanup",
                                 trim(beforeLength),
                                 trim(rewritten),
                                 "Replaced string_view length/strlen usage with size().");
                changed = true;
            }

            const std::string cStringAdapter = "std::string(" + symbol + ").c_str()";

            const std::string beforeKnownCapi = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(\b((?:std::)?fopen)\s*\(\s*)" + escaped + R"(\s*,)"),
                                           "$1(" + cStringAdapter + ",");
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(\b((?:std::)?system)\s*\(\s*)" + escaped + R"(\s*\))"),
                                           "$1(" + cStringAdapter + ")");
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(\b((?:std::)?(?:printf|sprintf|snprintf))\s*\(\s*("[^"\n]*%s[^"\n]*")\s*,\s*)" + escaped + R"(\s*\))"),
                                           "$1($2, " + cStringAdapter + ")");
            if (rewritten != beforeKnownCapi) {
                needsStringInclude = true;
                addAppliedChange(changes,
                                 "std::string_view semantic cleanup",
                                 trim(beforeKnownCapi),
                                 trim(rewritten),
                                 "Passed a temporary null-terminated std::string to a C API that cannot consume std::string_view directly.");
                changed = true;
            }

            for (const std::string& functionName : constCharSinks) {
                const std::string beforeSink = rewritten;
                const std::regex sinkPattern(R"(\b)" + escapeRegex(functionName) + R"(\s*\(\s*)" + escaped + R"(\s*\))");
                rewritten = std::regex_replace(rewritten,
                                               sinkPattern,
                                               functionName + "(" + cStringAdapter + ")");
                if (rewritten != beforeSink) {
                    needsStringInclude = true;
                    addAppliedChange(changes,
                                     "std::string_view semantic cleanup",
                                     trim(beforeSink),
                                     trim(rewritten),
                                     "Adapted a std::string_view argument for a visible const char* sink API.");
                    changed = true;
                }
            }

            return rewritten + trailingComment;
        });
    }

    if (!changed) {
        return code;
    }

    if (needsStringInclude) {
        const IncludeManager includeManager;
        code = includeManager.ensureInclude(std::move(code), "#include <string>");
    }
    return code;
}

std::set<std::string> collectSizeReturningFunctions(const std::string& code)
{
    std::set<std::string> names;
    const std::regex functionPattern(R"(\bstd::size_t\s+([A-Za-z_]\w*)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:\{|;))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        names.insert((*iterator)[1].str());
    }
    return names;
}

std::string repairSignedLoopIndicesForSizeGetters(std::string code, std::vector<ConversionChange>& changes)
{
    const std::set<std::string> sizeReturningFunctions = collectSizeReturningFunctions(code);
    if (sizeReturningFunctions.empty()) {
        return code;
    }

    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool firstLine = true;
    bool changed = false;

    while (std::getline(input, line)) {
        std::string updatedLine = line;
        for (const std::string& functionName : sizeReturningFunctions) {
            const std::regex loopPattern(R"(^([ \t]*for\s*\()\s*(?:int|long)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*([^;]*\b)"
                                             + functionName
                                             + R"(\s*\([^;]*\)[^;]*)\s*;\s*((?:\+\+\2)|(?:\2\+\+))\s*\))");
            std::smatch match;
            if (!std::regex_search(updatedLine, match, loopPattern)) {
                continue;
            }

            const std::string before = updatedLine;
            updatedLine = match.prefix().str()
                + match[1].str()
                + "std::size_t "
                + match[2].str()
                + " = 0; "
                + match[2].str()
                + " < "
                + trim(match[3].str())
                + "; "
                + match[4].str()
                + ")"
                + match.suffix().str();
            addAppliedChange(changes,
                             "Signed loop index to std::size_t",
                             trim(before),
                             trim(updatedLine),
                             "Updated a loop index to match a std::size_t-returning count/size API after type propagation.");
            changed = true;
            break;
        }

        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << updatedLine;
    }

    if (!changed) {
        return code;
    }

    std::string updated = output.str();
    if (!code.empty() && code.back() == '\n') {
        updated.push_back('\n');
    }
    const IncludeManager includeManager;
    return includeManager.ensureInclude(std::move(updated), "#include <cstddef>");
}

std::string cleanupTransformationFormatting(std::string code, std::vector<ConversionChange>& changes)
{
    const std::string before = code;

    code = std::regex_replace(code, std::regex(R"(\boverride\s*\{)"), "override {");

    std::stringstream input(code);
    std::vector<std::string> formattedLines;
    std::string line;
    int blankRun = 0;
    int indentLevel = 0;
    while (std::getline(input, line)) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.pop_back();
        }

        std::string trimmedLine = trim(line);
        const bool blank = trimmedLine.empty();
        if (blank) {
            ++blankRun;
            if (blankRun > 1) {
                continue;
            }
            formattedLines.emplace_back();
            continue;
        }

        blankRun = 0;

        trimmedLine = std::regex_replace(trimmedLine,
                                         std::regex(R"(\b(if|for|while|switch)\s*\(([^{}\n]*)\)\s*\{)"),
                                         "$1 ($2) {");
        trimmedLine = std::regex_replace(trimmedLine,
                                         std::regex(R"(\b(class|struct)\s+([A-Za-z_]\w*)\s*\{)"),
                                         "$1 $2 {");
        trimmedLine = std::regex_replace(trimmedLine,
                                         std::regex(R"(\b(enum\s+class|enum)\s+([A-Za-z_]\w*)\s*\{)"),
                                         "$1 $2 {");
        trimmedLine = std::regex_replace(trimmedLine, std::regex(R"(\boverride\s*\{)"), "override {");

        auto countBraces = [](const std::string& text) {
            int opens = 0;
            int closes = 0;
            bool inString = false;
            bool inCharacter = false;
            bool escaped = false;
            for (std::size_t index = 0; index < text.size(); ++index) {
                const char character = text[index];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (character == '\\' && (inString || inCharacter)) {
                    escaped = true;
                    continue;
                }
                if (!inCharacter && character == '"') {
                    inString = !inString;
                    continue;
                }
                if (!inString && character == '\'') {
                    inCharacter = !inCharacter;
                    continue;
                }
                if (!inString && !inCharacter && character == '/' && index + 1 < text.size() && text[index + 1] == '/') {
                    break;
                }
                if (inString || inCharacter) {
                    continue;
                }
                if (character == '{') {
                    ++opens;
                } else if (character == '}') {
                    ++closes;
                }
            }
            return std::pair<int, int>{opens, closes};
        };

        const auto [openCount, closeCount] = countBraces(trimmedLine);
        int lineIndent = indentLevel;
        if (trimmedLine.starts_with("}")) {
            lineIndent = std::max(0, lineIndent - 1);
        }
        if (std::regex_match(trimmedLine, std::regex(R"((?:public|protected|private)\s*:\s*)"))) {
            lineIndent = std::max(0, indentLevel - 1);
        } else if (std::regex_match(trimmedLine, std::regex(R"((?:case\b.*|default)\s*:\s*)"))) {
            lineIndent = std::max(0, indentLevel - 1);
        }
        if (trimmedLine.starts_with("#")) {
            lineIndent = 0;
        }

        formattedLines.push_back(std::string(static_cast<std::size_t>(lineIndent) * 4, ' ') + trimmedLine);
        indentLevel = std::max(0, indentLevel + openCount - closeCount);
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < formattedLines.size(); ++index) {
        if (index != 0) {
            output << '\n';
        }
        output << formattedLines[index];
    }

    code = output.str();

    if (!before.empty() && before.back() == '\n' && (code.empty() || code.back() != '\n')) {
        code.push_back('\n');
    }

    if (code != before) {
        addAppliedChange(changes,
                         "Final transformation formatting cleanup",
                         "transformed formatting artifacts",
                         "normalized formatting",
                         "Cleaned trailing whitespace, excessive blank lines, and missing spaces before opening braces after transformations.");
    }
    return code;
}

std::string removeSelfInitializingPointerReferenceArtifacts(std::string code,
                                                            std::vector<ConversionChange>& changes)
{
    const std::string before = code;
    std::ostringstream output;
    std::istringstream input(code);
    std::string line;
    bool changed = false;
    bool firstLine = true;

    const std::regex selfInitializingDeclaration(
        R"(^[ \t]*(?:const\s+)?(?:auto|[A-Za-z_:][A-Za-z0-9_:<>, \t]*(?:\s+const)?)\s*[&*]\s*([A-Za-z_]\w*)\s*=\s*\1\s*;\s*$)",
        std::regex::ECMAScript);

    while (std::getline(input, line)) {
        if (std::regex_match(line, selfInitializingDeclaration)) {
            changed = true;
            continue;
        }

        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << line;
    }

    if (!changed) {
        return code;
    }

    code = output.str();
    if (!before.empty() && before.back() == '\n' && (code.empty() || code.back() != '\n')) {
        code.push_back('\n');
    }

    addAppliedChange(changes,
                     "Range-for self-initialization cleanup",
                     "self-initializing pointer/reference local",
                     "removed redundant local declaration",
                     "Removed a self-initializing pointer/reference declaration left after range-for modernization.");
    return code;
}

std::string safeRangeVariableName(const std::string& body, const std::string& forbiddenName)
{
    const std::vector<std::string> candidates{"item", "entry", "value", "element"};
    for (const std::string& candidate : candidates) {
        if (candidate == forbiddenName) {
            continue;
        }
        if (!std::regex_search(body, std::regex(R"(\b)" + escapeRegex(candidate) + R"(\b)"))) {
            return candidate;
        }
    }

    int suffix = 1;
    while (std::regex_search(body, std::regex(R"(\bitem)" + std::to_string(suffix) + R"(\b)"))) {
        ++suffix;
    }
    return "item" + std::to_string(suffix);
}

bool isIdentifierStart(char character)
{
    const auto value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

bool isIdentifierBody(char character)
{
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

std::size_t findMatchingBraceInText(const std::string& text, const std::size_t openBrace)
{
    if (openBrace >= text.size() || text[openBrace] != '{') {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < text.size(); ++index) {
        const char current = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';
        if (inLineComment) {
            if (current == '\n') {
                inLineComment = false;
            }
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                inBlockComment = false;
                ++index;
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
        if (!inString && !inCharacter && current == '/' && next == '/') {
            inLineComment = true;
            ++index;
            continue;
        }
        if (!inString && !inCharacter && current == '/' && next == '*') {
            inBlockComment = true;
            ++index;
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
                return index;
            }
        }
    }
    return std::string::npos;
}

std::vector<std::pair<std::size_t, std::size_t>> lambdaParameterShadowRanges(const std::string& body,
                                                                             const std::string& identifier)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const std::regex lambdaWithParameter(
        R"(\[[^\]\n]*\]\s*\([^)]*\b)"
            + escapeRegex(identifier)
            + R"(\b[^)]*\)\s*(?:mutable\s*)?(?:noexcept\s*)?(?:->[^{}\n]+)?\{)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(body.begin(), body.end(), lambdaWithParameter), end; iterator != end; ++iterator) {
        const std::size_t openBrace = static_cast<std::size_t>(iterator->position() + iterator->length() - 1);
        const std::size_t closeBrace = findMatchingBraceInText(body, openBrace);
        if (closeBrace != std::string::npos) {
            ranges.emplace_back(openBrace + 1, closeBrace);
        }
    }
    return ranges;
}

bool isInsideAnyRange(const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
                      const std::size_t position)
{
    return std::any_of(ranges.begin(), ranges.end(), [position](const auto& range) {
        return position >= range.first && position < range.second;
    });
}

bool isLikelyDeclarationIdentifier(const std::string& body,
                                   const std::size_t identifierBegin,
                                   const std::size_t identifierEnd)
{
    std::size_t before = identifierBegin;
    while (before > 0 && std::isspace(static_cast<unsigned char>(body[before - 1])) != 0) {
        --before;
    }
    if (before > 0 && (body[before - 1] == '.' || body[before - 1] == '>')) {
        return false;
    }

    std::size_t after = identifierEnd;
    while (after < body.size() && std::isspace(static_cast<unsigned char>(body[after])) != 0) {
        ++after;
    }
    if (after < body.size() && body[after] == '(') {
        return false;
    }

    std::size_t prefixBegin = identifierBegin;
    while (prefixBegin > 0) {
        const char previous = body[prefixBegin - 1];
        if (previous == ';' || previous == '{' || previous == '}' || previous == '\n' || previous == '(' || previous == ',') {
            break;
        }
        --prefixBegin;
    }
    const std::string prefix = trim(body.substr(prefixBegin, identifierBegin - prefixBegin));
    if (prefix.empty() || prefix.find("<<") != std::string::npos || prefix.find(">>") != std::string::npos) {
        return false;
    }

    const std::regex declarationPrefix(
        R"((?:const\s+|volatile\s+|static\s+)*(?:auto|bool|char|short|int|long|float|double|std::[A-Za-z_]\w*(?:\s*<[^;\n{}()]*>)?|[A-Z][A-Za-z0-9_:]*(?:\s*<[^;\n{}()]*>)?|[A-Za-z_]\w*::[A-Za-z_]\w*)(?:\s+const)?\s*[*&]*$)",
        std::regex::ECMAScript);
    return std::regex_match(prefix, declarationPrefix);
}

bool isPrimitiveDeclarationIdentifier(const std::string& body, const std::size_t identifierBegin)
{
    std::size_t prefixBegin = identifierBegin;
    while (prefixBegin > 0) {
        const char previous = body[prefixBegin - 1];
        if (previous == ';' || previous == '{' || previous == '}' || previous == '\n' || previous == '(' || previous == ',') {
            break;
        }
        --prefixBegin;
    }
    const std::string prefix = trim(body.substr(prefixBegin, identifierBegin - prefixBegin));
    const std::regex primitivePrefix(
        R"((?:const\s+|volatile\s+|static\s+)*(?:bool|char|short|int|long|float|double)(?:\s+const)?\s*[*&]*$)",
        std::regex::ECMAScript);
    return std::regex_match(prefix, primitivePrefix);
}

enum class ShadowKind
{
    None,
    Primitive,
    Full,
};

bool anyScopeShadows(const std::vector<ShadowKind>& scopeShadows)
{
    return std::any_of(scopeShadows.begin(), scopeShadows.end(), [](ShadowKind shadowed) {
        return shadowed != ShadowKind::None;
    });
}

bool anyFullShadow(const std::vector<ShadowKind>& scopeShadows)
{
    return std::any_of(scopeShadows.begin(), scopeShadows.end(), [](ShadowKind shadowed) {
        return shadowed == ShadowKind::Full;
    });
}

bool anyPrimitiveShadow(const std::vector<ShadowKind>& scopeShadows)
{
    return std::any_of(scopeShadows.begin(), scopeShadows.end(), [](ShadowKind shadowed) {
        return shadowed == ShadowKind::Primitive;
    });
}

bool identifierIsFollowedByMemberAccess(const std::string& body, std::size_t identifierEnd)
{
    while (identifierEnd < body.size() && std::isspace(static_cast<unsigned char>(body[identifierEnd])) != 0) {
        ++identifierEnd;
    }
    return identifierEnd < body.size()
        && (body[identifierEnd] == '.'
            || (body[identifierEnd] == '-' && identifierEnd + 1 < body.size() && body[identifierEnd + 1] == '>'));
}

std::string renameLoopVariableReferencesInBody(const std::string& body,
                                               const std::string& oldName,
                                               const std::string& newName)
{
    if (oldName == newName) {
        return body;
    }

    const auto lambdaShadowRanges = lambdaParameterShadowRanges(body, oldName);
    std::vector<std::pair<std::size_t, std::size_t>> replacements;
    std::vector<ShadowKind> scopeShadows{ShadowKind::None};

    bool inString = false;
    bool inCharacter = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool escaped = false;
    for (std::size_t index = 0; index < body.size();) {
        const char current = body[index];
        const char next = index + 1 < body.size() ? body[index + 1] : '\0';
        if (inLineComment) {
            if (current == '\n') {
                inLineComment = false;
            }
            ++index;
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                inBlockComment = false;
                index += 2;
            } else {
                ++index;
            }
            continue;
        }
        if (escaped) {
            escaped = false;
            ++index;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
            ++index;
            continue;
        }
        if (!inString && !inCharacter && current == '/' && next == '/') {
            inLineComment = true;
            index += 2;
            continue;
        }
        if (!inString && !inCharacter && current == '/' && next == '*') {
            inBlockComment = true;
            index += 2;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            ++index;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            ++index;
            continue;
        }
        if (inString || inCharacter) {
            ++index;
            continue;
        }
        if (current == '{') {
            scopeShadows.push_back(ShadowKind::None);
            ++index;
            continue;
        }
        if (current == '}') {
            if (scopeShadows.size() > 1) {
                scopeShadows.pop_back();
            }
            ++index;
            continue;
        }
        if (!isIdentifierStart(current)) {
            ++index;
            continue;
        }

        const std::size_t identifierBegin = index;
        ++index;
        while (index < body.size() && isIdentifierBody(body[index])) {
            ++index;
        }
        const std::size_t identifierEnd = index;
        if (body.compare(identifierBegin, identifierEnd - identifierBegin, oldName) != 0) {
            continue;
        }

        if (isLikelyDeclarationIdentifier(body, identifierBegin, identifierEnd)) {
            if (!isInsideAnyRange(lambdaShadowRanges, identifierBegin)) {
                scopeShadows.back() = isPrimitiveDeclarationIdentifier(body, identifierBegin)
                    ? ShadowKind::Primitive
                    : ShadowKind::Full;
            }
            continue;
        }
        if (isInsideAnyRange(lambdaShadowRanges, identifierBegin)) {
            continue;
        }
        if (anyScopeShadows(scopeShadows)) {
            const bool memberAccessFromPrimitiveShadow =
                identifierIsFollowedByMemberAccess(body, identifierEnd)
                && anyPrimitiveShadow(scopeShadows)
                && !anyFullShadow(scopeShadows);
            if (!memberAccessFromPrimitiveShadow) {
                continue;
            }
        }
        replacements.emplace_back(identifierBegin, identifierEnd);
    }

    std::string updated = body;
    for (auto iterator = replacements.rbegin(); iterator != replacements.rend(); ++iterator) {
        updated.replace(iterator->first, iterator->second - iterator->first, newName);
    }
    return updated;
}

std::string repairRangeForVariableNamingArtifacts(std::string code,
                                                  std::vector<ConversionChange>& changes)
{
    std::string updated = code;
    const std::regex rangeLoop(
        R"((^[ \t]*)for\s*\(\s*((?:const\s+)?auto\s*&?\s+)([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\n?\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, rangeLoop)) {
        const std::string indent = match[1].str();
        const std::string qualifier = match[2].str();
        const std::string variableName = match[3].str();
        const std::string containerName = match[4].str();
        std::string body = match[5].str();

        const std::regex duplicateDeclaration(
            R"((^|\n)[ \t]*(?:const\s+)?(?:auto|[A-Za-z_:][A-Za-z0-9_:<>, \t]*(?:\s+const)?)\s*(?:[&*]\s*)?)"
                + escapeRegex(variableName)
                + R"(\s*(?:=|;|\{))",
            std::regex::ECMAScript);
        const bool shadowsContainer = variableName == containerName;
        const bool duplicatesBodyLocal = std::regex_search(body, duplicateDeclaration);
        if (!shadowsContainer && !duplicatesBodyLocal) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string newName = safeRangeVariableName(body, containerName);
        body = renameLoopVariableReferencesInBody(body, variableName, newName);

        const std::string replacement =
            indent + "for (" + qualifier + newName + " : " + containerName + ")\n"
            + indent + "{\n" + body + "\n" + indent + "}";
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        addAppliedChange(changes,
                         "Range-for variable naming cleanup",
                         trim(match[0].str()),
                         trim(replacement),
                         "Renamed a range-for variable to avoid shadowing its container or a local declaration in the loop body.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = updated.substr(consumed);
    }

    return updated;
}
} // namespace

std::string SemanticTypeValidationPass::validateAndRepair(const std::string& code,
                                                          const ModernizationOptions&,
                                                          std::vector<ConversionChange>& changes) const
{
    const bool hadAppliedChanges = std::any_of(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.applied;
    });
    std::string updated = code;
    updated = repairStringViewArtifacts(updated, changes);
    updated = rollbackStringViewCStr(updated, changes);
    updated = cleanupCStringApiAfterStringModernization(std::move(updated), changes);
    updated = rewriteFprintfStringArguments(std::move(updated), collectStringSymbols(updated), changes);
    updated = repairSignedLoopIndicesForSizeGetters(updated, changes);
    updated = repairRangeForVariableNamingArtifacts(std::move(updated), changes);
    updated = removeSelfInitializingPointerReferenceArtifacts(std::move(updated), changes);
    const NsdmiScopeSafetyPass nsdmiScopeSafetyPass;
    updated = nsdmiScopeSafetyPass.validateAndRepair(updated, changes);
    if (hadAppliedChanges || updated != code) {
        updated = cleanupTransformationFormatting(updated, changes);
    }
    return updated;
}
