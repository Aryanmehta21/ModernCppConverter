#include "converter/ValueTypePointerOperationScanner.h"

#include "converter/SafeReplacementEngine.h"

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

bool isValueTypeRecord(const TypeChangeRecord& record)
{
    return record.newType == "std::string"
        || record.newType.starts_with("std::vector<")
        || record.newType.starts_with("std::array<")
        || record.newType.starts_with("std::optional<")
        || record.newType.starts_with("std::deque<")
        || record.newType.starts_with("std::list<")
        || record.newType.starts_with("std::map<")
        || record.newType.starts_with("std::set<")
        || record.newType.starts_with("std::unordered_map<")
        || record.newType.starts_with("std::unordered_set<");
}

bool isCleanupOnlyBody(const std::string& body, const std::string& targetExpression)
{
    std::stringstream bodyStream(body);
    std::string bodyLine;
    const std::regex cleanupDeleteLine("delete(?:\\s*\\[\\s*\\])?\\s+" + targetExpression + "\\s*;");
    const std::regex cleanupNullAssignmentLine("(" + targetExpression + ")\\s*=\\s*(?:nullptr|NULL|0)\\s*;");
    while (std::getline(bodyStream, bodyLine)) {
        const std::string stripped = trim(bodyLine);
        if (stripped.empty()
            || stripped == "{"
            || stripped == "}"
            || std::regex_match(stripped, cleanupDeleteLine)
            || std::regex_match(stripped, cleanupNullAssignmentLine)) {
            continue;
        }
        return false;
    }
    return true;
}

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;

    for (std::size_t index = openBrace; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if ((inString || inCharacter) && current == '\\') {
            escaped = true;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }

    return std::string::npos;
}

bool isCleanupOnlyDestructorBody(std::string body, const std::string& targetExpression)
{
    const std::regex cleanupIfBlock(R"(if\s*\(\s*)" + targetExpression
                                    + R"(\s*(?:==|!=)\s*(?:nullptr|NULL|0)\s*\)\s*\{\s*(?:delete(?:\s*\[\s*\])?\s+)"
                                    + targetExpression + R"(\s*;\s*)?(?:)"
                                    + targetExpression + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*)?\})",
                                    std::regex::ECMAScript);
    body = std::regex_replace(body, cleanupIfBlock, "");

    const std::regex cleanupSingleLineIf(R"(if\s*\(\s*)" + targetExpression
                                         + R"(\s*(?:==|!=)\s*(?:nullptr|NULL|0)\s*\)\s*(?:delete(?:\s*\[\s*\])?\s+)"
                                         + targetExpression + R"(\s*;|)"
                                         + targetExpression + R"(\s*=\s*(?:nullptr|NULL|0)\s*;))",
                                         std::regex::ECMAScript);
    body = std::regex_replace(body, cleanupSingleLineIf, "");

    const std::regex cleanupDeleteLine(R"((^|\n)[ \t]*delete(?:\s*\[\s*\])?\s+)" + targetExpression + R"(\s*;\s*)",
                                       std::regex::ECMAScript);
    body = std::regex_replace(body, cleanupDeleteLine, "\n");

    const std::regex cleanupNullAssignmentLine(R"((^|\n)[ \t]*)" + targetExpression
                                               + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*)",
                                               std::regex::ECMAScript);
    body = std::regex_replace(body, cleanupNullAssignmentLine, "\n");

    return trim(body).empty();
}

std::string cleanupEmptyDestructor(std::string code,
                                   const TypeChangeRecord& record,
                                   std::vector<ConversionChange>& changes)
{
    if (!record.isClassMember || record.scopeName.empty()) {
        return code;
    }

    const std::string className = escapeRegex(record.scopeName);
    const std::regex destructorPattern(R"((^|\n)[ \t]*~)" + className + R"(\s*\(\s*\)\s*\{)",
                                       std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_search(code, match, destructorPattern)) {
        return code;
    }

    const std::size_t destructorStart = static_cast<std::size_t>(match.position(0))
        + (match[1].matched && !match[1].str().empty() ? 1U : 0U);
    const std::size_t openBrace = static_cast<std::size_t>(match.position(0) + match.length(0) - 1U);
    const std::size_t closeBrace = findMatchingBrace(code, openBrace);
    if (closeBrace == std::string::npos) {
        return code;
    }

    const std::string body = code.substr(openBrace + 1U, closeBrace - openBrace - 1U);
    const std::string targetExpression = accessExpressionRegex(record.symbolName);
    if (!trim(body).empty() && !isCleanupOnlyDestructorBody(body, targetExpression)) {
        return code;
    }

    std::size_t destructorEnd = closeBrace + 1U;
    while (destructorEnd < code.size() && (code[destructorEnd] == ' ' || code[destructorEnd] == '\t' || code[destructorEnd] == '\r')) {
        ++destructorEnd;
    }
    if (destructorEnd < code.size() && code[destructorEnd] == '\n') {
        ++destructorEnd;
    }

    const std::string before = trim(code.substr(destructorStart, destructorEnd - destructorStart));
    addAppliedChange(changes,
                     "Rule of Zero destructor cleanup",
                     before,
                     "removed",
                     "Removed a cleanup-only destructor after standard library value-type modernization.");
    addAppliedChange(changes,
                     "Rule of Zero special member removal",
                     before,
                     "removed",
                     "The special member only existed for manual cleanup now handled by standard library members.");
    code.replace(destructorStart, destructorEnd - destructorStart, "");
    return code;
}
} // namespace

