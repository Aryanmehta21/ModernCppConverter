#include "converter/OwnershipSanityScanner.h"

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

bool isSmartPointerCollectionRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<std::unique_ptr<")
        || record.newType.starts_with("std::array<std::unique_ptr<")
        || record.newType.starts_with("std::vector<std::shared_ptr<");
}

std::string removeIndexDeleteLoops(std::string code,
                                   const TypeChangeRecord& record,
                                   std::vector<ConversionChange>& changes,
                                   bool& changed)
{
    const std::regex loopPattern(
        R"(\n?[ \t]*for\s*\([^\n;]+;[^\n;]+;[^\n\)]*\)\s*\n?[ \t]*\{\s*\n?[ \t]*delete\s+)"
            + escapeRegex(record.symbolName)
            + R"(\s*\[[^\]]+\]\s*;\s*\n?[ \t]*\}\s*)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, loopPattern)) {
        addAppliedChange(changes,
                         "Ownership sanity cleanup",
                         trim(match[0].str()),
                         "removed",
                         "Removed leftover nested delete loop for a smart-owned collection.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
        changed = true;
    }
    return code;
}

std::string removeRangeDeleteLoops(std::string code,
                                   const TypeChangeRecord& record,
                                   std::vector<ConversionChange>& changes,
                                   bool& changed)
{
    const std::regex loopPattern(
        R"(\n?[ \t]*for\s*\(\s*(?:auto|auto\s*\*|const\s+auto\s*\*|[A-Za-z_:][A-Za-z0-9_:<>,\s]*\s*\*)\s+([A-Za-z_]\w*)\s*:\s*)"
            + escapeRegex(record.symbolName)
            + R"(\s*\)\s*\n?[ \t]*\{\s*\n?[ \t]*delete\s+\1\s*;\s*\n?[ \t]*\}\s*)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, loopPattern)) {
        addAppliedChange(changes,
                         "Ownership sanity cleanup",
                         trim(match[0].str()),
                         "removed",
                         "Removed leftover range delete loop for a smart-owned collection.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
        changed = true;
    }
    return code;
}
} // namespace

std::string OwnershipSanityScanner::rewrite(const std::string& code,
                                            const TransformationContext& context,
                                            std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isSmartPointerCollectionRecord(record)) {
            continue;
        }

        bool changed = false;
        updated = removeIndexDeleteLoops(std::move(updated), record, changes, changed);
        updated = removeRangeDeleteLoops(std::move(updated), record, changes, changed);

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);

            const std::regex deleteArrayPattern(R"(^[ \t]*delete\s*\[\s*\]\s*)"
                                                + escapeRegex(record.symbolName)
                                                + R"(\s*;\s*$)");
            if (std::regex_match(codePart, deleteArrayPattern)) {
                addAppliedChange(changes,
                                 "Ownership sanity cleanup",
                                 trim(codePart),
                                 "removed",
                                 "Removed leftover delete[] for a smart-owned collection.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex deleteElementPattern(R"(^[ \t]*delete\s+)"
                                                  + escapeRegex(record.symbolName)
                                                  + R"(\s*\[[^\]]+\]\s*;\s*$)");
            if (std::regex_match(codePart, deleteElementPattern)) {
                addAppliedChange(changes,
                                 "Ownership sanity cleanup",
                                 trim(codePart),
                                 "removed",
                                 "Removed leftover delete of an element now owned by a smart pointer collection.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            return line;
        });

        const std::regex partialDeletePattern(R"(delete(?:\s*\[\s*\])?\s+)"
                                              + escapeRegex(record.symbolName)
                                              + R"((?:\s*\[[^\]]+\])?\s*;)");
        if (std::regex_search(updated, partialDeletePattern)) {
            addSuggestion(changes,
                          "Ownership sanity scanner",
                          record.symbolName,
                          "Manual cleanup still appears near a smart-owned collection. Review for a partially modernized ownership graph or double ownership risk.");
        }

        const std::regex orphanAllocationPattern(escapeRegex(record.symbolName) + R"(\s*\[[^\]]+\]\s*=\s*new\s+)");
        if (std::regex_search(updated, orphanAllocationPattern)) {
            addSuggestion(changes,
                          "Ownership sanity scanner",
                          record.symbolName,
                          "Raw allocation still targets a smart-owned collection. Review for orphan allocation or incomplete ownership modernization.");
        }

        if (changed) {
            addAppliedChange(changes,
                             "Ownership sanity scanner",
                             record.symbolName,
                             "ownership leftovers removed",
                             "Removed cleanup artifacts after graph-level ownership modernization.");
        }
    }

    return updated;
}
