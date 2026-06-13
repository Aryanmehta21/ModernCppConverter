#include "converter/ScopedEnumOutputPropagationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct EnumInfo
{
    std::string name;
    std::string underlyingType;
    std::vector<std::string> enumerators;
};

struct ExpressionCandidate
{
    std::string pattern;
    std::string enumName;
};

struct ScopedEnumCastRewriteState
{
    std::set<std::pair<std::string, std::string>> processedExpressions;

    [[nodiscard]] bool wasProcessed(const std::string& enumName, const std::string& expression) const
    {
        return processedExpressions.contains({enumName, trimCopy(expression)});
    }

    void markProcessed(const std::string& enumName, const std::string& expression)
    {
        processedExpressions.insert({enumName, trimCopy(expression)});
    }

private:
    [[nodiscard]] static std::string trimCopy(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }
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

std::vector<std::string> parseEnumeratorNames(const std::string& body)
{
    std::vector<std::string> enumerators;
    std::stringstream stream(body);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        std::string name = trim(entry);
        const std::size_t assignment = name.find('=');
        if (assignment != std::string::npos) {
            name = trim(name.substr(0, assignment));
        }
        if (std::regex_match(name, std::regex(R"([A-Za-z_]\w*)"))) {
            enumerators.push_back(name);
        }
    }
    return enumerators;
}

std::map<std::string, EnumInfo> collectEnumClasses(const std::string& code)
{
    std::map<std::string, EnumInfo> enums;
    const std::regex enumPattern(R"(\benum\s+class\s+([A-Za-z_]\w*)\s*(?::\s*([^\{\n]+?))?\s*\{([^{}]*)\}\s*;)",
                                 std::regex::ECMAScript);
    for (std::sregex_iterator it(code.begin(), code.end(), enumPattern), end; it != end; ++it) {
        EnumInfo info;
        info.name = (*it)[1].str();
        info.underlyingType = trim((*it)[2].str());
        info.enumerators = parseEnumeratorNames((*it)[3].str());
        enums[info.name] = info;
    }
    return enums;
}

bool isIdentifierCharacter(char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

std::string castTypeFor(const EnumInfo& info)
{
    if (!info.underlyingType.empty()) {
        return info.underlyingType;
    }
    return "std::underlying_type_t<" + info.name + ">";
}

bool usesUnderlyingTypeTrait(const EnumInfo& info)
{
    return info.underlyingType.empty();
}

bool isAlreadyInsideStaticCast(const std::string& line, std::size_t position, std::size_t length)
{
    const std::size_t lastCast = line.rfind("static_cast<", position);
    if (lastCast == std::string::npos) {
        return false;
    }
    const std::size_t castClose = line.find(')', lastCast);
    return castClose != std::string::npos && castClose >= position + length;
}

bool followsMemberAccessOperator(const std::string& line, std::size_t position)
{
    std::size_t cursor = position;
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(line[cursor - 1]))) {
        --cursor;
    }
    if (cursor == 0) {
        return false;
    }
    if (line[cursor - 1] == '.') {
        return true;
    }
    if (cursor >= 2 && line.substr(cursor - 2, 2) == "->") {
        return true;
    }
    if (cursor >= 2 && line.substr(cursor - 2, 2) == "::") {
        return true;
    }
    return false;
}

bool isInsideOutputSafeWrapper(const std::string& line, std::size_t position, std::size_t length)
{
    if (isAlreadyInsideStaticCast(line, position, length)) {
        return true;
    }

    const std::size_t lastToUnderlying = line.rfind("std::to_underlying", position);
    if (lastToUnderlying != std::string::npos) {
        const std::size_t openParen = line.find('(', lastToUnderlying);
        const std::size_t closeParen = line.find(')', lastToUnderlying);
        if (openParen != std::string::npos && closeParen != std::string::npos && openParen < position && closeParen >= position + length) {
            return true;
        }
    }

    for (const std::string_view helper : {"to_string", "toString", "enumToString", "format_as"}) {
        const std::size_t helperPosition = line.rfind(helper, position);
        if (helperPosition == std::string::npos) {
            continue;
        }
        const std::size_t openParen = line.find('(', helperPosition);
        const std::size_t closeParen = line.find(')', helperPosition);
        if (openParen != std::string::npos && closeParen != std::string::npos && openParen < position && closeParen >= position + length) {
            return true;
        }
    }

    return false;
}

