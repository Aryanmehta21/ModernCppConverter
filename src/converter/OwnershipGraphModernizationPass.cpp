#include "converter/OwnershipGraphModernizationPass.h"

#include "converter/IncludeManager.h"
#include "converter/OwnershipGraphAnalyzer.h"
#include "converter/SafeReplacementEngine.h"

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

std::string argumentsForMakeUnique(const std::smatch& match, const int groupIndex)
{
    if (groupIndex < static_cast<int>(match.size()) && match[groupIndex].matched) {
        return trim(match[groupIndex].str());
    }
    return {};
}

std::string makeUniqueExpression(const std::string& elementType, const std::string& arguments)
{
    return "std::make_unique<" + elementType + ">(" + arguments + ")";
}

std::string removeNestedIndexDeleteLoops(std::string code,
                                         const std::string& storageName,
                                         std::vector<ConversionChange>& changes,
                                         bool& changed)
{
    const std::regex deleteLoopPattern(
        R"(\n?[ \t]*for\s*\([^\n;]+;[^\n;]+;[^\n\)]*\)\s*\n?[ \t]*\{\s*\n?[ \t]*delete\s+)"
            + escapeRegex(storageName)
            + R"(\s*\[[^\]]+\]\s*;\s*\n?[ \t]*\}\s*)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, deleteLoopPattern)) {
        addAppliedChange(changes,
                         "Nested delete loop elimination",
                         trim(match[0].str()),
                         "removed",
                         "Removed a nested delete loop because the collection now owns elements with smart pointers.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
        changed = true;
    }
    return code;
}

std::string removeRangeDeleteLoops(std::string code,
                                   const std::string& storageName,
                                   std::vector<ConversionChange>& changes,
                                   bool& changed)
{
    const std::regex rangeDeleteLoopPattern(
        R"(\n?[ \t]*for\s*\(\s*(?:auto|auto\s*\*|const\s+auto\s*\*|[A-Za-z_:][A-Za-z0-9_:<>,\s]*\s*\*)\s+([A-Za-z_]\w*)\s*:\s*)"
            + escapeRegex(storageName)
            + R"(\s*\)\s*\n?[ \t]*\{\s*\n?[ \t]*delete\s+\1\s*;\s*\n?[ \t]*\}\s*)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, rangeDeleteLoopPattern)) {
        addAppliedChange(changes,
                         "Nested delete loop elimination",
                         trim(match[0].str()),
                         "removed",
                         "Removed a range delete loop because the collection now stores owning smart pointers.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
        changed = true;
    }
    return code;
}

std::string removeOuterDeleteArray(std::string code,
                                   const std::string& storageName,
                                   std::vector<ConversionChange>& changes,
                                   bool& changed)
{
    const SafeReplacementEngine safeReplacement;
    return safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::regex deleteArrayPattern(R"(^[ \t]*delete\s*\[\s*\]\s*)"
                                            + escapeRegex(storageName)
                                            + R"(\s*;\s*$)");
        if (!std::regex_match(codePart, deleteArrayPattern)) {
            return line;
        }

        addAppliedChange(changes,
                         "Nested delete loop elimination",
                         trim(codePart),
                         "removed",
                         "Removed outer delete[] because the standard container now owns the collection storage.");
        changed = true;
        return trailingComment.empty() ? std::string{} : trailingComment;
    });
}

