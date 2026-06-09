#include "converter/FilePointerModernizationPass.h"

#include "converter/IncludeManager.h"

#include <regex>
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

bool containsCFileApi(const std::string& code)
{
    return std::regex_search(code, std::regex(R"(\b(?:FILE|fopen|fclose|fprintf|fread|fwrite|std::fopen|std::fclose|std::fprintf)\b)"));
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
        const std::regex fclosePattern(R"(^[ \t]*(?:std::)?fclose\s*\(\s*)" + escapeRegex(fileName) + R"(\s*\)\s*;\s*$)");
        const std::regex fprintfPattern(std::string(R"re(^([ \t]*)(?:std::)?fprintf\s*\(\s*)re")
                                        + escapeRegex(fileName)
                                        + R"re(\s*,\s*"((?:\\.|[^"\\])*)"\s*\)\s*;\s*$)re");
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

        if (fcloseLine == std::string::npos || fprintfLines.empty()) {
            continue;
        }

        std::ostringstream before;
        for (std::size_t lineIndex = index; lineIndex <= fcloseLine; ++lineIndex) {
            if (lineIndex > index) {
                before << '\n';
            }
            before << lines[lineIndex];
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

        for (const std::size_t fprintfLine : fprintfLines) {
            std::smatch fprintfMatch;
            std::regex_match(lines[fprintfLine], fprintfMatch, fprintfPattern);
            lines[fprintfLine] = fprintfMatch[1].str() + fileName + " << \"" + fprintfMatch[2].str() + "\";";
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

    if (!changed) {
        return code;
    }

    std::vector<std::string> compacted;
    compacted.reserve(lines.size());
    for (const std::string& line : lines) {
        if (!line.empty()) {
            compacted.push_back(line);
        }
    }

    std::string updated = joinLines(compacted);
    const IncludeManager includeManager;
    updated = includeManager.ensureInclude(std::move(updated), "#include <fstream>");
    updated = includeManager.removeIncludeIfUnused(std::move(updated), "#include <cstdio>", {
        "FILE",
        "fopen",
        "fclose",
        "fprintf",
        "fread",
        "fwrite",
        "std::fopen",
        "std::fclose",
        "std::fprintf",
    });
    if (containsCFileApi(updated)) {
        return updated;
    }
    return updated;
}
