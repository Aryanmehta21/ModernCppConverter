#include "converter/MallocFreeModernizationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct ConversionCandidate
{
    enum class Kind {
        SingleObject,
        DynamicArray,
    };

    Kind kind = Kind::SingleObject;
    std::string variable;
    std::string elementType;
    std::string sizeExpression;
    bool declarationOwnsAllocation = false;
    bool classMember = false;
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

std::string collapseWhitespace(std::string value)
{
    value = std::regex_replace(std::move(value), std::regex(R"(\s+)"), " ");
    return trim(std::move(value));
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

std::vector<std::string> splitLines(const std::string& code)
{
    std::vector<std::string> lines;
    std::stringstream input(code);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string mallocCallPattern()
{
    return R"((?:(?:std::)?malloc))";
}

std::string callocCallPattern()
{
    return R"((?:(?:std::)?calloc))";
}

std::string stripPointerAllocationCast(std::string expression, const std::string& type)
{
    const std::string escapedType = escapeRegex(type);
    expression = trim(std::move(expression));
    std::smatch match;
    const std::regex cStyleCastPattern(R"(^\(\s*)" + escapedType + R"(\s*\*\s*\)\s*(.+)$)");
    if (std::regex_match(expression, match, cStyleCastPattern)) {
        return trim(match[1].str());
    }

    const std::regex staticCastPattern(R"(^static_cast\s*<\s*)" + escapedType + R"(\s*\*\s*>\s*\(\s*(.+)\s*\)$)");
    if (std::regex_match(expression, match, staticCastPattern)) {
        return trim(match[1].str());
    }

    return expression;
}

bool isByteType(const std::string& type)
{
    const std::string normalized = collapseWhitespace(type);
    return normalized == "unsigned char" || normalized == "std::byte" || normalized == "std::uint8_t" || normalized == "uint8_t";
}

bool isSingleObjectSize(const std::string& expression, const std::string& type)
{
    return std::regex_match(trim(expression), std::regex(R"(sizeof\s*\(\s*)" + escapeRegex(type) + R"(\s*\))"));
}

bool parseArraySizeExpression(const std::string& expression, const std::string& type, std::string& sizeExpression)
{
    const std::string escapedType = escapeRegex(type);
    std::smatch match;
    const std::string trimmed = trim(expression);
    const std::regex leftCountPattern(R"((.+?)\s*\*\s*sizeof\s*\(\s*)" + escapedType + R"(\s*\))");
    if (std::regex_match(trimmed, match, leftCountPattern)) {
        sizeExpression = trim(match[1].str());
        return !sizeExpression.empty();
    }
    const std::regex rightCountPattern(R"(sizeof\s*\(\s*)" + escapedType + R"(\s*\)\s*\*\s*(.+))");
    if (std::regex_match(trimmed, match, rightCountPattern)) {
        sizeExpression = trim(match[1].str());
        return !sizeExpression.empty();
    }
    return false;
}

bool hasMatchingFree(const std::string& code, const std::string& variable)
{
    const std::string escaped = escapeRegex(variable);
    return std::regex_search(code, std::regex(R"(\b(?:std::)?free\s*\(\s*)" + escaped + R"(\s*\))"));
}

bool hasReallocForVariable(const std::string& code, const std::string& variable)
{
    const std::string escaped = escapeRegex(variable);
    return std::regex_search(code, std::regex(R"(\b(?:std::)?realloc\s*\(\s*)" + escaped + R"(\s*,)"))
        || std::regex_search(code, std::regex(R"(\b)" + escaped + R"(\s*=\s*(?:\([^)]*\)\s*)?(?:std::)?realloc\s*\()"));
}

bool hasEscapingUse(const std::string& code, const std::string& variable)
{
    const std::string escaped = escapeRegex(variable);
    if (std::regex_search(code, std::regex(R"(\breturn\s+)" + escaped + R"(\s*;)"))) {
        return true;
    }
    if (std::regex_search(code, std::regex(R"(\b[A-Za-z_]\w*\s*=\s*)" + escaped + R"(\s*;)"))) {
        return true;
    }

    const std::regex callPattern(R"(\b([A-Za-z_:][A-Za-z0-9_:]*)\s*\([^;\n]*\b)" + escaped + R"(\b[^;\n]*\))");
    for (std::sregex_iterator it(code.begin(), code.end(), callPattern), end; it != end; ++it) {
        const std::string function = (*it)[1].str();
        if (function != "free"
            && function != "std::free"
            && function != "sizeof"
            && function != "if"
            && function != "while"
            && function != "for"
            && function != "switch") {
            return true;
        }
    }

    return false;
}

std::vector<ConversionCandidate> collectCandidates(const std::string& code,
                                                   std::vector<ConversionChange>& changes)
{
    std::vector<ConversionCandidate> candidates;
    const std::vector<std::string> lines = splitLines(code);

    const std::regex pointerDeclarationWithInitializer(
        R"(^[ \t]*([A-Za-z_:][A-Za-z0-9_:]*(?:\s+[A-Za-z_:][A-Za-z0-9_:]*)*)\s*\*\s*([A-Za-z_]\w*)\s*=\s*(.+?)\s*;\s*$)");
    const std::regex pointerMemberDeclaration(
        R"(^[ \t]*([A-Za-z_:][A-Za-z0-9_:]*(?:\s+[A-Za-z_:][A-Za-z0-9_:]*)*)\s*\*\s*([A-Za-z_]\w*)\s*;\s*$)");

    for (const std::string& line : lines) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;
        if (!std::regex_match(codePart, match, pointerDeclarationWithInitializer)) {
            continue;
        }

        const std::string type = collapseWhitespace(match[1].str());
        const std::string variable = match[2].str();
        const std::string initializer = stripPointerAllocationCast(match[3].str(), type);
        std::smatch allocationMatch;
        const std::regex mallocPattern(mallocCallPattern() + R"(\s*\(\s*(.+)\s*\))");
        const std::regex callocPattern(callocCallPattern() + R"(\s*\(\s*([^,]+)\s*,\s*sizeof\s*\(\s*)" + escapeRegex(type) + R"(\s*\)\s*\))");

        ConversionCandidate candidate;
        candidate.variable = variable;
        candidate.elementType = type;
        candidate.declarationOwnsAllocation = true;
        if (std::regex_match(initializer, allocationMatch, mallocPattern)) {
            const std::string sizeExpression = trim(allocationMatch[1].str());
            std::string arraySize;
            if (isSingleObjectSize(sizeExpression, type)) {
                candidate.kind = ConversionCandidate::Kind::SingleObject;
            } else if (parseArraySizeExpression(sizeExpression, type, arraySize)) {
                candidate.kind = ConversionCandidate::Kind::DynamicArray;
                candidate.sizeExpression = arraySize;
            } else if (isByteType(type)) {
                candidate.kind = ConversionCandidate::Kind::DynamicArray;
                candidate.sizeExpression = sizeExpression;
            } else {
                addSuggestion(changes,
                              "malloc/free ownership modernization",
                              trim(codePart),
                              "malloc size did not match sizeof(T) or count * sizeof(T), so ownership conversion was preserved for review.");
                continue;
            }
        } else if (std::regex_match(initializer, allocationMatch, callocPattern)) {
            candidate.kind = ConversionCandidate::Kind::DynamicArray;
            candidate.sizeExpression = trim(allocationMatch[1].str());
        } else {
            continue;
        }

        candidates.push_back(std::move(candidate));
    }

    for (const std::string& line : lines) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch declarationMatch;
        if (!std::regex_match(codePart, declarationMatch, pointerMemberDeclaration)) {
            continue;
        }
        const std::string type = collapseWhitespace(declarationMatch[1].str());
        const std::string variable = declarationMatch[2].str();
        if (std::any_of(candidates.begin(), candidates.end(), [&variable](const ConversionCandidate& candidate) {
                return candidate.variable == variable;
            })) {
            continue;
        }

        const std::string escapedVariable = escapeRegex(variable);
        std::smatch allocationMatch;
        const std::regex assignmentPattern(R"(\b)" + escapedVariable + R"(\s*=\s*(.+?)\s*;)");

        ConversionCandidate candidate;
        candidate.variable = variable;
        candidate.elementType = type;
        candidate.classMember = true;
        if (std::regex_search(code, allocationMatch, assignmentPattern)) {
            const std::string initializer = stripPointerAllocationCast(allocationMatch[1].str(), type);
            std::smatch initializerMatch;
            const std::regex mallocPattern(mallocCallPattern() + R"(\s*\(\s*(.+?)\s*\))");
            const std::regex callocPattern(callocCallPattern() + R"(\s*\(\s*([^,]+)\s*,\s*sizeof\s*\(\s*)" + escapeRegex(type) + R"(\s*\)\s*\))");
            if (!std::regex_match(initializer, initializerMatch, mallocPattern)
                && !std::regex_match(initializer, initializerMatch, callocPattern)) {
                continue;
            }

            if (initializer.find("calloc") != std::string::npos) {
                candidate.kind = ConversionCandidate::Kind::DynamicArray;
                candidate.sizeExpression = trim(initializerMatch[1].str());
                candidates.push_back(std::move(candidate));
                continue;
            }

            const std::string sizeExpression = trim(initializerMatch[1].str());
            std::string arraySize;
            if (isSingleObjectSize(sizeExpression, type)) {
                candidate.kind = ConversionCandidate::Kind::SingleObject;
            } else if (parseArraySizeExpression(sizeExpression, type, arraySize)) {
                candidate.kind = ConversionCandidate::Kind::DynamicArray;
                candidate.sizeExpression = arraySize;
            } else if (isByteType(type)) {
                candidate.kind = ConversionCandidate::Kind::DynamicArray;
                candidate.sizeExpression = sizeExpression;
            } else {
                continue;
            }
        } else {
            continue;
        }
        candidates.push_back(std::move(candidate));
    }

    return candidates;
}

std::string removeFreeCleanup(std::string code,
                              const ConversionCandidate& candidate,
                              std::vector<ConversionChange>& changes,
                              bool& changed)
{
    const std::string escaped = escapeRegex(candidate.variable);
    const std::string pointerCondition = "(?:" + escaped + "|" + escaped + R"(\s*!=\s*(?:nullptr|NULL|0))" + ")";
    const std::regex singleLineIfPattern(
        R"([ \t]*if\s*\(\s*)" + pointerCondition + R"(\s*\)\s*\{\s*(?:std::)?free\s*\(\s*)"
        + escaped + R"(\s*\)\s*;\s*(?:)" + escaped + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*)?\}\s*\n?)");
    std::smatch match;
    while (std::regex_search(code, match, singleLineIfPattern)) {
        addAppliedChange(changes,
                         "Remove free after RAII modernization",
                         trim(match[0].str()),
                         "removed",
                         "Removed a free-only cleanup block after malloc ownership became RAII.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "");
        changed = true;
    }

    const std::regex multilineIfPattern(R"([ \t]*if\s*\(\s*)" + pointerCondition
        + R"(\s*\)\s*\n[ \t]*\{\s*\n[ \t]*(?:std::)?free\s*\(\s*)"
        + escaped + R"(\s*\)\s*;\s*\n(?:[ \t]*)?(?:)" + escaped
        + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*\n)?[ \t]*\}\s*\n?)",
        std::regex::ECMAScript | std::regex::multiline);
    while (std::regex_search(code, match, multilineIfPattern)) {
        addAppliedChange(changes,
                         "Remove free after RAII modernization",
                         trim(match[0].str()),
                         "removed",
                         "Removed a multi-line free-only cleanup block after malloc ownership became RAII.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "");
        changed = true;
    }

    const SafeReplacementEngine safeReplacement;
    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::regex freeLinePattern(R"(^[ \t]*(?:std::)?free\s*\(\s*)" + escaped + R"(\s*\)\s*;\s*$)");
        if (std::regex_match(codePart, freeLinePattern)) {
            addAppliedChange(changes,
                             "Remove free after RAII modernization",
                             trim(codePart),
                             "removed",
                             "Removed manual free because the storage is now owned by a standard RAII type.");
            changed = true;
            return trailingComment.empty() ? std::string{} : trailingComment;
        }

        const std::regex nullAssignmentPattern(R"(^[ \t]*)" + escaped + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*$)");
        if (std::regex_match(codePart, nullAssignmentPattern)) {
            addAppliedChange(changes,
                             "Remove free after RAII modernization",
                             trim(codePart),
                             "removed",
                             "Removed a stale null assignment after free cleanup was eliminated.");
            changed = true;
            return trailingComment.empty() ? std::string{} : trailingComment;
        }

        return line;
    });

    return code;
}

