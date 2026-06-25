#include "converter/VectorGrowthEmulationCleanupPass.h"

#include "converter/SafeReplacementEngine.h"

#include <regex>
#include <set>
#include <sstream>
#include <string>
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

std::string vectorElementType(const std::string& vectorType)
{
    const std::string prefix = "std::vector<";
    if (!vectorType.starts_with(prefix) || vectorType.back() != '>') {
        return {};
    }
    return trim(vectorType.substr(prefix.size(), vectorType.size() - prefix.size() - 1));
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
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

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

std::string ensureReserveAfterCapacityAssignment(const std::string& line,
                                                 const std::string& vectorName,
                                                 const std::set<std::string>& capacitySymbols,
                                                 bool& changed)
{
    std::string trailingComment;
    const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    std::smatch match;
    const std::regex assignmentPattern(R"(^([ \t]*)([A-Za-z_]\w*)\s*=\s*([^;]+)\s*;\s*$)");
    if (!std::regex_match(codePart, match, assignmentPattern)) {
        return line;
    }

    const std::string target = match[2].str();
    const std::string source = trim(match[3].str());
    if (!capacitySymbols.contains(target) && !capacitySymbols.contains(source)) {
        return line;
    }

    changed = true;
    return codePart + trailingComment + "\n" + match[1].str() + vectorName + ".reserve(" + target + ");";
}

std::string makeAppendObjectName(const std::string& code, const std::set<std::string>& usedNames)
{
    std::vector<std::string> candidates{"appendedItem", "modernizedItem"};
    for (int suffix = 2; suffix < 100; ++suffix) {
        candidates.push_back("appendedItem" + std::to_string(suffix));
    }

    for (const std::string& candidate : candidates) {
        if (!usedNames.contains(candidate)
            && !std::regex_search(code, std::regex(R"(\b)" + escapeRegex(candidate) + R"(\b)"))) {
            return candidate;
        }
    }
    return "appendedItem" + std::to_string(usedNames.size() + 1);
}

bool lineMentionsAny(const std::string& line, const std::set<std::string>& symbols)
{
    for (const std::string& symbol : symbols) {
        if (!symbol.empty() && line.find(symbol) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string sizeComparableExpression(const std::string& expression)
{
    const std::string value = trim(expression);
    if (value.starts_with("static_cast<")
        || value.find(".size()") != std::string::npos
        || std::regex_match(value, std::regex(R"(\d+[uUlL]*)"))) {
        return value;
    }
    return "static_cast<std::size_t>(" + value + ")";
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
                         "Removed an empty control block left after eliminating manual vector growth code.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
    }
    return code;
}

std::string removeDanglingGrowthIfShells(std::string code,
                                         const std::string& vectorName,
                                         std::vector<ConversionChange>& changes)
{
    const std::vector<std::string> lines = splitLines(code);
    std::vector<std::string> rewritten;
    rewritten.reserve(lines.size());
    const std::regex ifLinePattern(R"(^[ \t]*if\s*\(\s*)" + escapeRegex(vectorName) + R"(\.size\s*\(\s*\)\s*(?:>=|>|==).*\)\s*$)");
    const std::regex localAppendObjectPattern(R"(^[ \t]*[A-Za-z_:][A-Za-z0-9_:<>]*\s+[A-Za-z_]\w*\{\}\s*;\s*$)");
    const std::regex directPushPattern(R"(^[ \t]*)" + escapeRegex(vectorName) + R"(\.push_back\s*\()");

    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!std::regex_match(lines[index], ifLinePattern)) {
            rewritten.push_back(lines[index]);
            continue;
        }

        std::size_t braceLine = index + 1;
        while (braceLine < lines.size() && trim(lines[braceLine]).empty()) {
            ++braceLine;
        }
        if (braceLine >= lines.size() || trim(lines[braceLine]) != "{") {
            rewritten.push_back(lines[index]);
            continue;
        }

        std::size_t nextCodeLine = braceLine + 1;
        while (nextCodeLine < lines.size() && trim(lines[nextCodeLine]).empty()) {
            ++nextCodeLine;
        }
        if (nextCodeLine >= lines.size()
            || (!std::regex_match(lines[nextCodeLine], localAppendObjectPattern)
                && !std::regex_search(lines[nextCodeLine], directPushPattern))) {
            rewritten.push_back(lines[index]);
            continue;
        }

        addAppliedChange(changes,
                         "Remove empty cleanup block",
                         trim(lines[index]) + "\n{",
                         "removed",
                         "Removed an empty allocation-capacity growth condition left after std::vector append modernization.");
        index = braceLine;
    }

    return joinLines(rewritten);
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

bool bodyLooksLikeManualGrowthForTemp(const std::string& body,
                                      const std::string& targetExpression,
                                      const std::string& tempName,
                                      const std::string& elementType)
{
    const std::string escapedTemp = escapeRegex(tempName);
    const bool copiesIntoTemp = std::regex_search(body, std::regex(escapedTemp + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
    const bool allocatesTemp = std::regex_search(
        body,
        std::regex(escapeRegex(elementType) + R"(\s*\*\s*)" + escapedTemp + R"(\s*=\s*new\s+)" + escapeRegex(elementType) + R"(\s*\[)"));
    const bool assignsTempToVector = std::regex_search(body, std::regex(targetExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;)"));
    const bool deletesOldStorage = std::regex_search(body, std::regex(R"(delete\s*\[\s*\]\s*)" + targetExpression));
    const bool updatesCapacityFromTemporary = std::regex_search(
        body,
        std::regex(R"((^|\n)[ \t]*[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\s*;)"));
    const bool containsAppendToTarget = std::regex_search(
        body,
        std::regex(targetExpression + R"(\s*\[[^\]]+\](?:\s*\.\s*[A-Za-z_]\w*)?\s*=)"));
    return copiesIntoTemp
        && !containsAppendToTarget
        && (allocatesTemp || assignsTempToVector || deletesOldStorage || updatesCapacityFromTemporary);
}

std::string removeManualGrowthIfBlocks(std::string code,
                                       const std::string& targetExpression,
                                       const std::string& tempName,
                                       const std::string& elementType,
                                       std::vector<ConversionChange>& changes,
                                       bool& changed)
{
    const std::regex ifHeaderPattern(R"((^[ \t]*)if\s*\([^;\n]*\)\s*\n\1\{)",
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
        if (!bodyLooksLikeManualGrowthForTemp(body, targetExpression, tempName, elementType)) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         "Vector emulation elimination",
                         trim(blockText),
                         "removed",
                         "Removed an obsolete manual dynamic-array growth block as a whole because std::vector now owns allocation and growth.");
        addAppliedChange(changes,
                         "Remove manual vector growth copy loop",
                         "copy loop involving " + tempName,
                         "removed",
                         "Removed an element-copy loop that only emulated std::vector reallocation.");
        code.replace(position, closeBrace - position + 1, "");
        changed = true;
        consumed = position;
        search = code.substr(consumed);
    }

    return code;
}

bool isStandaloneCountIncrement(const std::string& codePart, const std::string& countName)
{
    const std::regex incrementPattern("^[ \\t]*(?:\\+\\+" + escapeRegex(countName) + "|" + escapeRegex(countName) + "\\+\\+)\\s*;\\s*$");
    return std::regex_match(codePart, incrementPattern);
}

bool containsUnsafeControlTransfer(const std::string& codePart)
{
    return std::regex_search(codePart, std::regex(R"(\b(?:return|break|continue|throw|goto)\b)"));
}

std::string replaceIndexedMemberAccessOutsideLiterals(const std::string& codePart,
                                                      const std::string& targetExpression,
                                                      const std::string& countName,
                                                      const std::string& objectName,
                                                      bool& replaced)
{
    const std::regex accessPattern("^" + targetExpression
                                   + R"(\s*\[\s*)" + escapeRegex(countName)
                                   + R"(\s*\]\s*\.)");
    std::string output;
    output.reserve(codePart.size());

    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = 0; index < codePart.size();) {
        const char current = codePart[index];
        if (escaped) {
            output.push_back(current);
            escaped = false;
            ++index;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            output.push_back(current);
            escaped = true;
            ++index;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            output.push_back(current);
            ++index;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            output.push_back(current);
            ++index;
            continue;
        }

        if (!inString && !inCharacter) {
            std::smatch match;
            const std::string suffix = codePart.substr(index);
            if (std::regex_search(suffix, match, accessPattern)) {
                output += objectName + ".";
                index += static_cast<std::size_t>(match.length());
                replaced = true;
                continue;
            }
        }

        output.push_back(current);
        ++index;
    }

    return output;
}

bool indexedAccessesAreOnlyForCount(const std::vector<std::string>& lines,
                                    std::size_t first,
                                    std::size_t last,
                                    const std::string& targetExpression,
                                    const std::string& countName)
{
    const std::regex indexedAccessPattern(targetExpression + R"(\s*\[\s*([^\]\n]+)\s*\])");
    for (std::size_t lineIndex = first; lineIndex <= last && lineIndex < lines.size(); ++lineIndex) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(lines[lineIndex], trailingComment);
        for (std::sregex_iterator iterator(codePart.begin(), codePart.end(), indexedAccessPattern), end; iterator != end; ++iterator) {
            if (trim((*iterator)[1].str()) != countName) {
                return false;
            }

            const std::size_t afterBracket = static_cast<std::size_t>(iterator->position() + iterator->length());
            std::size_t cursor = afterBracket;
            while (cursor < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[cursor])) != 0) {
                ++cursor;
            }
            if (cursor >= codePart.size() || codePart[cursor] != '.') {
                return false;
            }
        }
    }
    return true;
}

