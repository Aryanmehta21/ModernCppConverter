#include "converter/ReturnTypePropagationPass.h"

#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct ConstReturnRule
{
    std::string valueType;
    std::string declarator;
    std::string functionName;
};

struct ReferenceReturnRule
{
    std::string functionName;
    bool isConst = false;
};

struct ConstPointerSymbol
{
    std::string valueType;
    std::string name;
};

struct ConstPointerContainer
{
    std::string valueType;
    std::string name;
};

struct RawPointerParameter
{
    std::string functionName;
    std::string valueType;
    std::string parameterName;
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

std::string typeRegex(const std::string& type)
{
    std::string pattern;
    pattern.reserve(type.size() * 2);
    bool previousWasSpace = false;
    for (const char character : type) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!previousWasSpace) {
                pattern += R"(\s+)";
            }
            previousWasSpace = true;
            continue;
        }
        previousWasSpace = false;
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            pattern.push_back('\\');
        }
        pattern.push_back(character);
    }
    return pattern;
}

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string before,
                      std::string after)
{
    changes.push_back(ConversionChange{
        "Const return receiver propagation",
        std::move(before),
        std::move(after),
        "Updated a direct receiving declaration after a visible function return type became const-qualified.",
        true,
        false,
    });
}

void addReferenceAppliedChange(std::vector<ConversionChange>& changes,
                               std::string before,
                               std::string after)
{
    changes.push_back(ConversionChange{
        "Reference return receiver propagation",
        std::move(before),
        std::move(after),
        "Updated a direct receiving declaration after a visible function return type became a standard-library reference.",
        true,
        false,
    });
}

void addConstContainerAppliedChange(std::vector<ConversionChange>& changes,
                                    std::string before,
                                    std::string after)
{
    changes.push_back(ConversionChange{
        "Const pointer container propagation",
        std::move(before),
        std::move(after),
        "Updated an observer container to store const pointers after a visible producer began returning const T*.",
        true,
        false,
    });
}

void addConstParameterAppliedChange(std::vector<ConversionChange>& changes,
                                    std::string before,
                                    std::string after)
{
    changes.push_back(ConversionChange{
        "Const pointer parameter propagation",
        std::move(before),
        std::move(after),
        "Updated a visible observer/predicate parameter to accept const T* after call sites began passing const pointer values.",
        true,
        false,
    });
}

void addConstIteratorAppliedChange(std::vector<ConversionChange>& changes,
                                   std::string before,
                                   std::string after)
{
    changes.push_back(ConversionChange{
        "Const pointer iterator propagation",
        std::move(before),
        std::move(after),
        "Updated an explicit iterator/local pointer declaration after an observer container began storing const pointers.",
        true,
        false,
    });
}

std::vector<ConstReturnRule> collectConstReturnRules(const std::string& code)
{
    std::vector<ConstReturnRule> rules;
    std::set<std::string> seen;

    const std::regex functionPattern(
        R"(\bconst\s+([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*([*&])\s*([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[^{}]+)?(?:\{|;))",
        std::regex::ECMAScript);

    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        ConstReturnRule rule{
            trim((*iterator)[1].str()),
            (*iterator)[2].str(),
            (*iterator)[3].str(),
        };
        if (rule.valueType.empty() || rule.functionName.empty()) {
            continue;
        }
        const std::string key = rule.valueType + rule.declarator + rule.functionName;
        if (seen.insert(key).second) {
            rules.push_back(std::move(rule));
        }
    }

    return rules;
}

std::vector<ConstPointerSymbol> collectConstPointerSymbols(const std::string& code)
{
    std::vector<ConstPointerSymbol> symbols;
    std::set<std::string> seen;
    const std::regex declarationPattern(
        R"(\bconst\s+([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*\*\s*([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        ConstPointerSymbol symbol{
            trim((*iterator)[1].str()),
            (*iterator)[2].str(),
        };
        const std::string key = symbol.valueType + "*" + symbol.name;
        if (!symbol.valueType.empty() && seen.insert(key).second) {
            symbols.push_back(std::move(symbol));
        }
    }
    return symbols;
}

