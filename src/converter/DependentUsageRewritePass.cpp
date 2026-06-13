#include "converter/DependentUsageRewritePass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/ValueTypePointerOperationScanner.h"

#include <regex>
#include <sstream>
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
        if (std::string(R"(\.^$|()[]{}*+?)").find(character) != std::string::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string accessExpressionRegex(const std::string& symbolName)
{
    return R"((?:(?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)(?:[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?)*(?:\.|->))?)"
        + escapeRegex(symbolName)
        + R"(\b)";
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

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

bool isArrayRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::array<");
}

bool isStringRecord(const TypeChangeRecord& record)
{
    return record.newType == "std::string";
}

bool isValueTypeRecord(const TypeChangeRecord& record)
{
    return isVectorRecord(record) || isArrayRecord(record) || isStringRecord(record);
}

bool isUniquePtrRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::unique_ptr<");
}

bool isSharedPtrRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::shared_ptr<");
}

void addRuleOfZeroSpecialMemberRemoval(std::vector<ConversionChange>& changes,
                                       const std::string& before,
                                       const std::string& after,
                                       const std::string& reason)
{
    addAppliedChange(changes,
                     "Rule of Zero special member removal",
                     before,
                     after,
                     reason);
}

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBracePosition)
{
    int depth = 0;
    for (std::size_t index = openBracePosition; index < code.size(); ++index) {
        if (code[index] == '{') {
            ++depth;
        } else if (code[index] == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }

    return std::string::npos;
}

bool hasObviousBusinessLogic(const std::string& functionText)
{
    static const std::regex sideEffectPattern(R"(\b(?:std::cout|std::cerr|printf|fprintf|throw|open|close|lock|unlock|callback|telemetry|log)\b)");
    return std::regex_search(functionText, sideEffectPattern);
}

bool cleanupOnlyCopyConstructorBody(const std::string& functionText,
                                    const TypeChangeRecord& record,
                                    const std::string& parameterName)
{
    if (hasObviousBusinessLogic(functionText)) {
        return false;
    }

    const std::string symbol = escapeRegex(record.symbolName);
    const std::string parameter = parameterName.empty() ? R"([A-Za-z_]\w*)" : escapeRegex(parameterName);
    const std::regex manualCopyLoop(symbol + R"(\s*\[[^\]]+\]\s*=\s*)" + parameter + R"(\.)" + symbol + R"(\s*\[[^\]]+\])");
    const std::regex vectorResize(symbol + R"(\.resize\s*\()");
    const std::regex oldAllocation(symbol + R"(\s*=\s*new\s+)");
    const std::regex oldInitializerAllocation(symbol + R"(\s*\(\s*new\s+)");
    return std::regex_search(functionText, manualCopyLoop)
        && (std::regex_search(functionText, vectorResize)
            || std::regex_search(functionText, oldAllocation)
            || std::regex_search(functionText, oldInitializerAllocation));
}

bool cleanupOnlyCopyAssignmentBody(const std::string& functionText,
                                   const TypeChangeRecord& record,
                                   const std::string& parameterName)
{
    if (hasObviousBusinessLogic(functionText)) {
        return false;
    }

    const std::string symbol = escapeRegex(record.symbolName);
    const std::string parameter = parameterName.empty() ? R"([A-Za-z_]\w*)" : escapeRegex(parameterName);
    const std::regex manualCopyLoop(symbol + R"(\s*\[[^\]]+\]\s*=\s*)" + parameter + R"(\.)" + symbol + R"(\s*\[[^\]]+\])");
    const std::regex vectorResize(symbol + R"(\.resize\s*\()");
    const std::regex oldAllocation(symbol + R"(\s*=\s*new\s+)");
    const std::regex oldDelete(R"(delete\s*\[\s*\]\s*)" + symbol);
    return std::regex_search(functionText, manualCopyLoop)
        && (std::regex_search(functionText, vectorResize)
            || std::regex_search(functionText, oldAllocation)
            || std::regex_search(functionText, oldDelete));
}

std::string cleanupEmptyDestructor(std::string code,
                                   const TypeChangeRecord& record,
                                   std::vector<ConversionChange>& changes)
{
    if (!record.isClassMember || record.scopeName.empty()) {
        return code;
    }

    const std::string className = escapeRegex(record.scopeName);
    const std::regex emptyDestructorPattern(R"(\n?[ \t]*~)" + className + R"(\s*\(\s*\)\s*\{\s*\}\s*)");
    std::smatch match;
    if (!std::regex_search(code, match, emptyDestructorPattern)) {
        return code;
    }

    const std::string before = trim(match[0].str());
    addAppliedChange(changes,
                     "Rule of Zero destructor cleanup",
                     before,
                     "removed",
                     "Removed an empty cleanup-only destructor after standard library value-type modernization.");
    addAppliedChange(changes,
                     "Rule of Zero destructor removal",
                     before,
                     "removed",
                     "Removed an obsolete destructor whose cleanup is now handled by standard library members.");
    addRuleOfZeroSpecialMemberRemoval(changes,
                                      before,
                                      "removed",
                                      "The special member only existed for manual cleanup now handled by standard library members.");
    code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
    return code;
}

std::string cleanupOrphanBraceAfterDefaultedSpecialMember(std::string code,
                                                          const TypeChangeRecord& record,
                                                          std::vector<ConversionChange>& changes)
{
    if (!record.isClassMember || record.scopeName.empty()) {
        return code;
    }

    const std::regex orphanBracePattern(
        R"((=\s*default\s*;\s*\n)[ \t]*\}\s*\n(?=[ \t]+[A-Za-z_:][A-Za-z0-9_:<>,&*\s]*\s+[A-Za-z_]\w*\s*\())",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, orphanBracePattern)) {
        addAppliedChange(changes,
                         "Rule of Zero special member removal",
                         "orphaned special-member closing brace",
                         "removed",
                         "Removed a stale brace left after replacing a cleanup-only special member with a defaulted declaration.");
        code.replace(static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     match[1].str());
    }

    return code;
}

