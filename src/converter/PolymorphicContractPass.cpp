#include "converter/PolymorphicContractPass.h"

#include "converter/ClassContextResolver.h"
#include "converter/DestructorDeclarationValidationPass.h"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct VirtualMethod
{
    std::string name;
    std::string normalizedParameters;
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

std::vector<VirtualMethod> virtualMethods(const std::string& classText)
{
    std::vector<VirtualMethod> methods;
    const std::regex virtualMethodPattern(
        R"(\bvirtual\s+(?!~)(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(const)?\s*(?:=\s*0)?\s*[;{])");
    for (std::sregex_iterator iterator(classText.begin(), classText.end(), virtualMethodPattern), end; iterator != end; ++iterator) {
        methods.push_back(VirtualMethod{
            (*iterator)[1].str(),
            normalizeParameterList((*iterator)[2].str()),
            (*iterator)[3].matched,
        });
    }
    return methods;
}

std::string ensureVirtualDestructor(std::string classText,
                                    const ClassContext& context,
                                    std::vector<ConversionChange>& changes)
{
    if (!context.hasVirtualMethods) {
        return classText;
    }

    if (!context.confident || context.name.empty()) {
        addSuggestion(changes,
                      "Add virtual destructor for polymorphic base",
                      trim(context.block.text),
                      "Skipped virtual destructor insertion because the enclosing class name could not be resolved confidently from the class declaration header.");
        return classText;
    }

    const std::regex destructorPattern(R"((?:virtual\s+)?~)" + escapeRegex(context.name) + R"(\s*\()");
    std::smatch match;
    if (std::regex_search(classText, match, destructorPattern)) {
        const std::string destructorText = match[0].str();
        if (destructorText.find("virtual") != std::string::npos) {
            return classText;
        }
        classText.replace(static_cast<std::size_t>(match.position()),
                          static_cast<std::size_t>(match.length()),
                          "virtual ~" + context.name + "(");
        addAppliedChange(changes,
                         "Add virtual destructor for polymorphic base",
                         trim(destructorText),
                         "virtual ~" + context.name + "(",
                         "Marked an existing destructor virtual because the class declares virtual methods.");
        return classText;
    }

    const std::string destructorLine = "    virtual ~" + context.name + "() = default;\n";
    const std::regex publicPattern(R"((public\s*:\s*\n))");
    if (std::regex_search(classText, match, publicPattern)) {
        classText.insert(static_cast<std::size_t>(match.position() + match.length()), destructorLine);
    } else if (context.block.keyword == "class") {
        classText.insert(classText.find('{') + 1, "\npublic:\n" + destructorLine);
    } else {
        classText.insert(classText.find('{') + 1, "\n" + destructorLine);
    }
    addAppliedChange(changes,
                     "Add virtual destructor for polymorphic base",
                     context.name,
                     "virtual ~" + context.name + "() = default;",
                     "Added a virtual default destructor because the class declares virtual methods.");
    return classText;
}

std::map<std::string, std::string> inheritanceMap(const std::vector<ClassContext>& contexts)
{
    std::map<std::string, std::string> inheritance;
    for (const ClassContext& context : contexts) {
        if (context.confident && !context.baseNames.empty()) {
            inheritance[context.name] = context.baseNames.front();
        }
    }
    return inheritance;
}
} // namespace

std::string PolymorphicContractPass::rewrite(const std::string& code,
                                             std::vector<ConversionChange>& changes) const
{
    const DestructorDeclarationValidationPass validationPass;
    std::string updated = validationPass.validateAndRepair(code, changes);

    std::vector<ClassContext> contexts = ClassContextResolver().resolve(updated);
    std::map<std::string, std::vector<VirtualMethod>> baseVirtualMethods;
    std::map<std::string, std::string> inheritance = inheritanceMap(contexts);

    for (const ClassContext& context : contexts) {
        if (context.confident) {
            baseVirtualMethods[context.name] = virtualMethods(context.block.text);
        }
    }

    std::sort(contexts.begin(), contexts.end(), [](const ClassContext& left, const ClassContext& right) {
        return left.block.start > right.block.start;
    });

    for (const ClassContext& context : contexts) {
        std::string classText = updated.substr(context.block.start, context.block.end - context.block.start);
        classText = ensureVirtualDestructor(std::move(classText), context, changes);

        const auto baseIt = inheritance.find(context.name);
        if (baseIt != inheritance.end()) {
            const auto methodsIt = baseVirtualMethods.find(baseIt->second);
            if (methodsIt != baseVirtualMethods.end()) {
                for (const VirtualMethod& method : methodsIt->second) {
                    const std::regex methodPattern(
                        R"((^[ \t]*)(?:virtual\s+)?((?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+))"
                            + escapeRegex(method.name)
                            + R"(\s*\(([^)]*)\)\s*(const)?\s*(?![^;\n{]*\boverride\b)([;{]))",
                        std::regex::ECMAScript | std::regex::multiline);
                    std::smatch match;
                    std::string search = classText;
                    std::size_t consumed = 0;
                    while (std::regex_search(search, match, methodPattern)) {
                        if (normalizeParameterList(match[3].str()) != method.normalizedParameters
                            || static_cast<bool>(match[4].matched) != method.isConst) {
                            consumed += static_cast<std::size_t>(match.position() + match.length());
                            search = match.suffix().str();
                            continue;
                        }
                        const std::string before = match[0].str();
                        const std::string after = match[1].str()
                            + match[2].str()
                            + method.name + "(" + match[3].str() + ")"
                            + (method.isConst ? " const" : "")
                            + " override" + match[5].str();
                        classText.replace(consumed + static_cast<std::size_t>(match.position()),
                                          static_cast<std::size_t>(match.length()),
                                          after);
                        addAppliedChange(changes,
                                         "Add override to derived virtual method",
                                         trim(before),
                                         trim(after),
                                         "Added override where a derived method exactly matches a visible base virtual signature.");
                        consumed += static_cast<std::size_t>(match.position()) + after.size();
                        search = classText.substr(consumed);
                    }
                }
            }
        }

        updated.replace(context.block.start, context.block.end - context.block.start, classText);
    }

    return validationPass.validateAndRepair(updated, changes);
}
