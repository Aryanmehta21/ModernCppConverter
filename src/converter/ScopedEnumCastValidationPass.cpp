#include "converter/ScopedEnumCastValidationPass.h"

#include "converter/SafeReplacementEngine.h"

#include <cctype>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>

namespace
{
struct ParsedStaticCast
{
    std::size_t start = 0;
    std::size_t typeStart = 0;
    std::size_t typeEnd = 0;
    std::size_t expressionStart = 0;
    std::size_t expressionEnd = 0;
    std::size_t end = 0;
    std::string type;
    std::string expression;
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

std::optional<std::size_t> findMatchingParen(const std::string& text, std::size_t openPosition)
{
    if (openPosition >= text.size() || text[openPosition] != '(') {
        return std::nullopt;
    }

    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t index = openPosition; index < text.size(); ++index) {
        const char current = text[index];
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
        if (inString || inCharacter) {
            continue;
        }
        if (current == '(') {
            ++depth;
        } else if (current == ')') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> findStaticCastTypeEnd(const std::string& text, std::size_t typeStart)
{
    int templateDepth = 1;
    for (std::size_t index = typeStart; index < text.size(); ++index) {
        if (text[index] == '<') {
            ++templateDepth;
        } else if (text[index] == '>') {
            --templateDepth;
            if (templateDepth == 0) {
                return index;
            }
        }
    }
    return std::nullopt;
}

std::optional<ParsedStaticCast> parseStaticCastAt(const std::string& text, std::size_t position)
{
    constexpr std::string_view prefix = "static_cast<";
    if (text.compare(position, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    const std::size_t typeStart = position + prefix.size();
    const std::optional<std::size_t> typeEnd = findStaticCastTypeEnd(text, typeStart);
    if (!typeEnd.has_value()) {
        return std::nullopt;
    }

    std::size_t openParen = *typeEnd + 1;
    while (openParen < text.size() && std::isspace(static_cast<unsigned char>(text[openParen]))) {
        ++openParen;
    }
    if (openParen >= text.size() || text[openParen] != '(') {
        return std::nullopt;
    }

    const std::optional<std::size_t> closeParen = findMatchingParen(text, openParen);
    if (!closeParen.has_value()) {
        return std::nullopt;
    }

    ParsedStaticCast parsed;
    parsed.start = position;
    parsed.typeStart = typeStart;
    parsed.typeEnd = *typeEnd;
    parsed.expressionStart = openParen + 1;
    parsed.expressionEnd = *closeParen;
    parsed.end = *closeParen + 1;
    parsed.type = trim(text.substr(parsed.typeStart, parsed.typeEnd - parsed.typeStart));
    parsed.expression = trim(text.substr(parsed.expressionStart, parsed.expressionEnd - parsed.expressionStart));
    return parsed;
}

std::string normalizeNestedCasts(std::string line, bool& changed)
{
    bool changedThisRound = true;
    while (changedThisRound) {
        changedThisRound = false;
        std::size_t position = 0;
        while ((position = line.find("static_cast<", position)) != std::string::npos) {
            const std::optional<ParsedStaticCast> outer = parseStaticCastAt(line, position);
            if (!outer.has_value()) {
                position += std::string_view("static_cast<").size();
                continue;
            }

            const std::string innerTrimmed = trim(outer->expression);
            const std::optional<ParsedStaticCast> inner = parseStaticCastAt(innerTrimmed, 0);
            if (inner.has_value() && inner->end == innerTrimmed.size() && trim(inner->type) == trim(outer->type)) {
                const std::string replacement = "static_cast<" + outer->type + ">(" + inner->expression + ")";
                line.replace(outer->start, outer->end - outer->start, replacement);
                changed = true;
                changedThisRound = true;
                position = outer->start + replacement.size();
                continue;
            }

            position = outer->end;
        }
    }
    return line;
}

std::string repairMemberAccessCastPlacement(std::string line, bool& changed)
{
    static const std::regex misplacedPattern(
        R"(((?:\b[A-Za-z_]\w*|\(\s*\*\s*[A-Za-z_]\w*\s*\)|[A-Za-z_]\w*\s*\[[^\]\n]+\])(?:\s*(?:->|\.)\s*[A-Za-z_]\w*(?:\s*\[[^\]\n]+\])?)*)\s*(->|\.)\s*static_cast<([^\(]+)>\(\s*([A-Za-z_]\w*\s*\([^;\n]*\))\s*\))",
        std::regex::ECMAScript);

    std::string repaired;
    std::size_t searchStart = 0;
    for (std::sregex_iterator it(line.begin(), line.end(), misplacedPattern), end; it != end; ++it) {
        const std::size_t position = static_cast<std::size_t>(it->position());
        const std::size_t length = static_cast<std::size_t>(it->length());
        repaired.append(line.substr(searchStart, position - searchStart));
        repaired.append("static_cast<");
        repaired.append(trim((*it)[3].str()));
        repaired.append(">(");
        repaired.append(trim((*it)[1].str()));
        repaired.append((*it)[2].str());
        repaired.append(trim((*it)[4].str()));
        repaired.push_back(')');
        searchStart = position + length;
        changed = true;
    }

    if (searchStart == 0) {
        return line;
    }

    repaired.append(line.substr(searchStart));
    return repaired;
}

std::string normalizeCaseLabelCasts(std::string line, bool& changed)
{
    static const std::regex caseCastPattern(
        R"((\bcase\s+)static_cast<[^>\n]+>\(\s*([A-Za-z_]\w*\s*::\s*[A-Za-z_]\w*)\s*\)(\s*:))",
        std::regex::ECMAScript);

    const std::string before = line;
    line = std::regex_replace(line, caseCastPattern, "$1$2$3");
    changed = changed || line != before;
    return line;
}
} // namespace

std::string ScopedEnumCastValidationPass::validateAndNormalize(const std::string& code,
                                                               std::vector<ConversionChange>& changes) const
{
    bool changed = false;
    const SafeReplacementEngine safeReplacement;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::string before = codePart;
        codePart = repairMemberAccessCastPlacement(codePart, changed);
        codePart = normalizeCaseLabelCasts(codePart, changed);
        codePart = normalizeNestedCasts(codePart, changed);
        if (codePart != before) {
            addAppliedChange(changes,
                             "Scoped enum cast validation",
                             trim(before),
                             trim(codePart),
                             "Normalized scoped-enum output casts so the conversion is idempotent and member-access expressions remain syntactically valid.");
        }
        return codePart + trailingComment;
    });

    return changed ? updated : code;
}
