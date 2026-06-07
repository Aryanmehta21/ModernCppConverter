#include "converter/OrphanedTempBufferLoopCleanupPass.h"

#include "converter/SafeReplacementEngine.h"

#include <regex>
#include <set>
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

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
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

std::set<std::string> collectUndeclaredIdentifiers(const std::string& compilerDiagnostics)
{
    std::set<std::string> symbols;
    const std::vector<std::regex> patterns{
        std::regex(R"(undeclared identifier '([A-Za-z_]\w*)')"),
        std::regex(R"('([A-Za-z_]\w*)' was not declared)"),
        std::regex("\"([A-Za-z_]\\w*)\" was not declared"),
    };

    for (const std::regex& pattern : patterns) {
        for (std::sregex_iterator iterator(compilerDiagnostics.begin(), compilerDiagnostics.end(), pattern), end; iterator != end; ++iterator) {
            symbols.insert((*iterator)[1].str());
        }
    }
    return symbols;
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

bool hasDeclaration(const std::string& code, const std::string& symbol)
{
    const std::string escapedSymbol = escapeRegex(symbol);
    const std::regex declarationPattern(
        R"(\b(?:auto|const\s+auto|[A-Za-z_:][A-Za-z0-9_:<>]*(?:\s*[*&])?)\s+)"
            + escapedSymbol
            + R"(\b)");
    return std::regex_search(code, declarationPattern);
}

std::set<std::string> collectCopyLoopTempCandidates(const std::string& code,
                                                    const std::string& targetExpression)
{
    std::set<std::string> symbols;
    const std::regex copyStatementPattern(
        R"(\b([A-Za-z_]\w*)\s*\[[^\]\n]+\]\s*=\s*)"
            + targetExpression
            + R"(\s*\[[^\]\n]+\]\s*;)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), copyStatementPattern), end; iterator != end; ++iterator) {
        const std::string candidate = (*iterator)[1].str();
        if (!hasDeclaration(code, candidate)) {
            symbols.insert(candidate);
        }
    }

    const std::regex assignmentPattern(targetExpression + R"(\s*=\s*([A-Za-z_]\w*)\s*;)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), assignmentPattern), end; iterator != end; ++iterator) {
        const std::string candidate = (*iterator)[1].str();
        if (!hasDeclaration(code, candidate)) {
            symbols.insert(candidate);
        }
    }

    return symbols;
}

bool loopCopiesBetweenTempAndVector(const std::string& body,
                                    const std::string& targetExpression,
                                    const std::string& tempName)
{
    const std::string escapedTemp = escapeRegex(tempName);
    const bool tempReceivesVector = std::regex_search(
        body,
        std::regex(escapedTemp + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
    const bool vectorReceivesTemp = std::regex_search(
        body,
        std::regex(targetExpression + R"(\s*\[[^\]]+\]\s*=\s*)" + escapedTemp + R"(\s*\[[^\]]+\])"));
    return tempReceivesVector || vectorReceivesTemp;
}

std::string removeOrphanCopyLoops(std::string code,
                                  const std::string& targetExpression,
                                  const std::set<std::string>& tempNames,
                                  std::vector<ConversionChange>& changes,
                                  bool& changed)
{
    if (tempNames.empty()) {
        return code;
    }

    const std::regex forHeaderPattern(R"((^[ \t]*)for\s*\([^\n]*\)\s*\n\1\{)",
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
        bool removeLoop = false;
        std::string matchedTemp;
        for (const std::string& tempName : tempNames) {
            if (loopCopiesBetweenTempAndVector(body, targetExpression, tempName)) {
                removeLoop = true;
                matchedTemp = tempName;
                break;
            }
        }

        if (!removeLoop) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         "Orphaned temp buffer loop cleanup",
                         trim(blockText),
                         "removed",
                         "Removed a complete manual growth copy loop that referenced an orphaned temporary buffer after std::vector modernization.");
        addAppliedChange(changes,
                         "Remove manual vector growth copy loop",
                         "copy loop involving " + matchedTemp,
                         "removed",
                         "Removed a loop whose only purpose was copying vector storage into obsolete raw growth storage.");
        code.replace(position, closeBrace - position + 1, "");
        changed = true;
        consumed = position;
        search = code.substr(consumed);
    }

    return code;
}

std::string removeEmptyIfBlocks(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex emptyIfPattern(R"(\n?[ \t]*if\s*\([^)\n]+\)\s*\n[ \t]*\{\s*\n[ \t]*\}\s*\n?)",
                                    std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, emptyIfPattern)) {
        addAppliedChange(changes,
                         "Remove empty cleanup block",
                         trim(match[0].str()),
                         "removed",
                         "Removed an empty block left after orphaned temp-buffer loop cleanup.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
    }
    return code;
}
} // namespace

std::string OrphanedTempBufferLoopCleanupPass::rewrite(const std::string& code,
                                                       const TransformationContext& context,
                                                       const std::string& compilerDiagnostics,
                                                       std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorRecord(record)) {
            continue;
        }

        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        std::set<std::string> orphanSymbols = collectUndeclaredIdentifiers(compilerDiagnostics);
        const std::set<std::string> inferredTempBuffers = collectCopyLoopTempCandidates(updated, targetExpression);
        orphanSymbols.insert(inferredTempBuffers.begin(), inferredTempBuffers.end());

        if (orphanSymbols.empty()) {
            continue;
        }

        bool changed = false;
        updated = removeOrphanCopyLoops(updated, targetExpression, orphanSymbols, changes, changed);

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);

            for (const std::string& symbol : orphanSymbols) {
                const std::string escapedSymbol = escapeRegex(symbol);
                const std::regex vectorAssignmentPattern("^[ \\t]*" + targetExpression + R"(\s*=\s*)" + escapedSymbol + R"(\s*;\s*$)");
                const std::regex deleteTempPattern(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapedSymbol + R"(\s*;\s*$)");
                const std::regex capacityAssignmentPattern(R"(^[ \t]*[A-Za-z_]\w*\s*=\s*)" + escapedSymbol + R"(\s*;\s*$)");
                const std::regex tempCopyLinePattern(R"(^[ \t]*)" + escapedSymbol + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\]\s*;\s*$)");
                if (std::regex_match(codePart, vectorAssignmentPattern)
                    || std::regex_match(codePart, deleteTempPattern)
                    || std::regex_match(codePart, capacityAssignmentPattern)
                    || std::regex_match(codePart, tempCopyLinePattern)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Orphaned temp buffer loop cleanup",
                                     trim(codePart),
                                     "removed",
                                     "Removed a leftover manual-growth statement that referenced an orphaned temporary buffer or capacity variable.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }

            return line;
        });

        updated = removeEmptyIfBlocks(std::move(updated), changes);

        if (changed) {
            addAppliedChange(changes,
                             "Orphaned temp buffer loop cleanup",
                             record.symbolName,
                             "orphaned temp-buffer growth loops removed",
                             "Mapped orphaned temporary growth symbols back to vector modernization and removed dependent copy-loop fallout before verification.");
        }
    }

    return updated;
}
