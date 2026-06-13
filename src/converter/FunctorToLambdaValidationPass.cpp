#include "converter/FunctorToLambdaValidationPass.h"

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

bool FunctorToLambdaValidationPass::isValid(const std::string& code, std::string& reason) const
{
    if (!bracesBalanced(code)) {
        reason = "functor-to-lambda rewrite produced unbalanced braces";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\bauto\s+[A-Za-z_]\w*\s*=\s*;)"))) {
        reason = "functor-to-lambda rewrite produced an empty lambda initializer";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\[[^\]]*=\s*(?:,|\]))"))) {
        reason = "functor-to-lambda rewrite produced an empty init-capture";
        return false;
    }
    if (std::regex_search(code, std::regex(R"(\[[^\]]*\]\s*\([^)]*\)\s*\{\s*return\s*;)"))) {
        reason = "functor-to-lambda rewrite produced an empty predicate return expression";
        return false;
    }

    reason.clear();
    return true;
}