std::string cleanupVectorCopyOperations(std::string code,
                                        const TypeChangeRecord& record,
                                        std::vector<ConversionChange>& changes)
{
    if (!record.isClassMember || record.scopeName.empty()) {
        return code;
    }

    const std::string className = escapeRegex(record.scopeName);
    bool ruleOfZeroChanged = false;

    const std::regex copyConstructorSignature(
        "(^[ \\t]*)"
            + className
            + R"(\s*\(\s*const\s+)"
            + className
            + R"(\s*&\s*([A-Za-z_]\w*)?\s*\)\s*(?::[^{]*)?\{)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (std::regex_search(code, match, copyConstructorSignature)) {
        const std::size_t functionStart = static_cast<std::size_t>(match.position());
        const std::size_t openBrace = functionStart + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        const std::string parameterName = match[2].matched ? match[2].str() : std::string{};
        if (closeBrace != std::string::npos) {
            const std::string functionText = code.substr(functionStart, closeBrace - functionStart + 1);
            if (cleanupOnlyCopyConstructorBody(functionText, record, parameterName)) {
                const std::string replacement = match[1].str() + record.scopeName + "(const " + record.scopeName + "&) = default;";
                addAppliedChange(changes,
                                 "Remove obsolete copy constructor after vector modernization",
                                 trim(functionText),
                                 trim(replacement),
                                 "Manual array copy logic was removed because std::vector performs correct value copying.");
                addAppliedChange(changes,
                                 "Remove obsolete copy constructor after container modernization",
                                 trim(functionText),
                                 trim(replacement),
                                 "Manual resource-copy logic was removed because the converted standard container performs correct value copying.");
                addAppliedChange(changes,
                                 "Rule of Zero copy constructor removal",
                                 trim(functionText),
                                 trim(replacement),
                                 "Removed obsolete copy-constructor resource management now handled by standard library members.");
                addRuleOfZeroSpecialMemberRemoval(changes,
                                                  trim(functionText),
                                                  trim(replacement),
                                                  "The copy constructor only existed for manual resource copying now handled by standard library members.");
                code.replace(functionStart, closeBrace - functionStart + 1, replacement);
                ruleOfZeroChanged = true;
            }
        }
    }

    const std::regex copyAssignmentSignature(
        "(^[ \\t]*)"
            + className
            + R"(\s*&\s*operator=\s*\(\s*const\s+)"
            + className
            + R"(\s*&\s*([A-Za-z_]\w*)?\s*\)\s*\{)",
        std::regex::ECMAScript | std::regex::multiline);
    if (std::regex_search(code, match, copyAssignmentSignature)) {
        const std::size_t functionStart = static_cast<std::size_t>(match.position());
        const std::size_t openBrace = functionStart + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        const std::string parameterName = match[2].matched ? match[2].str() : std::string{};
        if (closeBrace != std::string::npos) {
            const std::string functionText = code.substr(functionStart, closeBrace - functionStart + 1);
            if (cleanupOnlyCopyAssignmentBody(functionText, record, parameterName)) {
                const std::string replacement = match[1].str() + record.scopeName + "& operator=(const " + record.scopeName + "&) = default;";
                addAppliedChange(changes,
                                 "Remove obsolete copy assignment after vector modernization",
                                 trim(functionText),
                                 trim(replacement),
                                 "Manual array assignment logic was removed because std::vector performs correct value assignment.");
                addAppliedChange(changes,
                                 "Remove obsolete assignment operator after container modernization",
                                 trim(functionText),
                                 trim(replacement),
                                 "Manual resource-assignment logic was removed because the converted standard container performs correct value assignment.");
                addAppliedChange(changes,
                                 "Rule of Zero assignment operator removal",
                                 trim(functionText),
                                 trim(replacement),
                                 "Removed obsolete assignment-operator resource management now handled by standard library members.");
                addRuleOfZeroSpecialMemberRemoval(changes,
                                                  trim(functionText),
                                                  trim(replacement),
                                                  "The assignment operator only existed for manual resource copying now handled by standard library members.");
                code.replace(functionStart, closeBrace - functionStart + 1, replacement);
                ruleOfZeroChanged = true;
            }
        }
    }

    const std::string beforeDestructorCleanup = code;
    code = cleanupEmptyDestructor(std::move(code), record, changes);
    code = cleanupOrphanBraceAfterDefaultedSpecialMember(std::move(code), record, changes);
    if (code != beforeDestructorCleanup) {
        addAppliedChange(changes,
                         "Remove obsolete destructor after vector modernization",
                         "~" + record.scopeName + "()",
                         "removed",
                         "Removed an empty destructor left after dependent vector cleanup.");
        ruleOfZeroChanged = true;
    }

    if (ruleOfZeroChanged) {
        addAppliedChange(changes,
                         "Rule of Zero cascade cleanup",
                         record.scopeName,
                         "standard-library resource members handle cleanup/copying",
                         "Removed or defaulted special member logic that only existed for resources now managed by standard library types.");
    }

    return code;
}
} // namespace

