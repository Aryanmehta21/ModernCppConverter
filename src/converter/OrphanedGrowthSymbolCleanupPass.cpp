#include "converter/OrphanedGrowthSymbolCleanupPass.h"

#include "converter/SafeReplacementEngine.h"

#include <cctype>
#include <regex>
#include <set>
#include <sstream>
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

std::set<std::string> collectTempBufferSymbols(const std::string& code, const std::string& targetExpression)
{
    std::set<std::string> symbols;
    const std::regex copyLoopPattern(
        R"(for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*[A-Za-z_]\w*\s*;\s*(?:\+\+\1|\1\+\+)\s*\)\s*\n[ \t]*\{\s*\n[ \t]*([A-Za-z_]\w*)\s*\[\s*\1\s*\]\s*=\s*)"
        + targetExpression
        + R"(\s*\[\s*\1\s*\]\s*;\s*\n[ \t]*\})",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), copyLoopPattern), end; iterator != end; ++iterator) {
        symbols.insert((*iterator)[2].str());
    }
    return symbols;
}

std::set<std::string> collectCapacityTempsFromBody(const std::string& body)
{
    std::set<std::string> symbols;
    const std::regex capacityAssignmentPattern(R"(\b[A-Za-z_]\w*\s*=\s*([A-Za-z_]\w*)\s*;)");
    for (std::sregex_iterator iterator(body.begin(), body.end(), capacityAssignmentPattern), end; iterator != end; ++iterator) {
        symbols.insert((*iterator)[1].str());
    }
    return symbols;
}

std::set<std::string> collectUndeclaredIdentifiers(const std::string& compilerDiagnostics)
{
    std::set<std::string> symbols;
    const std::vector<std::regex> patterns{
        std::regex(R"(undeclared identifier '([A-Za-z_]\w*)')"),
        std::regex(R"('([A-Za-z_]\w*)' was not declared)"),
    };

    for (const std::regex& pattern : patterns) {
        for (std::sregex_iterator iterator(compilerDiagnostics.begin(), compilerDiagnostics.end(), pattern), end; iterator != end; ++iterator) {
            symbols.insert((*iterator)[1].str());
        }
    }

    return symbols;
}