bool isInsideQuotedText(const std::string& line, std::size_t position)
{
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = 0; index < position && index < line.size(); ++index) {
        const char current = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
    }
    return inString || inCharacter;
}

bool isInsideSwitchCaseLabel(const std::string& line, const std::size_t position)
{
    const std::size_t casePosition = line.rfind("case", position);
    if (casePosition == std::string::npos) {
        return false;
    }

    const bool leftBoundaryOk = casePosition == 0 || !isIdentifierCharacter(line[casePosition - 1]);
    const std::size_t afterCase = casePosition + std::string_view("case").size();
    const bool rightBoundaryOk = afterCase >= line.size() || !isIdentifierCharacter(line[afterCase]);
    if (!leftBoundaryOk || !rightBoundaryOk) {
        return false;
    }

    const std::size_t previousStatement = line.rfind(';', position);
    if (previousStatement != std::string::npos && previousStatement > casePosition) {
        return false;
    }

    const std::size_t labelColon = line.find(':', afterCase);
    return labelColon != std::string::npos && position < labelColon;
}

bool lineMayOutputOrFormat(const std::string& codePart)
{
    if (codePart.find("<<") != std::string::npos) {
        return true;
    }
    if (codePart.find("std::format(") != std::string::npos || codePart.find("fmt::format(") != std::string::npos) {
        return true;
    }
    static const std::regex loggingPattern(R"(\b(?:LOG|LOG_[A-Z0-9_]+|TRACE|DEBUG|INFO|WARN|WARNING|ERROR|FATAL)\s*\()",
                                           std::regex::ECMAScript);
    return std::regex_search(codePart, loggingPattern);
}

void addCandidate(std::vector<ExpressionCandidate>& candidates,
                  std::set<std::pair<std::string, std::string>>& seen,
                  std::string pattern,
                  const std::string& enumName)
{
    if (pattern.empty()) {
        return;
    }
    const auto key = std::make_pair(pattern, enumName);
    if (seen.insert(key).second) {
        candidates.push_back(ExpressionCandidate{std::move(pattern), enumName});
    }
}