std::string modernizePointerToPointerCollection(std::string code,
                                                const OwnershipGraphNode& node,
                                                std::vector<ConversionChange>& changes,
                                                bool& changed)
{
    const SafeReplacementEngine safeReplacement;
    const std::string elementType = node.elementType;
    const std::string storage = node.storageName;
    const std::string escapedType = escapeRegex(elementType);
    const std::string escapedStorage = escapeRegex(storage);
    const std::string vectorType = "std::vector<std::unique_ptr<" + elementType + ">>";

    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;

        const std::regex localDeclarationWithAllocation(
            R"(^([ \t]*))" + escapedType + R"(\s*\*\s*\*\s*)"
                + escapedStorage
                + R"(\s*=\s*new\s+)" + escapedType + R"(\s*\*\s*\[\s*([^\]]+)\s*\]\s*;\s*$)");
        if (std::regex_match(codePart, match, localDeclarationWithAllocation)) {
            const std::string replacement = match[1].str() + vectorType + " " + storage + ";\n"
                + match[1].str() + storage + ".reserve(" + trim(match[2].str()) + ");";
            addAppliedChange(changes,
                             "Pointer-to-pointer ownership collection to std::vector<std::unique_ptr>",
                             trim(codePart),
                             trim(replacement),
                             "Converted a raw pointer-to-pointer allocation into vector-backed exclusive ownership.");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex declarationPattern(
            R"(^([ \t]*))" + escapedType + R"(\s*\*\s*\*\s*)"
                + escapedStorage + R"(\s*;\s*$)");
        if (std::regex_match(codePart, match, declarationPattern)) {
            const std::string replacement = match[1].str() + vectorType + " " + storage + ";";
            addAppliedChange(changes,
                             "Pointer-to-pointer ownership collection to std::vector<std::unique_ptr>",
                             trim(codePart),
                             trim(replacement),
                             "Converted pointer-to-pointer storage to a vector of unique_ptr owners.");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex outerAllocationPattern(
            R"(^([ \t]*))" + escapedStorage + R"(\s*=\s*new\s+)" + escapedType + R"(\s*\*\s*\[\s*([^\]]+)\s*\]\s*;\s*$)");
        if (std::regex_match(codePart, match, outerAllocationPattern)) {
            const std::string replacement = match[1].str() + storage + ".reserve(" + trim(match[2].str()) + ");";
            addAppliedChange(changes,
                             "Pointer-to-pointer outer allocation to vector reserve",
                             trim(codePart),
                             trim(replacement),
                             "Replaced outer pointer-array allocation with vector::reserve().");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex innerAllocationPattern(
            R"(^([ \t]*))" + escapedStorage + R"(\s*\[\s*[^\]]+\]\s*=\s*new\s+)"
                + escapedType + R"(\s*(?:\(([^;]*)\))?\s*;\s*$)");
        if (std::regex_match(codePart, match, innerAllocationPattern)) {
            const std::string replacement = match[1].str() + storage + ".push_back("
                + makeUniqueExpression(elementType, argumentsForMakeUnique(match, 2)) + ");";
            addAppliedChange(changes,
                             "Pointer-to-pointer element allocation to std::make_unique",
                             trim(codePart),
                             trim(replacement),
                             "Converted owned element allocation into vector push_back of std::make_unique().");
            changed = true;
            return replacement + trailingComment;
        }

        return line;
    });

    code = removeNestedIndexDeleteLoops(std::move(code), storage, changes, changed);
    code = removeOuterDeleteArray(std::move(code), storage, changes, changed);
    return code;
}

std::string modernizeFixedPointerArray(std::string code,
                                       const OwnershipGraphNode& node,
                                       std::vector<ConversionChange>& changes,
                                       bool& changed)
{
    const SafeReplacementEngine safeReplacement;
    const std::string elementType = node.elementType;
    const std::string storage = node.storageName;
    const std::string escapedType = escapeRegex(elementType);
    const std::string escapedStorage = escapeRegex(storage);
    const std::string arrayType = "std::array<std::unique_ptr<" + elementType + ">, " + node.sizeExpression + ">";

    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;

        const std::regex declarationPattern(
            R"(^([ \t]*))" + escapedType + R"(\s*\*\s*)"
                + escapedStorage + R"(\s*\[\s*)" + escapeRegex(node.sizeExpression) + R"(\s*\]\s*;\s*$)");
        if (std::regex_match(codePart, match, declarationPattern)) {
            const std::string replacement = match[1].str() + arrayType + " " + storage + ";";
            addAppliedChange(changes,
                             "Fixed pointer array ownership to std::array<std::unique_ptr>",
                             trim(codePart),
                             trim(replacement),
                             "Converted a fixed-size owning pointer array to std::array of std::unique_ptr.");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex innerAllocationPattern(
            R"(^([ \t]*))" + escapedStorage + R"(\s*\[\s*([^\]]+)\s*\]\s*=\s*new\s+)"
                + escapedType + R"(\s*(?:\(([^;]*)\))?\s*;\s*$)");
        if (std::regex_match(codePart, match, innerAllocationPattern)) {
            const std::string replacement = match[1].str() + storage + "[" + trim(match[2].str()) + "] = "
                + makeUniqueExpression(elementType, argumentsForMakeUnique(match, 3)) + ";";
            addAppliedChange(changes,
                             "Fixed pointer array element allocation to std::make_unique",
                             trim(codePart),
                             trim(replacement),
                             "Converted fixed array element allocation to std::make_unique().");
            changed = true;
            return replacement + trailingComment;
        }

        return line;
    });

    code = removeNestedIndexDeleteLoops(std::move(code), storage, changes, changed);
    return code;
}