std::vector<ConstPointerContainer> collectConstPointerContainers(const std::string& code)
{
    std::vector<ConstPointerContainer> containers;
    std::set<std::string> seen;
    const std::regex declarationPattern(
        R"(\bstd::vector\s*<\s*const\s+([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*\*\s*>\s+([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        ConstPointerContainer container{
            trim((*iterator)[1].str()),
            (*iterator)[2].str(),
        };
        const std::string key = container.valueType + "*" + container.name;
        if (!container.valueType.empty() && seen.insert(key).second) {
            containers.push_back(std::move(container));
        }
    }
    return containers;
}

bool initializerCallsFunction(const std::string& initializer, const std::string& functionName)
{
    const std::regex callPattern(R"((?:^|[^A-Za-z0-9_:])(?:[A-Za-z_]\w*(?:\s*(?:\.|->|::)\s*)?)?)"
                                 + escapeRegex(functionName)
                                 + R"(\s*\()",
                                 std::regex::ECMAScript);
    return std::regex_search(initializer, callPattern);
}

bool sameType(const std::string& left, const std::string& right)
{
    auto compact = [](std::string value) {
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
                        return std::isspace(character) != 0;
                    }),
                    value.end());
        return value;
    };
    return compact(left) == compact(right);
}

bool expressionIsConstPointerValue(const std::string& expression,
                                   const std::string& valueType,
                                   const std::vector<ConstReturnRule>& rules,
                                   const std::vector<ConstPointerSymbol>& symbols,
                                   const std::vector<ConstPointerContainer>& containers)
{
    const std::string candidate = trim(expression);
    if (candidate.empty()) {
        return false;
    }

    for (const ConstPointerSymbol& symbol : symbols) {
        if (sameType(symbol.valueType, valueType) && candidate == symbol.name) {
            return true;
        }
    }

    for (const ConstPointerContainer& container : containers) {
        if (!sameType(container.valueType, valueType)) {
            continue;
        }
        const std::regex elementPattern(R"(^)" + escapeRegex(container.name) + R"(\s*\[[^\]]+\]\s*$)",
                                        std::regex::ECMAScript);
        if (std::regex_match(candidate, elementPattern)) {
            return true;
        }
    }

    for (const ConstReturnRule& rule : rules) {
        if (rule.declarator == "*" && sameType(rule.valueType, valueType)
            && initializerCallsFunction(candidate, rule.functionName)) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> splitArguments(const std::string& text)
{
    std::vector<std::string> arguments;
    std::size_t start = 0;
    int parenDepth = 0;
    int angleDepth = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '(' || character == '[' || character == '{') {
            ++parenDepth;
        } else if (character == ')' || character == ']' || character == '}') {
            --parenDepth;
        } else if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (character == ',' && parenDepth == 0 && angleDepth == 0) {
            arguments.push_back(trim(text.substr(start, index - start)));
            start = index + 1;
        }
    }
    arguments.push_back(trim(text.substr(start)));
    return arguments;
}

std::size_t findMatchingCloseParen(const std::string& text, const std::size_t openParen)
{
    if (openParen >= text.size() || text[openParen] != '(') {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    for (std::size_t index = openParen; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inChar)) {
            escaped = true;
            continue;
        }
        if (character == '"' && !inChar) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inChar = !inChar;
            continue;
        }
        if (inString || inChar) {
            continue;
        }
        if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

bool isIdentifierBoundary(const std::string& text, const std::size_t position, const std::size_t length)
{
    const bool leftBoundary = position == 0
        || (std::isalnum(static_cast<unsigned char>(text[position - 1])) == 0 && text[position - 1] != '_');
    const std::size_t right = position + length;
    const bool rightBoundary = right >= text.size()
        || (std::isalnum(static_cast<unsigned char>(text[right])) == 0 && text[right] != '_');
    return leftBoundary && rightBoundary;
}

