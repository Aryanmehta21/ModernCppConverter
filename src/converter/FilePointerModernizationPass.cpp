#include "converter/FilePointerModernizationPass.h"

#include "converter/IncludeManager.h"

#include <cctype>
#include <regex>
#include <set>
#include <sstream>
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

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
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
        if (specifierIndex < format.size() && std::isdigit(static_cast<unsigned char>(format[specifierIndex])) != 0) {
            return {};
        }
        if (specifierIndex < format.size() && (format[specifierIndex] == '.' || format[specifierIndex] == '*')) {
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
        if (specifierIndex >= format.size() || !isAllowedSpecifier(length, format[specifierIndex])) {
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
    bool inCharacter = false;
    bool escaped = false;
    for (const char character : arguments) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            current.push_back(character);
            escaped = true;
            continue;
        }
        if (character == '"' && !inCharacter) {
            inString = !inString;
            current.push_back(character);
            continue;
        }
        if (character == '\'' && !inString) {
            inCharacter = !inCharacter;
            current.push_back(character);
            continue;
        }
        if (!inString && !inCharacter) {
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
        } else if (!part.text.empty()) {
            output << " << " << quoteLiteral(part.text);
            wroteAnyPart = true;
        }
    }
    if (!wroteAnyPart) {
        output << " << \"\"";
    }
    return output.str();
}

bool containsCFileApi(const std::string& code)
{
    return std::regex_search(code, std::regex(R"(\b(?:FILE|fopen|fclose|fprintf|fputs|fread|fwrite|std::fopen|std::fclose|std::fprintf|std::fputs)\b)"));
}

struct StreamSymbol
{
    std::string type;
    std::string name;
};

