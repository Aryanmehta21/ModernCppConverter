#include "converter/ModernizationPolishValidator.h"

#include "converter/ClassContextResolver.h"

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

bool ModernizationPolishValidator::isValid(const std::string& code, std::string& reason) const
{
    if (!bracesBalanced(code)) {
        reason = "Polish candidate produced unbalanced braces.";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\boverride\s+override\b)"))) {
        reason = "Polish candidate produced duplicate override specifiers.";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\bvirtual\s*virtual\b)"))) {
        reason = "Polish candidate produced duplicate virtual specifiers.";
        return false;
    }
    if (code.find("->static_cast") != std::string::npos || code.find(".static_cast") != std::string::npos) {
        reason = "Polish candidate produced invalid member-access cast syntax.";
        return false;
    }

    for (const ClassContext& context : ClassContextResolver().resolve(code)) {
        if (context.destructor.exists && context.confident && context.destructor.name != context.name) {
            reason = "Polish candidate produced a destructor whose name does not match the enclosing class.";
            return false;
        }
    }

    reason.clear();
    return true;
}
