#include "converter/PrintfModernizationPass.h"

#include "converter/IncludeManager.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct FormatPart
{
    bool placeholder = false;
    std::string text;
};

struct ParsedFormat
{
    std::vector<FormatPart> parts;
    int placeholderCount = 0;
    bool safe = false;
};

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

void addSuggestion(std::vector<ConversionChange>& changes,
                   std::string ruleName,
                   std::string before,
                   std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        {},
        std::move(reason),
        false,
        false,
    });
}

std::vector<std::string> splitLines(const std::string& code)
{
    std::vector<std::string> lines;
    std::stringstream input(code);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2U);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
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

bool expressionIsKnownString(const std::string& expression, const std::set<std::string>& stringSymbols)
{
    const std::string normalized = trim(expression);
    if (normalized.find(".c_str()") != std::string::npos) {
        return false;
    }
    for (const std::string& symbol : stringSymbols) {
        if (std::regex_match(normalized, std::regex(accessExpressionRegex(symbol)))) {
            return true;
        }
    }
    return false;
}

bool containsPrintfFamily(const std::string& code)
{
    return std::regex_search(code, std::regex(R"(\b(?:std::)?(?:printf|fprintf|sprintf|snprintf|vprintf|vfprintf)\s*\()"));
}

bool isAllowedSpecifier(const std::string& length, const char specifier)
{
    if (specifier == 's' || specifier == 'c') {
        return length.empty();
    }
    if (specifier == 'd' || specifier == 'i' || specifier == 'u') {
        return length.empty() || length == "l" || length == "ll" || length == "z";
    }
    return false;
}

void appendLiteralPart(ParsedFormat& parsed, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    if (!parsed.parts.empty() && !parsed.parts.back().placeholder) {
        parsed.parts.back().text += text;
        return;
    }
    parsed.parts.push_back(FormatPart{false, text});
}

ParsedFormat parseSimpleFormat(const std::string& format)
{
    ParsedFormat parsed;
    std::string literal;
    for (std::size_t index = 0; index < format.size(); ++index) {
        const char character = format[index];
        if (character != '%') {
            literal.push_back(character);
            continue;
        }

        if (index + 1 >= format.size()) {
            return {};
        }
        if (format[index + 1] == '%') {
            literal.push_back('%');
            ++index;
            continue;
        }

        appendLiteralPart(parsed, literal);
        literal.clear();

        std::size_t specifierIndex = index + 1;
        if (specifierIndex < format.size() && std::string_view("-+ #0").find(format[specifierIndex]) != std::string_view::npos) {
            return {};
        }
        if (specifierIndex < format.size() && std::isdigit(static_cast<unsigned char>(format[specifierIndex]))) {
            return {};
        }
        if (specifierIndex < format.size() && format[specifierIndex] == '.') {
            return {};
        }
        if (specifierIndex < format.size() && format[specifierIndex] == '*') {
            return {};
        }

        std::string length;
        if (specifierIndex + 1 < format.size()
            && ((format[specifierIndex] == 'l' && format[specifierIndex + 1] == 'l')
                || (format[specifierIndex] == 'h' && format[specifierIndex + 1] == 'h'))) {
            length = format.substr(specifierIndex, 2);
            specifierIndex += 2;
        } else if (specifierIndex < format.size()
                   && std::string_view("hlzt").find(format[specifierIndex]) != std::string_view::npos) {
            length = format.substr(specifierIndex, 1);
            ++specifierIndex;
        }
        if (specifierIndex >= format.size()) {
            return {};
        }

        const char specifier = format[specifierIndex];
        if (!isAllowedSpecifier(length, specifier)) {
            return {};
        }
        parsed.parts.push_back(FormatPart{true, {}});
        ++parsed.placeholderCount;
        index = specifierIndex;
    }

    appendLiteralPart(parsed, literal);
    parsed.safe = true;
    return parsed;
}

std::vector<std::string> splitArguments(const std::string& arguments)
{
    std::vector<std::string> result;
    std::string current;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    for (const char character : arguments) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inChar)) {
            current.push_back(character);
            escaped = true;
            continue;
        }
        if (character == '"' && !inChar) {
            inString = !inString;
            current.push_back(character);
            continue;
        }
        if (character == '\'' && !inString) {
            inChar = !inChar;
            current.push_back(character);
            continue;
        }
        if (!inString && !inChar) {
            if (character == '(') {
                ++parenDepth;
            } else if (character == ')') {
                --parenDepth;
            } else if (character == '[') {
                ++bracketDepth;
            } else if (character == ']') {
                --bracketDepth;
            } else if (character == '{') {
                ++braceDepth;
            } else if (character == '}') {
                --braceDepth;
            } else if (character == ',' && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                result.push_back(trim(current));
                current.clear();
                continue;
            }
        }
        current.push_back(character);
    }
    if (!trim(current).empty()) {
        result.push_back(trim(current));
    }
    return result;
}

