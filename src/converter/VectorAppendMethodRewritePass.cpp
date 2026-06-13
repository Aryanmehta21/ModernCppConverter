#include "converter/VectorAppendMethodRewritePass.h"

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

bool hasReserveForSymbol(const std::string& code, const std::string& targetExpression)
{
    return std::regex_search(code, std::regex(targetExpression + R"(\.reserve\s*\()"));
}

bool hasResizeForSymbol(const std::string& code, const std::string& targetExpression)
{
    return std::regex_search(code, std::regex(targetExpression + R"(\.resize\s*\()"));
}
} // namespace

std::string VectorAppendMethodRewritePass::rewrite(const std::string& code,
                                                   const TransformationContext& context,
                                                   std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SafeReplacementEngine safeReplacement;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorRecord(record)) {
            continue;
        }

        const std::string targetExpression = accessExpressionRegex(record.symbolName);
        bool changed = false;

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex appendAssignmentPattern("^([ \\t]*)(" + targetExpression
                                                        + ")\\s*\\[\\s*([A-Za-z_]\\w*)\\s*\\]\\s*=\\s*([^;]+)\\s*;\\s*$");
            if (std::regex_match(codePart, match, appendAssignmentPattern)) {
                const std::string indexName = match[3].str();
                const std::regex incrementPattern("(?:\\+\\+" + escapeRegex(indexName) + "|" + escapeRegex(indexName) + "\\+\\+)");
                if (std::regex_search(updated, incrementPattern)) {
                    const std::string replacement = match[1].str() + record.symbolName + ".push_back(" + trim(match[4].str()) + ");";
                    changed = true;
                    addAppliedChange(changes,
                                     "Vector append method rewrite",
                                     trim(codePart),
                                     trim(replacement),
                                     "Rewrote append-style indexed vector assignment to push_back so std::vector owns growth safely.");
                    addAppliedChange(changes,
                                     "Indexed append to vector push_back",
                                     trim(codePart),
                                     trim(replacement),
                                     "Converted an append-style indexed write into std::vector::push_back().");
                    return replacement + trailingComment;
                }
            }

            const std::regex postIncrementAppendPattern("^([ \\t]*)(" + targetExpression
                                                          + ")\\s*\\[\\s*([A-Za-z_]\\w*)\\s*\\+\\+\\s*\\]\\s*=\\s*([^;]+)\\s*;\\s*$");
            if (std::regex_match(codePart, match, postIncrementAppendPattern)) {
                const std::string replacement = match[1].str() + record.symbolName + ".push_back(" + trim(match[4].str()) + ");";
                changed = true;
                addAppliedChange(changes,
                                 "Vector append method rewrite",
                                 trim(codePart),
                                 trim(replacement),
                                 "Rewrote post-increment indexed vector append to push_back so std::vector owns growth safely.");
                addAppliedChange(changes,
                                 "Indexed append to vector push_back",
                                 trim(codePart),
                                 trim(replacement),
                                 "Converted post-increment append-style indexed write into std::vector::push_back().");
                return replacement + trailingComment;
            }

            return line;
        });

        const bool hasReserve = hasReserveForSymbol(updated, targetExpression);
        const bool hasResize = hasResizeForSymbol(updated, targetExpression);
        const std::regex indexedWritePattern(targetExpression + R"(\s*\[[^\]\n]+\](?:\s*\.\s*[A-Za-z_]\w*)?\s*=)");
        if (hasReserve && !hasResize && std::regex_search(updated, indexedWritePattern)) {
            const std::string before = record.symbolName + ".reserve(...) with indexed writes";
            updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
                std::string trailingComment;
                std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
                if (codePart.find(record.symbolName) != std::string::npos) {
                    const std::string reserveCall = ".reserve(";
                    const std::size_t reservePosition = codePart.find(reserveCall);
                    if (reservePosition != std::string::npos) {
                        codePart.replace(reservePosition, reserveCall.size(), ".resize(");
                    }
                }
                return codePart + trailingComment;
            });
            changed = true;
            addAppliedChange(changes,
                             "Reserve vs resize safety fix",
                             before,
                             record.symbolName + ".resize(...)",
                             "Converted reserve() to resize() because leftover fixed-index writes require constructed vector elements.");
        }

        if (std::regex_search(updated, indexedWritePattern) && hasReserveForSymbol(updated, targetExpression) && !hasResizeForSymbol(updated, targetExpression)) {
            addSuggestion(changes,
                          "Invalid leftover pattern scanner",
                          record.symbolName,
                          "A converted std::vector still has indexed writes after reserve-only initialization. Manual review is required if the append rewrite could not prove safety.");
        }

        if (changed) {
            addAppliedChange(changes,
                             "Vector append method rewrite",
                             record.symbolName,
                             "append/index usage normalized",
                             "Scanned methods using converted vector storage and removed append-style indexed writes that could exceed reserved capacity.");
        }
    }

    return updated;
}