bool spanMutatesCountBeforeIncrement(const std::vector<std::string>& lines,
                                     std::size_t first,
                                     std::size_t incrementLine,
                                     const std::string& countName)
{
    const std::string escapedCount = escapeRegex(countName);
    const std::vector<std::regex> mutationPatterns{
        std::regex(R"(\b)" + escapedCount + R"(\s*(?:=|\+=|-=|\*=|/=|%=))"),
        std::regex(R"((?:\+\+|--)\s*)" + escapedCount + R"(\b)"),
        std::regex(R"(\b)" + escapedCount + R"(\s*(?:\+\+|--))"),
    };

    for (std::size_t lineIndex = first + 1; lineIndex < incrementLine && lineIndex < lines.size(); ++lineIndex) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(lines[lineIndex], trailingComment);
        if (containsUnsafeControlTransfer(codePart)) {
            return true;
        }
        for (const std::regex& pattern : mutationPatterns) {
            if (std::regex_search(codePart, pattern)) {
                return true;
            }
        }
    }
    return false;
}

std::string rewriteFieldAppendSequences(const std::string& code,
                                        const TypeChangeRecord& record,
                                        const std::string& elementType,
                                        const std::string& targetExpression,
                                        std::vector<ConversionChange>& changes,
                                        bool& changed)
{
    const std::vector<std::string> lines = splitLines(code);
    std::vector<std::string> rewrittenLines;
    rewrittenLines.reserve(lines.size());

    const std::regex fieldAssignmentPattern("^([ \\t]*)" + targetExpression
                                                + "\\s*\\[\\s*([A-Za-z_]\\w*)\\s*\\]\\s*\\.\\s*([A-Za-z_]\\w*)\\s*=\\s*([^;]+)\\s*;\\s*$");
    std::set<std::string> usedAppendObjectNames;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::smatch firstMatch;
        std::string firstTrailingComment;
        const std::string firstCodePart = SafeReplacementEngine::splitTrailingLineComment(lines[index], firstTrailingComment);
        if (!std::regex_match(firstCodePart, firstMatch, fieldAssignmentPattern)) {
            rewrittenLines.push_back(lines[index]);
            continue;
        }

        const std::string baseIndent = firstMatch[1].str();
        const std::string countName = firstMatch[2].str();
        const std::string objectName = makeAppendObjectName(code, usedAppendObjectNames);

        std::size_t incrementLine = std::string::npos;
        constexpr std::size_t maxAppendSpanLines = 80;
        for (std::size_t scan = index + 1; scan < lines.size() && scan <= index + maxAppendSpanLines; ++scan) {
            std::string incrementTrailingComment;
            const std::string incrementCodePart = SafeReplacementEngine::splitTrailingLineComment(lines[scan], incrementTrailingComment);
            if (isStandaloneCountIncrement(incrementCodePart, countName)) {
                incrementLine = scan;
                break;
            }
        }

        if (incrementLine != std::string::npos
            && indexedAccessesAreOnlyForCount(lines, index, incrementLine - 1, targetExpression, countName)
            && !spanMutatesCountBeforeIncrement(lines, index, incrementLine, countName)) {
            std::vector<std::string> originalLines;
            std::vector<std::string> modernLines;
            bool replacedAny = false;
            for (std::size_t lineIndex = index; lineIndex < incrementLine; ++lineIndex) {
                originalLines.push_back(lines[lineIndex]);
                std::string trailingComment;
                const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(lines[lineIndex], trailingComment);
                bool lineReplaced = false;
                std::string modernCodePart = replaceIndexedMemberAccessOutsideLiterals(codePart,
                                                                                       targetExpression,
                                                                                       countName,
                                                                                       objectName,
                                                                                       lineReplaced);
                replacedAny = replacedAny || lineReplaced;
                modernLines.push_back(modernCodePart + trailingComment);
            }

            std::string incrementTrailingComment;
            const std::string incrementCodePart = SafeReplacementEngine::splitTrailingLineComment(lines[incrementLine], incrementTrailingComment);
            originalLines.push_back(lines[incrementLine]);
            if (!replacedAny || !isStandaloneCountIncrement(incrementCodePart, countName)) {
                rewrittenLines.push_back(lines[index]);
                continue;
            }

            std::ostringstream before;
            for (const std::string& originalLine : originalLines) {
                if (before.tellp() > 0) {
                    before << '\n';
                }
                before << originalLine;
            }

            rewrittenLines.push_back(baseIndent + elementType + " " + objectName + "{};");
            rewrittenLines.insert(rewrittenLines.end(), modernLines.begin(), modernLines.end());
            rewrittenLines.push_back(baseIndent + record.symbolName + ".push_back(" + objectName + ");" + incrementTrailingComment);

            std::ostringstream after;
            after << baseIndent << elementType << " " << objectName << "{};";
            for (const std::string& modernLine : modernLines) {
                after << '\n' << modernLine;
            }
            after << '\n' << baseIndent << record.symbolName << ".push_back(" << objectName << ");";

            addAppliedChange(changes,
                             "Indexed append to vector push_back",
                             trim(before.str()),
                             trim(after.str()),
                             "Grouped field-by-field indexed aggregate append into one local value and one std::vector::push_back().");
            usedAppendObjectNames.insert(objectName);
            changed = true;
            index = incrementLine;
            continue;
        }

        std::vector<std::string> originalAssignmentLines;
        std::vector<std::string> modernAssignmentLines;
        std::size_t scan = index;

        for (; scan < lines.size(); ++scan) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(lines[scan], trailingComment);
            std::smatch assignmentMatch;
            if (!std::regex_match(codePart, assignmentMatch, fieldAssignmentPattern)
                || assignmentMatch[2].str() != countName) {
                break;
            }

            originalAssignmentLines.push_back(lines[scan]);
            modernAssignmentLines.push_back(assignmentMatch[1].str() + objectName + "." + assignmentMatch[3].str()
                                            + " = " + trim(assignmentMatch[4].str()) + ";" + trailingComment);
        }

        const bool hasFollowingLine = scan < lines.size();
        std::string incrementTrailingComment;
        std::string incrementCodePart;
        if (hasFollowingLine) {
            incrementCodePart = SafeReplacementEngine::splitTrailingLineComment(lines[scan], incrementTrailingComment);
        }
        const std::regex incrementPattern("^[ \\t]*(?:\\+\\+" + escapeRegex(countName) + "|" + escapeRegex(countName) + "\\+\\+)\\s*;\\s*$");
        const bool hasCountIncrement = hasFollowingLine && std::regex_match(incrementCodePart, incrementPattern);
        if (!hasCountIncrement && originalAssignmentLines.size() < 2) {
            rewrittenLines.push_back(lines[index]);
            continue;
        }

        std::ostringstream before;
        for (const std::string& originalLine : originalAssignmentLines) {
            if (before.tellp() > 0) {
                before << '\n';
            }
            before << originalLine;
        }
        if (hasCountIncrement) {
            before << '\n' << lines[scan];
        }

        rewrittenLines.push_back(baseIndent + elementType + " " + objectName + "{};");
        for (const std::string& assignmentLine : modernAssignmentLines) {
            rewrittenLines.push_back(assignmentLine);
        }
        rewrittenLines.push_back(baseIndent + record.symbolName + ".push_back(" + objectName + ");" + incrementTrailingComment);

        std::ostringstream after;
        after << baseIndent << elementType << " " << objectName << "{};";
        for (const std::string& assignmentLine : modernAssignmentLines) {
            after << '\n' << assignmentLine;
        }
        after << '\n' << baseIndent << record.symbolName << ".push_back(" << objectName << ");";

        addAppliedChange(changes,
                         "Indexed append to vector push_back",
                         trim(before.str()),
                         trim(after.str()),
                         hasCountIncrement
                             ? "Replaced field-by-field indexed append plus count increment with construction of a local value and std::vector::push_back()."
                             : "Replaced field-by-field indexed append whose old count increment had already been removed with construction of a local value and std::vector::push_back().");
        usedAppendObjectNames.insert(objectName);
        changed = true;
        index = hasCountIncrement ? scan : scan - 1;
    }

    return joinLines(rewrittenLines);
}
} // namespace

