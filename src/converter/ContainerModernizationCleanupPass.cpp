#include "converter/ContainerModernizationCleanupPass.h"

#include "converter/DependentUsageRewritePass.h"
#include "converter/IncludeManager.h"
#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"
#include "converter/ValueTypePointerOperationScanner.h"
#include "converter/VectorParadigmRewritePass.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
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

std::string accessExpressionRegex(const std::string& symbolName)
{
    return R"((?:(?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)(?:[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?)*(?:\.|->))?)"
        + escapeRegex(symbolName)
        + R"(\b)";
}

std::string vectorElementType(const std::string& vectorType)
{
    const std::string prefix = "std::vector<";
    if (!vectorType.starts_with(prefix) || vectorType.back() != '>') {
        return {};
    }
    return trim(vectorType.substr(prefix.size(), vectorType.size() - prefix.size() - 1));
}

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

std::string lowercase(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
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

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBracePosition)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBracePosition; index < code.size(); ++index) {
        const char character = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (character == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

std::set<std::string> collectGrowthSymbols(const std::string& code,
                                           const std::string& targetExpression,
                                           const std::string& elementType)
{
    std::set<std::string> symbols;
    const std::string escapedElementType = escapeRegex(elementType);

    const std::regex tempAllocationPattern(
        R"(\b(?:auto\s*\*|(?:const\s+)?)" + escapedElementType + R"(\s*\*)\s*([A-Za-z_]\w*)\s*=\s*new\s+)"
        + escapedElementType + R"(\s*\[\s*([^\]]+)\s*\])");
    for (std::sregex_iterator it(code.begin(), code.end(), tempAllocationPattern), end; it != end; ++it) {
        symbols.insert((*it)[1].str());
        const std::string sizeExpression = trim((*it)[2].str());
        if (std::regex_match(sizeExpression, std::regex(R"([A-Za-z_]\w*)"))) {
            symbols.insert(sizeExpression);
        }
    }

    const std::regex copyLoopPattern(R"(\b([A-Za-z_]\w*)\s*\[[^\]\n]+\]\s*=\s*)"
                                     + targetExpression
                                     + R"(\s*\[[^\]\n]+\]\s*;)");
    for (std::sregex_iterator it(code.begin(), code.end(), copyLoopPattern), end; it != end; ++it) {
        symbols.insert((*it)[1].str());
    }

    const std::regex assignmentPattern(targetExpression + R"(\s*=\s*([A-Za-z_]\w*)\s*;)");
    for (std::sregex_iterator it(code.begin(), code.end(), assignmentPattern), end; it != end; ++it) {
        symbols.insert((*it)[1].str());
    }

    const std::regex capacityTemporaryPattern(R"(\b(?:auto|int|long|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*[A-Za-z_]\w*\s*(?:\*|\+)\s*[^;]+;)");
    for (std::sregex_iterator it(code.begin(), code.end(), capacityTemporaryPattern), end; it != end; ++it) {
        symbols.insert((*it)[1].str());
    }

    return symbols;
}

bool bodyLooksLikeGrowthBlock(const std::string& body,
                              const std::string& targetExpression,
                              const std::set<std::string>& growthSymbols)
{
    if (growthSymbols.empty()) {
        return false;
    }

    bool mentionsGrowthSymbol = false;
    for (const std::string& symbol : growthSymbols) {
        if (!symbol.empty() && std::regex_search(body, std::regex(R"(\b)" + escapeRegex(symbol) + R"(\b)"))) {
            mentionsGrowthSymbol = true;
            break;
        }
    }
    if (!mentionsGrowthSymbol) {
        return false;
    }

    const bool allocatesTemp = std::regex_search(body, std::regex(R"(\b[A-Za-z_:][A-Za-z0-9_:<>]*\s*\*\s*[A-Za-z_]\w*\s*=\s*new\s+[A-Za-z_:][A-Za-z0-9_:<>]*\s*\[)"));
    const bool copiesVector = std::regex_search(body, std::regex(R"(\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
    const bool assignsToVector = std::regex_search(body, std::regex(targetExpression + R"(\s*=\s*[A-Za-z_]\w*\s*;)"));
    const bool deletesVector = std::regex_search(body, std::regex(R"(delete\s*\[\s*\]\s*)" + targetExpression));
    const bool updatesCapacity = std::regex_search(body, std::regex(R"(\b[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\s*;)"));
    return (allocatesTemp || copiesVector) && (assignsToVector || deletesVector || updatesCapacity);
}

std::string removeManualGrowthBlocks(std::string code,
                                     const std::string& targetExpression,
                                     const std::set<std::string>& growthSymbols,
                                     std::vector<ConversionChange>& changes,
                                     bool& changed)
{
    const std::regex ifHeaderPattern(R"((^[ \t]*)if\s*\([^;\n]*\)\s*(?:\n\1)?\{)",
                                     std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, ifHeaderPattern)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string blockText = code.substr(position, closeBrace - position + 1);
        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        if (!bodyLooksLikeGrowthBlock(body, targetExpression, growthSymbols)) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         "Container modernization cleanup",
                         trim(blockText),
                         "removed",
                         "Removed a complete manual raw-array allocation/growth block after storage became std::vector.");
        addAppliedChange(changes,
                         "Remove manual vector growth copy loop",
                         "manual copy loop in growth block",
                         "removed",
                         "Removed a copy loop that only emulated std::vector reallocation.");
        code.replace(position, closeBrace - position + 1, "");
        changed = true;
        consumed = position;
        search = code.substr(consumed);
    }
    return code;
}

bool loopCopiesBetweenVectorAndGrowthSymbol(const std::string& body,
                                            const std::string& targetExpression,
                                            const std::set<std::string>& growthSymbols)
{
    for (const std::string& symbol : growthSymbols) {
        if (symbol.empty()) {
            continue;
        }
        const std::string escapedSymbol = escapeRegex(symbol);
        const bool tempReceivesVector = std::regex_search(
            body,
            std::regex(escapedSymbol + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
        const bool vectorReceivesTemp = std::regex_search(
            body,
            std::regex(targetExpression + R"(\s*\[[^\]]+\]\s*=\s*)" + escapedSymbol + R"(\s*\[[^\]]+\])"));
        if (tempReceivesVector || vectorReceivesTemp) {
            return true;
        }
    }
    return false;
}

bool isSimpleZeroBasedTraversalHeader(const std::string& blockText)
{
    return std::regex_search(blockText,
                             std::regex(R"(for\s*\(\s*(?:int|auto|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*[A-Za-z_]\w*\s*;\s*(?:\+\+\1|\1\+\+)\s*\))"));
}

std::string removeObsoleteGrowthLoops(std::string code,
                                      const std::string& targetExpression,
                                      const std::set<std::string>& growthSymbols,
                                      std::vector<ConversionChange>& changes,
                                      bool& changed)
{
    const std::regex forHeaderPattern(R"((^[ \t]*)for\s*\([^\n]*\)\s*(?:\n\1)?\{)",
                                      std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, forHeaderPattern)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string blockText = code.substr(position, closeBrace - position + 1);
        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        const bool emptyTraversalLoop = trim(body).empty() && isSimpleZeroBasedTraversalHeader(blockText);
        const bool obsoleteCopyLoop = loopCopiesBetweenVectorAndGrowthSymbol(body, targetExpression, growthSymbols);
        if (!emptyTraversalLoop && !obsoleteCopyLoop) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         obsoleteCopyLoop ? "Remove manual vector growth copy loop" : "Remove empty cleanup block",
                         trim(blockText),
                         "removed",
                         obsoleteCopyLoop
                             ? "Removed a loop whose only purpose was copying converted vector storage through obsolete raw growth storage."
                             : "Removed an empty traversal loop left after manual vector-growth cleanup.");
        code.replace(position, closeBrace - position + 1, "");
        changed = true;
        consumed = position;
        search = code.substr(consumed);
    }
    return code;
}

std::string removeGrowthSymbolLines(std::string code,
                                    const std::string& targetExpression,
                                    const std::string& elementType,
                                    const std::set<std::string>& growthSymbols,
                                    std::vector<ConversionChange>& changes,
                                    bool& changed)
{
    const SafeReplacementEngine safeReplacement;
    const std::string escapedElementType = escapeRegex(elementType);
    return safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::string stripped = trim(codePart);
        if (stripped.empty()) {
            return line;
        }

        for (const std::string& symbol : growthSymbols) {
            if (symbol.empty()) {
                continue;
            }
            const std::string escapedSymbol = escapeRegex(symbol);
            const std::vector<std::regex> obsoleteLinePatterns{
                std::regex(R"(^[ \t]*(?:auto|int|long|size_t|std::size_t)\s+)" + escapedSymbol + R"(\s*=\s*[^;]+;\s*$)"),
                std::regex(R"(^[ \t]*(?:auto\s*\*|(?:const\s+)?)" + escapedElementType + R"(\s*\*)\s*)" + escapedSymbol + R"(\s*=\s*new\s+)" + escapedElementType + R"(\s*\[[^\]]+\]\s*;\s*$)"),
                std::regex(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapedSymbol + R"(\s*;\s*$)"),
                std::regex(R"(^[ \t]*)" + targetExpression + R"(\s*=\s*)" + escapedSymbol + R"(\s*;\s*$)"),
                std::regex(R"(^[ \t]*[A-Za-z_]\w*\s*=\s*)" + escapedSymbol + R"(\s*;\s*$)"),
                std::regex(R"(^[ \t]*)" + escapedSymbol + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\]\s*;\s*$)"),
            };
            for (const std::regex& pattern : obsoleteLinePatterns) {
                if (std::regex_match(codePart, pattern)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Container modernization cleanup",
                                     stripped,
                                     "removed",
                                     "Removed a leftover manual-growth temporary statement after std::vector modernization.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }
        }

        return line;
    });
}

std::string removeEmptyBlocks(std::string code,
                              std::vector<ConversionChange>& changes,
                              bool& changed)
{
    const std::regex ifHeaderPattern(R"((^[ \t]*)if\s*\()",
                                     std::regex::ECMAScript | std::regex::multiline);
    std::smatch headerMatch;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, headerMatch, ifHeaderPattern)) {
        const std::size_t position = consumed + static_cast<std::size_t>(headerMatch.position());
        const std::size_t openBrace = code.find('{', position);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t semicolon = code.find(';', position);
        if (semicolon != std::string::npos && semicolon < openBrace) {
            consumed = semicolon + 1;
            search = code.substr(consumed);
            continue;
        }
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }
        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        if (!trim(body).empty()) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        const std::string blockText = code.substr(position, closeBrace - position + 1);
        changed = true;
        addAppliedChange(changes,
                         "Remove empty cleanup block",
                         trim(blockText),
                         "removed",
                         "Removed an empty block left after container modernization cleanup.");
        code.replace(position, closeBrace - position + 1, "\n");
        consumed = position;
        search = code.substr(consumed);
    }

    const std::vector<std::regex> emptyBlockPatterns{
        std::regex(R"(\n?[ \t]*if\s*\([^)\n]+\)\s*(?:\n[ \t]*)?\{\s*\}\s*\n?)",
                   std::regex::ECMAScript | std::regex::multiline),
        std::regex(R"(\n?[ \t]*if\s*\([^)\n]+\)\s*\n[ \t]*\{\s*\n[ \t]*\}\s*\n?)",
                   std::regex::ECMAScript | std::regex::multiline),
    };
    bool replaced = true;
    while (replaced) {
        replaced = false;
        for (const std::regex& emptyBlockPattern : emptyBlockPatterns) {
            std::smatch match;
            if (std::regex_search(code, match, emptyBlockPattern)) {
                changed = true;
                replaced = true;
                addAppliedChange(changes,
                                 "Remove empty cleanup block",
                                 trim(match[0].str()),
                                 "removed",
                                 "Removed an empty block left after container modernization cleanup.");
                code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
                break;
            }
        }
    }
    return code;
}