std::string rewriteCandidate(std::string code,
                             const ConversionCandidate& candidate,
                             TransformationContext& context,
                             std::vector<ConversionChange>& changes,
                             bool& changed)
{
    const std::string escapedVariable = escapeRegex(candidate.variable);
    const std::string escapedType = escapeRegex(candidate.elementType);
    const SafeReplacementEngine safeReplacement;

    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;

        const std::regex declarationMallocPattern(
            R"(^([ \t]*))" + escapedType + R"(\s*\*\s*)" + escapedVariable + R"(\s*=\s*.+?(?:malloc|calloc)\s*\(.+\)\s*;\s*$)");
        if (candidate.declarationOwnsAllocation && std::regex_match(codePart, match, declarationMallocPattern)) {
            std::string replacement;
            if (candidate.kind == ConversionCandidate::Kind::SingleObject) {
                replacement = match[1].str() + "auto " + candidate.variable + " = std::make_unique<" + candidate.elementType + ">();";
            } else {
                replacement = match[1].str() + "std::vector<" + candidate.elementType + "> "
                    + candidate.variable + "(" + candidate.sizeExpression + ");";
            }
            addAppliedChange(changes,
                             candidate.kind == ConversionCandidate::Kind::SingleObject
                                 ? "malloc/free ownership to std::unique_ptr"
                                 : "malloc/free array ownership to std::vector",
                             trim(codePart),
                             trim(replacement),
                             candidate.kind == ConversionCandidate::Kind::SingleObject
                                 ? "Converted a local malloc/free single-object owner to std::make_unique."
                                 : "Converted a local malloc/free array owner to std::vector storage.");
            changed = true;
            return replacement + trailingComment;
        }

        if (candidate.classMember) {
            const std::regex declarationPattern(R"(^([ \t]*))" + escapedType + R"(\s*\*\s*)" + escapedVariable + R"(\s*;\s*$)");
            if (std::regex_match(codePart, match, declarationPattern)) {
                const std::string replacement = candidate.kind == ConversionCandidate::Kind::SingleObject
                    ? match[1].str() + "std::unique_ptr<" + candidate.elementType + "> " + candidate.variable + ";"
                    : match[1].str() + "std::vector<" + candidate.elementType + "> " + candidate.variable + ";";
                addAppliedChange(changes,
                                 candidate.kind == ConversionCandidate::Kind::SingleObject
                                     ? "malloc/free member ownership to std::unique_ptr"
                                     : "malloc/free member array ownership to std::vector",
                                 trim(codePart),
                                 trim(replacement),
                                 "Converted a malloc/free-owned member declaration to a standard RAII owner.");
                changed = true;
                return replacement + trailingComment;
            }

            const std::regex assignmentPattern(R"(^([ \t]*))" + escapedVariable + R"(\s*=\s*.+?(?:malloc|calloc)\s*\(.+\)\s*;\s*$)");
            if (std::regex_match(codePart, match, assignmentPattern)) {
                const std::string replacement = candidate.kind == ConversionCandidate::Kind::SingleObject
                    ? match[1].str() + candidate.variable + " = std::make_unique<" + candidate.elementType + ">();"
                    : match[1].str() + candidate.variable + ".resize(" + candidate.sizeExpression + ");";
                addAppliedChange(changes,
                                 candidate.kind == ConversionCandidate::Kind::SingleObject
                                     ? "malloc allocation to std::make_unique"
                                     : "malloc allocation to vector resize",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced malloc/calloc member allocation with construction of the converted RAII member.");
                changed = true;
                return replacement + trailingComment;
            }
        }

        return line;
    });

    code = removeFreeCleanup(std::move(code), candidate, changes, changed);

    if (changed) {
        context.registerTypeChange(TypeChangeRecord{
            candidate.variable,
            candidate.elementType + "*",
            candidate.kind == ConversionCandidate::Kind::SingleObject
                ? "std::unique_ptr<" + candidate.elementType + ">"
                : "std::vector<" + candidate.elementType + ">",
            {},
            candidate.classMember,
            candidate.kind == ConversionCandidate::Kind::SingleObject
                ? "malloc/free ownership to std::unique_ptr"
                : "malloc/free array ownership to std::vector",
            {"remove free cleanup", "validate no malloc/free ownership leftovers"},
            {},
            false,
        });
    }

    return code;
}
} // namespace

