#include "converter/IndexLoopModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <regex>
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

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        "Index loop to range-based for",
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: IndexLoopModernizationPass",
        "pass started",
        {},
        "candidates found: " + std::to_string(candidates)
            + ", candidates converted: " + std::to_string(converted)
            + ", candidates skipped: " + std::to_string(skipped),
        false,
        true,
    });
}

void addSkippedDiagnostic(std::vector<ConversionChange>& changes, std::string reason)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: IndexLoopModernizationPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}

bool containsIdentifier(const std::string& text, const std::string& identifier)
{
    return std::regex_search(text, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::string baseNameForCollection(std::string collection)
{
    const std::size_t separator = collection.find_last_of(".>");
    if (separator != std::string::npos) {
        collection = collection.substr(separator + 1);
    }
    collection.erase(std::remove_if(collection.begin(), collection.end(), [](unsigned char character) {
                         return !std::isalnum(character) && character != '_';
                     }),
                     collection.end());
    if (collection.size() > 1 && collection.back() == 's') {
        collection.pop_back();
    }
    return collection.empty() ? "item" : collection;
}

std::string finalIdentifierFromCollection(std::string collection)
{
    collection = trim(std::move(collection));
    std::smatch match;
    const std::regex trailingIdentifier(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]+\])?\s*$)");
    if (std::regex_search(collection, match, trailingIdentifier)) {
        return match[1].str();
    }
    return {};
}

std::string variableNameForCollection(const std::string& collection, const std::string& body)
{
    std::vector<std::string> candidates;
    candidates.push_back(baseNameForCollection(collection));
    candidates.push_back("item");
    candidates.push_back("element");
    candidates.push_back("value");
    candidates.push_back("entry");

    const std::string collectionIdentifier = finalIdentifierFromCollection(collection);
    for (const std::string& candidate : candidates) {
        if (candidate.empty() || candidate == collectionIdentifier || containsIdentifier(body, candidate)) {
            continue;
        }
        return candidate;
    }
    return collectionIdentifier == "item" ? "element" : "item";
}
} // namespace

std::string IndexLoopModernizationPass::rewrite(const std::string& code,
                                                const ModernizationOptions& options,
                                                std::vector<ConversionChange>& changes) const
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    std::string updated = code;
    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::regex loopPattern(
        R"((^[ \t]*)for\s*\(\s*(?:std::size_t|size_t|int|auto)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*()"
            + collectionExpression
            + R"()\s*\.\s*size\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    while (std::regex_search(search, match, loopPattern)) {
        ++candidates;
        const std::string indent = match[1].str();
        const std::string indexName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        if (body.find(".erase") != std::string::npos
            || body.find(".insert") != std::string::npos
            || body.find(collection + ".push_back") != std::string::npos
            || body.find(collection + ".emplace_back") != std::string::npos) {
            ++skipped;
            addSkippedDiagnostic(changes, "Index loop was preserved because the loop mutates container size while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string indexedExpression = escapeRegex(collection) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
        const std::regex anyIndexAccess(R"(\b[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*\s*\[\s*)"
                                        + escapeRegex(indexName)
                                        + R"(\s*\])");
        std::string bodyWithoutTarget = std::regex_replace(body, std::regex(indexedExpression), "");
        if (std::regex_search(bodyWithoutTarget, anyIndexAccess)
            || std::regex_search(bodyWithoutTarget, std::regex("\\b" + escapeRegex(indexName) + "\\b"))
            || std::regex_search(body, std::regex("\\b" + escapeRegex(indexName) + R"(\s*(?:\+|-|\*|/|%))"))
            || std::regex_search(body, std::regex(R"((?:\+|-|\*|/|%)\s*)" + escapeRegex(indexName) + R"(\b)"))) {
            ++skipped;
            addSkippedDiagnostic(changes, "Index loop was preserved because the index has semantic meaning beyond selecting the current element.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool mutableElement = std::regex_search(body, std::regex(indexedExpression + R"(\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--))"))
            || std::regex_search(body, std::regex(R"((?:\+\+|--)\s*)" + indexedExpression))
            || std::regex_search(body, std::regex(indexedExpression + R"(\s*(?:\.|->)[^;\n]*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--|\())"));
        const std::string itemName = variableNameForCollection(collection, body);
        body = std::regex_replace(body, std::regex(indexedExpression), itemName);
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = indent + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + itemName + " : " + collection + ")\n" + indent + "{\n" + body + "\n" + indent + "}";
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        ++converted;
        addAppliedChange(changes,
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a size-based index loop to range-based for because the index only selected the current element.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = updated.substr(consumed);
    }

    addDiagnostic(changes, candidates, converted, skipped);
    return updated;
}
