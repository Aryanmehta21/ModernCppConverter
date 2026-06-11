#include "converter/FunctorToLambdaPass.h"

#include "converter/IncludeManager.h"
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
struct PredicateFunctor
{
    ClassBlock block;
    std::string parameterName;
    std::string condition;
    std::vector<std::string> memberNames;
};

struct LambdaPredicateVariable
{
    std::string variableName;
    std::string constructorName;
    std::string declarationText;
    std::string parameterName;
    std::string condition;
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

std::string lastParameterName(const std::string& parameterList)
{
    std::string parameter = trim(parameterList);
    const auto comma = parameter.find_last_of(',');
    if (comma != std::string::npos) {
        parameter = trim(parameter.substr(comma + 1));
    }

    std::smatch match;
    const std::regex namePattern(R"(\b([A-Za-z_]\w*)\s*$)");
    if (std::regex_search(parameter, match, namePattern)) {
        return match[1].str();
    }
    return {};
}

std::vector<std::string> splitArguments(const std::string& argumentList)
{
    std::vector<std::string> arguments;
    std::string current;
    int parenDepth = 0;
    int angleDepth = 0;
    int braceDepth = 0;
    for (const char character : argumentList) {
        if (character == '(') {
            ++parenDepth;
        } else if (character == ')' && parenDepth > 0) {
            --parenDepth;
        } else if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (character == '{') {
            ++braceDepth;
        } else if (character == '}' && braceDepth > 0) {
            --braceDepth;
        }

        if (character == ',' && parenDepth == 0 && angleDepth == 0 && braceDepth == 0) {
            arguments.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!trim(current).empty()) {
        arguments.push_back(trim(current));
    }
    return arguments;
}

std::vector<std::string> collectSimpleDataMembers(const std::string& classText)
{
    std::vector<std::string> members;
    std::stringstream stream(classText);
    std::string line;
    const std::regex memberPattern(R"(^[ \t]*(?:const\s+)?[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+([A-Za-z_]\w*)\s*(?:=\s*[^;]+)?;\s*$)");
    while (std::getline(stream, line)) {
        if (line.find('(') != std::string::npos
            || line.find("operator") != std::string::npos
            || line.find("return") != std::string::npos) {
            continue;
        }
        std::smatch match;
        if (std::regex_match(line, match, memberPattern)) {
            members.push_back(match[1].str());
        }
    }
    return members;
}

std::vector<PredicateFunctor> collectPredicateFunctors(const std::string& code)
{
    const ClassResourceAnalyzer analyzer;
    std::vector<PredicateFunctor> functors;
    const std::regex operatorPattern(
        R"(\b(?:bool|auto)\s+operator\s*\(\s*\)\s*\(([^)]*)\)\s*const\s*\{\s*return\s+([^;{}]+)\s*;\s*\})");

    for (const ClassBlock& block : analyzer.analyzeClasses(code)) {
        if (std::regex_search(block.text, std::regex(R"(\b(?:class|struct)\s+)" + escapeRegex(block.name) + R"(\s*:)"))) {
            continue;
        }
        std::smatch match;
        if (!std::regex_search(block.text, match, operatorPattern)) {
            continue;
        }

        std::vector<std::string> memberNames = collectSimpleDataMembers(block.text);
        if (memberNames.size() > 4U) {
            continue;
        }

        const std::string parameterName = lastParameterName(match[1].str());
        if (parameterName.empty()) {
            continue;
        }
        functors.push_back(PredicateFunctor{
            block,
            parameterName,
            trim(match[2].str()),
            std::move(memberNames),
        });
    }
    return functors;
}

std::size_t countAlgorithmPredicateUses(const std::string& code, const std::string& functorName)
{
    const std::regex algorithmUse(
        R"(std::(?:find_if|count_if|any_of|all_of|none_of|remove_if)\s*\([\s\S]*?,\s*)"
        + escapeRegex(functorName)
        + R"(\s*(?:\{\s*[^{}]*\s*\}|\(\s*[^()]*\s*\)))");
    return static_cast<std::size_t>(std::distance(std::sregex_iterator(code.begin(), code.end(), algorithmUse),
                                                  std::sregex_iterator()));
}

std::string captureListForFunctor(const PredicateFunctor& functor, const std::string& constructorArguments)
{
    if (functor.memberNames.empty()) {
        return "[]";
    }

    std::vector<std::string> arguments = splitArguments(constructorArguments);
    if (arguments.size() != functor.memberNames.size()) {
        return {};
    }

    std::ostringstream capture;
    capture << '[';
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) {
            capture << ", ";
        }
        capture << functor.memberNames[index] << " = " << arguments[index];
    }
    capture << ']';
    return capture.str();
}

std::string lambdaForCondition(const PredicateFunctor& functor, const std::string& constructorArguments)
{
    const std::string captureList = captureListForFunctor(functor, constructorArguments);
    if (captureList.empty()) {
        return {};
    }

    std::string condition = std::regex_replace(functor.condition,
                                               std::regex("\\b" + escapeRegex(functor.parameterName) + "\\b"),
                                               "item");
    return captureList + "(const auto& item) {\n        return " + condition + ";\n    }";
}

std::string uppercaseFirst(std::string value)
{
    if (!value.empty()) {
        value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    }
    return value;
}