std::string DependentUsageRewritePass::rewrite(const std::string& code,
                                               const TransformationContext& context,
                                               std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    updated = rewriteValueTypePointerUsages(updated, context, changes);
    updated = rewriteVectorUsages(updated, context, changes);
    updated = rewriteStringUsages(updated, context, changes);
    updated = rewriteSmartPointerUsages(updated, context, changes);
    updated = runConsistencyChecks(updated, context, changes);
    return updated;
}

std::string DependentUsageRewritePass::rewriteValueTypePointerUsages(const std::string& code,
                                                                     const TransformationContext& context,
                                                                     std::vector<ConversionChange>& changes) const
{
    const ValueTypePointerOperationScanner scanner;
    return scanner.rewrite(code, context, changes);
}

std::string DependentUsageRewritePass::rewriteVectorUsages(const std::string& code,
                                                           const TransformationContext& context,
                                                           std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorRecord(record)) {
            continue;
        }

        const std::string symbol = escapeRegex(record.symbolName);
        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        bool changed = false;

        const std::regex commaInitializerPattern(R"(,\s*)" + symbol + R"(\s*\(\s*nullptr\s*\))");
        if (std::regex_search(updated, commaInitializerPattern)) {
            updated = std::regex_replace(updated, commaInitializerPattern, "");
            addAppliedChange(changes,
                             "Remove nullptr check after vector modernization",
                             record.symbolName + "(nullptr)",
                             "removed",
                             "Removed pointer-style nullptr member initialization after std::vector modernization.");
            changed = true;
        }

        const std::regex firstInitializerPattern(R"(:\s*)" + symbol + R"(\s*\(\s*nullptr\s*\)\s*,\s*)");
        if (std::regex_search(updated, firstInitializerPattern)) {
            updated = std::regex_replace(updated, firstInitializerPattern, ": ");
            addAppliedChange(changes,
                             "Remove nullptr check after vector modernization",
                             record.symbolName + "(nullptr)",
                             "removed",
                             "Removed pointer-style nullptr member initialization from the front of a constructor initializer list after std::vector modernization.");
            changed = true;
        }

        const std::regex soleInitializerPattern(R"(:\s*)" + symbol + R"(\s*\(\s*nullptr\s*\)\s*\n([ \t]*\{))");
        if (std::regex_search(updated, soleInitializerPattern)) {
            updated = std::regex_replace(updated, soleInitializerPattern, "\n$1");
            addAppliedChange(changes,
                             "Remove nullptr check after vector modernization",
                             record.symbolName + "(nullptr)",
                             "removed",
                             "Removed sole pointer-style nullptr member initialization after std::vector modernization.");
            changed = true;
        }

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string codePart;
            std::string trailingComment;
            codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::string rewritten = codePart;
            std::smatch match;

            const std::regex allocationPattern("^([ \\t]*)(" + targetExpression + ")\\s*=\\s*new\\s+(.+?)\\s*\\[\\s*([^\\]]+)\\s*\\]\\s*;\\s*$");
            if (std::regex_match(codePart, match, allocationPattern)) {
                rewritten = match[1].str() + trim(match[2].str()) + ".resize(" + trim(match[4].str()) + ");";
                addAppliedChange(changes,
                                 "Replace raw array allocation with vector resize",
                                 trim(codePart),
                                 trim(rewritten),
                                 "Updated a dependent new[] allocation after the symbol became std::vector.");
                changed = true;
            }

            const std::regex deleteArrayPattern("^[ \\t]*delete\\s*\\[\\s*\\]\\s*(" + targetExpression + ")\\s*;\\s*$");
            if (std::regex_match(rewritten, match, deleteArrayPattern)) {
                addAppliedChange(changes,
                                 "Remove delete array after vector modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed delete[] because std::vector owns and releases its storage.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex nullAssignmentPattern("^([ \\t]*)(" + targetExpression + ")\\s*=\\s*nullptr\\s*;\\s*$");
            if (std::regex_match(rewritten, match, nullAssignmentPattern)) {
                addAppliedChange(changes,
                                 "Remove nullptr check after vector modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed pointer null assignment because std::vector is a value object.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex nullCheckDeletePattern("^([ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*\\{\\s*delete\\s*\\[\\s*\\]\\s*\\2\\s*;\\s*\\}\\s*$");
            if (std::regex_match(rewritten, nullCheckDeletePattern)) {
                addAppliedChange(changes,
                                 "Remove delete array after vector modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed a null-check/delete[] cleanup block after std::vector modernization.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            return rewritten + trailingComment;
        });

        const std::regex deleteArrayBlockPattern("[ \\t]*if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*\\n[ \\t]*\\{\\s*\\n[ \\t]*delete\\s*\\[\\s*\\]\\s*\\1\\s*;\\s*\\n[ \\t]*\\}\\s*\\n?");
        if (std::regex_search(updated, deleteArrayBlockPattern)) {
            updated = std::regex_replace(updated, deleteArrayBlockPattern, "");
            addAppliedChange(changes,
                             "Remove delete array after vector modernization",
                             "if (" + record.symbolName + " != nullptr) { delete[] " + record.symbolName + "; }",
                             "removed",
                             "Removed multi-line pointer cleanup after std::vector modernization.");
            changed = true;
        }

        const std::regex emptyNullBlockPattern("[ \\t]*if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*\\n[ \\t]*\\{\\s*\\n[ \\t]*\\}\\s*\\n?");
        if (std::regex_search(updated, emptyNullBlockPattern)) {
            updated = std::regex_replace(updated, emptyNullBlockPattern, "");
            addAppliedChange(changes,
                             "Remove obsolete empty cleanup block",
                             "if (" + record.symbolName + " != nullptr) { }",
                             "removed",
                             "Removed an empty pointer null-check block left after vector cleanup.");
            addAppliedChange(changes,
                             "Remove empty cleanup block",
                             "if (" + record.symbolName + " != nullptr) { }",
                             "removed",
                             "Removed an empty pointer-style cleanup block after value-type modernization.");
            changed = true;
        }

        const std::regex guardingNullBlockPattern("(^[ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*\\n\\1\\{\\s*\\n([\\s\\S]*?)\\n\\1\\}",
                                                  std::regex::ECMAScript | std::regex::multiline);
        std::smatch guardMatch;
        if (std::regex_search(updated, guardMatch, guardingNullBlockPattern)) {
            updated.replace(static_cast<std::size_t>(guardMatch.position()),
                            static_cast<std::size_t>(guardMatch.length()),
                            guardMatch[3].str());
            addAppliedChange(changes,
                             "Remove nullptr check after vector modernization",
                             trim(guardMatch[0].str()),
                             trim(guardMatch[3].str()),
                             "Unwrapped a pointer null-check block because std::vector is always a valid value object after modernization.");
            changed = true;
        }

        if (changed) {
            const IncludeManager includeManager;
            updated = includeManager.ensureInclude(updated, "#include <vector>");
        }

        updated = cleanupVectorCopyOperations(updated, record, changes);
    }

    return updated;
}

std::string DependentUsageRewritePass::rewriteStringUsages(const std::string& code,
                                                           const TransformationContext& context,
                                                           std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isStringRecord(record)) {
            continue;
        }

        const std::string symbol = escapeRegex(record.symbolName);
        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        bool changed = false;

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            std::string rewritten = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex strncpyPattern("^([ \\t]*)(?:std::)?strncpy\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^,;]+?)\\s*,\\s*(?:sizeof\\s*\\(\\s*\\2\\s*\\)|[^;]+)\\s*\\)\\s*;\\s*$");
            if (std::regex_match(rewritten, match, strncpyPattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + " = " + trim(match[3].str()) + ";";
                addAppliedChange(changes,
                                 "Replace C-string copy with string assignment",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Updated strncpy after the destination became std::string.");
                rewritten = replacement;
                changed = true;
            }

            const std::regex strcpyPattern("^([ \\t]*)(?:std::)?strcpy\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^;]+?)\\s*\\)\\s*;\\s*$");
            if (std::regex_match(rewritten, match, strcpyPattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + " = " + trim(match[3].str()) + ";";
                addAppliedChange(changes,
                                 "Replace C-string copy with string assignment",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Updated strcpy after the destination became std::string.");
                rewritten = replacement;
                changed = true;
            }

            const std::regex nullTerminationPattern("^[ \\t]*(" + targetExpression + ")\\s*\\[[^;\\n]+\\]\\s*=\\s*'\\\\0'\\s*;\\s*$");
            if (std::regex_match(rewritten, match, nullTerminationPattern)) {
                addAppliedChange(changes,
                                 "Remove manual null termination after string modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed manual null termination because std::string manages its own terminator.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::string beforeComparisonRewrite = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex("(?:std::)?strcmp\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^)]+?)\\s*\\)\\s*==\\s*0"),
                                           "$1 == $2");
            rewritten = std::regex_replace(rewritten,
                                           std::regex("(?:std::)?strcmp\\s*\\(\\s*(" + targetExpression + ")\\s*,\\s*([^)]+?)\\s*\\)\\s*!=\\s*0"),
                                           "$1 != $2");
            if (rewritten != beforeComparisonRewrite) {
                addAppliedChange(changes,
                                 "Replace C-string comparison with string comparison",
                                 trim(beforeComparisonRewrite),
                                 trim(rewritten),
                                 "Replaced strcmp on a converted std::string with ordinary string comparison.");
                changed = true;
            }

            const std::string beforeLengthRewrite = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex("(?:std::)?strlen\\s*\\(\\s*(" + targetExpression + ")\\s*\\)"),
                                           "$1.size()");
            if (rewritten != beforeLengthRewrite) {
                addAppliedChange(changes,
                                 "Replace strlen with string size",
                                 trim(beforeLengthRewrite),
                                 trim(rewritten),
                                 "Replaced strlen on a converted std::string with size().");
                changed = true;
            }

            const std::string beforeSizeofRewrite = rewritten;
            rewritten = std::regex_replace(rewritten,
                                           std::regex("sizeof\\s*\\(\\s*(" + targetExpression + ")\\s*\\)"),
                                           "$1.size()");
            if (rewritten != beforeSizeofRewrite) {
                addAppliedChange(changes,
                                 "Remove invalid sizeof string buffer usage",
                                 trim(beforeSizeofRewrite),
                                 trim(rewritten),
                                 "Replaced sizeof on a converted std::string so buffer-capacity logic does not use sizeof(std::string).");
                changed = true;
            }

            return rewritten + trailingComment;
        });

        if (changed) {
            const IncludeManager includeManager;
            updated = includeManager.ensureInclude(updated, "#include <string>");
            updated = includeManager.removeIncludeIfUnused(updated,
                                                           "#include <cstring>",
                                                           {"std::strcpy", "std::strncpy", "std::strcmp", "std::strlen", "strcpy(", "strncpy(", "strcmp(", "strlen("});
        }
    }

    return updated;
}

