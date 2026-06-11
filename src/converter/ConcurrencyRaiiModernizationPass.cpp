#include "converter/ConcurrencyRaiiModernizationPass.h"

#include "converter/IncludeManager.h"

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
} // namespace

std::string ConcurrencyRaiiModernizationPass::rewrite(const std::string& code,
                                                      const ModernizationOptions&,
                                                      std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = false;
    const std::regex safeLockPattern(
        R"((^[ \t]*)([A-Za-z_]\w*((\.|->)[A-Za-z_]\w*)*)\.lock\s*\(\s*\)\s*;\s*\n([\s\S]*?)\n\1\2\.unlock\s*\(\s*\)\s*;)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, safeLockPattern)) {
        const std::string body = match[5].str();
        const std::string mutexExpression = match[2].str();
        if (body.find("return") != std::string::npos
            || body.find("throw") != std::string::npos
            || body.find(mutexExpression + ".unlock") != std::string::npos
            || body.find(mutexExpression + "->unlock") != std::string::npos) {
            addSuggestion(changes,
                          "mutex RAII guard suggestion",
                          trim(match[0].str()),
                          "Manual lock/unlock was preserved because control flow inside the locked region is not a simple same-scope sequence.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }
        const std::string replacement = match[1].str() + "std::lock_guard<std::mutex> lock(" + mutexExpression + ");\n" + body;
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        addAppliedChange(changes,
                         "Manual mutex lock/unlock to RAII guard",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted same-scope manual mutex lock/unlock to std::lock_guard so unlocking is exception-safe.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = updated.substr(consumed);
    }

    const std::regex remainingLockPattern(R"([A-Za-z_]\w*((\.|->)[A-Za-z_]\w*)*\.lock\s*\(\s*\))");
    if (std::regex_search(updated, remainingLockPattern) && updated.find(".unlock(") != std::string::npos) {
        addSuggestion(changes,
                      "mutex RAII guard suggestion",
                      "manual lock/unlock",
                      "A lock/unlock pair remains because the converter could not prove same-scope RAII conversion was safe.");
    }

    const std::regex threadJoinPattern(R"(\bstd::thread\s+([A-Za-z_]\w*)\s*\([^;\n]*\)\s*;[\s\S]*?\b\1\.join\s*\(\s*\)\s*;)",
                                       std::regex::ECMAScript);
    if (std::regex_search(updated, threadJoinPattern)) {
        addSuggestion(changes,
                      "std::jthread / RAII thread suggestion",
                      "manual std::thread join",
                      "Manual thread join was preserved. Consider std::jthread or an RAII thread wrapper after reviewing stop-token and lifetime semantics.");
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <mutex>");
    }
    return updated;
}