std::string ValueTypePointerOperationScanner::rewrite(const std::string& code,
                                                      const TransformationContext& context,
                                                      std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isValueTypeRecord(record)) {
            continue;
        }

        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        bool changed = false;

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            std::string rewritten = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex nullAssignmentPattern("^([ \\t]*)(" + targetExpression + ")\\s*=\\s*nullptr\\s*;\\s*$");
            if (std::regex_match(rewritten, match, nullAssignmentPattern)) {
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed pointer-style nullptr assignment because the symbol is now a standard value type.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex deletePattern("^[ \\t]*delete(?:\\s*\\[\\s*\\])?\\s*(" + targetExpression + ")\\s*;\\s*$");
            if (std::regex_match(rewritten, match, deletePattern)) {
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed manual delete because the symbol is now a standard value type.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex emptyIfPattern("^([ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*(?:==|!=)\\s*nullptr\\s*\\)\\s*\\{\\s*\\}\\s*$");
            if (std::regex_match(rewritten, match, emptyIfPattern)) {
                addAppliedChange(changes,
                                 "Remove empty cleanup block",
                                 trim(rewritten),
                                 "removed",
                                 "Removed an empty pointer-style cleanup block after value-type modernization.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex cleanupIfPattern("^([ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*\\{\\s*(?:delete(?:\\s*\\[\\s*\\])?\\s*\\2\\s*;|\\2\\s*=\\s*nullptr\\s*;)\\s*\\}\\s*$");
            if (std::regex_match(rewritten, match, cleanupIfPattern)) {
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(rewritten),
                                 "removed",
                                 "Removed pointer-style cleanup guarded by nullptr because the symbol is now a standard value type.");
                changed = true;
                return trailingComment.empty() ? std::string{} : trailingComment;
            }

            const std::regex nullEqualityStatementPattern("^([ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*==\\s*nullptr\\s*\\)\\s*(.+)$");
            if (std::regex_match(rewritten, match, nullEqualityStatementPattern)) {
                const std::string replacement = match[1].str() + "if (" + trim(match[2].str()) + ".empty()) " + trim(match[3].str());
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Replaced a pointer-style null check with an empty() value-state check after standard value-type modernization.");
                changed = true;
                return replacement + trailingComment;
            }

            const std::regex nonNullStatementPattern("^([ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*!=\\s*nullptr\\s*\\)\\s*(.+)$");
            if (std::regex_match(rewritten, match, nonNullStatementPattern)) {
                const std::string replacement = match[1].str() + trim(match[3].str());
                addAppliedChange(changes,
                                 "Remove invalid nullptr check after value-type modernization",
                                 trim(rewritten),
                                 trim(replacement),
                                 "Removed an invalid pointer-style non-null guard after value-type modernization while preserving the guarded statement.");
                changed = true;
                return replacement + trailingComment;
            }

            return rewritten + trailingComment;
        });

        const std::regex nullBlockPattern("(^[ \\t]*)if\\s*\\(\\s*(" + targetExpression + ")\\s*(==|!=)\\s*nullptr\\s*\\)\\s*\\n\\1\\{\\s*\\n([\\s\\S]*?)\\n\\1\\}",
                                          std::regex::ECMAScript | std::regex::multiline);
        std::smatch blockMatch;
        while (std::regex_search(updated, blockMatch, nullBlockPattern)) {
            const std::string indent = blockMatch[1].str();
            const std::string expression = trim(blockMatch[2].str());
            const std::string comparisonOperator = blockMatch[3].str();
            const std::string body = blockMatch[4].str();
            const bool cleanupOnly = isCleanupOnlyBody(body, targetExpression);

            std::string replacement;
            std::string ruleName = "Remove invalid nullptr check after value-type modernization";
            if (cleanupOnly) {
                replacement = {};
                ruleName = "Remove empty cleanup block";
            } else if (comparisonOperator == "!=") {
                replacement = body;
            } else {
                replacement = indent + "if (" + expression + ".empty())\n" + indent + "{\n" + body + "\n" + indent + "}";
            }

            addAppliedChange(changes,
                             ruleName,
                             trim(blockMatch[0].str()),
                             replacement.empty() ? "removed" : trim(replacement),
                             cleanupOnly
                                 ? "Removed pointer-style cleanup that became empty after value-type modernization."
                                 : "Removed an invalid pointer-style nullptr check after value-type modernization while preserving the guarded statements.");
            updated.replace(static_cast<std::size_t>(blockMatch.position()),
                            static_cast<std::size_t>(blockMatch.length()),
                            replacement);
            changed = true;
        }

        if (changed) {
            addAppliedChange(changes,
                             "Value-type pointer operation scanner",
                             record.symbolName,
                             "pointer-specific operations removed or rewritten",
                             "Scanned a symbol converted to a standard value type and removed invalid pointer-specific operations before verification.");
            updated = cleanupEmptyDestructor(std::move(updated), record, changes);
        }
    }

    return updated;
}
