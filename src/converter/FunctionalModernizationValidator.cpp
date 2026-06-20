#include "converter/FunctionalModernizationValidator.h"

#include <regex>
#include <set>

namespace
{
bool bracesBalanced(const std::string& code)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (const char character : code) {
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
            if (depth < 0) {
                return false;
            }
        }
    }
    return depth == 0;
}

std::set<std::string> collectPairLikeRangeContainers(const std::string& code)
{
    std::set<std::string> containers;
    const std::regex mapPattern(
        R"(\b(?:const\s+)?std::(?:unordered_)?map\s*<[^;\n{}]+>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), mapPattern), end; iterator != end; ++iterator) {
        containers.insert((*iterator)[1].str());
    }

    const std::regex pairContainerPattern(
        R"(\b(?:const\s+)?std::(?:vector|list|deque|set|unordered_set)\s*<\s*std::pair\s*<[^;\n{}]+>\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), pairContainerPattern), end; iterator != end; ++iterator) {
        containers.insert((*iterator)[1].str());
    }
    return containers;
}

bool streamsKnownPairRangeElementDirectly(const std::string& code)
{
    const std::set<std::string> pairLikeContainers = collectPairLikeRangeContainers(code);
    if (pairLikeContainers.empty()) {
        return false;
    }

    const std::regex rangeLoop(
        R"(for\s*\(\s*(?:const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\{([\s\S]*?)\})",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), rangeLoop), end; iterator != end; ++iterator) {
        const std::string element = (*iterator)[1].str();
        const std::string collection = (*iterator)[2].str();
        const std::string body = (*iterator)[3].str();
        if (pairLikeContainers.find(collection) == pairLikeContainers.end()) {
            continue;
        }
        if (std::regex_search(body, std::regex(R"(<<\s*)" + element + R"(\b(?!\s*(?:\.|->)))"))) {
            return true;
        }
    }
    return false;
}

bool containsSelfInitializingPointerOrReference(const std::string& code)
{
    return std::regex_search(
        code,
        std::regex(R"((^|\n)[ \t]*(?:const\s+)?(?:auto|[A-Za-z_:][A-Za-z0-9_:<>, \t]*(?:\s+const)?)\s*[&*]\s*([A-Za-z_]\w*)\s*=\s*\2\s*;)",
                   std::regex::ECMAScript));
}

bool rangeLoopBodyRedeclaresLoopVariable(const std::string& code)
{
    const std::regex rangeLoop(
        R"(for\s*\(\s*(?:const\s+)?auto\s*&?\s+([A-Za-z_]\w*)\s*:\s*[^)]+\)\s*\{([\s\S]*?)\n[ \t]*\})",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), rangeLoop), end; iterator != end; ++iterator) {
        const std::string variableName = (*iterator)[1].str();
        const std::string body = (*iterator)[2].str();
        const std::regex declarationPattern(
            R"((^|\n)[ \t]*(?:const\s+)?(?:auto|[A-Za-z_:][A-Za-z0-9_:<>, \t]*(?:\s+const)?)\s*(?:[&*]\s*)?)"
                + variableName
                + R"(\s*(?:=|;|\{))",
            std::regex::ECMAScript);
        if (std::regex_search(body, declarationPattern)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool FunctionalModernizationValidator::isValid(const std::string& code, std::string& reason) const
{
    if (!bracesBalanced(code)) {
        reason = "Functional modernization produced unbalanced braces.";
        return false;
    }
    if (code.find("->static_cast") != std::string::npos || code.find(".static_cast") != std::string::npos) {
        reason = "Functional modernization produced invalid member-access cast syntax.";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\[\s*key\s*,\s*key\s*\])"))) {
        reason = "Functional modernization produced duplicate structured binding names.";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\[\s*value\s*,\s*value\s*\])"))) {
        reason = "Functional modernization produced duplicate structured binding names.";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(for\s*\(\s*(?:const\s+)?auto\s*&?\s+([A-Za-z_]\w*)\s*:\s*\1\s*\))"))) {
        reason = "Functional modernization produced a range variable that shadows its container.";
        return false;
    }
    if (containsSelfInitializingPointerOrReference(code)) {
        reason = "Functional modernization produced a self-initializing pointer or reference variable.";
        return false;
    }
    if (rangeLoopBodyRedeclaresLoopVariable(code)) {
        reason = "Functional modernization produced a range loop variable that is redeclared inside the loop body.";
        return false;
    }
    if (streamsKnownPairRangeElementDirectly(code)) {
        reason = "Functional modernization left direct streaming of a pair-like range element.";
        return false;
    }
    reason.clear();
    return true;
}