std::string MallocFreeModernizationPass::rewrite(const std::string& code,
                                                 const ModernizationOptions& options,
                                                 TransformationContext& context,
                                                 std::vector<ConversionChange>& changes) const
{
    if (code.find("malloc") == std::string::npos
        && code.find("calloc") == std::string::npos
        && code.find("realloc") == std::string::npos) {
        return code;
    }

    if (!options.applySafeOwnershipModernization) {
        addSuggestion(changes,
                      "malloc/free ownership modernization",
                      "malloc/free",
                      "Safe ownership modernization is disabled, so malloc/free ownership was preserved.");
        return code;
    }

    std::string updated = code;
    bool changed = false;
    const std::vector<ConversionCandidate> candidates = collectCandidates(code, changes);

    for (const ConversionCandidate& candidate : candidates) {
        const std::regex convertedDeclarationPattern(
            R"((?:std::unique_ptr|std::vector)\s*<[^;\n]+>\s+)" + escapeRegex(candidate.variable) + R"(\b)");
        if (std::regex_search(updated, convertedDeclarationPattern)) {
            continue;
        }

        const std::string beforeCandidate = updated;
        if (!hasMatchingFree(beforeCandidate, candidate.variable)) {
            addSuggestion(changes,
                          "malloc/free ownership modernization",
                          candidate.variable,
                          "malloc/calloc allocation did not have a matching free, so ownership was not changed automatically.");
            continue;
        }
        if (hasReallocForVariable(beforeCandidate, candidate.variable)) {
            addSuggestion(changes,
                          "malloc/free ownership modernization",
                          candidate.variable,
                          "realloc changes allocation identity and capacity semantics, so this malloc ownership was preserved for manual review.");
            continue;
        }
        if (hasEscapingUse(beforeCandidate, candidate.variable)) {
            addSuggestion(changes,
                          "malloc/free ownership modernization",
                          candidate.variable,
                          "The malloc-owned pointer appears to escape or is passed to an unknown API, so the converter preserved C allocation ownership.");
            continue;
        }

        bool candidateChanged = false;
        updated = rewriteCandidate(std::move(updated), candidate, context, changes, candidateChanged);

        const std::regex leftoverFreePattern(R"(\b(?:std::)?free\s*\(\s*)" + escapeRegex(candidate.variable) + R"(\s*\))");
        const std::regex leftoverMallocAssignmentPattern(
            R"(\b)" + escapeRegex(candidate.variable) + R"(\s*=\s*(?:\([^)]*\)\s*)?(?:std::)?(?:malloc|calloc)\s*\()");
        if (candidateChanged
            && (std::regex_search(updated, leftoverFreePattern)
                || std::regex_search(updated, leftoverMallocAssignmentPattern))) {
            addSuggestion(changes,
                          "malloc/free ownership validation",
                          candidate.variable,
                          "A malloc/free ownership conversion left incompatible C allocation operations behind, so the candidate was rolled back.");
            updated = beforeCandidate;
            candidateChanged = false;
        }

        changed = changed || candidateChanged;
    }

    if (!changed) {
        return code;
    }

    const IncludeManager includeManager;
    bool needsMemory = updated.find("std::unique_ptr<") != std::string::npos
        || updated.find("std::make_unique<") != std::string::npos;
    bool needsVector = updated.find("std::vector<") != std::string::npos;
    if (needsMemory) {
        updated = includeManager.ensureInclude(std::move(updated), "#include <memory>");
    }
    if (needsVector) {
        updated = includeManager.ensureInclude(std::move(updated), "#include <vector>");
    }
    updated = includeManager.removeIncludeIfUnused(std::move(updated),
                                                   "#include <cstdlib>",
                                                   {"malloc(", "calloc(", "realloc(", "free(", "std::malloc", "std::calloc", "std::realloc", "std::free"});
    return updated;
}