std::vector<std::string> extractCallArgumentLists(const std::string& text, const std::string& functionName)
{
    std::vector<std::string> arguments;
    std::size_t searchPosition = 0;
    while (searchPosition < text.size()) {
        const std::size_t namePosition = text.find(functionName, searchPosition);
        if (namePosition == std::string::npos) {
            break;
        }
        searchPosition = namePosition + functionName.size();
        if (!isIdentifierBoundary(text, namePosition, functionName.size())) {
            continue;
        }
        std::size_t openParen = searchPosition;
        while (openParen < text.size() && std::isspace(static_cast<unsigned char>(text[openParen])) != 0) {
            ++openParen;
        }
        if (openParen >= text.size() || text[openParen] != '(') {
            continue;
        }
        const std::size_t closeParen = findMatchingCloseParen(text, openParen);
        if (closeParen == std::string::npos) {
            continue;
        }
        arguments.push_back(text.substr(openParen + 1, closeParen - openParen - 1));
        searchPosition = closeParen + 1;
    }
    return arguments;
}

std::vector<std::string> extractMemberCallArgumentLists(const std::string& text,
                                                        const std::string& objectName,
                                                        const std::string& functionName)
{
    std::vector<std::string> arguments;
    std::size_t searchPosition = 0;
    while (searchPosition < text.size()) {
        const std::size_t objectPosition = text.find(objectName, searchPosition);
        if (objectPosition == std::string::npos) {
            break;
        }
        searchPosition = objectPosition + objectName.size();
        if (!isIdentifierBoundary(text, objectPosition, objectName.size())) {
            continue;
        }
        std::size_t cursor = searchPosition;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '.') {
            continue;
        }
        ++cursor;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (text.compare(cursor, functionName.size(), functionName) != 0
            || !isIdentifierBoundary(text, cursor, functionName.size())) {
            continue;
        }
        cursor += functionName.size();
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '(') {
            continue;
        }
        const std::size_t closeParen = findMatchingCloseParen(text, cursor);
        if (closeParen == std::string::npos) {
            continue;
        }
        arguments.push_back(text.substr(cursor + 1, closeParen - cursor - 1));
        searchPosition = closeParen + 1;
    }
    return arguments;
}

std::vector<ReferenceReturnRule> collectReferenceReturnRules(const std::string& code)
{
    std::vector<ReferenceReturnRule> rules;
    std::set<std::string> seen;
    const std::regex functionPattern(
        R"(\b(const\s+)?(?:std::(?:vector|array)\s*<[^;{}()]+>|std::(?:unique_ptr|shared_ptr)\s*<[^;{}()]+>|std::string)\s*&\s*([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[^{}]+)?\{)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        ReferenceReturnRule rule{
            (*iterator)[2].str(),
            (*iterator)[1].matched,
        };
        if (!rule.functionName.empty() && seen.insert(rule.functionName).second) {
            rules.push_back(std::move(rule));
        }
    }
    return rules;
}

std::string rewriteLineForRule(const std::string& line,
                               const ConstReturnRule& rule,
                               bool& changed,
                               std::vector<ConversionChange>& changes)
{
    std::string trailingComment;
    const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    if (codePart.find('=') == std::string::npos || codePart.find(rule.functionName) == std::string::npos) {
        return line;
    }

    const std::regex declarationPattern(
        R"(^([ \t]*)(?!const\b))" + typeRegex(rule.valueType)
            + R"(\s*)" + escapeRegex(rule.declarator)
            + R"(\s*([A-Za-z_]\w*)\s*=\s*(.+;\s*)$)",
        std::regex::ECMAScript);

    std::smatch match;
    if (!std::regex_match(codePart, match, declarationPattern)) {
        return line;
    }

    const std::string variableName = match[2].str();
    const std::string initializer = match[3].str();
    if (!initializerCallsFunction(initializer, rule.functionName)) {
        return line;
    }

    const std::string replacement = match[1].str() + "const " + rule.valueType + rule.declarator
        + " " + variableName + " = " + initializer;
    if (trim(replacement) == trim(codePart)) {
        return line;
    }

    changed = true;
    addAppliedChange(changes, trim(codePart), trim(replacement));
    return replacement + trailingComment;
}

