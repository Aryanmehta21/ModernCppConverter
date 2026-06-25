#include "converter/QualityModernizationPass.h"

#include "converter/IncludeManager.h"
#include "converter/RewriteCoordinator.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
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

std::size_t findMatchingBrace(const std::string& text, std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
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

std::string finalIdentifierFromCollection(std::string collection)
{
    collection = trim(std::move(collection));
    std::smatch match;
    const std::regex trailingIdentifier(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]+\])?\s*$)");
    if (std::regex_search(collection, match, trailingIdentifier)) {
        return match[1].str();
    }
    return {};
}

bool containsIdentifier(const std::string& text, const std::string& identifier)
{
    return std::regex_search(text, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::vector<std::pair<std::size_t, std::size_t>> protectedSourceRanges(const std::string& text)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    enum class State
    {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharLiteral,
    };

    State state = State::Code;
    std::size_t rangeStart = 0;
    for (std::size_t index = 0; index < text.size();) {
        const char current = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';

        switch (state) {
        case State::Code:
            if (current == '/' && next == '/') {
                state = State::LineComment;
                rangeStart = index;
                index += 2;
            } else if (current == '/' && next == '*') {
                state = State::BlockComment;
                rangeStart = index;
                index += 2;
            } else if (current == '"') {
                state = State::StringLiteral;
                rangeStart = index++;
            } else if (current == '\'') {
                state = State::CharLiteral;
                rangeStart = index++;
            } else {
                ++index;
            }
            break;
        case State::LineComment:
            if (current == '\n') {
                ranges.push_back({rangeStart, index});
                state = State::Code;
            }
            ++index;
            break;
        case State::BlockComment:
            if (current == '*' && next == '/') {
                index += 2;
                ranges.push_back({rangeStart, index});
                state = State::Code;
            } else {
                ++index;
            }
            break;
        case State::StringLiteral:
        case State::CharLiteral:
            if (current == '\\' && index + 1 < text.size()) {
                index += 2;
                break;
            }
            if ((state == State::StringLiteral && current == '"')
                || (state == State::CharLiteral && current == '\'')) {
                ++index;
                ranges.push_back({rangeStart, index});
                state = State::Code;
            } else {
                ++index;
            }
            break;
        }
    }

    if (state != State::Code) {
        ranges.push_back({rangeStart, text.size()});
    }
    return ranges;
}

bool isInsideProtectedRange(const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
                            std::size_t start,
                            std::size_t end)
{
    return std::any_of(ranges.begin(), ranges.end(), [start, end](const auto& range) {
        return start < range.second && range.first < end;
    });
}

std::string maskProtectedSource(const std::string& text)
{
    std::string masked = text;
    for (const auto& range : protectedSourceRanges(text)) {
        for (std::size_t index = range.first; index < range.second && index < masked.size(); ++index) {
            if (masked[index] != '\n') {
                masked[index] = ' ';
            }
        }
    }
    return masked;
}

RewriteApplicationResult replaceIndexedExpressionsWithRangeEdits(const std::string& body,
                                                                  const std::string& indexedExpression,
                                                                  const std::string& itemName)
{
    std::vector<RewriteEdit> edits;
    const std::regex expressionPattern(indexedExpression, std::regex::ECMAScript);
    const std::vector<std::pair<std::size_t, std::size_t>> protectedRanges = protectedSourceRanges(body);
    for (std::sregex_iterator it(body.begin(), body.end(), expressionPattern), end; it != end; ++it) {
        const std::size_t start = static_cast<std::size_t>(it->position());
        const std::size_t finish = start + static_cast<std::size_t>(it->length());
        if (isInsideProtectedRange(protectedRanges, start, finish)) {
            continue;
        }

        SourceRange range;
        range.start.offset = start;
        range.start.column = start + 1;
        range.end.offset = finish;
        range.end.column = finish + 1;
        range.entityKind = SourceEntityKind::Expression;
        range.entityName = it->str();

        RewriteEdit edit;
        edit.range = std::move(range);
        edit.replacementText = itemName;
        edit.passName = "QualityModernizationPass";
        edit.reason = "Replace index expression with the generated range-for variable.";
        edit.affectedSymbol = itemName;
        edits.push_back(std::move(edit));
    }
    return RewriteCoordinator{}.apply(body, edits);
}

std::string variableNameForCollection(std::string collection, const std::string& body)
{
    const std::string collectionIdentifier = finalIdentifierFromCollection(collection);
    const std::size_t separator = collection.find_last_of(".>");
    if (separator != std::string::npos) {
        collection = collection.substr(separator + 1);
    }
    collection.erase(std::remove_if(collection.begin(), collection.end(), [](unsigned char character) {
                         return !std::isalnum(character) && character != '_';
                     }),
                     collection.end());
    if (collection.size() > 1 && collection.back() == 's') {
        collection.pop_back();
    }
    std::vector<std::string> candidates;
    candidates.push_back(collection.empty() ? "item" : collection);
    candidates.push_back("item");
    candidates.push_back("entry");
    candidates.push_back("value");
    candidates.push_back("element");
    for (const std::string& candidate : candidates) {
        if (!candidate.empty() && candidate != collectionIdentifier && !containsIdentifier(body, candidate)) {
            return candidate;
        }
    }
    return collectionIdentifier == "item" ? "element" : "item";
}

bool expressionUsesIdentifierAfter(const std::string& text, const std::string& identifier, std::size_t position)
{
    if (position >= text.size()) {
        return false;
    }
    return std::regex_search(text.substr(position), std::regex("\\b" + escapeRegex(identifier) + "\\b"));
}

bool isAllCapsConstantName(const std::string& identifier)
{
    bool sawLetter = false;
    for (const char character : identifier) {
        if (std::isalpha(static_cast<unsigned char>(character))) {
            sawLetter = true;
            if (!std::isupper(static_cast<unsigned char>(character))) {
                return false;
            }
        } else if (!std::isdigit(static_cast<unsigned char>(character)) && character != '_') {
            return false;
        }
    }
    return sawLetter;
}

bool isClassScopeSafeInitializer(const std::string& expression)
{
    const std::string value = trim(expression);
    if (value.empty()) {
        return false;
    }
    if (value == "true" || value == "false" || value == "nullptr") {
        return true;
    }
    if (std::regex_match(value, std::regex(R"([-+]?(0[xX][0-9A-Fa-f]+|\d+)([uUlLfF]*)?)"))) {
        return true;
    }
    if (std::regex_match(value, std::regex(R"([-+]?(\d+\.\d*|\.\d+)([fFlL]*)?)"))) {
        return true;
    }
    if (std::regex_match(value, std::regex(R"('([^'\\]|\\.)')"))
        || std::regex_match(value, std::regex(R"("([^"\\]|\\.)*")"))) {
        return true;
    }

    if (value.find("this") != std::string::npos
        || value.find('(') != std::string::npos
        || value.find('[') != std::string::npos
        || value.find('.') != std::string::npos
        || value.find("->") != std::string::npos) {
        return false;
    }

    const std::string withoutConstants = std::regex_replace(value,
                                                            std::regex(R"(\b(true|false|nullptr)\b|0[xX][0-9A-Fa-f]+|\d+(\.\d*)?)"),
                                                            "");
    const std::regex identifierPattern(R"(\b[A-Za-z_]\w*(?:::[A-Za-z_]\w*)?\b)");
    for (std::sregex_iterator it(withoutConstants.begin(), withoutConstants.end(), identifierPattern), end; it != end; ++it) {
        const std::string identifier = (*it)[0].str();
        const std::size_t scope = identifier.find("::");
        if (scope != std::string::npos) {
            const std::string left = identifier.substr(0, scope);
            const std::string right = identifier.substr(scope + 2);
            if (!left.empty() && std::isupper(static_cast<unsigned char>(left.front()))
                && !right.empty() && std::isupper(static_cast<unsigned char>(right.front()))) {
                continue;
            }
            return false;
        }
        if (!isAllCapsConstantName(identifier)) {
            return false;
        }
    }

    return true;
}

std::string rewriteStringViewSafety(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex functionPattern(
        R"((^[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+[A-Za-z_]\w*\s*\([^)]*?)std::string_view\s+([A-Za-z_]\w*)([^)]*\)\s*(const\s*)?\{([\s\S]*?)^\s*\}))",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, functionPattern)) {
        const std::string parameterName = match[2].str();
        const std::string body = match[5].str();
        const std::string cstrNeedle = parameterName + ".c_str()";
        if (body.find(cstrNeedle) == std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string replacement = match[0].str();
        const std::string beforeHeader = trim(replacement.substr(0, replacement.find('{')));
        replacement.replace(replacement.find("std::string_view " + parameterName),
                            std::string("std::string_view " + parameterName).size(),
                            "const std::string& " + parameterName);
        const std::string afterHeader = trim(replacement.substr(0, replacement.find('{')));
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "StringViewRollbackPass",
                         beforeHeader,
                         afterHeader,
                         "Rolled back std::string_view because the function body calls c_str(), which requires a null-terminated std::string lifetime.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::string rewriteIndexLoops(std::string code,
                              const ModernizationOptions& options,
                              std::vector<ConversionChange>& changes)
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    const std::regex loopPattern(
        R"((^[ \t]*)for\s*\(\s*(?:std::size_t|size_t|int|auto)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*([A-Za-z_]\w*)\.size\s*\(\s*\)\s*;\s*(\+\+\2|\2\+\+)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, loopPattern)) {
        const std::string indent = match[1].str();
        const std::string indexName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[5].str();
        if (body.find(".erase") != std::string::npos
            || body.find(".insert") != std::string::npos
            || body.find(collection + ".push_back") != std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string indexedExpression = escapeRegex(collection) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
        const std::string analysisBody = maskProtectedSource(body);
        const std::string bodyWithoutIndexed = std::regex_replace(analysisBody, std::regex(indexedExpression), "");
        if (std::regex_search(bodyWithoutIndexed, std::regex("\\b" + escapeRegex(indexName) + "\\b"))) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }
        const bool mutableElement = std::regex_search(analysisBody, std::regex(indexedExpression + R"(\s*(=|\+=|-=|\*=|/=|%=))"));
        const std::string itemName = variableNameForCollection(collection, body);
        body = replaceIndexedExpressionsWithRangeEdits(body, indexedExpression, itemName).code;
        const std::string replacement = indent + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + itemName + " : " + collection + ")\n" + indent + "{\n" + body + "\n" + indent + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Index loop to range-based for",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted an index loop to a range-based for loop because the index was only used for container element access.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::string rewriteMoveSemantics(std::string code,
                                 const ModernizationOptions& options,
                                 std::vector<ConversionChange>& changes)
{
    if (!options.useMoveSemantics) {
        return code;
    }

    std::string updated = code;
    const std::regex functionHeader(R"((^[ \t]*[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+[A-Za-z_]\w*\s*\([^;{}]*\)\s*(const\s*)?\{))",
                                    std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    bool changed = false;
    while (std::regex_search(search, match, functionHeader)) {
        const std::size_t headerStart = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = headerStart + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(updated, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }
        const std::string functionText = updated.substr(headerStart, closeBrace - headerStart + 1);
        const std::string header = match[1].str();
        const std::size_t parametersStart = header.find('(');
        const std::size_t parametersEnd = header.rfind(')');
        if (parametersStart == std::string::npos || parametersEnd == std::string::npos || parametersEnd <= parametersStart) {
            consumed = closeBrace + 1;
            search = updated.substr(consumed);
            continue;
        }

        const std::string parameters = header.substr(parametersStart + 1, parametersEnd - parametersStart - 1);
        std::vector<std::string> valueParameters;
        const std::regex parameterPattern(R"((std::[A-Za-z_][A-Za-z0-9_:<>]*|[A-Z][A-Za-z0-9_:<>]*)\s+([A-Za-z_]\w*)(\s*(,|$)))");
        for (std::sregex_iterator it(parameters.begin(), parameters.end(), parameterPattern), end; it != end; ++it) {
            const std::string full = (*it)[0].str();
            if (full.find('&') == std::string::npos && full.find('*') == std::string::npos && full.find("const") == std::string::npos) {
                valueParameters.push_back((*it)[2].str());
            }
        }

        std::string rewrittenFunction = functionText;
        bool functionChanged = false;
        for (const std::string& parameter : valueParameters) {
            const std::regex pushPattern(R"((\.\s*(push_back|emplace_back)\s*\(\s*))" + escapeRegex(parameter) + R"(\s*\))");
            std::smatch pushMatch;
            std::string body = rewrittenFunction;
            if (!std::regex_search(body, pushMatch, pushPattern)) {
                continue;
            }
            const std::size_t pushEnd = static_cast<std::size_t>(pushMatch.position() + pushMatch.length());
            if (expressionUsesIdentifierAfter(body, parameter, pushEnd)) {
                continue;
            }
            rewrittenFunction = std::regex_replace(rewrittenFunction,
                                                   pushPattern,
                                                   "$1std::move(" + parameter + "))",
                                                   std::regex_constants::format_first_only);
            addAppliedChange(changes,
                             "Move semantics for safe value transfer",
                             trim(pushMatch[0].str()),
                             trim(std::regex_replace(pushMatch[0].str(), std::regex(escapeRegex(parameter)), "std::move(" + parameter + ")")),
                             "Moved a by-value parameter into a container because the value is not used afterward.");
            functionChanged = true;
        }

        if (functionChanged) {
            updated.replace(headerStart, closeBrace - headerStart + 1, rewrittenFunction);
            changed = true;
            consumed = headerStart + rewrittenFunction.size();
        } else {
            consumed = closeBrace + 1;
        }
        search = updated.substr(consumed);
    }

    if (changed) {
        updated = IncludeManager().ensureInclude(updated, "#include <utility>");
    }
    return updated;
}

std::string rewriteManualSwap(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex swapPattern(
        R"((^[ \t]*)([A-Za-z_:][A-Za-z0-9_:<>,\s*&]*?)\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*;\s*\n\1\4\s*=\s*([A-Za-z_]\w*)\s*;\s*\n\1\5\s*=\s*\3\s*;)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    bool changed = false;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, swapPattern)) {
        if (match[4].str() == match[5].str()) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }
        const std::string replacement = match[1].str() + "std::swap(" + match[4].str() + ", " + match[5].str() + ");";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Manual temporary swap to std::swap",
                         trim(match[0].str()),
                         trim(replacement),
                         "Replaced a three-statement temporary swap with std::swap.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    if (changed) {
        code = IncludeManager().ensureInclude(code, "#include <utility>");
    }
    return code;
}

std::string rewriteNoexcept(std::string code, std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        if (codePart.find("noexcept") != std::string::npos
            || codePart.find("throw") != std::string::npos
            || codePart.find("new ") != std::string::npos
            || codePart.find("std::") != std::string::npos) {
            return line;
        }
        const std::regex getterPattern(R"(^([ \t]*(?:int|bool|long|double|float|std::size_t|size_t|auto)\s+[A-Za-z_]\w*\s*\([^)]*\)\s*(const\s*)?)\{\s*return\s+[^;{}]+\s*;\s*\}\s*$)");
        std::smatch match;
        if (!std::regex_match(codePart, match, getterPattern)) {
            return line;
        }
        const std::size_t bracePosition = codePart.find('{');
        if (bracePosition == std::string::npos) {
            return line;
        }
        const std::string directReplacement = codePart.substr(0, bracePosition) + "noexcept " + codePart.substr(bracePosition);
        addAppliedChange(changes,
                         "NoexceptModernizationPass",
                         trim(codePart),
                         trim(directReplacement),
                         "Added noexcept to a simple getter with no throwing operations.");
        changed = true;
        return directReplacement + trailingComment;
    });
    return changed ? updated : code;
}

struct ConstructorBlock
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

std::vector<ConstructorBlock> constructorsForClass(const std::string& classText, const std::string& className)
{
    std::vector<ConstructorBlock> constructors;
    const std::regex constructorHeader("\\b" + escapeRegex(className) + R"(\s*\([^;{}]*\)\s*(?::[^{]*)?\{)");
    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, constructorHeader)) {
        const std::size_t start = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = classText.find('{', start);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }
        constructors.push_back(ConstructorBlock{start, closeBrace + 1, classText.substr(start, closeBrace - start + 1)});
        consumed = closeBrace + 1;
        search = classText.substr(consumed);
    }
    return constructors;
}

