#include "converter/ReturnTypePropagationPass.h"

#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
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

std::vector<ConstReturnRule> collectConstReturnRules(const std::string& code)
{
    std::vector<ConstReturnRule> rules;
    std::set<std::string> seen;

    const std::regex functionPattern(
        R"(\bconst\s+([A-Za-z_:][A-Za-z0-9_:]*(?:::[A-Za-z_]\w*)?(?:\s*<[^;\n{}()]+>)?)\s*([*&])\s*([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[^{}]+)?\{)",
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

bool initializerCallsFunction(const std::string& initializer, const std::string& functionName)
{
    const std::regex callPattern(R"((?:^|[^A-Za-z0-9_:])(?:[A-Za-z_]\w*(?:\s*(?:\.|->|::)\s*)?)?)"
                                 + escapeRegex(functionName)
                                 + R"(\s*\()",
                                 std::regex::ECMAScript);
    return std::regex_search(initializer, callPattern);
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
    const std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string rewritten = line;
        for (const ConstReturnRule& rule : rules) {
            rewritten = rewriteLineForRule(rewritten, rule, changed, changes);
        }
        for (const ReferenceReturnRule& rule : referenceRules) {
            rewritten = rewriteLineForReferenceRule(rewritten, rule, changed, changes);
        }
        return rewritten;
    });

    return changed ? updated : code;
}