std::vector<LambdaPredicateVariable> collectLambdaPredicateVariables(const std::string& code)
{
    std::vector<LambdaPredicateVariable> predicates;
    const std::regex lambdaVariable(
        R"((^[ \t]*const\s+auto\s+([A-Za-z_]\w*)\s*=\s*\[\s*\]\s*\(([^)]*)\)\s*\{\s*return\s+([^;{}]+)\s*;\s*\}\s*;\s*\n?))",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), lambdaVariable), end; iterator != end; ++iterator) {
        const std::string parameterName = lastParameterName((*iterator)[3].str());
        if (parameterName.empty()) {
            continue;
        }
        predicates.push_back(LambdaPredicateVariable{
            (*iterator)[2].str(),
            uppercaseFirst((*iterator)[2].str()),
            (*iterator)[1].str(),
            parameterName,
            trim((*iterator)[4].str()),
        });
    }
    return predicates;
}

std::string inlineLambdaForCondition(const LambdaPredicateVariable& predicate)
{
    std::string condition = std::regex_replace(predicate.condition,
                                               std::regex("\\b" + escapeRegex(predicate.parameterName) + "\\b"),
                                               "item");
    return "[](const auto& item) {\n        return " + condition + ";\n    }";
}

std::string repairGeneratedLambdaPredicateUses(std::string code,
                                               bool& changed,
                                               std::vector<ConversionChange>& changes)
{
    for (const LambdaPredicateVariable& predicate : collectLambdaPredicateVariables(code)) {
        const std::regex constructorPredicate(
            R"((std::(?:find_if|count_if|any_of|all_of|none_of|remove_if)\s*\([\s\S]*?,\s*))"
            + escapeRegex(predicate.constructorName)
            + R"(\s*(?:\{\s*\}|\(\s*\)))");
        std::smatch match;
        if (!std::regex_search(code, match, constructorPredicate)) {
            continue;
        }

        const std::string lambda = inlineLambdaForCondition(predicate);
        const std::string before = match[0].str();
        const std::string after = match[1].str() + lambda;
        code.replace(static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     after);
        addAppliedChange(changes,
                         "Functor predicate to lambda",
                         trim(before),
                         trim(after),
                         "Repaired a partially modernized functor predicate by inlining the generated lambda into the standard algorithm call.");

        const std::size_t declarationPosition = code.find(predicate.declarationText);
        if (declarationPosition != std::string::npos) {
            code.erase(declarationPosition, predicate.declarationText.size());
            addAppliedChange(changes,
                             "Remove obsolete predicate functor",
                             trim(predicate.declarationText),
                             "removed",
                             "Removed a generated predicate lambda variable after its algorithm use was inlined.");
        }
        changed = true;
    }
    return code;
}
} // namespace

std::string FunctorToLambdaPass::rewrite(const std::string& code,
                                         std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = false;
    updated = repairGeneratedLambdaPredicateUses(std::move(updated), changed, changes);

    std::vector<PredicateFunctor> functors = collectPredicateFunctors(updated);
    if (functors.empty()) {
        if (!changed) {
            return code;
        }
        const IncludeManager includeManager;
        return includeManager.ensureInclude(updated, "#include <algorithm>");
    }

    for (const PredicateFunctor& functor : functors) {
        if (countAlgorithmPredicateUses(updated, functor.block.name) != 1U) {
            continue;
        }

        const std::regex algorithmUse(
            R"((std::(?:find_if|count_if|any_of|all_of|none_of|remove_if)\s*\([\s\S]*?,\s*))"
            + escapeRegex(functor.block.name)
            + R"(\s*(?:\{\s*([^{}]*)\s*\}|\(\s*([^()]*)\s*\)))");
        std::smatch match;
        if (!std::regex_search(updated, match, algorithmUse)) {
            continue;
        }

        const std::string constructorArguments = match[2].matched ? match[2].str() : match[3].str();
        const std::string lambda = lambdaForCondition(functor, constructorArguments);
        if (lambda.empty()) {
            continue;
        }
        const std::string before = match[0].str();
        const std::string after = match[1].str() + lambda;
        updated.replace(static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        after);
        addAppliedChange(changes,
                         "Functor predicate to lambda",
                         trim(before),
                         trim(after),
                         "Converted a stateless predicate functor used once by a standard algorithm into an inline lambda.");

        const std::vector<ClassBlock> refreshedClasses = ClassResourceAnalyzer().analyzeClasses(updated);
        const auto functorClass = std::find_if(refreshedClasses.begin(), refreshedClasses.end(), [&functor](const ClassBlock& block) {
            return block.name == functor.block.name;
        });
        if (functorClass != refreshedClasses.end()) {
            const std::regex remainingReference("\\b" + escapeRegex(functor.block.name) + "\\b");
            std::string withoutClass = updated;
            withoutClass.replace(functorClass->start, functorClass->end - functorClass->start, "");
            if (!std::regex_search(withoutClass, remainingReference)) {
                addAppliedChange(changes,
                                 "Remove obsolete predicate functor",
                                 trim(functorClass->text),
                                 "removed",
                                 "Removed a stateless predicate functor after its only algorithm use was converted to a lambda.");
                updated = std::move(withoutClass);
            }
        }
        changed = true;
    }

    if (!changed) {
        return code;
    }

    const IncludeManager includeManager;
    return includeManager.ensureInclude(updated, "#include <algorithm>");
}
