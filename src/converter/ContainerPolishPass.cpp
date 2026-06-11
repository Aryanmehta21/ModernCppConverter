#include "converter/ContainerPolishPass.h"

#include "converter/SafeReplacementEngine.h"

#include <regex>
#include <string_view>
#include <utility>

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
} // namespace

std::string ContainerPolishPass::rewrite(const std::string& code,
                                         std::vector<ConversionChange>& changes) const
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;

        const std::regex pairInsertPattern(
            R"(^([ \t]*)([A-Za-z_]\w*)\.insert\s*\(\s*std::(?:pair\s*<[^;()]+>\s*\(\s*([^,;]+)\s*,\s*([^)]+)\)|make_pair\s*\(\s*([^,;]+)\s*,\s*([^)]+)\))\s*\)\s*;\s*$)",
            std::regex::ECMAScript);
        if (std::regex_match(codePart, match, pairInsertPattern)) {
            const std::string key = trim(match[3].matched ? match[3].str() : match[5].str());
            const std::string value = trim(match[4].matched ? match[4].str() : match[6].str());
            const std::string replacement = match[1].str() + match[2].str() + ".emplace(" + key + ", " + value + ");";
            addAppliedChange(changes,
                             "Container insert pair to emplace",
                             trim(codePart),
                             trim(replacement),
                             "Replaced pair construction in an associative container insertion with emplace().");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex pushTemporaryPattern(
            R"(^([ \t]*)([A-Za-z_]\w*)\.push_back\s*\(\s*([A-Za-z_:]\w*(?:::\w+)*)\s*\(([^;\n{}]*)\)\s*\)\s*;\s*$)",
            std::regex::ECMAScript);
        if (std::regex_match(codePart, match, pushTemporaryPattern)) {
            const std::string typeName = trim(match[3].str());
            const std::string arguments = trim(match[4].str());
            if (typeName.find("unique_ptr") == std::string::npos && typeName.find("make_unique") == std::string::npos) {
                const std::string replacement = match[1].str() + match[2].str() + ".emplace_back(" + arguments + ");";
                addAppliedChange(changes,
                                 "push_back temporary to emplace_back",
                                 trim(codePart),
                                 trim(replacement),
                                 "Constructed the pushed value in-place with emplace_back when the push_back argument is a temporary object.");
                changed = true;
                return replacement + trailingComment;
            }
        }

        return line;
    });
    return changed ? updated : code;
}