std::string rewriteNsdmi(std::string code, std::vector<ConversionChange>& changes)
{
    std::vector<ClassBlock> classes = ClassResourceAnalyzer().analyzeClasses(code);
    std::sort(classes.begin(), classes.end(), [](const ClassBlock& left, const ClassBlock& right) {
        return left.start > right.start;
    });

    bool changed = false;
    for (const ClassBlock& block : classes) {
        std::string classText = code.substr(block.start, block.end - block.start);
        const std::vector<ConstructorBlock> constructors = constructorsForClass(classText, block.name);
        if (constructors.empty()) {
            continue;
        }

        const std::regex memberPattern(R"(^([ \t]*)(int|long|bool|double|float|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*;\s*$)",
                                       std::regex::ECMAScript | std::regex::multiline);
        std::vector<std::pair<std::string, std::string>> defaults;
        for (std::sregex_iterator it(classText.begin(), classText.end(), memberPattern), end; it != end; ++it) {
            const std::string member = (*it)[3].str();
            std::string assignedValue;
            bool allConstructorsAssign = true;
            for (const ConstructorBlock& constructor : constructors) {
                const std::regex assignmentPattern(R"(^[ \t]*)" + escapeRegex(member) + R"(\s*=\s*([^;\n]+)\s*;\s*$)",
                                                   std::regex::ECMAScript | std::regex::multiline);
                std::smatch assignmentMatch;
                if (!std::regex_search(constructor.text, assignmentMatch, assignmentPattern)) {
                    allConstructorsAssign = false;
                    break;
                }
                const std::string value = trim(assignmentMatch[1].str());
                if (assignedValue.empty()) {
                    assignedValue = value;
                } else if (assignedValue != value) {
                    allConstructorsAssign = false;
                    break;
                }
            }
            if (allConstructorsAssign && !assignedValue.empty() && isClassScopeSafeInitializer(assignedValue)) {
                defaults.push_back({member, assignedValue});
            }
        }

        if (defaults.empty()) {
            continue;
        }

        const std::string beforeClass = classText;
        for (const auto& [member, value] : defaults) {
            classText = std::regex_replace(classText,
                                           std::regex(R"(^([ \t]*)(int|long|bool|double|float|std::size_t|size_t)\s+)"
                                                          + escapeRegex(member) + R"(\s*;\s*$)",
                                                      std::regex::ECMAScript | std::regex::multiline),
                                           "$1$2 " + member + " = " + value + ";");
            classText = std::regex_replace(classText,
                                           std::regex(R"(^[ \t]*)" + escapeRegex(member) + R"(\s*=\s*)"
                                                          + escapeRegex(value) + R"(\s*;\s*\n?)",
                                                      std::regex::ECMAScript | std::regex::multiline),
                                           "");
        }
        if (classText != beforeClass) {
            code.replace(block.start, block.end - block.start, classText);
            addAppliedChange(changes,
                             "NsdmiModernizationPass",
                             "constructor default assignments",
                             "non-static data member initializers",
                             "Moved identical constructor default assignments into member declarations.");
            changed = true;
        }
    }
    return changed ? code : code;
}

