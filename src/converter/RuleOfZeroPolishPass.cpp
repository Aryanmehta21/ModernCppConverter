#include "converter/RuleOfZeroPolishPass.h"

#include <algorithm>
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

std::string removePattern(std::string code,
                          const std::regex& pattern,
                          const std::string& ruleName,
                          const std::string& reason,
                          std::vector<ConversionChange>& changes)
{
    std::smatch match;
    while (std::regex_search(code, match, pattern)) {
        addAppliedChange(changes, ruleName, trim(match[0].str()), "removed", reason);
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "");
    }
    return code;
}
} // namespace

std::string RuleOfZeroPolishPass::rewrite(const std::string& code,
                                          std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    updated = removePattern(updated,
                            std::regex(R"(^[ \t]*(?!virtual\b)~([A-Za-z_]\w*)\s*\(\s*\)\s*(?:=\s*default\s*;|\{\s*\})\s*)",
                                       std::regex::ECMAScript | std::regex::multiline),
                            "Rule of Zero polish destructor removal",
                            "Removed an explicitly empty/default destructor so compiler-generated destruction can be used.",
                            changes);
    const bool coreDefaultedCopyForApiStability = std::any_of(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.ruleName.find("Remove obsolete copy constructor") != std::string::npos
            || change.ruleName.find("Remove obsolete copy assignment") != std::string::npos
            || change.ruleName.find("Rule of Zero cascade cleanup") != std::string::npos;
    });
    if (!coreDefaultedCopyForApiStability) {
        updated = removePattern(updated,
                                std::regex(R"(^[ \t]*([A-Za-z_]\w*)\s*\(\s*const\s+\1\s*&\s*\)\s*=\s*default\s*;\s*)",
                                           std::regex::ECMAScript | std::regex::multiline),
                                "Rule of Zero polish copy constructor removal",
                                "Removed an explicitly defaulted copy constructor whose behavior is already compiler-generated.",
                                changes);
        updated = removePattern(updated,
                                std::regex(R"(^[ \t]*([A-Za-z_]\w*)\s*&\s*operator\s*=\s*\(\s*const\s+\1\s*&\s*\)\s*=\s*default\s*;\s*)",
                                           std::regex::ECMAScript | std::regex::multiline),
                                "Rule of Zero polish assignment operator removal",
                                "Removed an explicitly defaulted copy assignment operator whose behavior is already compiler-generated.",
                                changes);
    }
    return updated;
}