std::string rewriteAutoPointerLineForRule(const std::string& line,
                                          const ConstReturnRule& rule,
                                          bool& changed,
                                          std::vector<ConversionChange>& changes)
{
    std::string trailingComment;
    const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    if (codePart.find('=') == std::string::npos || codePart.find(rule.functionName) == std::string::npos) {
        return line;
    }

    const std::regex declarationPattern(
        R"(^([ \t]*)(?!const\b)auto\s*\*\s*([A-Za-z_]\w*)\s*=\s*(.+;\s*)$)",
        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(codePart, match, declarationPattern)) {
        return line;
    }

    const std::string initializer = match[3].str();
    if (!initializerCallsFunction(initializer, rule.functionName)) {
        return line;
    }

    const std::string replacement = match[1].str() + "const auto* " + match[2].str() + " = " + initializer;
    if (trim(replacement) == trim(codePart)) {
        return line;
    }

    changed = true;
    addAppliedChange(changes, trim(codePart), trim(replacement));
    return replacement + trailingComment;
}

std::set<std::string> findContainersNeedingConstPointers(const std::string& code,
                                                         const std::vector<ConstReturnRule>& rules,
                                                         const std::vector<ConstPointerSymbol>& symbols,
                                                         const std::vector<ConstPointerContainer>& containers)
{
    std::set<std::string> names;
    const std::regex rawPointerVectorPattern(
        R"(\bstd::vector\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*\*\s*>\s+([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), rawPointerVectorPattern), end; iterator != end; ++iterator) {
        const std::string valueType = trim((*iterator)[1].str());
        const std::string containerName = (*iterator)[2].str();
        for (const char* mutator : {"push_back", "emplace_back"}) {
            for (const std::string& argumentList : extractMemberCallArgumentLists(code, containerName, mutator)) {
                for (const std::string& argument : splitArguments(argumentList)) {
                    if (expressionIsConstPointerValue(argument, valueType, rules, symbols, containers)) {
                        names.insert(containerName);
                        break;
                    }
                }
                if (names.find(containerName) != names.end()) {
                    break;
                }
            }
        }
    }
    return names;
}

std::string rewriteConstPointerContainerDeclaration(const std::string& line,
                                                    const std::set<std::string>& containersToConst,
                                                    bool& changed,
                                                    std::vector<ConversionChange>& changes)
{
    if (containersToConst.empty()) {
        return line;
    }

    std::string trailingComment;
    std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    const std::string before = codePart;

    for (const std::string& containerName : containersToConst) {
        if (codePart.find(containerName) == std::string::npos) {
            continue;
        }
        const std::regex declarationPattern(
            R"(\bstd::vector\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*\*\s*>\s+)"
                + escapeRegex(containerName)
                + R"(\b)",
            std::regex::ECMAScript);
        std::smatch match;
        if (!std::regex_search(codePart, match, declarationPattern)) {
            continue;
        }
        const std::string replacement = "std::vector<const " + trim(match[1].str()) + "*> " + containerName;
        codePart.replace(static_cast<std::size_t>(match.position()),
                         static_cast<std::size_t>(match.length()),
                         replacement);
    }

    if (codePart != before) {
        changed = true;
        addConstContainerAppliedChange(changes, trim(before), trim(codePart));
    }
    return codePart + trailingComment;
}

std::vector<RawPointerParameter> collectRawPointerParameters(const std::string& code)
{
    std::vector<RawPointerParameter> parameters;
    std::set<std::string> seen;
    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*\s+)+([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?(?:\{|;))",
        std::regex::ECMAScript);
    const std::regex parameterPattern(
        R"((?:^|,)\s*(?!const\b)([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*\*\s*([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        const std::string functionName = (*iterator)[1].str();
        const std::string parameterText = (*iterator)[2].str();
        for (std::sregex_iterator parameter(parameterText.begin(), parameterText.end(), parameterPattern), paramEnd; parameter != paramEnd; ++parameter) {
            RawPointerParameter rawParameter{
                functionName,
                trim((*parameter)[1].str()),
                (*parameter)[2].str(),
            };
            const std::string key = rawParameter.functionName + "|" + rawParameter.valueType + "|" + rawParameter.parameterName;
            if (seen.insert(key).second) {
                parameters.push_back(std::move(rawParameter));
            }
        }
    }
    return parameters;
}