std::string rewriteCHeaders(std::string code, std::vector<ConversionChange>& changes)
{
    const std::vector<std::pair<std::string, std::string>> headers{
        {"#include <assert.h>", "#include <cassert>"},
        {"#include <stdio.h>", "#include <cstdio>"},
        {"#include <stdlib.h>", "#include <cstdlib>"},
        {"#include <string.h>", "#include <cstring>"},
        {"#include <math.h>", "#include <cmath>"},
        {"#include <stdint.h>", "#include <cstdint>"},
        {"#include <stddef.h>", "#include <cstddef>"},
    };

    bool changed = false;
    for (const auto& [oldHeader, newHeader] : headers) {
        if (code.find(oldHeader) != std::string::npos) {
            code = std::regex_replace(code, std::regex(escapeRegex(oldHeader)), newHeader);
            addAppliedChange(changes,
                             "CHeaderModernizationPass",
                             oldHeader,
                             newHeader,
                             "Replaced a C standard library header with the C++ header form.");
            changed = true;
        }
    }

    const std::vector<std::string> symbols{"printf", "fprintf", "sprintf", "snprintf", "strlen", "strcpy", "strncpy", "strcmp", "malloc", "free"};
    for (const std::string& symbol : symbols) {
        const std::regex symbolPattern("(^|[^A-Za-z0-9_:])" + symbol + R"(\s*\()");
        const std::string before = code;
        code = std::regex_replace(code, symbolPattern, "$1std::" + symbol + "(");
        if (code != before) {
            changed = true;
        }
    }
    return changed ? code : code;
}