std::vector<StreamSymbol> collectStreamSymbols(const std::string& code)
{
    std::vector<StreamSymbol> streams;
    const std::regex streamDeclaration(
        R"(\bstd::(ofstream|ifstream|fstream)\s+([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    std::set<std::string> seen;
    for (std::sregex_iterator iterator(code.begin(), code.end(), streamDeclaration), end; iterator != end; ++iterator) {
        const std::string name = (*iterator)[2].str();
        if (!seen.insert(name).second) {
            continue;
        }
        streams.push_back(StreamSymbol{(*iterator)[1].str(), name});
    }
    return streams;
}

bool isOutputStream(const StreamSymbol& stream)
{
    return stream.type == "ofstream" || stream.type == "fstream";
}

std::string rewriteStreamNullComparisons(std::string line, const std::string& streamName)
{
    const std::string escapedName = escapeRegex(streamName);
    line = std::regex_replace(line,
                              std::regex(R"(\b)" + escapedName + R"(\s*!=\s*(?:nullptr|NULL)\b)"),
                              streamName);
    line = std::regex_replace(line,
                              std::regex(R"(\b(?:nullptr|NULL)\s*!=\s*)" + escapedName + R"(\b)"),
                              streamName);
    line = std::regex_replace(line,
                              std::regex(R"(\b)" + escapedName + R"(\s*==\s*(?:nullptr|NULL)\b)"),
                              "!" + streamName);
    line = std::regex_replace(line,
                              std::regex(R"(\b(?:nullptr|NULL)\s*==\s*)" + escapedName + R"(\b)"),
                              "!" + streamName);
    return line;
}

std::string cleanupStreamArtifacts(std::string code, std::vector<ConversionChange>& changes, bool& changed)
{
    const std::vector<StreamSymbol> streams = collectStreamSymbols(code);
    if (streams.empty()) {
        return code;
    }

    std::vector<std::string> lines = splitLines(code);
    for (std::string& line : lines) {
        for (const StreamSymbol& stream : streams) {
            const std::string beforeNullCleanup = line;
            line = rewriteStreamNullComparisons(line, stream.name);
            if (line != beforeNullCleanup) {
                changed = true;
                addAppliedChange(changes,
                                 "FILE stream nullptr artifact cleanup",
                                 trim(beforeNullCleanup),
                                 trim(line),
                                 "Replaced pointer-style nullptr/NULL comparison on a C++ stream with stream state testing.");
            }

            const std::regex fclosePattern(R"(^[ \t]*(?:std::)?fclose\s*\(\s*)"
                                           + escapeRegex(stream.name)
                                           + R"(\s*\)\s*;\s*$)");
            if (std::regex_match(line, fclosePattern)) {
                addAppliedChange(changes,
                                 "Remove fclose after FILE stream RAII",
                                 trim(line),
                                 {},
                                 "Removed fclose() because the converted C++ stream closes via RAII.");
                line.clear();
                changed = true;
                continue;
            }

            const std::regex fprintfPattern(std::string(R"re(^([ \t]*)(?:std::)?fprintf\s*\(\s*)re")
                                            + escapeRegex(stream.name)
                                            + R"re(\s*,\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*(.*))?\)\s*;\s*$)re");
            std::smatch fprintfMatch;
            if (isOutputStream(stream) && std::regex_match(line, fprintfMatch, fprintfPattern)) {
                const ParsedFormat parsed = parseSimpleFormat(fprintfMatch[2].str());
                const std::vector<std::string> arguments = splitArguments(fprintfMatch[3].matched ? fprintfMatch[3].str() : std::string{});
                if (parsed.safe && parsed.placeholderCount == static_cast<int>(arguments.size())) {
                    const std::string before = line;
                    line = fprintfMatch[1].str() + buildStreamExpression(stream.name, parsed, arguments) + ";";
                    changed = true;
                    addAppliedChange(changes,
                                     "fprintf stream artifact cleanup",
                                     trim(before),
                                     trim(line),
                                     "Rewrote fprintf() left behind after FILE* RAII conversion to stream insertion.");
                }
            }

            const std::regex fputsPattern(std::string(R"re(^([ \t]*)(?:std::)?fputs\s*\(\s*"((?:\\.|[^"\\])*)"\s*,\s*)re")
                                          + escapeRegex(stream.name)
                                          + R"re(\s*\)\s*;\s*$)re");
            std::smatch fputsMatch;
            if (isOutputStream(stream) && std::regex_match(line, fputsMatch, fputsPattern)) {
                const std::string before = line;
                line = fputsMatch[1].str() + stream.name + " << \"" + fputsMatch[2].str() + "\";";
                changed = true;
                addAppliedChange(changes,
                                 "fputs stream artifact cleanup",
                                 trim(before),
                                 trim(line),
                                 "Rewrote fputs() left behind after FILE* RAII conversion to stream insertion.");
            }
        }
    }

    std::vector<std::string> compacted;
    compacted.reserve(lines.size());
    for (const std::string& line : lines) {
        if (!line.empty()) {
            compacted.push_back(line);
        }
    }
    return joinLines(compacted);
}
} // namespace

std::string FilePointerModernizationPass::rewrite(const std::string& code,
                                                  std::vector<ConversionChange>& changes) const
{
    std::vector<std::string> lines = splitLines(code);
    bool changed = false;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::smatch fopenMatch;
        const std::regex fopenPattern(R"re(^([ \t]*)(?:std::)?FILE\s*\*\s*([A-Za-z_]\w*)\s*=\s*(?:std::)?fopen\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*\)\s*;\s*$)re");
        if (!std::regex_match(lines[index], fopenMatch, fopenPattern)) {
            continue;
        }

        const std::string indent = fopenMatch[1].str();
        const std::string fileName = fopenMatch[2].str();
        const std::string pathExpression = trim(fopenMatch[3].str());
        const std::string mode = fopenMatch[4].str();
        if (mode.find('b') != std::string::npos || mode.find('w') == std::string::npos) {
            addSuggestion(changes,
                          "FILE pointer to fstream RAII",
                          trim(lines[index]),
                          "Binary or non-write FILE* usage was preserved for manual RAII review.");
            continue;
        }

        std::size_t fcloseLine = std::string::npos;
        std::vector<std::size_t> fprintfLines;
        std::vector<std::size_t> fputsLines;
        const std::regex fclosePattern(R"(^[ \t]*(?:std::)?fclose\s*\(\s*)" + escapeRegex(fileName) + R"(\s*\)\s*;\s*$)");
        const std::regex fprintfPattern(std::string(R"re(^([ \t]*)(?:std::)?fprintf\s*\(\s*)re")
                                        + escapeRegex(fileName)
                                        + R"re(\s*,\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*(.*))?\)\s*;\s*$)re");
        const std::regex fputsPattern(std::string(R"re(^([ \t]*)(?:std::)?fputs\s*\(\s*"((?:\\.|[^"\\])*)"\s*,\s*)re")
                                      + escapeRegex(fileName)
                                      + R"re(\s*\)\s*;\s*$)re");
        for (std::size_t scan = index + 1; scan < lines.size(); ++scan) {
            if (std::regex_match(lines[scan], fclosePattern)) {
                fcloseLine = scan;
                break;
            }
            std::smatch fprintfMatch;
            if (std::regex_match(lines[scan], fprintfMatch, fprintfPattern)) {
                fprintfLines.push_back(scan);
                continue;
            }
            std::smatch fputsMatch;
            if (std::regex_match(lines[scan], fputsMatch, fputsPattern)) {
                fputsLines.push_back(scan);
                continue;
            }
            if (lines[scan].find(fileName) != std::string::npos
                && lines[scan].find("if") == std::string::npos
                && lines[scan].find("nullptr") == std::string::npos) {
                addSuggestion(changes,
                              "FILE pointer to fstream RAII",
                              trim(lines[index]),
                              "FILE* usage was preserved because operations other than simple text fprintf/fclose were detected.");
                fcloseLine = std::string::npos;
                break;
            }
        }

        if (fcloseLine == std::string::npos || (fprintfLines.empty() && fputsLines.empty())) {
            continue;
        }

        std::ostringstream before;
        for (std::size_t lineIndex = index; lineIndex <= fcloseLine; ++lineIndex) {
            if (lineIndex > index) {
                before << '\n';
            }
            before << lines[lineIndex];
        }

        bool fprintfBlockSafe = true;
        for (const std::size_t fprintfLine : fprintfLines) {
            std::smatch fprintfMatch;
            std::regex_match(lines[fprintfLine], fprintfMatch, fprintfPattern);
            const ParsedFormat parsed = parseSimpleFormat(fprintfMatch[2].str());
            const std::vector<std::string> arguments = splitArguments(fprintfMatch[3].matched ? fprintfMatch[3].str() : std::string{});
            if (!parsed.safe || parsed.placeholderCount != static_cast<int>(arguments.size())) {
                addSuggestion(changes,
                              "FILE pointer to fstream RAII",
                              trim(lines[fprintfLine]),
                              "FILE* fprintf was preserved because the format string or argument list is too complex for safe stream conversion.");
                fprintfBlockSafe = false;
                break;
            }
        }
        if (!fprintfBlockSafe) {
            continue;
        }

        lines[index] = indent + "std::ofstream " + fileName + "(" + pathExpression + ");";
        const std::regex nullCheckPattern(
            std::string(R"(^([ \t]*)if\s*\(\s*(?:!\s*)?)")
            + escapeRegex(fileName)
            + R"((?:\s*==\s*nullptr)?\s*\)(.*)$)");
        if (index + 1 < lines.size() && std::regex_match(lines[index + 1], nullCheckPattern)) {
            lines[index + 1] = std::regex_replace(lines[index + 1],
                                                  std::regex(escapeRegex(fileName) + R"(\s*==\s*nullptr|!\s*)" + escapeRegex(fileName)),
                                                  "!" + fileName);
        }

        bool allFprintfLinesSafe = true;
        for (const std::size_t fprintfLine : fprintfLines) {
            std::smatch fprintfMatch;
            std::regex_match(lines[fprintfLine], fprintfMatch, fprintfPattern);
            const ParsedFormat parsed = parseSimpleFormat(fprintfMatch[2].str());
            const std::vector<std::string> arguments = splitArguments(fprintfMatch[3].matched ? fprintfMatch[3].str() : std::string{});
            if (!parsed.safe || parsed.placeholderCount != static_cast<int>(arguments.size())) {
                addSuggestion(changes,
                              "FILE pointer to fstream RAII",
                              trim(lines[fprintfLine]),
                              "FILE* fprintf was preserved because the format string or argument list is too complex for safe stream conversion.");
                allFprintfLinesSafe = false;
                break;
            }
            lines[fprintfLine] = fprintfMatch[1].str() + buildStreamExpression(fileName, parsed, arguments) + ";";
        }
        if (!allFprintfLinesSafe) {
            continue;
        }
        for (const std::size_t fputsLine : fputsLines) {
            std::smatch fputsMatch;
            std::regex_match(lines[fputsLine], fputsMatch, fputsPattern);
            lines[fputsLine] = fputsMatch[1].str() + fileName + " << \"" + fputsMatch[2].str() + "\";";
        }
        lines[fcloseLine].clear();
        changed = true;

        std::ostringstream after;
        for (std::size_t lineIndex = index; lineIndex <= fcloseLine; ++lineIndex) {
            if (lines[lineIndex].empty()) {
                continue;
            }
            if (after.tellp() > 0) {
                after << '\n';
            }
            after << lines[lineIndex];
        }

        addAppliedChange(changes,
                         "FILE pointer to fstream RAII",
                         trim(before.str()),
                         trim(after.str()),
                         "Replaced simple text-write FILE* ownership with std::ofstream RAII and removed fclose().");
    }

    std::string updated = code;
    if (changed) {
        std::vector<std::string> compacted;
        compacted.reserve(lines.size());
        for (const std::string& line : lines) {
            if (!line.empty()) {
                compacted.push_back(line);
            }
        }

        updated = joinLines(compacted);
    }

    bool artifactChanged = false;
    updated = cleanupStreamArtifacts(std::move(updated), changes, artifactChanged);
    if (!changed && !artifactChanged) {
        return code;
    }

    const IncludeManager includeManager;
    updated = includeManager.ensureInclude(std::move(updated), "#include <fstream>");
    updated = includeManager.removeIncludeIfUnused(std::move(updated), "#include <cstdio>", {
        "FILE",
        "fopen",
        "fclose",
        "fprintf",
        "fputs",
        "fread",
        "fwrite",
        "std::fopen",
        "std::fclose",
        "std::fprintf",
        "std::fputs",
    });
    if (containsCFileApi(updated)) {
        return updated;
    }
    return updated;
}
