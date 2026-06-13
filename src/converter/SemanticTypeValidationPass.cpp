#include "converter/SemanticTypeValidationPass.h"

#include "converter/NsdmiScopeSafetyPass.h"

#include <regex>
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
} // namespace

std::string SemanticTypeValidationPass::validateAndRepair(const std::string& code,
                                                          const ModernizationOptions&,
                                                          std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    updated = rollbackStringViewCStr(updated, changes);
    const NsdmiScopeSafetyPass nsdmiScopeSafetyPass;
    updated = nsdmiScopeSafetyPass.validateAndRepair(updated, changes);
    return updated;
}