std::string modernizeRawPointerVector(std::string code,
                                      const OwnershipGraphNode& node,
                                      std::vector<ConversionChange>& changes,
                                      bool& changed)
{
    const SafeReplacementEngine safeReplacement;
    const std::string elementType = node.elementType;
    const std::string storage = node.storageName;
    const std::string escapedType = escapeRegex(elementType);
    const std::string escapedStorage = escapeRegex(storage);
    const std::string vectorType = "std::vector<std::unique_ptr<" + elementType + ">>";

    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;

        const std::regex declarationPattern(
            R"(^([ \t]*)std::vector\s*<\s*)" + escapedType + R"(\s*\*\s*>\s+)"
                + escapedStorage + R"(\s*;\s*$)");
        if (std::regex_match(codePart, match, declarationPattern)) {
            const std::string replacement = match[1].str() + vectorType + " " + storage + ";";
            addAppliedChange(changes,
                             "Owning raw pointer container to std::vector<std::unique_ptr>",
                             trim(codePart),
                             trim(replacement),
                             "Converted a vector of owning raw pointers to vector<unique_ptr<T>>.");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex pushNewPattern(
            R"(^([ \t]*))" + escapedStorage + R"(\s*\.\s*(?:push_back|emplace_back)\s*\(\s*new\s+)"
                + escapedType + R"(\s*(?:\(([^;]*)\))?\s*\)\s*;\s*$)");
        if (std::regex_match(codePart, match, pushNewPattern)) {
            const std::string replacement = match[1].str() + storage + ".push_back("
                + makeUniqueExpression(elementType, argumentsForMakeUnique(match, 2)) + ");";
            addAppliedChange(changes,
                             "Owning raw pointer insertion to std::make_unique",
                             trim(codePart),
                             trim(replacement),
                             "Converted insertion of a newly allocated object into smart-pointer ownership.");
            changed = true;
            return replacement + trailingComment;
        }

        return line;
    });

    code = removeRangeDeleteLoops(std::move(code), storage, changes, changed);
    code = removeNestedIndexDeleteLoops(std::move(code), storage, changes, changed);
    return code;
}
} // namespace

std::string OwnershipGraphModernizationPass::modernize(const std::string& code,
                                                       const ModernizationOptions& options,
                                                       TransformationContext& context,
                                                       std::vector<ConversionChange>& changes) const
{
    if (options.offlineModernizationLevel == OfflineModernizationLevel::Conservative
        || !options.useSmartPointers
        || !options.applySafeOwnershipModernization) {
        return code;
    }

    const OwnershipGraphAnalyzer analyzer;
    const std::vector<OwnershipGraphNode> nodes = analyzer.analyze(code);
    const StringOwnershipPatternDetector stringDetector;
    for (const StringOwnershipOpportunity& opportunity : stringDetector.detect(code)) {
        addSuggestion(changes,
                      "String ownership pattern detector",
                      opportunity.className + "::" + opportunity.memberName,
                      opportunity.reason + " Consider internal std::string storage, Rule of Five review, and string_view for non-owning read-only parameters.");
    }

    std::string updated = code;
    bool changed = false;
    for (const OwnershipGraphNode& node : nodes) {
        const std::string beforeNode = updated;
        if (node.isPointerToPointer) {
            updated = modernizePointerToPointerCollection(std::move(updated), node, changes, changed);
            if (updated != beforeNode) {
                context.registerTypeChange(TypeChangeRecord{
                    node.storageName,
                    node.elementType + "**",
                    "std::vector<std::unique_ptr<" + node.elementType + ">>",
                    node.scopeName,
                    node.isClassMember,
                    "Pointer-to-pointer ownership collection to std::vector<std::unique_ptr>",
                    {"remove nested delete loops", "remove outer delete[]", "rewrite element allocation"},
                    {},
                    false,
                });
            }
        } else if (node.isFixedPointerArray) {
            updated = modernizeFixedPointerArray(std::move(updated), node, changes, changed);
            if (updated != beforeNode) {
                context.registerTypeChange(TypeChangeRecord{
                    node.storageName,
                    node.elementType + "*[" + node.sizeExpression + "]",
                    "std::array<std::unique_ptr<" + node.elementType + ">, " + node.sizeExpression + ">",
                    node.scopeName,
                    node.isClassMember,
                    "Fixed pointer array ownership to std::array<std::unique_ptr>",
                    {"remove nested delete loops", "rewrite element allocation"},
                    {},
                    false,
                });
            }
        } else if (node.isStdVectorRawPointer) {
            updated = modernizeRawPointerVector(std::move(updated), node, changes, changed);
            if (updated != beforeNode) {
                context.registerTypeChange(TypeChangeRecord{
                    node.storageName,
                    "std::vector<" + node.elementType + "*>",
                    "std::vector<std::unique_ptr<" + node.elementType + ">>",
                    node.scopeName,
                    node.isClassMember,
                    "Owning raw pointer container to std::vector<std::unique_ptr>",
                    {"rewrite new object insertion", "remove delete loops"},
                    {},
                    false,
                });
            }
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(std::move(updated), "#include <memory>");
        updated = includeManager.ensureInclude(std::move(updated), "#include <vector>");
        if (updated.find("std::array<") != std::string::npos) {
            updated = includeManager.ensureInclude(std::move(updated), "#include <array>");
        }
        addAppliedChange(changes,
                         "Ownership graph modernization",
                         "raw ownership graph",
                         "standard-library ownership graph",
                         "Modernized a complete allocation/storage/cleanup ownership graph instead of changing isolated syntax.");
    }

    return updated;
}
