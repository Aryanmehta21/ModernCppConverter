#include "converter/SemanticTypeValidationPass.h"

#include "converter/IncludeManager.h"
#include "converter/NsdmiScopeSafetyPass.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
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
} // namespace

std::string SemanticTypeValidationPass::validateAndRepair(const std::string& code,
                                                          const ModernizationOptions&,
                                                          std::vector<ConversionChange>& changes) const
{
    const bool hadAppliedChanges = std::any_of(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.applied;
    });
    std::string updated = code;
    updated = rollbackStringViewCStr(updated, changes);
    updated = repairSignedLoopIndicesForSizeGetters(updated, changes);
    const NsdmiScopeSafetyPass nsdmiScopeSafetyPass;
    updated = nsdmiScopeSafetyPass.validateAndRepair(updated, changes);
    if (hadAppliedChanges || updated != code) {
        updated = cleanupTransformationFormatting(updated, changes);
    }
    return updated;
}