std::vector<ExpressionCandidate> buildCandidates(const std::string& code,
                                                 const std::map<std::string, EnumInfo>& enums)
{
    std::vector<ExpressionCandidate> candidates;
    std::set<std::pair<std::string, std::string>> seen;
    std::map<std::string, std::string> variableTypes;
    std::map<std::string, std::string> functionReturnTypes;
    std::map<std::string, std::string> sequentialContainers;
    std::map<std::string, std::string> mapContainers;

    for (const auto& [enumName, info] : enums) {
        addCandidate(candidates, seen, R"(\b)" + escapeRegex(enumName) + R"(\s*::\s*[A-Za-z_]\w*\b)", enumName);

        const std::regex declarationPattern(R"(\b(?:constexpr\s+|inline\s+|static\s+|virtual\s+|const\s+|volatile\s+)*?)"
                                            + escapeRegex(enumName)
                                            + R"(\s*(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
                                            std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), declarationPattern), end; it != end; ++it) {
            const std::string symbol = (*it)[1].str();
            const std::size_t afterSymbol = static_cast<std::size_t>(it->position(1) + it->length(1));
            std::size_t cursor = afterSymbol;
            while (cursor < code.size() && std::isspace(static_cast<unsigned char>(code[cursor]))) {
                ++cursor;
            }
            if (cursor < code.size() && code[cursor] == '(') {
                functionReturnTypes[symbol] = enumName;
            } else {
                variableTypes[symbol] = enumName;
            }
        }

        const std::regex vectorPattern(R"(\bstd::(?:vector|deque|list)\s*<\s*)"
                                       + escapeRegex(enumName)
                                       + R"(\s*>\s+([A-Za-z_]\w*)\b)",
                                       std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), vectorPattern), end; it != end; ++it) {
            sequentialContainers[(*it)[1].str()] = enumName;
        }

        const std::regex arrayPattern(R"(\bstd::array\s*<\s*)"
                                      + escapeRegex(enumName)
                                      + R"(\s*,\s*[^>]+>\s+([A-Za-z_]\w*)\b)",
                                      std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), arrayPattern), end; it != end; ++it) {
            sequentialContainers[(*it)[1].str()] = enumName;
        }

        const std::regex mapPattern(R"(\bstd::(?:unordered_)?map\s*<\s*[^,>]+,\s*)"
                                    + escapeRegex(enumName)
                                    + R"(\s*>\s+([A-Za-z_]\w*)\b)",
                                    std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), mapPattern), end; it != end; ++it) {
            mapContainers[(*it)[1].str()] = enumName;
        }

        const std::regex lambdaPattern(R"(\bauto\s+([A-Za-z_]\w*)\s*=\s*\[[^\]]*\]\s*\([^)]*\)\s*\{[^{}]*\breturn\s+)"
                                       + escapeRegex(enumName)
                                       + R"(\s*::\s*[A-Za-z_]\w*\s*;[^{}]*\}\s*;)",
                                       std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), lambdaPattern), end; it != end; ++it) {
            functionReturnTypes[(*it)[1].str()] = enumName;
        }
    }

    for (const auto& [symbol, enumName] : variableTypes) {
        addCandidate(candidates, seen, R"(\b)" + escapeRegex(symbol) + R"(\b)", enumName);
        addCandidate(candidates,
                     seen,
                     R"(\b(?:this|[A-Za-z_]\w*(?:\s*\[[^\]\n]+\])?)\s*(?:\.|->)\s*)" + escapeRegex(symbol) + R"(\b)",
                     enumName);
    }

    for (const auto& [functionName, enumName] : functionReturnTypes) {
        const std::string escaped = escapeRegex(functionName);
        addCandidate(candidates,
                     seen,
                     R"(\b(?:(?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n]+\])?)*(?:\.|->)\s*)?)" + escaped + R"(\s*\([^;\n()]*\))",
                     enumName);
        addCandidate(candidates,
                     seen,
                     R"(\(\s*\*\s*[A-Za-z_]\w*\s*\)\s*->\s*)" + escaped + R"(\s*\([^;\n()]*\))",
                     enumName);
        addCandidate(candidates,
                     seen,
                     R"(\(\s*[A-Za-z_]\w*\s*\)\s*->\s*)" + escaped + R"(\s*\([^;\n()]*\))",
                     enumName);
    }

    for (const auto& [container, enumName] : sequentialContainers) {
        const std::string escaped = escapeRegex(container);
        addCandidate(candidates, seen, R"(\b)" + escaped + R"(\s*\[[^\]\n]+\])", enumName);
        const std::regex rangeLoopPattern(R"(for\s*\(\s*(?:const\s+)?(?:auto|auto\s*&|auto&|[A-Za-z_:<>]+\s*(?:&|const&)?)\s+([A-Za-z_]\w*)\s*:\s*)"
                                          + escaped + R"(\s*\))",
                                          std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), rangeLoopPattern), end; it != end; ++it) {
            addCandidate(candidates, seen, R"(\b)" + escapeRegex((*it)[1].str()) + R"(\b)", enumName);
        }
    }

    for (const auto& [container, enumName] : mapContainers) {
        const std::string escaped = escapeRegex(container);
        addCandidate(candidates, seen, R"(\b)" + escaped + R"(\s*\[[^\]\n]+\])", enumName);

        const std::regex structuredLoopPattern(R"(for\s*\([^[]*\[\s*[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)\s*\]\s*:\s*)"
                                               + escaped + R"(\s*\))",
                                               std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), structuredLoopPattern), end; it != end; ++it) {
            addCandidate(candidates, seen, R"(\b)" + escapeRegex((*it)[1].str()) + R"(\b)", enumName);
        }

        const std::regex iteratorLoopPattern(R"(for\s*\([^;]*\s+([A-Za-z_]\w*)\s*=\s*)"
                                             + escaped
                                             + R"(\.begin\(\)\s*;\s*\1\s*!=\s*)"
                                             + escaped
                                             + R"(\.end\(\)\s*;\s*\+\+\1\s*\))",
                                             std::regex::ECMAScript);
        for (std::sregex_iterator it(code.begin(), code.end(), iteratorLoopPattern), end; it != end; ++it) {
            const std::string iteratorName = escapeRegex((*it)[1].str());
            addCandidate(candidates, seen, R"(\b)" + iteratorName + R"(\s*->\s*second\b)", enumName);
            addCandidate(candidates, seen, R"(\(\s*\*)" + iteratorName + R"(\s*\)\s*\.\s*second\b)", enumName);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const ExpressionCandidate& left, const ExpressionCandidate& right) {
        return left.pattern.size() > right.pattern.size();
    });
    return candidates;
}