std::string quoteLiteral(const std::string& text)
{
    return "\"" + text + "\"";
}

std::string formatStringForStdFormat(const ParsedFormat& parsed)
{
    std::string output;
    for (const FormatPart& part : parsed.parts) {
        if (part.placeholder) {
            output += "{}";
            continue;
        }
        for (const char character : part.text) {
            if (character == '{' || character == '}') {
                output.push_back(character);
            }
            output.push_back(character);
        }
    }
    return output;
}

std::string buildStreamExpression(const std::string& stream,
                                  const ParsedFormat& parsed,
                                  const std::vector<std::string>& arguments)
{
    std::ostringstream output;
    output << stream;
    int argumentIndex = 0;
    bool wroteAnyPart = false;
    for (const FormatPart& part : parsed.parts) {
        if (part.placeholder) {
            output << " << " << arguments[static_cast<std::size_t>(argumentIndex++)];
            wroteAnyPart = true;
            continue;
        }
        if (!part.text.empty()) {
            output << " << " << quoteLiteral(part.text);
            wroteAnyPart = true;
        }
    }
    if (!wroteAnyPart) {
        output << " << \"\"";
    }
    return output.str();
}

std::string buildFormatExpression(const std::string& stream,
                                  const ParsedFormat& parsed,
                                  const std::vector<std::string>& arguments)
{
    std::ostringstream output;
    output << stream << " << std::format(" << quoteLiteral(formatStringForStdFormat(parsed));
    for (const std::string& argument : arguments) {
        output << ", " << argument;
    }
    output << ")";
    return output.str();
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

std::string rewritePreservedFileStringArgument(const std::string& line,
                                               const std::set<std::string>& stringSymbols,
                                               bool& changed)
{
    std::smatch match;
    const std::regex fprintfPattern(R"re(^([ \t]*)((?:std::)?fprintf)\s*\(\s*([^,]+)\s*,\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*(.*))?\)\s*;\s*$)re");
    if (!std::regex_match(line, match, fprintfPattern)) {
        return line;
    }

    std::vector<std::string> arguments = splitArguments(match[5].matched ? match[5].str() : std::string{});
    bool lineChanged = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (simpleFormatStringNeedsCStringAt(match[4].str(), index)
            && expressionIsKnownString(arguments[index], stringSymbols)) {
            arguments[index] = trim(arguments[index]) + ".c_str()";
            lineChanged = true;
        }
    }
    if (!lineChanged) {
        return line;
    }

    std::ostringstream replacement;
    replacement << match[1].str() << match[2].str() << "(" << trim(match[3].str()) << ", \"" << match[4].str() << "\"";
    for (const std::string& argument : arguments) {
        replacement << ", " << argument;
    }
    replacement << ");";
    changed = true;
    return replacement.str();
}

std::string rewriteOneCall(const std::string& indent,
                           const std::string& stream,
                           const std::string& format,
                           const std::string& argumentTail,
                           const ModernizationOptions& options,
                           bool& usedFormat,
                           std::string& reason)
{
    const ParsedFormat parsed = parseSimpleFormat(format);
    if (!parsed.safe) {
        reason = "Format string contains width, precision, floating-point, pointer, hex/octal, or otherwise complex specifiers that require manual review.";
        return {};
    }

    const std::vector<std::string> arguments = splitArguments(argumentTail);
    if (parsed.placeholderCount != static_cast<int>(arguments.size())) {
        reason = "Format argument count does not match simple placeholder count.";
        return {};
    }

    const bool canUseStdFormat = parsed.placeholderCount > 0
        && options.useStdFormatForStreams
        && options.targetStandard == CppStandard::Cpp20;
    usedFormat = canUseStdFormat;
    const std::string expression = canUseStdFormat
        ? buildFormatExpression(stream, parsed, arguments)
        : buildStreamExpression(stream, parsed, arguments);
    return indent + expression + ";";
}
} // namespace