bool hasResidualGrowthFragments(const std::string& code,
                                const std::string& targetExpression,
                                const std::string& elementType)
{
    const std::string escapedElementType = escapeRegex(elementType);
    const std::vector<std::regex> patterns{
        std::regex(R"(\b(?:newCapacity|newCap)\b)"),
        std::regex(R"(\b(?:auto\s*\*|(?:const\s+)?)" + escapedElementType + R"(\s*\*)\s*[A-Za-z_]\w*\s*=\s*new\s+)" + escapedElementType + R"(\s*\[)"),
        std::regex(R"(\bnew\s+)" + escapedElementType + R"(\s*\[)"),
        std::regex(R"(delete\s*\[\s*\]\s*)" + targetExpression),
        std::regex(targetExpression + R"(\s*=\s*[A-Za-z_]\w*\s*;)"),
        std::regex(R"(for\s*\([^)]*\)\s*(?:\n[ \t]*)?\{\s*\})"),
    };
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::regex& pattern) {
        return std::regex_search(code, pattern);
    });
}

std::optional<std::string> findMemberInitializerExpression(const std::string& classText, const std::string& memberName)
{
    const std::string escapedMember = escapeRegex(memberName);
    const std::regex initializerPattern(R"((?:^|[:,])\s*)" + escapedMember + R"(\s*\(\s*([^()]*)\s*\))",
                                        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (std::regex_search(classText, match, initializerPattern)) {
        return trim(match[1].str());
    }
    return std::nullopt;
}

std::optional<std::string> findMemberAssignmentExpression(const std::string& classText, const std::string& memberName)
{
    const std::string escapedMember = escapeRegex(memberName);
    const std::regex assignmentPattern(R"(^[ \t]*)" + escapedMember + R"(\s*=\s*([^;]+)\s*;\s*$)",
                                       std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (std::regex_search(classText, match, assignmentPattern)) {
        return trim(match[1].str());
    }
    return std::nullopt;
}

std::string removeMemberInitializer(std::string classText, const std::string& memberName)
{
    const std::string escapedMember = escapeRegex(memberName);
    bool changed = true;
    while (changed) {
        const std::string before = classText;
        classText = std::regex_replace(classText,
                                       std::regex(R"(:\s*)" + escapedMember + R"(\s*\([^()]*\)\s*,\s*)"),
                                       ": ");
        classText = std::regex_replace(classText,
                                       std::regex(R"(,\s*)" + escapedMember + R"(\s*\([^()]*\))"),
                                       "");
        classText = std::regex_replace(classText,
                                       std::regex(R"(:\s*)" + escapedMember + R"(\s*\([^()]*\)\s*)"),
                                       "");
        changed = classText != before;
    }

    classText = std::regex_replace(classText,
                                   std::regex(R"(\n[ \t]*:\s*\n(?=[ \t]*\{))"),
                                   "\n");
    classText = std::regex_replace(classText,
                                   std::regex(R"([ \t]+:\s*(?=\{))"),
                                   " ");
    return classText;
}

std::string removeNumericMemberDeclaration(std::string classText, const std::string& memberName)
{
    const std::string escapedMember = escapeRegex(memberName);
    return std::regex_replace(classText,
                              std::regex(R"(^[ \t]*(?:int|long|size_t|std::size_t)\s+)" + escapedMember
                                             + R"(\s*(?:=\s*[^;]+)?;\s*\n?)",
                                         std::regex::ECMAScript | std::regex::multiline),
                              "");
}

std::string removeSimpleMemberAssignment(std::string classText, const std::string& memberName)
{
    const std::string escapedMember = escapeRegex(memberName);
    return std::regex_replace(classText,
                              std::regex(R"(^[ \t]*)" + escapedMember + R"(\s*=\s*[^;]+;\s*\n?)",
                                         std::regex::ECMAScript | std::regex::multiline),
                              "");
}

bool containsWord(const std::string& text, const std::string& word)
{
    return std::regex_search(text, std::regex(R"(\b)" + escapeRegex(word) + R"(\b)"));
}

std::vector<std::string> numericMemberNames(const std::string& classText)
{
    std::vector<std::string> names;
    const std::regex memberPattern(R"(^[ \t]*(?:int|long|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*(?:=\s*[^;]+)?;\s*$)",
                                   std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(classText.begin(), classText.end(), memberPattern), end; iterator != end; ++iterator) {
        names.push_back((*iterator)[1].str());
    }
    return names;
}

std::string rewriteVectorSizeGetters(std::string classText,
                                     const TypeChangeRecord& record,
                                     std::vector<ConversionChange>& changes,
                                     bool& changed)
{
    const std::string escapedSymbol = escapeRegex(record.symbolName);
    const std::regex getterPattern(
        R"((^[ \t]*)(?:constexpr\s+)?(?:int|long|size_t|std::size_t|auto)\s+([A-Za-z_]\w*)\s*\(\s*\)\s*(const\s*)?\{\s*return\s+)"
            + escapedSymbol
            + R"(\.size\s*\(\s*\)\s*;\s*\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, getterPattern)) {
        const std::string loweredName = lowercase(match[2].str());
        const bool countLike = loweredName.find("count") != std::string::npos
            || loweredName.find("size") != std::string::npos
            || loweredName.find("length") != std::string::npos;
        if (!countLike) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string replacement = match[1].str() + "std::size_t " + match[2].str()
            + "() const { return " + record.symbolName + ".size(); }";
        if (trim(match[0].str()) == trim(replacement)) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        classText.replace(consumed + static_cast<std::size_t>(match.position()),
                          static_cast<std::size_t>(match.length()),
                          replacement);
        addAppliedChange(changes,
                         "Post-vector count getter polish",
                         trim(match[0].str()),
                         trim(replacement),
                         "Changed a vector-backed count/size getter to std::size_t and preserved const correctness.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = classText.substr(consumed);
    }

    return classText;
}

std::string rewriteVectorIndexGetters(std::string classText,
                                      const TypeChangeRecord& record,
                                      const std::string& elementType,
                                      std::vector<ConversionChange>& changes,
                                      bool& changed)
{
    const std::string escapedElement = escapeRegex(elementType);
    const std::string escapedSymbol = escapeRegex(record.symbolName);
    const std::regex boundedGetterHeaderPattern(
        R"((^[ \t]*)(?:const\s+)?)" + escapedElement
            + R"(\s*\*\s*([A-Za-z_]\w*)\s*\(\s*(?:int|long|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*\)\s*(const\s*)?\{)",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch boundedMatch;
    std::string boundedSearch = classText;
    std::size_t boundedConsumed = 0;
    while (std::regex_search(boundedSearch, boundedMatch, boundedGetterHeaderPattern)) {
        const std::size_t position = boundedConsumed + static_cast<std::size_t>(boundedMatch.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(boundedMatch.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string body = classText.substr(openBrace + 1, closeBrace - openBrace - 1);
        const std::string indexName = boundedMatch[3].str();
        const bool hasBoundsCheck = std::regex_search(
            body,
            std::regex(escapeRegex(indexName) + R"(\s*<\s*)" + escapedSymbol + R"(\.size\s*\(\s*\))"));
        const bool returnsVectorElement = std::regex_search(
            body,
            std::regex(R"(return\s+&\s*)" + escapedSymbol
                       + R"(\s*\[\s*(?:static_cast\s*<\s*std::size_t\s*>\s*\(\s*)?)"
                       + escapeRegex(indexName)
                       + R"(\s*\)?\s*\]\s*;)"));
        const bool returnsNull = std::regex_search(body, std::regex(R"(return\s+nullptr\s*;)"));
        if (!hasBoundsCheck || !returnsVectorElement || !returnsNull) {
            boundedConsumed = closeBrace + 1;
            boundedSearch = classText.substr(boundedConsumed);
            continue;
        }

        const std::string indent = boundedMatch[1].str();
        const std::string replacement = indent + "const " + elementType + "* " + boundedMatch[2].str()
            + "(std::size_t " + indexName + ") const\n"
            + indent + "{\n"
            + indent + "    if (" + indexName + " < " + record.symbolName + ".size()) {\n"
            + indent + "        return &" + record.symbolName + "[" + indexName + "];\n"
            + indent + "    }\n"
            + indent + "    return nullptr;\n"
            + indent + "}";
        const std::string before = classText.substr(position, closeBrace - position + 1);
        if (trim(before) == trim(replacement)) {
            boundedConsumed = closeBrace + 1;
            boundedSearch = classText.substr(boundedConsumed);
            continue;
        }

        classText.replace(position, closeBrace - position + 1, replacement);
        addAppliedChange(changes,
                         "Post-vector bounds-safe accessor polish",
                         trim(before),
                         trim(replacement),
                         "Normalized an existing vector-backed bounds-checked accessor to std::size_t and clean const formatting.");
        changed = true;
        boundedConsumed = position + replacement.size();
        boundedSearch = classText.substr(boundedConsumed);
    }

    const std::regex getterPattern(
        R"((^[ \t]*)(?:const\s+)?)" + escapedElement
            + R"(\s*\*\s*([A-Za-z_]\w*)\s*\(\s*(?:int|long|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*\)\s*(const\s*)?\{([\s\S]*?)\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, getterPattern)) {
        const std::string body = match[5].str();
        const std::string indexName = match[3].str();
        const std::string returnPatternText = R"(return\s+&\s*)" + escapedSymbol
            + R"(\s*\[\s*(?:static_cast\s*<\s*std::size_t\s*>\s*\(\s*)?)"
            + escapeRegex(indexName)
            + R"(\s*\)?\s*\]\s*;)";
        const bool returnsVectorElement = std::regex_search(body, std::regex(returnPatternText));
        const bool hasComplexBody = std::regex_search(body, std::regex(R"(\b(?:delete|new|push_back|emplace_back|erase|insert)\b)"));
        const bool hasNestedBlock = body.find('{') != std::string::npos || body.find('}') != std::string::npos;
        if (!returnsVectorElement || hasComplexBody || hasNestedBlock) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string indent = match[1].str();
        const std::string replacement = indent + "const " + elementType + "* " + match[2].str()
            + "(std::size_t " + indexName + ") const\n"
            + indent + "{\n"
            + indent + "    if (" + indexName + " < " + record.symbolName + ".size()) {\n"
            + indent + "        return &" + record.symbolName + "[" + indexName + "];\n"
            + indent + "    }\n"
            + indent + "    return nullptr;\n"
            + indent + "}";
        classText.replace(consumed + static_cast<std::size_t>(match.position()),
                          static_cast<std::size_t>(match.length()),
                          replacement);
        addAppliedChange(changes,
                         "Post-vector bounds-safe accessor polish",
                         trim(match[0].str()),
                         trim(replacement),
                         "Rewrote a vector-backed index accessor to be const, std::size_t-based, and bounds-safe.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = classText.substr(consumed);
    }

    return classText;
}

std::string removeDefaultCopyConstructor(std::string classText,
                                         const std::string& className,
                                         std::vector<ConversionChange>& changes,
                                         bool& changed)
{
    const std::string escapedClass = escapeRegex(className);
    const std::regex copyPattern(R"(^[ \t]*)" + escapedClass + R"(\s*\(\s*const\s+)"
                                     + escapedClass + R"(\s*&\s*\)\s*=\s*default\s*;\s*\n?)",
                                 std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(classText, match, copyPattern)) {
        addAppliedChange(changes,
                         "Post-vector Rule of Zero copy constructor removal",
                         trim(match[0].str()),
                         "removed",
                         "Removed an explicitly defaulted copy constructor because std::vector copy semantics are already correct.");
        classText.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "");
        changed = true;
    }
    return classText;
}

std::string removeStaleCountMembers(std::string classText,
                                    const TypeChangeRecord& record,
                                    std::vector<ConversionChange>& changes,
                                    bool& changed)
{
    for (const std::string& memberName : numericMemberNames(classText)) {
        const std::string lowered = lowercase(memberName);
        const bool countLike = lowered == "count"
            || lowered == "used"
            || lowered.find("count") != std::string::npos;
        if (!countLike || memberName == record.symbolName) {
            continue;
        }

        std::string stripped = removeNumericMemberDeclaration(classText, memberName);
        stripped = removeMemberInitializer(std::move(stripped), memberName);
        stripped = std::regex_replace(stripped,
                                      std::regex(R"(^[ \t]*)" + escapeRegex(memberName) + R"(\s*=\s*0\s*;\s*\n?)",
                                                 std::regex::ECMAScript | std::regex::multiline),
                                      "");
        if (containsWord(stripped, memberName)) {
            continue;
        }

        const std::string before = classText;
        classText = removeNumericMemberDeclaration(std::move(classText), memberName);
        classText = removeMemberInitializer(std::move(classText), memberName);
        classText = std::regex_replace(classText,
                                       std::regex(R"(^[ \t]*)" + escapeRegex(memberName) + R"(\s*=\s*0\s*;\s*\n?)",
                                                  std::regex::ECMAScript | std::regex::multiline),
                                       "");
        if (classText != before) {
            addAppliedChange(changes,
                             "Post-vector stale count member removal",
                             memberName,
                             "removed",
                             "Removed a stale count member after all remaining count uses were replaced by std::vector::size().");
            changed = true;
        }
    }
    return classText;
}

bool hasLogicalCapacityUse(const std::string& classText, const std::string& capacityName, const std::string& vectorName)
{
    std::string stripped = std::regex_replace(classText,
                                              std::regex(escapeRegex(vectorName) + R"(\.reserve\s*\(\s*)"
                                                             + escapeRegex(capacityName) + R"(\s*\)\s*;?)"),
                                              "");
    stripped = removeMemberInitializer(std::move(stripped), capacityName);
    stripped = removeNumericMemberDeclaration(std::move(stripped), capacityName);
    stripped = removeSimpleMemberAssignment(std::move(stripped), capacityName);
    return containsWord(stripped, capacityName);
}

std::string removeStaleCapacityMembers(std::string classText,
                                       const TypeChangeRecord& record,
                                       std::vector<ConversionChange>& changes,
                                       bool& changed)
{
    for (const std::string& memberName : numericMemberNames(classText)) {
        const std::string escapedMember = escapeRegex(memberName);
        const std::regex reservePattern(escapeRegex(record.symbolName) + R"(\.reserve\s*\(\s*)"
                                            + escapedMember + R"(\s*\))");
        if (!std::regex_search(classText, reservePattern)) {
            continue;
        }

        if (hasLogicalCapacityUse(classText, memberName, record.symbolName)) {
            continue;
        }

        std::optional<std::string> expression = findMemberInitializerExpression(classText, memberName);
        if (!expression || expression->empty()) {
            expression = findMemberAssignmentExpression(classText, memberName);
        }
        if (!expression || expression->empty()) {
            continue;
        }

        const std::string before = classText;
        classText = std::regex_replace(classText,
                                       reservePattern,
                                       record.symbolName + ".reserve(" + *expression + ")");
        classText = removeSimpleMemberAssignment(std::move(classText), memberName);
        classText = removeMemberInitializer(std::move(classText), memberName);
        classText = removeNumericMemberDeclaration(std::move(classText), memberName);
        if (classText != before) {
            addAppliedChange(changes,
                             "Post-vector stale capacity member removal",
                             memberName,
                             "reserve(" + *expression + ")",
                             "Removed an allocation-capacity member that only fed std::vector::reserve().");
            changed = true;
        }
    }

    return classText;
}

std::string cleanupVectorClassFormatting(std::string classText)
{
    classText = std::regex_replace(classText,
                                   std::regex(R"((\n[ \t]*(?:explicit\s+)?(?:[A-Za-z_~]\w*|~[A-Za-z_]\w*)[^\n;{}]*\([^;\n{}]*\)(?:\s+const)?(?:\s+noexcept)?)[ \t]*\n[ \t]{8,}\{)"),
                                   "$1\n    {");
    classText = std::regex_replace(classText,
                                   std::regex(R"([ \t]+\n)"),
                                   "\n");
    classText = std::regex_replace(classText,
                                   std::regex(R"(\n{3,})"),
                                   "\n\n");
    classText = std::regex_replace(classText,
                                   std::regex(R"(\n[ \t]*:\s*\n(?=[ \t]*\{))"),
                                   "\n");
    classText = std::regex_replace(classText,
                                   std::regex(R"([ \t]+:\s*(?=\{))"),
                                   " ");
    return classText;
}

std::string polishVectorClasses(std::string code,
                                const TransformationContext& context,
                                std::vector<ConversionChange>& changes)
{
    const ClassResourceAnalyzer analyzer;
    const std::vector<ClassBlock> classes = analyzer.analyzeClasses(code);
    for (auto classIterator = classes.rbegin(); classIterator != classes.rend(); ++classIterator) {
        std::vector<TypeChangeRecord> classVectorRecords;
        for (const TypeChangeRecord& record : context.typeChanges()) {
            if (isVectorRecord(record)
                && record.isClassMember
                && record.scopeName == classIterator->name) {
                classVectorRecords.push_back(record);
            }
        }
        if (classVectorRecords.empty()) {
            continue;
        }

        std::string classText = code.substr(classIterator->start, classIterator->end - classIterator->start);
        const std::string beforeClass = classText;
        bool changed = false;
        for (const TypeChangeRecord& record : classVectorRecords) {
            const std::string elementType = vectorElementType(record.newType);
            if (elementType.empty()) {
                continue;
            }
            classText = rewriteVectorSizeGetters(std::move(classText), record, changes, changed);
            classText = rewriteVectorIndexGetters(std::move(classText), record, elementType, changes, changed);
            classText = removeStaleCapacityMembers(std::move(classText), record, changes, changed);
            classText = removeStaleCountMembers(std::move(classText), record, changes, changed);
        }
        classText = removeDefaultCopyConstructor(std::move(classText), classIterator->name, changes, changed);
        classText = cleanupVectorClassFormatting(std::move(classText));
        if (classText != beforeClass) {
            code.replace(classIterator->start, classIterator->end - classIterator->start, classText);
            if (changed) {
                addAppliedChange(changes,
                                 "Post-vector cleanup polish",
                                 classIterator->name,
                                 "polished vector-backed class",
                                 "Removed stale count/capacity bookkeeping and cleaned vector-backed accessors after manual-growth cleanup.");
            }
        }
    }
    return code;
}

std::string finalVectorLeftoverSweep(std::string code,
                                     const TransformationContext& context,
                                     std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorRecord(record)) {
            continue;
        }

        const std::string elementType = vectorElementType(record.newType);
        if (elementType.empty()) {
            continue;
        }

        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        std::set<std::string> growthSymbols = collectGrowthSymbols(code, targetExpression, elementType);
        bool changed = false;

        code = removeManualGrowthBlocks(std::move(code), targetExpression, growthSymbols, changes, changed);
        growthSymbols = collectGrowthSymbols(code, targetExpression, elementType);
        code = removeObsoleteGrowthLoops(std::move(code), targetExpression, growthSymbols, changes, changed);
        growthSymbols = collectGrowthSymbols(code, targetExpression, elementType);
        code = removeGrowthSymbolLines(std::move(code), targetExpression, elementType, growthSymbols, changes, changed);
        code = removeEmptyBlocks(std::move(code), changes, changed);

        code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex vectorDeletePattern(R"(^[ \t]*delete\s*\[\s*\]\s*)" + targetExpression + R"(\s*;\s*$)");
            if (std::regex_match(codePart, vectorDeletePattern)) {
                changed = true;
                addAppliedChange(changes,
                                 "Remove delete array after vector modernization",
                                 trim(codePart),
                                 "removed",
                                 "Removed delete[] because the converted std::vector owns cleanup.");
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex vectorNewAssignment("^([ \\t]*)" + targetExpression + R"(\s*=\s*new\s+)"
                                                 + escapeRegex(elementType) + R"(\s*\[\s*([^\]]+)\s*\]\s*;\s*$)");
            if (std::regex_match(codePart, match, vectorNewAssignment)) {
                const std::string replacement = match[1].str() + record.symbolName + ".reserve(" + trim(match[2].str()) + ");";
                changed = true;
                addAppliedChange(changes,
                                 "Replace raw array allocation with vector reserve",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced a leftover new[] assignment to converted vector storage with reserve().");
                return replacement + trailingComment;
            }

            const std::regex vectorNullAssignment("^([ \\t]*)" + targetExpression + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*$)");
            if (std::regex_match(codePart, vectorNullAssignment)) {
                changed = true;
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(codePart),
                                 "removed",
                                 "Removed pointer-era null assignment for converted vector storage.");
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            return line;
        });

        const std::regex vectorNullComparison(targetExpression + R"(\s*(?:==|!=)\s*(?:nullptr|NULL|0))");
        if (std::regex_search(code, vectorNullComparison)) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "A converted std::vector still has a pointer-style null comparison. The cleanup pass could not prove a safe rewrite for that expression.");
        }

        const std::regex rawPointerAssignment(targetExpression + R"(\s*=\s*[A-Za-z_]\w*\s*;)");
        if (std::regex_search(code, rawPointerAssignment)) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "A converted std::vector still has a raw-symbol assignment. Manual review is required if this was not growth emulation.");
        }

        code = removeEmptyBlocks(std::move(code), changes, changed);
        if (hasResidualGrowthFragments(code, targetExpression, elementType)) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "Converted std::vector storage still has manual-growth fragments after cleanup; compile verification must fail or rollback this vector modernization.");
        }
        if (changed) {
            addAppliedChange(changes,
                             "Container modernization cleanup",
                             record.symbolName,
                             "std::vector-native cleanup complete",
                             "Performed a final consistency sweep so converted vector storage does not retain raw-array management fragments.");
        }
    }

    return code;
}
} // namespace