bool bodyContainsManualGrowthForTemp(const std::string& body,
                                     const std::string& targetExpression,
                                     const std::string& tempName)
{
    const std::string escapedTemp = escapeRegex(tempName);
    const bool copiesIntoTemp = std::regex_search(body, std::regex(escapedTemp + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
    const bool assignsTempToVector = std::regex_search(body, std::regex(targetExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;)"));
    const bool deletesOldStorage = std::regex_search(body, std::regex(R"(delete\s*\[\s*\]\s*)" + targetExpression));
    const bool declaresTempBuffer = std::regex_search(body, std::regex(R"(\b[A-Za-z_]\w*\s*\*\s*)" + escapedTemp + R"(\s*=)"));
    const bool updatesCapacityFromTemporary = std::regex_search(body, std::regex(R"((^|\n)[ \t]*[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\s*;)"));
    return copiesIntoTemp && (assignsTempToVector || deletesOldStorage || declaresTempBuffer || updatesCapacityFromTemporary);
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
                         "Removed an empty block left after orphaned vector-growth cleanup.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
    }
    return code;
}

std::string lowercase(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}
} // namespace

std::string OrphanedGrowthSymbolCleanupPass::rewrite(const std::string& code,
                                                     const TransformationContext& context,
                                                     const std::string& compilerDiagnostics,
                                                     std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;
    const std::string loweredDiagnostics = lowercase(compilerDiagnostics);

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorRecord(record)) {
            continue;
        }

        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        std::set<std::string> tempBuffers = collectTempBufferSymbols(updated, targetExpression);
        const std::set<std::string> undeclaredSymbols = collectUndeclaredIdentifiers(compilerDiagnostics);
        std::set<std::string> orphanCapacityTemps;
        orphanCapacityTemps.insert(undeclaredSymbols.begin(), undeclaredSymbols.end());
        bool changed = false;

        if (tempBuffers.empty() && loweredDiagnostics.find("undeclared") == std::string::npos && loweredDiagnostics.find("not declared") == std::string::npos) {
            continue;
        }

        for (const std::string& tempName : tempBuffers) {
            const std::regex ifBlockPattern(R"((^[ \t]*)if\s*\([^;\n]*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
                                            std::regex::ECMAScript | std::regex::multiline);
            std::smatch blockMatch;
            std::string search = updated;
            std::size_t consumed = 0;
            while (std::regex_search(search, blockMatch, ifBlockPattern)) {
                const std::size_t position = consumed + static_cast<std::size_t>(blockMatch.position());
                const std::string body = blockMatch[2].str();
                if (!bodyContainsManualGrowthForTemp(body, targetExpression, tempName)) {
                    consumed += static_cast<std::size_t>(blockMatch.position() + blockMatch.length());
                    search = blockMatch.suffix().str();
                    continue;
                }

                const std::set<std::string> capacityTemps = collectCapacityTempsFromBody(body);
                orphanCapacityTemps.insert(capacityTemps.begin(), capacityTemps.end());
                addAppliedChange(changes,
                                 "Orphaned growth symbol cleanup",
                                 trim(blockMatch[0].str()),
                                 "removed",
                                 "Removed an obsolete manual vector-growth block that referenced temporary growth symbols after raw-array-to-vector modernization.");
                updated.replace(position, static_cast<std::size_t>(blockMatch.length()), "");
                changed = true;
                search = updated.substr(position);
                consumed = position;
            }
        }

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex selfAssignmentPattern(R"(^[ \t]*([A-Za-z_]\w*)\s*=\s*\1\s*;\s*$)");
            if (std::regex_match(codePart, selfAssignmentPattern)) {
                changed = true;
                addAppliedChange(changes,
                                 "Remove self-assignment after growth cleanup",
                                 trim(codePart),
                                 "removed",
                                 "Removed a self-assignment left by obsolete manual growth cleanup.");
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            for (const std::string& tempName : tempBuffers) {
                const std::string escapedTemp = escapeRegex(tempName);
                const std::regex copyLinePattern("^[ \\t]*" + escapedTemp + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\]\s*;\s*$)");
                const std::regex assignTempPattern("^[ \\t]*" + targetExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;\s*$)");
                const std::regex deleteTempPattern(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapedTemp + R"(\s*;\s*$)");
                if (std::regex_match(codePart, copyLinePattern)
                    || std::regex_match(codePart, assignTempPattern)
                    || std::regex_match(codePart, deleteTempPattern)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Orphaned growth symbol cleanup",
                                     trim(codePart),
                                     "removed",
                                     "Removed a leftover statement that referenced an obsolete temporary growth buffer.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }

            for (const std::string& capacityTemp : orphanCapacityTemps) {
                if (capacityTemp.empty()) {
                    continue;
                }
                const std::string escapedCapacityTemp = escapeRegex(capacityTemp);
                const std::regex capacityAssignmentPattern(R"(^[ \t]*[A-Za-z_]\w*\s*=\s*)" + escapedCapacityTemp + R"(\s*;\s*$)");
                const std::regex capacityDeclarationPattern(R"(^[ \t]*(?:auto|int|long|size_t|std::size_t)\s+)" + escapedCapacityTemp + R"(\s*=.+;\s*$)");
                if (std::regex_match(codePart, capacityAssignmentPattern)
                    || std::regex_match(codePart, capacityDeclarationPattern)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Orphaned growth symbol cleanup",
                                     trim(codePart),
                                     "removed",
                                     "Removed a temporary capacity calculation/update tied only to obsolete manual vector growth.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }

            return line;
        });

        updated = removeEmptyIfBlocks(std::move(updated), changes);

        if (changed) {
            addAppliedChange(changes,
                             "Orphaned growth symbol cleanup",
                             record.symbolName,
                             "obsolete growth symbols removed",
                             "Scanned converted vector storage for orphaned manual-growth temporary symbols before compile verification.");
        } else if (!compilerDiagnostics.empty() && loweredDiagnostics.find("undeclared") != std::string::npos) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "Compiler diagnostics mentioned undeclared identifiers, but no safe orphaned vector-growth block could be removed automatically.");
        }
    }

    return updated;
}
