#include "converter/RangeForModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
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

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: RangeForModernizationPass",
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
        "Functional diagnostics: RangeForModernizationPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}

std::string variableNameForCollection(std::string collection)
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

bool isUnsafeIteratorBody(const std::string& body, const std::string& iteratorName)
{
    return body.find("erase(") != std::string::npos
        || body.find("insert(") != std::string::npos
        || body.find("std::advance(" + iteratorName) != std::string::npos
        || body.find("++" + iteratorName) != std::string::npos
        || body.find(iteratorName + "++") != std::string::npos
        || body.find("--" + iteratorName) != std::string::npos
        || body.find(iteratorName + "--") != std::string::npos;
}

bool isConstIteratorTraversal(const std::string& iteratorType, const std::string& loopText)
{
    return iteratorType.find("const_iterator") != std::string::npos
        || loopText.find(".cbegin") != std::string::npos
        || loopText.find(".cend") != std::string::npos;
}

std::string modernizeMapLoops(std::string code,
                              const ModernizationOptions& options,
                              std::vector<ConversionChange>& changes,
                              int& candidates,
                              int& converted,
                              int& skipped)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::string iteratorType =
        R"((?:auto|typename\s+[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*)"
            + iteratorType
            + R"(\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.\s*c?begin\s*\(\s*\)\s*;\s*\2\s*!=\s*\3\s*\.\s*c?end\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        ++candidates;
        const std::string iteratorName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        if (isUnsafeIteratorBody(body, iteratorName)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map iterator loop was preserved because it mutates iteration or changes the container while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex arrowFirst("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*first\b)");
        const std::regex arrowSecond("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*second\b)");
        const std::regex derefFirst(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\)\s*\.\s*first\b)");
        const std::regex derefSecond(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\)\s*\.\s*second\b)");
        const bool usesFirst = std::regex_search(body, arrowFirst) || std::regex_search(body, derefFirst);
        const bool usesSecond = std::regex_search(body, arrowSecond) || std::regex_search(body, derefSecond);
        if (!usesFirst || !usesSecond) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map iterator loop was preserved because it did not use both key and value.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool mutableValue = std::regex_search(body, std::regex(R"((?:->\s*second|\)\s*\.\s*second)\s*(?:=|\+=|-=|\*=|/=|%=))"));
        body = std::regex_replace(body, arrowFirst, "key");
        body = std::regex_replace(body, arrowSecond, "value");
        body = std::regex_replace(body, derefFirst, "key");
        body = std::regex_replace(body, derefSecond, "value");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = match[1].str() + "for (" + (mutableValue ? "auto& " : "const auto& ")
            + "[key, value] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Map iterator loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a map-like iterator loop that only accessed first/second into a structured-binding range loop.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string modernizeGenericLoops(std::string code,
                                  const ModernizationOptions& options,
                                  std::vector<ConversionChange>& changes,
                                  int& candidates,
                                  int& converted,
                                  int& skipped)
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::string iteratorType =
        R"((auto|typename\s+[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*)"
            + iteratorType
            + R"(\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.\s*c?begin\s*\(\s*\)\s*;\s*\3\s*!=\s*\4\s*\.\s*c?end\s*\(\s*\)\s*;\s*(?:\+\+\3|\3\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        ++candidates;
        const std::string iteratorTypeText = match[2].str();
        const std::string iteratorName = match[3].str();
        const std::string collection = match[4].str();
        std::string body = match[5].str();
        if (isUnsafeIteratorBody(body, iteratorName)) {
            ++skipped;
            addSuggestion(changes,
                          "Explicit iterator loop to range-based for",
                          trim(match[0].str()),
                          "Iterator loop was preserved because it mutates iteration or changes the container while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex plainDeref(R"(\*\s*)" + escapeRegex(iteratorName) + R"(\b)");
        const std::regex parenDeref(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\))");
        const std::regex arrowAccess("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*)");
        std::string bodyWithoutDeref = std::regex_replace(body, parenDeref, "");
        bodyWithoutDeref = std::regex_replace(bodyWithoutDeref, plainDeref, "");
        bodyWithoutDeref = std::regex_replace(bodyWithoutDeref, arrowAccess, "");
        if (std::regex_search(bodyWithoutDeref, std::regex("\\b" + escapeRegex(iteratorName) + "\\b"))) {
            ++skipped;
            addSkippedDiagnostic(changes, "Iterator loop was preserved because the iterator is used for more than dereferencing the current element.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool constIterator = isConstIteratorTraversal(iteratorTypeText, match[0].str());
        const bool mutatesThroughArrow = std::regex_search(body, std::regex("\\b" + escapeRegex(iteratorName)
                                                                            + R"(\s*->\s*[A-Za-z_]\w*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--))"))
            || std::regex_search(body, std::regex("\\b" + escapeRegex(iteratorName)
                                                  + R"(\s*->\s*[A-Za-z_]\w*\s*\()"));
        const bool mutatesThroughDottedDeref = std::regex_search(body, std::regex(R"(\(\s*\*\s*)"
                                                                                  + escapeRegex(iteratorName)
                                                                                  + R"(\s*\)\s*\.\s*[A-Za-z_]\w*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--|\())"));
        const bool mutableElement = std::regex_search(body, std::regex(R"((?:\*\s*)" + escapeRegex(iteratorName) + R"(\b|\(\s*\*\s*)"
                                                                       + escapeRegex(iteratorName)
                                                                       + R"(\s*\)(?:\s*(?:\.|->)[^;\n]*)?)\s*(?:=|\+=|-=|\*=|/=|%=))"))
            || (!constIterator && (mutatesThroughArrow || mutatesThroughDottedDeref));
        const std::string itemName = variableNameForCollection(collection);
        body = std::regex_replace(body, parenDeref, itemName);
        body = std::regex_replace(body, plainDeref, itemName);
        body = std::regex_replace(body, arrowAccess, itemName + ".");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = match[1].str() + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + itemName + " : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Explicit iterator loop to range-based for",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a safe explicit iterator traversal into a range-based for loop.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string modernizePairRangeLoops(std::string code,
                                    const ModernizationOptions& options,
                                    std::vector<ConversionChange>& changes,
                                    int& candidates,
                                    int& converted,
                                    int& skipped)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::regex rangeLoop(
        R"((^[ \t]*)for\s*\(\s*(const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, rangeLoop)) {
        ++candidates;
        const std::string referenceKind = match[2].str();
        const std::string element = match[3].str();
        const std::string collection = match[4].str();
        std::string body = match[5].str();
        const std::regex firstPattern("\\b" + escapeRegex(element) + R"(\.first\b|\(\s*)"
                                      + escapeRegex(element)
                                      + R"(\s*\)\s*\.\s*first\b)");
        const std::regex secondPattern("\\b" + escapeRegex(element) + R"(\.second\b|\(\s*)"
                                       + escapeRegex(element)
                                       + R"(\s*\)\s*\.\s*second\b)");
        if (!std::regex_search(body, firstPattern) || !std::regex_search(body, secondPattern)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Pair range loop was preserved because it did not use both first and second.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        body = std::regex_replace(body, firstPattern, "key");
        body = std::regex_replace(body, secondPattern, "value");
        const std::string replacement = match[1].str() + "for (" + referenceKind + " [key, value] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Map range loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted pair member access in a range loop to structured bindings.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}
} // namespace

std::string RangeForModernizationPass::rewrite(const std::string& code,
                                               const ModernizationOptions& options,
                                               std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    updated = modernizeMapLoops(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizeGenericLoops(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizePairRangeLoops(std::move(updated), options, changes, candidates, converted, skipped);
    addDiagnostic(changes, candidates, converted, skipped);
    return updated;
}