std::string ContainerModernizationCleanupPass::rewrite(const std::string& code,
                                                       const TransformationContext& context,
                                                       std::vector<ConversionChange>& changes) const
{
    if (context.empty()) {
        return code;
    }

    const std::string before = code;
    const DependentUsageRewritePass dependentUsageRewritePass;
    const VectorParadigmRewritePass vectorParadigmRewritePass;
    const OrphanedGrowthSymbolCleanupPass orphanedGrowthSymbolCleanupPass;
    const OrphanedTempBufferLoopCleanupPass orphanedTempBufferLoopCleanupPass;
    const ValueTypePointerOperationScanner valueTypePointerOperationScanner;

    std::string updated = dependentUsageRewritePass.rewrite(code, context, changes);
    updated = vectorParadigmRewritePass.rewrite(updated, context, changes);
    updated = orphanedGrowthSymbolCleanupPass.rewrite(updated, context, {}, changes);
    updated = orphanedTempBufferLoopCleanupPass.rewrite(updated, context, {}, changes);
    updated = valueTypePointerOperationScanner.rewrite(updated, context, changes);
    updated = finalVectorLeftoverSweep(std::move(updated), context, changes);
    updated = polishVectorClasses(std::move(updated), context, changes);
    if (updated.find("std::size_t") != std::string::npos) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <cstddef>");
    }

    if (updated != before) {
        addAppliedChange(changes,
                         "Container modernization cleanup",
                         "raw-array container fragments",
                         "std::vector-native container logic",
                         "Cascaded raw-array-to-vector cleanup across allocation, growth, delete, count, and orphan temporary fragments.");
    }

    return updated;
}