bool isLikelySignatureLine(const std::string& codePart, const std::string& functionName)
{
    const std::size_t namePosition = codePart.find(functionName);
    if (namePosition == std::string::npos) {
        return false;
    }
    const std::string prefix = trim(codePart.substr(0, namePosition));
    if (prefix.empty()
        || prefix == "if"
        || prefix == "for"
        || prefix == "while"
        || prefix == "switch"
        || prefix == "return") {
        return false;
    }
    const std::size_t closeParen = codePart.find(')', namePosition);
    if (closeParen == std::string::npos) {
        return false;
    }
    const std::string suffix = trim(codePart.substr(closeParen + 1));
    return suffix.empty()
        || suffix == ";"
        || suffix == "{"
        || suffix.starts_with("const")
        || suffix.starts_with("noexcept")
        || suffix.starts_with("override")
        || suffix.starts_with("{");
}

bool callUsesConstPointerArgument(const std::string& code,
                                  const RawPointerParameter& parameter,
                                  const std::vector<ConstReturnRule>& rules,
                                  const std::vector<ConstPointerSymbol>& symbols,
                                  const std::vector<ConstPointerContainer>& containers)
{
    std::stringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        if (isLikelySignatureLine(codePart, parameter.functionName)) {
            continue;
        }
        for (const std::string& argumentList : extractCallArgumentLists(codePart, parameter.functionName)) {
            for (const std::string& argument : splitArguments(argumentList)) {
                if (expressionIsConstPointerValue(argument, parameter.valueType, rules, symbols, containers)) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::set<std::pair<std::string, std::string>> findParametersNeedingConstPointers(
    const std::string& code,
    const std::vector<ConstReturnRule>& rules,
    const std::vector<ConstPointerSymbol>& symbols,
    const std::vector<ConstPointerContainer>& containers)
{
    std::set<std::pair<std::string, std::string>> targets;
    for (const RawPointerParameter& parameter : collectRawPointerParameters(code)) {
        if (callUsesConstPointerArgument(code, parameter, rules, symbols, containers)) {
            targets.emplace(parameter.functionName, parameter.valueType);
        }
    }
    return targets;
}

bool replaceParameterListType(std::string& codePart, const std::string& functionName, const std::string& valueType)
{
    const std::size_t namePosition = codePart.find(functionName);
    if (namePosition == std::string::npos || !isLikelySignatureLine(codePart, functionName)) {
        return false;
    }
    const std::size_t openParen = codePart.find('(', namePosition);
    const std::size_t closeParen = codePart.find(')', openParen);
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen) {
        return false;
    }
    std::string parameters = codePart.substr(openParen + 1, closeParen - openParen - 1);
    const std::string before = parameters;
    const std::regex parameterPattern(
        R"((^|,\s*)(?!const\b))" + typeRegex(valueType) + R"(\s*\*\s*([A-Za-z_]\w*))",
        std::regex::ECMAScript);
    parameters = std::regex_replace(parameters, parameterPattern, "$1const " + valueType + "* $2");
    if (parameters == before) {
        return false;
    }
    codePart.replace(openParen + 1, closeParen - openParen - 1, parameters);
    return true;
}

bool replaceOperatorParameterListType(std::string& codePart, const std::string& valueType)
{
    if (codePart.find("operator()") == std::string::npos) {
        return false;
    }
    const std::size_t operatorPosition = codePart.find("operator()");
    const std::size_t openParen = codePart.find('(', operatorPosition + std::string("operator()").size());
    const std::size_t closeParen = codePart.find(')', openParen);
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen) {
        return false;
    }
    std::string parameters = codePart.substr(openParen + 1, closeParen - openParen - 1);
    const std::string before = parameters;
    const std::regex parameterPattern(
        R"((^|,\s*)(?!const\b))" + typeRegex(valueType) + R"(\s*\*\s*([A-Za-z_]\w*))",
        std::regex::ECMAScript);
    parameters = std::regex_replace(parameters, parameterPattern, "$1const " + valueType + "* $2");
    if (parameters == before) {
        return false;
    }
    codePart.replace(openParen + 1, closeParen - openParen - 1, parameters);
    return true;
}

std::string rewriteConstPointerParameterLine(const std::string& line,
                                             const std::set<std::pair<std::string, std::string>>& parameterTargets,
                                             const std::set<std::string>& constPointerTypes,
                                             bool& changed,
                                             std::vector<ConversionChange>& changes)
{
    if (parameterTargets.empty() && constPointerTypes.empty()) {
        return line;
    }

    std::string trailingComment;
    std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    const std::string before = codePart;

    for (const auto& target : parameterTargets) {
        replaceParameterListType(codePart, target.first, target.second);
    }
    if (codePart.find("operator()") != std::string::npos) {
        for (const std::string& valueType : constPointerTypes) {
            replaceOperatorParameterListType(codePart, valueType);
        }
    }

    if (codePart != before) {
        changed = true;
        addConstParameterAppliedChange(changes, trim(before), trim(codePart));
    }
    return codePart + trailingComment;
}

std::string rewriteConstPointerIteratorLine(const std::string& line,
                                            const std::vector<ConstPointerContainer>& containers,
                                            std::map<std::string, std::string>& iteratorValueTypes,
                                            bool& changed,
                                            std::vector<ConversionChange>& changes)
{
    if (containers.empty()) {
        return line;
    }

    std::string trailingComment;
    std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    const std::string before = codePart;

    for (const ConstPointerContainer& container : containers) {
        if (codePart.find(container.name) == std::string::npos) {
            continue;
        }

        const std::regex iteratorDeclaration(
            R"(^([ \t]*(?:for\s*\(\s*)?)std::vector\s*<\s*)"
                + typeRegex(container.valueType)
                + R"(\s*\*\s*>\s*(::\s*(?:const_)?iterator)\s+([A-Za-z_]\w*)\s*=\s*)"
                + escapeRegex(container.name)
                + R"(\s*\.\s*(c?begin)\s*\(\s*\)(.*)$)",
            std::regex::ECMAScript);
        std::smatch match;
        if (!std::regex_match(codePart, match, iteratorDeclaration)) {
            continue;
        }

        const std::string iteratorName = match[3].str();
        iteratorValueTypes[iteratorName] = container.valueType;
        const std::string beginCall = match[4].str();
        const std::string suffix = match[5].str();
        const bool constIterator = match[2].str().find("const_iterator") != std::string::npos || beginCall == "cbegin";
        const std::string iteratorKind = constIterator ? "const_iterator" : "iterator";
        codePart = match[1].str()
            + "std::vector<const " + container.valueType + "*>::" + iteratorKind
            + " " + iteratorName + " = " + container.name + "." + beginCall + "()" + suffix;
        break;
    }

    if (codePart == before) {
        return line;
    }

    changed = true;
    addConstIteratorAppliedChange(changes, trim(before), trim(codePart));
    return codePart + trailingComment;
}

std::string rewriteConstPointerIteratorLocalLine(const std::string& line,
                                                 const std::map<std::string, std::string>& iteratorValueTypes,
                                                 bool& changed,
                                                 std::vector<ConversionChange>& changes)
{
    if (iteratorValueTypes.empty()) {
        return line;
    }

    std::string trailingComment;
    std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    const std::string before = codePart;

    for (const auto& [iteratorName, valueType] : iteratorValueTypes) {
        if (codePart.find(iteratorName) == std::string::npos) {
            continue;
        }
        const std::regex localPointerDeclaration(
            R"(^([ \t]*)(?!const\b))"
                + typeRegex(valueType)
                + R"(\s*\*\s*([A-Za-z_]\w*)\s*=\s*\*\s*)"
                + escapeRegex(iteratorName)
                + R"(\s*;\s*$)",
            std::regex::ECMAScript);
        std::smatch match;
        if (!std::regex_match(codePart, match, localPointerDeclaration)) {
            continue;
        }
        codePart = match[1].str() + "const " + valueType + "* " + match[2].str() + " = *" + iteratorName + ";";
        break;
    }

    if (codePart == before) {
        return line;
    }

    changed = true;
    addConstIteratorAppliedChange(changes, trim(before), trim(codePart));
    return codePart + trailingComment;
}

std::string rewriteLineForReferenceRule(const std::string& line,
                                        const ReferenceReturnRule& rule,
                                        bool& changed,
                                        std::vector<ConversionChange>& changes)
{
    std::string trailingComment;
    const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
    if (codePart.find('=') == std::string::npos || codePart.find(rule.functionName) == std::string::npos) {
        return line;
    }

    const std::regex alreadyReferencePattern(R"(^[ \t]*(?:const\s+)?auto\s*&)");
    if (std::regex_search(codePart, alreadyReferencePattern)) {
        return line;
    }
    if (trim(codePart).starts_with("const ")) {
        return line;
    }

    const std::regex declarationPattern(
        R"(^([ \t]*)(?:const\s+)?[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*\s+([A-Za-z_]\w*)\s*=\s*(.+;\s*)$)",
        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(codePart, match, declarationPattern)) {
        return line;
    }

    const std::string initializer = match[3].str();
    if (!initializerCallsFunction(initializer, rule.functionName)) {
        return line;
    }

    const std::string replacement = match[1].str()
        + (rule.isConst ? "const auto& " : "auto& ")
        + match[2].str()
        + " = "
        + initializer;
    if (trim(replacement) == trim(codePart)) {
        return line;
    }

    changed = true;
    addReferenceAppliedChange(changes, trim(codePart), trim(replacement));
    return replacement + trailingComment;
}
} // namespace