std::string rewriteEnumConstantHacks(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex enumConstantPattern(R"(enum\s*\{\s*([A-Za-z_]\w*)\s*=\s*([-+]?(0x[0-9A-Fa-f]+|\d+))\s*\}\s*;)");
    std::smatch match;
    bool changed = false;
    while (std::regex_search(code, match, enumConstantPattern)) {
        const std::string replacement = "static constexpr int " + match[1].str() + " = " + match[2].str() + ";";
        addAppliedChange(changes,
                         "CompileTimeConstantModernizationPass",
                         trim(match[0].str()),
                         replacement,
                         "Converted an enum constant hack to a constexpr integral constant.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
        changed = true;
    }
    return changed ? code : code;
}

void suggestTrailingReturnTypes(const std::string& code, std::vector<ConversionChange>& changes)
{
    const std::regex dependentReturnPattern(R"(\bdecltype\s*\()");
    for (std::sregex_iterator it(code.begin(), code.end(), dependentReturnPattern), end; it != end; ++it) {
        const std::size_t position = static_cast<std::size_t>(it->position());
        const std::size_t lineStart = code.rfind('\n', position) == std::string::npos ? 0 : code.rfind('\n', position) + 1;
        const std::size_t brace = code.find('{', position);
        const std::size_t semicolon = code.find(';', position);
        if (brace == std::string::npos || (semicolon != std::string::npos && semicolon < brace)) {
            continue;
        }
        const std::string signature = code.substr(lineStart, brace - lineStart + 1);
        if (signature.find("->") != std::string::npos || signature.find(')') == std::string::npos) {
            continue;
        }
        addSuggestion(changes,
                      "Trailing return type modernization",
                      trim(signature),
                      "A complex dependent decltype return could be clearer as a trailing return type, but this polish is suggestion-only by default.");
    }
}
} // namespace

std::string QualityModernizationPass::rewrite(const std::string& code,
                                              const ModernizationOptions& options,
                                              std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const bool aggressiveQuality = options.offlineModernizationLevel == OfflineModernizationLevel::AggressiveSafe
        || options.offlineModernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
    updated = rewriteStringViewSafety(updated, changes);
    updated = rewriteIndexLoops(updated, options, changes);
    updated = rewriteCHeaders(updated, changes);
    if (aggressiveQuality) {
        updated = rewriteMoveSemantics(updated, options, changes);
        updated = rewriteManualSwap(updated, changes);
        updated = rewriteNoexcept(updated, changes);
        updated = rewriteNsdmi(updated, changes);
        updated = rewriteEnumConstantHacks(updated, changes);
    }
    suggestTrailingReturnTypes(updated, changes);
    return updated;
}