std::string replaceCandidate(const std::string& codePart,
                             const ExpressionCandidate& candidate,
                             const EnumInfo& enumInfo,
                             ScopedEnumCastRewriteState& rewriteState,
                             bool& lineChanged,
                             bool& needsTypeTraits)
{
    const std::regex pattern(candidate.pattern, std::regex::ECMAScript);
    std::string result;
    std::size_t searchStart = 0;

    for (std::sregex_iterator it(codePart.begin(), codePart.end(), pattern), end; it != end; ++it) {
        const std::size_t position = static_cast<std::size_t>(it->position());
        const std::size_t length = static_cast<std::size_t>(it->length());
        const std::string expression = it->str();

        if (position < searchStart
            || isInsideQuotedText(codePart, position)
            || isInsideSwitchCaseLabel(codePart, position)
            || followsMemberAccessOperator(codePart, position)
            || isInsideOutputSafeWrapper(codePart, position, length)
            || rewriteState.wasProcessed(candidate.enumName, expression)) {
            continue;
        }

        const bool leftBoundaryOk = position == 0 || !isIdentifierCharacter(codePart[position - 1]);
        const std::size_t after = position + length;
        const bool rightBoundaryOk = after >= codePart.size() || !isIdentifierCharacter(codePart[after]);
        if (!leftBoundaryOk || !rightBoundaryOk) {
            continue;
        }

        result.append(codePart.substr(searchStart, position - searchStart));
        result.append("static_cast<");
        result.append(castTypeFor(enumInfo));
        result.append(">(");
        result.append(expression);
        result.push_back(')');
        searchStart = position + length;
        lineChanged = true;
        rewriteState.markProcessed(candidate.enumName, expression);
        needsTypeTraits = needsTypeTraits || usesUnderlyingTypeTrait(enumInfo);
    }

    if (searchStart == 0) {
        return codePart;
    }

    result.append(codePart.substr(searchStart));
    return result;
}
} // namespace

std::string ScopedEnumOutputPropagationPass::rewrite(const std::string& code,
                                                     const ModernizationOptions&,
                                                     std::vector<ConversionChange>& changes) const
{
    const std::map<std::string, EnumInfo> enums = collectEnumClasses(code);
    if (enums.empty()) {
        return code;
    }

    const std::vector<ExpressionCandidate> candidates = buildCandidates(code, enums);
    if (candidates.empty()) {
        return code;
    }

    bool changed = false;
    bool needsTypeTraits = false;
    ScopedEnumCastRewriteState rewriteState;
    const SafeReplacementEngine safeReplacement;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        if (!lineMayOutputOrFormat(codePart)) {
            return line;
        }

        const std::string beforeLine = codePart;
        bool lineChanged = false;
        for (const ExpressionCandidate& candidate : candidates) {
            const auto enumIt = enums.find(candidate.enumName);
            if (enumIt == enums.end()) {
                continue;
            }
            codePart = replaceCandidate(codePart, candidate, enumIt->second, rewriteState, lineChanged, needsTypeTraits);
        }

        if (lineChanged) {
            changed = true;
            addAppliedChange(changes,
                             "Scoped enum output propagation",
                             trim(beforeLine),
                             trim(codePart),
                             "A scoped enum no longer converts implicitly to an integer, so output and formatting expressions now cast it to an output-safe underlying value.");
        }
        return codePart + trailingComment;
    });

    if (changed && needsTypeTraits) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <type_traits>");
    }

    return updated;
}