std::string PrintfModernizationPass::rewrite(const std::string& code,
                                             const ModernizationOptions& options,
                                             std::vector<ConversionChange>& changes) const
{
    std::vector<std::string> lines = splitLines(code);
    bool changed = false;
    bool usedFormat = false;
    const std::set<std::string> stringSymbols = collectStringSymbols(code);
    const std::regex printfPattern(R"re(^([ \t]*)(?:std::)?printf\s*\(\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*(.*))?\)\s*;\s*$)re");
    const std::regex fprintfStandardPattern(R"re(^([ \t]*)(?:std::)?fprintf\s*\(\s*(stdout|stderr)\s*,\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*(.*))?\)\s*;\s*$)re");
    const std::regex fprintfOtherPattern(R"re(^[ \t]*(?:std::)?fprintf\s*\(\s*([^,]+),)re");

    for (std::string& line : lines) {
        std::smatch match;
        std::string rewritten;
        std::string reason;
        bool lineUsedFormat = false;
        std::string stream;
        if (std::regex_match(line, match, printfPattern)) {
            stream = "std::cout";
            rewritten = rewriteOneCall(match[1].str(),
                                       stream,
                                       match[2].str(),
                                       match[3].matched ? match[3].str() : std::string{},
                                       options,
                                       lineUsedFormat,
                                       reason);
        } else if (std::regex_match(line, match, fprintfStandardPattern)) {
            stream = match[2].str() == "stderr" ? "std::cerr" : "std::cout";
            rewritten = rewriteOneCall(match[1].str(),
                                       stream,
                                       match[3].str(),
                                       match[4].matched ? match[4].str() : std::string{},
                                       options,
                                       lineUsedFormat,
                                       reason);
        } else if (std::regex_search(line, match, fprintfOtherPattern)) {
            const std::string target = trim(match[1].str());
            bool compatibilityChanged = false;
            const std::string compatibilityRewrite = rewritePreservedFileStringArgument(line, stringSymbols, compatibilityChanged);
            if (compatibilityChanged) {
                addAppliedChange(changes,
                                 "fprintf std::string argument compatibility",
                                 trim(line),
                                 trim(compatibilityRewrite),
                                 "Added c_str() for std::string values passed to preserved FILE* %s formatting.");
                line = compatibilityRewrite;
                changed = true;
                continue;
            }
            addSuggestion(changes,
                          "printf-family output modernization suggestion",
                          trim(line),
                          target == "stdout" || target == "stderr"
                              ? "The fprintf call was preserved because only simple literal format strings are rewritten automatically."
                              : "fprintf targeting a FILE* was preserved; FILE ownership/output modernization is handled by the FILE I/O RAII pass when safe.");
            continue;
        } else if (line.find("printf") != std::string::npos || line.find("fprintf") != std::string::npos) {
            addSuggestion(changes,
                          "printf-family output modernization suggestion",
                          trim(line),
                          "The printf-family call was preserved because only simple single-line output calls are rewritten automatically.");
            continue;
        } else {
            continue;
        }

        if (rewritten.empty()) {
            addSuggestion(changes,
                          "printf-family output modernization suggestion",
                          trim(line),
                          reason.empty() ? "The printf-family call was preserved for manual review." : reason);
            continue;
        }

        const std::string before = trim(line);
        const std::string after = trim(rewritten);
        line = rewritten;
        changed = true;
        usedFormat = usedFormat || lineUsedFormat;
        addAppliedChange(changes,
                         lineUsedFormat ? "printf-family output to std::format" : "printf-family output to iostream",
                         before,
                         after,
                         lineUsedFormat
                             ? "Converted a simple printf-family output call to std::format and stream output under the C++20 format option."
                             : "Converted a simple printf-family output call to an iostream chain while preserving the output target.");
    }

    if (!changed) {
        return code;
    }

    std::string updated = joinLines(lines);
    const IncludeManager includeManager;
    updated = includeManager.ensureInclude(std::move(updated), "#include <iostream>");
    if (usedFormat) {
        updated = includeManager.ensureInclude(std::move(updated), "#include <format>");
    }
    updated = includeManager.removeIncludeIfUnused(std::move(updated), "#include <cstdio>", {
        "printf(",
        "fprintf(",
        "sprintf(",
        "snprintf(",
        "vprintf(",
        "vfprintf(",
        "std::printf",
        "std::fprintf",
        "std::sprintf",
        "std::snprintf",
    });
    updated = includeManager.removeIncludeIfUnused(std::move(updated), "#include <stdio.h>", {
        "printf(",
        "fprintf(",
        "sprintf(",
        "snprintf(",
        "vprintf(",
        "vfprintf(",
        "std::printf",
        "std::fprintf",
        "std::sprintf",
        "std::snprintf",
    });
    if (containsPrintfFamily(updated)) {
        return updated;
    }
    return updated;
}
