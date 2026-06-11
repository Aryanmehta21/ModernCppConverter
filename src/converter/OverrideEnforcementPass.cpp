#include "converter/OverrideEnforcementPass.h"

#include "converter/ClassContextResolver.h"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct MethodSignature
{
    std::string name;
    std::string parameters;
    bool isConst = false;
    std::string noexceptText;
    std::string refQualifier;
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

std::vector<std::string> splitParameters(const std::string& parameterList)
{
    std::vector<std::string> parameters;
    std::string current;
    int angleDepth = 0;
    for (const char character : parameterList) {
        if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        }
        if (character == ',' && angleDepth == 0) {
            parameters.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!trim(current).empty()) {
        parameters.push_back(trim(current));
    }
    return parameters;
}

std::string normalizeParameterList(const std::string& parameterList)
{
    std::ostringstream output;
    bool first = true;
    for (std::string parameter : splitParameters(parameterList)) {
        parameter = std::regex_replace(parameter, std::regex(R"(\s*=\s*.*$)"), "");
        parameter = std::regex_replace(parameter, std::regex(R"((.+[&*\w:>])\s+[A-Za-z_]\w*$)"), "$1");
        parameter = std::regex_replace(trim(parameter), std::regex(R"(\s+)"), " ");
        parameter = std::regex_replace(parameter, std::regex(R"(\s*([*&])\s*)"), "$1");
        if (!first) {
            output << ",";
        }
        first = false;
        output << parameter;
    }
    return output.str();
}

std::vector<MethodSignature> collectVirtualMethods(const std::string& classText)
{
    std::vector<MethodSignature> methods;
    const std::regex methodPattern(
        R"(\bvirtual\s+(?!~)(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(const)?\s*(noexcept(?:\s*\([^)]*\))?)?\s*(&&|&)?\s*(?:=\s*0)?\s*[;{])",
        std::regex::ECMAScript);
    for (std::sregex_iterator it(classText.begin(), classText.end(), methodPattern), end; it != end; ++it) {
        methods.push_back(MethodSignature{
            (*it)[1].str(),
            normalizeParameterList((*it)[2].str()),
            (*it)[3].matched,
            trim((*it)[4].str()),
            trim((*it)[5].str()),
        });
    }
    return methods;
}
} // namespace

std::string OverrideEnforcementPass::rewrite(const std::string& code,
                                             std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    std::vector<ClassContext> contexts = ClassContextResolver().resolve(updated);
    std::map<std::string, std::vector<MethodSignature>> virtualMethodsByClass;
    for (const ClassContext& context : contexts) {
        if (context.confident) {
            virtualMethodsByClass[context.name] = collectVirtualMethods(context.block.text);
        }
    }

    std::sort(contexts.begin(), contexts.end(), [](const ClassContext& left, const ClassContext& right) {
        return left.block.start > right.block.start;
    });

    for (const ClassContext& context : contexts) {
        if (!context.confident || context.baseNames.empty()) {
            continue;
        }
        const auto baseIt = virtualMethodsByClass.find(context.baseNames.front());
        if (baseIt == virtualMethodsByClass.end()) {
            continue;
        }

        std::string classText = updated.substr(context.block.start, context.block.end - context.block.start);
        for (const MethodSignature& baseMethod : baseIt->second) {
            const std::regex derivedPattern(
                R"((^[ \t]*)(?:virtual\s+)?((?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+))"
                    + escapeRegex(baseMethod.name)
                    + R"(\s*\(([^)]*)\)\s*(const)?\s*(noexcept(?:\s*\([^)]*\))?)?\s*(&&|&)?\s*(?![^;\n{]*\boverride\b)([;{]))",
                std::regex::ECMAScript | std::regex::multiline);

            std::smatch match;
            std::string search = classText;
            std::size_t consumed = 0;
            while (std::regex_search(search, match, derivedPattern)) {
                if (normalizeParameterList(match[3].str()) != baseMethod.parameters
                    || static_cast<bool>(match[4].matched) != baseMethod.isConst
                    || trim(match[5].str()) != baseMethod.noexceptText
                    || trim(match[6].str()) != baseMethod.refQualifier) {
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }

                const std::string before = match[0].str();
                const std::string after = match[1].str()
                    + match[2].str()
                    + baseMethod.name + "(" + match[3].str() + ")"
                    + (baseMethod.isConst ? " const" : "")
                    + (baseMethod.noexceptText.empty() ? "" : " " + baseMethod.noexceptText)
                    + (baseMethod.refQualifier.empty() ? "" : " " + baseMethod.refQualifier)
                    + " override" + match[7].str();
                classText.replace(consumed + static_cast<std::size_t>(match.position()),
                                  static_cast<std::size_t>(match.length()),
                                  after);
                addAppliedChange(changes,
                                 "Override enforcement",
                                 trim(before),
                                 trim(after),
                                 "Added override to a derived method that exactly matches a visible base virtual signature.");
                consumed += static_cast<std::size_t>(match.position()) + after.size();
                search = classText.substr(consumed);
            }
        }
        updated.replace(context.block.start, context.block.end - context.block.start, classText);
    }

    return updated;
}