std::string DependentUsageRewritePass::rewriteSmartPointerUsages(const std::string& code,
                                                                 const TransformationContext& context,
                                                                 std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isUniquePtrRecord(record) && !isSharedPtrRecord(record)) {
            continue;
        }

        const std::string symbol = escapeRegex(record.symbolName);
        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            std::string rewritten = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            const std::regex deletePattern(R"(^[ \t]*delete\s+)" + symbol + R"(\s*;\s*$)");
            if (std::regex_match(rewritten, deletePattern)) {
                addAppliedChange(changes,
                                 "Dependent smart pointer delete cleanup",
                                 trim(rewritten),
                                 "removed",
                                 "Removed manual delete because the symbol is now managed by a smart pointer.");
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            if (isUniquePtrRecord(record)) {
                rewritten = std::regex_replace(rewritten,
                                               std::regex("\\b" + symbol + R"(\s*=\s*nullptr\s*;)"),
                                               record.symbolName + ".reset();");
            }
            return rewritten + trailingComment;
        });
    }

    return updated;
}

std::string DependentUsageRewritePass::runConsistencyChecks(const std::string& code,
                                                            const TransformationContext& context,
                                                            std::vector<ConversionChange>& changes) const
{
    std::string updated = code;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        const std::string symbol = escapeRegex(record.symbolName);
        const std::string targetExpression = accessExpressionRegex(record.symbolName);

        if (isValueTypeRecord(record)) {
            const std::vector<std::pair<std::regex, std::string>> invalidPatterns{
                {std::regex(R"(delete(?:\s*\[\s*\])?\s*)" + targetExpression), "delete on converted standard value type"},
                {std::regex(targetExpression + R"(\s*(?:==|!=)\s*nullptr)"), "nullptr comparison on converted standard value type"},
                {std::regex(targetExpression + R"(\s*=\s*nullptr)"), "nullptr assignment on converted standard value type"},
            };
            for (const auto& [pattern, reason] : invalidPatterns) {
                if (std::regex_search(updated, pattern)) {
                    addSuggestion(changes,
                                  "Invalid leftover pattern scanner",
                                  record.symbolName,
                                  reason + ". Manual review may be required if the cleanup pass could not safely remove it.");
                }
            }
        }

        if (isVectorRecord(record)) {
            const std::regex invalidNewArray(targetExpression + R"(\s*=\s*new\s+)");
            if (std::regex_search(updated, invalidNewArray)) {
                addSuggestion(changes,
                              "Invalid leftover pattern scanner",
                              record.symbolName,
                              "new[] assigned to converted std::vector. Manual review may be required if the cleanup pass could not safely remove it.");
            }
        }

        if (isStringRecord(record)) {
            const std::vector<std::pair<std::regex, std::string>> invalidPatterns{
                {std::regex(R"((?:std::)?str(?:n?cpy|cmp|len)\s*\(\s*)" + targetExpression), "C-string API still targets converted std::string"},
                {std::regex(R"(sizeof\s*\(\s*)" + targetExpression + R"(\s*\))"), "sizeof used as buffer size on converted std::string"},
                {std::regex(targetExpression + R"(\s*\[[^;\n]+\]\s*=\s*'\\0')"), "manual null termination on converted std::string"},
            };
            for (const auto& [pattern, reason] : invalidPatterns) {
                if (std::regex_search(updated, pattern)) {
                    addSuggestion(changes,
                                  "Invalid leftover pattern scanner",
                                  record.symbolName,
                                  reason + ". Manual review may be required if the cleanup pass could not safely remove it.");
                }
            }
        }

        if (isUniquePtrRecord(record) && std::regex_search(updated, std::regex(R"(delete\s+)" + symbol))) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "delete remains for a converted std::unique_ptr symbol. Manual review may be required.");
        }
    }

    return updated;
}