std::string VectorGrowthEmulationCleanupPass::rewrite(const std::string& code,
                                                      const TransformationContext& context,
                                                      std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
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
        const std::regex appendProbePattern(targetExpression + R"(\s*\[\s*([A-Za-z_]\w*)\s*\](?:\s*\.\s*[A-Za-z_]\w*)?\s*=)");
        std::set<std::string> appendCountSymbols;
        for (std::sregex_iterator iterator(updated.begin(), updated.end(), appendProbePattern), end; iterator != end; ++iterator) {
            appendCountSymbols.insert((*iterator)[1].str());
        }
        const std::regex postIncrementAppendProbePattern(targetExpression + R"(\s*\[\s*([A-Za-z_]\w*)\s*\+\+\s*\](?:\s*\.\s*[A-Za-z_]\w*)?\s*=)");
        for (std::sregex_iterator iterator(updated.begin(), updated.end(), postIncrementAppendProbePattern), end; iterator != end; ++iterator) {
            appendCountSymbols.insert((*iterator)[1].str());
        }

        const bool hasAppendPattern = !appendCountSymbols.empty();

        std::set<std::string> growthBuffers;
        std::set<std::string> capacitySymbols;
        bool changed = false;

        const std::regex tempAllocationPattern("(^[ \\t]*)" + escapeRegex(elementType)
                                                   + "\\s*\\*\\s*([A-Za-z_]\\w*)\\s*=\\s*new\\s+"
                                                   + escapeRegex(elementType)
                                                   + "\\s*\\[\\s*([^\\]]+)\\s*\\]\\s*;\\s*",
                                               std::regex::ECMAScript | std::regex::multiline);
        std::smatch tempMatch;
        std::string search = updated;
        while (std::regex_search(search, tempMatch, tempAllocationPattern)) {
            const std::string tempName = tempMatch[2].str();
            if (std::regex_search(updated, std::regex(targetExpression + R"(\s*=\s*)" + escapeRegex(tempName) + R"(\s*;)"))) {
                growthBuffers.insert(tempName);
                capacitySymbols.insert(trim(tempMatch[3].str()));
            }
            search = tempMatch.suffix().str();
        }

        const std::regex orphanCopyLoopPattern(
            R"(for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*[A-Za-z_]\w*\s*;\s*(?:\+\+\1|\1\+\+)\s*\)\s*\n[ \t]*\{\s*\n[ \t]*([A-Za-z_]\w*)\s*\[\s*\1\s*\]\s*=\s*)"
            + targetExpression
            + R"(\s*\[\s*\1\s*\]\s*;\s*\n[ \t]*\})",
            std::regex::ECMAScript | std::regex::multiline);
        for (std::sregex_iterator iterator(updated.begin(), updated.end(), orphanCopyLoopPattern), end; iterator != end; ++iterator) {
            growthBuffers.insert((*iterator)[2].str());
        }

        if (growthBuffers.empty() && !hasAppendPattern) {
            updated = removeDanglingGrowthIfShells(std::move(updated), record.symbolName, changes);
            continue;
        }

        for (const std::string& tempName : growthBuffers) {
            const std::string escapedTemp = escapeRegex(tempName);
            updated = removeManualGrowthIfBlocks(updated, targetExpression, tempName, elementType, changes, changed);

            const std::regex growthIfBlockPattern(
                R"((^[ \t]*)if\s*\([^;\n]*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
                std::regex::ECMAScript | std::regex::multiline);
            std::smatch growthBlockMatch;
            std::string growthBlockSearch = updated;
            std::size_t growthBlockConsumed = 0;
            while (std::regex_search(growthBlockSearch, growthBlockMatch, growthIfBlockPattern)) {
                const std::size_t blockPosition = growthBlockConsumed + static_cast<std::size_t>(growthBlockMatch.position());
                const std::string body = growthBlockMatch[2].str();
                const bool allocatesTemp = std::regex_search(
                    body,
                    std::regex(escapeRegex(elementType) + R"(\s*\*\s*)" + escapedTemp + R"(\s*=\s*new\s+)" + escapeRegex(elementType) + R"(\s*\[)"));
                const bool assignsTempToVector = std::regex_search(body, std::regex(targetExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;)"));
                const bool copiesIntoTemp = std::regex_search(body, std::regex(escapedTemp + R"(\s*\[[^\]]+\]\s*=\s*)" + targetExpression + R"(\s*\[[^\]]+\])"));
                const bool deletesOldStorage = std::regex_search(body, std::regex(R"(delete\s*\[\s*\]\s*)" + targetExpression));
                const bool updatesCapacityFromTemporary = std::regex_search(
                    body,
                    std::regex(R"((^|\n)[ \t]*[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\s*;)"));

                if (!copiesIntoTemp || !(assignsTempToVector || allocatesTemp || deletesOldStorage || updatesCapacityFromTemporary)) {
                    growthBlockConsumed += static_cast<std::size_t>(growthBlockMatch.position() + growthBlockMatch.length());
                    growthBlockSearch = growthBlockMatch.suffix().str();
                    continue;
                }

                addAppliedChange(changes,
                                 "Vector emulation elimination",
                                 trim(growthBlockMatch[0].str()),
                                 "removed",
                                 "Removed a manual dynamic-array reallocation block because std::vector owns growth, copying, and storage replacement.");
                addAppliedChange(changes,
                                 "Remove manual vector growth copy loop",
                                 "copy loop involving " + tempName,
                                 "removed",
                                 "Removed an element-copy loop that only emulated std::vector reallocation.");
                updated.replace(blockPosition,
                                static_cast<std::size_t>(growthBlockMatch.length()),
                                "");
                changed = true;
                growthBlockSearch = updated.substr(blockPosition);
                growthBlockConsumed = blockPosition;
            }

            updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
                std::string trailingComment;
                const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);

                const std::regex tempAllocationLine(R"(^[ \t]*)" + escapeRegex(elementType) + R"(\s*\*\s*)"
                                                    + escapedTemp + R"(\s*=\s*new\s+)" + escapeRegex(elementType)
                                                    + R"(\s*\[[^\]]+\]\s*;\s*$)");
                if (std::regex_match(codePart, tempAllocationLine)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Vector growth emulation cleanup",
                                     trim(codePart),
                                     "removed",
                                     "Removed a temporary raw growth buffer because std::vector manages reallocation.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }

                const std::regex deleteTempLine(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapedTemp + R"(\s*;\s*$)");
                if (std::regex_match(codePart, deleteTempLine)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Remove delete array after vector growth cleanup",
                                     trim(codePart),
                                     "removed",
                                     "Removed delete[] for a temporary growth buffer eliminated by std::vector.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }

                const std::regex assignTempLine(R"(^[ \t]*)" + targetExpression + R"(\s*=\s*)" + escapedTemp + R"(\s*;\s*$)");
                if (std::regex_match(codePart, assignTempLine)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Remove raw pointer assignment to vector",
                                     trim(codePart),
                                     "removed",
                                     "Removed assignment from a raw growth buffer to a std::vector symbol.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }

                return line;
            });

            const std::regex copyLoopPattern(
                R"([ \t]*for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*([A-Za-z_]\w*)\s*;\s*(?:\+\+\1|\1\+\+)\s*\)\s*\n[ \t]*\{\s*\n[ \t]*)"
                + escapedTemp
                + R"(\s*\[\s*\1\s*\]\s*=\s*)"
                + targetExpression
                + R"(\s*\[\s*\1\s*\]\s*;\s*\n[ \t]*\}\s*\n?)",
                std::regex::ECMAScript | std::regex::multiline);
            if (std::regex_search(updated, copyLoopPattern)) {
                updated = std::regex_replace(updated, copyLoopPattern, "");
                changed = true;
                addAppliedChange(changes,
                                 "Remove manual vector growth copy loop",
                                 "for (...) { " + tempName + "[i] = " + record.symbolName + "[i]; }",
                                 "removed",
                                 "Removed a manual element-copy loop that only emulated std::vector growth.");
            }
        }

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex vectorNewAssignment("^([ \\t]*)(" + targetExpression + ")\\s*=\\s*new\\s+" + escapeRegex(elementType) + R"(\s*\[\s*([^\]]+)\s*\]\s*;\s*$)");
            if (std::regex_match(codePart, match, vectorNewAssignment)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + ".reserve(" + trim(match[3].str()) + ");";
                changed = true;
                addAppliedChange(changes,
                                 "Replace raw vector allocation with reserve",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced a raw new[] assignment to a converted std::vector with reserve().");
                return replacement + trailingComment;
            }

            const std::regex vectorConstructionPattern("^([ \\t]*)std::vector\\s*<\\s*" + escapeRegex(elementType)
                                                       + R"(\s*>\s+)" + escapeRegex(record.symbolName)
                                                       + R"(\s*\(\s*([^)]+)\s*\)\s*;\s*$)");
            if (hasAppendPattern && std::regex_match(codePart, match, vectorConstructionPattern)) {
                const std::string replacement = match[1].str() + "std::vector<" + elementType + "> " + record.symbolName + ";\n"
                    + match[1].str() + record.symbolName + ".reserve(" + trim(match[2].str()) + ");";
                changed = true;
                addAppliedChange(changes,
                                 "Vector growth reserve modernization",
                                 trim(codePart),
                                 trim(replacement),
                                 "The raw array is used with append-style indexing, so the converted std::vector keeps capacity with reserve() instead of pre-sizing elements.");
                return replacement + trailingComment;
            }

            const std::regex resizePattern("^([ \\t]*)(" + targetExpression + ")\\.resize\\(([^)]+)\\)\\s*;\\s*$");
            if (hasAppendPattern && std::regex_match(codePart, match, resizePattern)) {
                const std::string replacement = match[1].str() + trim(match[2].str()) + ".reserve(" + trim(match[3].str()) + ");";
                changed = true;
                addAppliedChange(changes,
                                 "Vector growth reserve modernization",
                                 trim(codePart),
                                 trim(replacement),
                                 "The vector is appended with push_back, so initial allocation became reserve instead of resize.");
                return replacement + trailingComment;
            }

            const std::regex countReturnPattern(R"(^([ \t]*)return\s+([A-Za-z_]\w*)\s*;\s*$)");
            if (std::regex_match(codePart, match, countReturnPattern) && hasAppendPattern) {
                const std::string countName = match[2].str();
                if (appendCountSymbols.contains(countName)) {
                    const std::string replacement = match[1].str() + "return " + record.symbolName + ".size();";
                    changed = true;
                    addAppliedChange(changes,
                                     "Count mirror to vector size",
                                     trim(codePart),
                                     trim(replacement),
                                     "Replaced a count mirror return with std::vector::size().");
                    return replacement + trailingComment;
                }
            }

            if (lineMentionsAny(codePart, capacitySymbols)) {
                const std::regex capacityTemporaryDeclaration(R"(^[ \t]*(?:auto|int|long|size_t|std::size_t)\s+[A-Za-z_]\w*\s*=\s*[^;]+;\s*$)");
                if (std::regex_match(codePart, capacityTemporaryDeclaration)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Vector emulation elimination",
                                     trim(codePart),
                                     "removed",
                                     "Removed a temporary capacity calculation used only for manual dynamic-array growth.");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }

            bool reserveChanged = false;
            std::string rewritten = ensureReserveAfterCapacityAssignment(codePart, record.symbolName, capacitySymbols, reserveChanged);
            if (reserveChanged && !hasAppendPattern) {
                changed = true;
                addAppliedChange(changes,
                                 "Vector capacity growth to reserve",
                                 trim(codePart),
                                 trim(rewritten),
                                 "Preserved logical capacity growth while using std::vector::reserve().");
                return rewritten + trailingComment;
            }

            return line;
        });

        updated = rewriteFieldAppendSequences(updated, record, elementType, targetExpression, changes, changed);

        const std::regex appendThenIncrementPattern("(^[ \\t]*)(" + targetExpression
                                                        + ")\\s*\\[\\s*([A-Za-z_]\\w*)\\s*\\]\\s*=\\s*([^;]+)\\s*;\\s*\\n\\1(?:\\+\\+\\3|\\3\\+\\+)\\s*;",
                                                    std::regex::ECMAScript | std::regex::multiline);
        std::smatch appendMatch;
        std::string appendSearch = updated;
        std::size_t appendConsumed = 0;
        while (std::regex_search(appendSearch, appendMatch, appendThenIncrementPattern)) {
            const std::string replacement = appendMatch[1].str() + record.symbolName + ".push_back(" + trim(appendMatch[4].str()) + ");";
            const std::size_t appendPosition = appendConsumed + static_cast<std::size_t>(appendMatch.position());
            if (replacement == appendMatch[0].str()) {
                appendConsumed += static_cast<std::size_t>(appendMatch.position() + appendMatch.length());
                appendSearch = updated.substr(appendConsumed);
                continue;
            }
            updated.replace(appendPosition,
                            static_cast<std::size_t>(appendMatch.length()),
                            replacement);
            changed = true;
            addAppliedChange(changes,
                             "Indexed append to vector push_back",
                             trim(appendMatch[0].str()),
                             trim(replacement),
                             "Replaced indexed append plus count increment with std::vector::push_back().");
            appendConsumed = appendPosition + replacement.size();
            appendSearch = updated.substr(appendConsumed);
        }

        const std::regex postIncrementAppendPattern("(^[ \\t]*)(" + targetExpression
                                                       + ")\\s*\\[\\s*([A-Za-z_]\\w*)\\s*\\+\\+\\s*\\]\\s*=\\s*([^;]+)\\s*;",
                                                   std::regex::ECMAScript | std::regex::multiline);
        appendSearch = updated;
        appendConsumed = 0;
        while (std::regex_search(appendSearch, appendMatch, postIncrementAppendPattern)) {
            const std::string replacement = appendMatch[1].str() + record.symbolName + ".push_back(" + trim(appendMatch[4].str()) + ");";
            const std::size_t appendPosition = appendConsumed + static_cast<std::size_t>(appendMatch.position());
            updated.replace(appendPosition,
                            static_cast<std::size_t>(appendMatch.length()),
                            replacement);
            changed = true;
            addAppliedChange(changes,
                             "Indexed append to vector push_back",
                             trim(appendMatch[0].str()),
                             trim(replacement),
                             "Replaced post-increment indexed append with std::vector::push_back().");
            appendConsumed = appendPosition + replacement.size();
            appendSearch = updated.substr(appendConsumed);
        }

        const std::regex capacityCountPattern(R"(\b([A-Za-z_]\w*)\s*(==|>=|>)\s*([A-Za-z_]\w*)\b)");
        std::string capacitySearch = updated;
        std::smatch capacityMatch;
        while (std::regex_search(capacitySearch, capacityMatch, capacityCountPattern)) {
            const std::string countName = capacityMatch[1].str();
            const std::string capacityName = capacityMatch[3].str();
            if (capacitySymbols.contains(capacityName) && appendCountSymbols.contains(countName)) {
                updated = std::regex_replace(updated,
                                             std::regex("\\b" + escapeRegex(countName) + R"(\s*)" + capacityMatch[2].str() + R"(\s*)" + escapeRegex(capacityName) + R"(\b)"),
                                             record.symbolName + ".size() " + capacityMatch[2].str() + " " + sizeComparableExpression(capacityName));
                changed = true;
                addAppliedChange(changes,
                                 "Count mirror to vector size",
                                 countName + " " + capacityMatch[2].str() + " " + capacityName,
                                 record.symbolName + ".size() " + capacityMatch[2].str() + " " + sizeComparableExpression(capacityName),
                                 "Replaced a count mirror in capacity logic with std::vector::size().");
            }
            capacitySearch = capacityMatch.suffix().str();
        }

        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;

            const std::regex standaloneCountIncrementPattern(R"(^[ \t]*(?:\+\+([A-Za-z_]\w*)|([A-Za-z_]\w*)\+\+)\s*;\s*$)");
            if (std::regex_match(codePart, match, standaloneCountIncrementPattern)) {
                const std::string countName = match[1].matched ? match[1].str() : match[2].str();
                if (appendCountSymbols.contains(countName)) {
                    changed = true;
                    addAppliedChange(changes,
                                     "Count mirror to vector size",
                                     trim(codePart),
                                     "removed",
                                     "Removed a manual count increment after append logic was converted to std::vector::push_back().");
                    return trailingComment.empty() ? std::string{} : trailingComment;
                }
            }

            const std::regex logicalLimitPattern("^([ \\t]*if\\s*\\([^\\n)]*)\\b([A-Za-z_]\\w*)\\b\\s*(>=|>|==)\\s*([^\\n)]+)(\\).*)$");
            if (std::regex_match(codePart, match, logicalLimitPattern) && appendCountSymbols.contains(match[2].str())) {
                const std::string replacement = match[1].str() + record.symbolName + ".size() "
                    + match[3].str() + " " + sizeComparableExpression(match[4].str()) + match[5].str();
                changed = true;
                addAppliedChange(changes,
                                 "Count mirror to vector size",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced a manual count mirror in a capacity/limit check with std::vector::size().");
                return replacement + trailingComment;
            }

            return line;
        });

        updated = removeEmptyIfBlocks(std::move(updated), changes);
        updated = removeDanglingGrowthIfShells(std::move(updated), record.symbolName, changes);

        if (changed) {
            addAppliedChange(changes,
                             "Vector growth emulation cleanup",
                             record.symbolName,
                             "manual growth logic replaced with std::vector operations",
                             "Cleaned up raw dynamic-array growth logic after the storage symbol became std::vector.");
        }
    }

    return updated;
}
