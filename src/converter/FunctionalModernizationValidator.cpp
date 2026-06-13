#include "converter/FunctionalModernizationValidator.h"

#include <regex>

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
    reason.clear();
    return true;
}