std::string ReturnTypePropagationPass::rewrite(const std::string& code,
                                               std::vector<ConversionChange>& changes) const
{
    const std::vector<ConstReturnRule> rules = collectConstReturnRules(code);
    const std::vector<ReferenceReturnRule> referenceRules = collectReferenceReturnRules(code);
    if (rules.empty() && referenceRules.empty()) {
        return code;
    }

    bool changed = false;
    const SafeReplacementEngine safeReplacement;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string rewritten = line;
        for (const ConstReturnRule& rule : rules) {
            rewritten = rewriteLineForRule(rewritten, rule, changed, changes);
            if (rule.declarator == "*") {
                rewritten = rewriteAutoPointerLineForRule(rewritten, rule, changed, changes);
            }
        }
        for (const ReferenceReturnRule& rule : referenceRules) {
            rewritten = rewriteLineForReferenceRule(rewritten, rule, changed, changes);
        }
        return rewritten;
    });

    const std::vector<ConstPointerSymbol> constPointerSymbols = collectConstPointerSymbols(updated);
    const std::vector<ConstPointerContainer> initialConstContainers = collectConstPointerContainers(updated);
    const std::set<std::string> containersToConst = findContainersNeedingConstPointers(updated,
                                                                                       rules,
                                                                                       constPointerSymbols,
                                                                                       initialConstContainers);
    if (!containersToConst.empty()) {
        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            return rewriteConstPointerContainerDeclaration(line, containersToConst, changed, changes);
        });
    }

    std::set<std::string> constPointerTypes;
    for (const ConstReturnRule& rule : rules) {
        if (rule.declarator == "*") {
            constPointerTypes.insert(rule.valueType);
        }
    }
    for (const ConstPointerSymbol& symbol : collectConstPointerSymbols(updated)) {
        constPointerTypes.insert(symbol.valueType);
    }
    const std::vector<ConstPointerContainer> constContainers = collectConstPointerContainers(updated);
    for (const ConstPointerContainer& container : constContainers) {
        constPointerTypes.insert(container.valueType);
    }

    if (!constContainers.empty()) {
        std::map<std::string, std::string> iteratorValueTypes;
        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            return rewriteConstPointerIteratorLine(line, constContainers, iteratorValueTypes, changed, changes);
        });
        if (!iteratorValueTypes.empty()) {
            updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
                return rewriteConstPointerIteratorLocalLine(line, iteratorValueTypes, changed, changes);
            });
        }
    }

    const std::set<std::pair<std::string, std::string>> parameterTargets = findParametersNeedingConstPointers(updated,
                                                                                                              rules,
                                                                                                              collectConstPointerSymbols(updated),
                                                                                                              constContainers);
    if (!parameterTargets.empty() || !constPointerTypes.empty()) {
        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            return rewriteConstPointerParameterLine(line, parameterTargets, constPointerTypes, changed, changes);
        });
    }

    return changed ? updated : code;
}
