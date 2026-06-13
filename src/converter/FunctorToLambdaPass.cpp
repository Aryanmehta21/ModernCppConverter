#include "converter/FunctorToLambdaPass.h"

#include "converter/FunctorToLambdaValidationPass.h"
#include "converter/IncludeManager.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct CaptureMapping
{
    std::string fieldName;
    std::string captureName;
    std::string constructorParameterName;
    std::size_t argumentIndex = 0;
};

struct PredicateFunctor
{
    ClassBlock block;
    std::string parameterName;
    std::string parameterDeclaration;
    std::string condition;
    std::vector<std::string> memberNames;
    std::vector<CaptureMapping> captureMappings;
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

void addSkippedDiagnostic(std::vector<ConversionChange>& changes,
                          std::string reason)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: FunctorToLambdaPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: FunctorToLambdaPass",
        "pass started",
        {},
        "candidates found: " + std::to_string(candidates)
            + ", candidates converted: " + std::to_string(converted)
            + ", candidates skipped: " + std::to_string(skipped),
        false,
        true,
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

std::vector<std::string> parameterNames(const std::string& parameterList)
{
    std::vector<std::string> names;
    for (const std::string& parameter : splitArguments(parameterList)) {
        const std::string name = lastParameterName(parameter);
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

std::string algorithmPattern()
{
    return R"(std::(?:(?:ranges::)?(?:find_if|count_if|any_of|all_of|none_of|remove_if|copy_if|partition|stable_partition)))";
}

bool hasSingleCallOperator(const std::string& classText)
{
    const std::regex callOperator(R"(\boperator\s*\(\s*\)\s*\()");
    return std::distance(std::sregex_iterator(classText.begin(), classText.end(), callOperator),
                         std::sregex_iterator()) == 1;
}

bool conditionLooksSideEffectFree(const std::string& condition)
{
    if (condition.find("++") != std::string::npos || condition.find("--") != std::string::npos) {
        return false;
    }
    if (std::regex_search(condition, std::regex(R"((^|[^=!<>])=([^=]|$))"))) {
        return false;
    }
    if (std::regex_search(condition, std::regex(R"(\b[A-Za-z_]\w*\s*\()"))) {
        return false;
    }
    return true;
}

bool isCxxKeyword(const std::string& value)
{
    static const std::vector<std::string> keywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    };
    return std::find(keywords.begin(), keywords.end(), value) != keywords.end();
}

std::string cleanCaptureName(const std::string& fieldName,
                             const std::string& parameterName,
                             const std::vector<std::string>& existingNames)
{
    std::string candidate = fieldName;
    if (candidate.rfind("m_", 0) == 0 && candidate.size() > 2U) {
        candidate = candidate.substr(2);
    } else if (candidate.size() > 1U && candidate.back() == '_') {
        candidate.pop_back();
    }

    if (candidate.empty()) {
        candidate = fieldName;
    }
    if (candidate == parameterName || isCxxKeyword(candidate)) {
        candidate += "Value";
    }
    std::string unique = candidate;
    int suffix = 2;
    while (std::find(existingNames.begin(), existingNames.end(), unique) != existingNames.end()) {
        unique = candidate + std::to_string(suffix++);
    }
    return unique;
}

bool constructorLooksSimple(const std::string& body)
{
    const std::string trimmedBody = trim(body);
    if (trimmedBody.empty()) {
        return true;
    }
    std::stringstream stream(trimmedBody);
    std::string line;
    const std::regex assignmentPattern(R"(^\s*(?:this\s*->\s*)?[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\s*;\s*$)");
    while (std::getline(stream, line)) {
        if (trim(line).empty()) {
            continue;
        }
        if (!std::regex_match(line, assignmentPattern)) {
            return false;
        }
    }
    return true;
}

std::string normalizeInitializerSource(std::string value)
{
    value = trim(value);
    std::smatch match;
    const std::regex movePattern(R"(^std::move\s*\(\s*([A-Za-z_]\w*)\s*\)$)");
    if (std::regex_match(value, match, movePattern)) {
        return match[1].str();
    }
    return value;
}

std::vector<CaptureMapping> collectCaptureMappings(const ClassBlock& block,
                                                   const std::vector<std::string>& memberNames,
                                                   const std::string& operatorParameterName)
{
    std::vector<CaptureMapping> mappings;
    if (memberNames.empty()) {
        return mappings;
    }

    const std::regex constructorPattern(
        R"(\b(?:explicit\s+)?)" + escapeRegex(block.name)
            + R"(\s*\(([^)]*)\)\s*(?::\s*([^{}]*))?\s*\{([^{}]*)\})");
    std::vector<std::smatch> constructors;
    for (std::sregex_iterator iterator(block.text.begin(), block.text.end(), constructorPattern), end; iterator != end; ++iterator) {
        constructors.push_back(*iterator);
    }

    std::vector<std::string> constructorParameterNames;
    std::vector<std::pair<std::string, std::string>> fieldToParameter;
    if (constructors.size() == 1U && constructorLooksSimple(constructors.front()[3].str())) {
        constructorParameterNames = parameterNames(constructors.front()[1].str());

        for (const std::string& initializer : splitArguments(constructors.front()[2].str())) {
            std::smatch initializerMatch;
            const std::regex initializerPattern(R"(^\s*([A-Za-z_]\w*)\s*(?:\(\s*([^(){}]+)\s*\)|\{\s*([^(){}]+)\s*\})\s*$)");
            if (std::regex_match(initializer, initializerMatch, initializerPattern)) {
                const std::string field = initializerMatch[1].str();
                const std::string source = normalizeInitializerSource(initializerMatch[2].matched ? initializerMatch[2].str() : initializerMatch[3].str());
                if (std::find(constructorParameterNames.begin(), constructorParameterNames.end(), source) != constructorParameterNames.end()) {
                    fieldToParameter.emplace_back(field, source);
                }
            }
        }

        std::stringstream constructorBody(constructors.front()[3].str());
        std::string line;
        const std::regex assignmentPattern(R"(^\s*(?:this\s*->\s*)?([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*;\s*$)");
        while (std::getline(constructorBody, line)) {
            std::smatch assignmentMatch;
            if (!std::regex_match(line, assignmentMatch, assignmentPattern)) {
                continue;
            }
            const std::string source = assignmentMatch[2].str();
            if (std::find(constructorParameterNames.begin(), constructorParameterNames.end(), source) != constructorParameterNames.end()) {
                fieldToParameter.emplace_back(assignmentMatch[1].str(), source);
            }
        }
    } else if (constructors.empty()) {
        // Aggregate predicate functors are common in legacy snippets and can still
        // be safely mapped positionally when the type has only simple fields.
        constructorParameterNames = memberNames;
        fieldToParameter.reserve(memberNames.size());
        for (const std::string& member : memberNames) {
            fieldToParameter.emplace_back(member, member);
        }
    } else {
        return {};
    }

    std::vector<std::string> captureNames;
    for (const std::string& member : memberNames) {
        const auto mapping = std::find_if(fieldToParameter.begin(), fieldToParameter.end(), [&member](const auto& pair) {
            return pair.first == member;
        });
        if (mapping == fieldToParameter.end()) {
            return {};
        }
        const std::string captureName = cleanCaptureName(member, operatorParameterName, captureNames);
        captureNames.push_back(captureName);
        const auto parameterIterator = std::find(constructorParameterNames.begin(), constructorParameterNames.end(), mapping->second);
        if (parameterIterator == constructorParameterNames.end()) {
            return {};
        }
        const std::size_t argumentIndex = static_cast<std::size_t>(std::distance(constructorParameterNames.begin(), parameterIterator));
        mappings.push_back(CaptureMapping{member, captureName, mapping->second, argumentIndex});
    }

    return mappings;
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
        if (block.text.find("virtual") != std::string::npos || !hasSingleCallOperator(block.text)) {
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
        if (!conditionLooksSideEffectFree(match[2].str())) {
            continue;
        }
        std::vector<CaptureMapping> captureMappings = collectCaptureMappings(block, memberNames, parameterName);
        if (!memberNames.empty() && captureMappings.empty()) {
            continue;
        }
        functors.push_back(PredicateFunctor{
            block,
            parameterName,
            trim(match[1].str()),
            trim(match[2].str()),
            std::move(memberNames),
            std::move(captureMappings),
        });
    }
    return functors;
}

std::size_t countAlgorithmPredicateUses(const std::string& code, const std::string& functorName)
{
    const std::regex algorithmUse(
        algorithmPattern()
        + R"(\s*\([\s\S]*?,\s*)"
        + escapeRegex(functorName)
        + R"(\s*(?:\{\s*[^{}]*\s*\}|\(\s*[^()]*\s*\)))");
    return static_cast<std::size_t>(std::distance(std::sregex_iterator(code.begin(), code.end(), algorithmUse),
                                                  std::sregex_iterator()));
}

std::string captureListForFunctor(const PredicateFunctor& functor, const std::string& constructorArguments)
{
    if (functor.captureMappings.empty()) {
        return "[]";
    }

    std::vector<std::string> arguments = splitArguments(constructorArguments);
    if (arguments.size() != functor.captureMappings.size()) {
        return {};
    }

    std::ostringstream capture;
    capture << '[';
    for (std::size_t index = 0; index < functor.captureMappings.size(); ++index) {
        if (index > 0) {
            capture << ", ";
        }
        if (functor.captureMappings[index].argumentIndex >= arguments.size()) {
            return {};
        }
        capture << functor.captureMappings[index].captureName << " = " << arguments[functor.captureMappings[index].argumentIndex];
    }
    capture << ']';
    return capture.str();
}

std::string rewriteFunctorCondition(const PredicateFunctor& functor,
                                    std::string condition,
                                    const std::string& lambdaParameterName)
{
    for (const CaptureMapping& mapping : functor.captureMappings) {
        condition = std::regex_replace(condition,
                                       std::regex(R"(\bthis\s*->\s*)" + escapeRegex(mapping.fieldName) + R"(\b)"),
                                       mapping.captureName);
    }
    for (const CaptureMapping& mapping : functor.captureMappings) {
        if (mapping.fieldName == functor.parameterName) {
            continue;
        }
        condition = std::regex_replace(condition,
                                       std::regex("\\b" + escapeRegex(mapping.fieldName) + "\\b"),
                                       mapping.captureName);
    }
    if (lambdaParameterName != functor.parameterName) {
        condition = std::regex_replace(condition,
                                       std::regex("\\b" + escapeRegex(functor.parameterName) + "\\b"),
                                       lambdaParameterName);
    }
    return condition;
}

std::string lambdaForCondition(const PredicateFunctor& functor, const std::string& constructorArguments)
{
    const std::string captureList = captureListForFunctor(functor, constructorArguments);
    if (captureList.empty()) {
        return {};
    }

    std::string condition = rewriteFunctorCondition(functor, functor.condition, "item");
    return captureList + "(const auto& item) {\n        return " + condition + ";\n    }";
}

std::string lambdaForLocalPredicate(const PredicateFunctor& functor, const std::string& constructorArguments)
{
    const std::string captureList = captureListForFunctor(functor, constructorArguments);
    if (captureList.empty()) {
        return {};
    }

    std::string parameterDeclaration = functor.parameterDeclaration;
    if (parameterDeclaration.empty()) {
        parameterDeclaration = "const auto& item";
    }
    const std::string condition = rewriteFunctorCondition(functor, functor.condition, functor.parameterName);
    return captureList + "(" + parameterDeclaration + ") {\n        return " + condition + ";\n    }";
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

std::size_t findEnclosingBlockEnd(const std::string& code, std::size_t position);
bool variableIsUsedAsCallableInScope(const std::string& code,
                                     const std::string& variableName,
                                     std::size_t declarationStart,
                                     std::size_t declarationEnd);

std::string repairGeneratedLambdaPredicateUses(std::string code,
                                               bool& changed,
                                               std::vector<ConversionChange>& changes)
{
    for (const LambdaPredicateVariable& predicate : collectLambdaPredicateVariables(code)) {
        const std::regex constructorPredicate(
            R"(()" + algorithmPattern() + R"(\s*\([\s\S]*?,\s*))"
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

    for (const LambdaPredicateVariable& predicate : collectLambdaPredicateVariables(code)) {
        const std::regex staleFunctorObject(
            R"((^[ \t]*)\b)" + escapeRegex(predicate.constructorName)
                + R"(\s+([A-Za-z_]\w*)\s*(?:\(\s*\)|\{\s*\})?\s*;\s*$)",
            std::regex::ECMAScript | std::regex::multiline);
        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, staleFunctorObject)) {
            const std::size_t position = consumed + static_cast<std::size_t>(match.position());
            const std::size_t declarationEnd = position + static_cast<std::size_t>(match.length());
            const std::string variableName = match[2].str();
            if (!variableIsUsedAsCallableInScope(code, variableName, position, declarationEnd)) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string lambda = inlineLambdaForCondition(predicate);
            const std::string replacement = match[1].str() + "auto " + variableName + " = " + lambda + ";";
            code.replace(position, static_cast<std::size_t>(match.length()), replacement);
            addAppliedChange(changes,
                             "Functor object to lambda",
                             trim(match[0].str()),
                             trim(replacement),
                             "Repaired a stale functor object declaration after an earlier pass converted the functor body to a generated lambda.");
            changed = true;
            consumed = position + replacement.size();
            search = code.substr(consumed);
        }

        const std::size_t declarationPosition = code.find(predicate.declarationText);
        if (declarationPosition != std::string::npos) {
            std::string withoutDeclaration = code;
            withoutDeclaration.erase(declarationPosition, predicate.declarationText.size());
            if (!std::regex_search(withoutDeclaration, std::regex("\\b" + escapeRegex(predicate.constructorName) + "\\b"))) {
                code = std::move(withoutDeclaration);
                addAppliedChange(changes,
                                 "Remove obsolete predicate functor",
                                 trim(predicate.declarationText),
                                 "removed",
                                 "Removed a generated predicate lambda variable after stale functor object uses were inlined.");
            }
        }
    }
    return code;
}

std::size_t findEnclosingBlockEnd(const std::string& code, const std::size_t position)
{
    const std::size_t openBrace = code.rfind('{', position);
    if (openBrace == std::string::npos) {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

bool variableIsUsedAsCallableInScope(const std::string& code,
                                     const std::string& variableName,
                                     const std::size_t declarationStart,
                                     const std::size_t declarationEnd)
{
    const std::size_t blockEnd = findEnclosingBlockEnd(code, declarationStart);
    if (blockEnd == std::string::npos || blockEnd <= declarationEnd) {
        return false;
    }
    std::string scopeTail = code.substr(declarationEnd, blockEnd - declarationEnd);
    const std::regex callableUse("\\b" + escapeRegex(variableName) + R"(\s*\()");
    const bool hasCallableUse = std::regex_search(scopeTail, callableUse);
    bool hasAlgorithmPredicateUse = false;
    std::stringstream scrubbedTail;
    std::stringstream scopeStream(scopeTail);
    std::string line;
    while (std::getline(scopeStream, line)) {
        if (std::regex_search(line, std::regex(algorithmPattern()))
            && std::regex_search(line, std::regex("\\b" + escapeRegex(variableName) + "\\b"))) {
            hasAlgorithmPredicateUse = true;
            line = std::regex_replace(line, std::regex("\\b" + escapeRegex(variableName) + "\\b"), "");
        }
        scrubbedTail << line << '\n';
    }
    scopeTail = scrubbedTail.str();
    if (!hasCallableUse && !hasAlgorithmPredicateUse) {
        return false;
    }

    scopeTail = std::regex_replace(scopeTail, callableUse, "");
    return !std::regex_search(scopeTail, std::regex("\\b" + escapeRegex(variableName) + "\\b"));
}

std::string removeUnusedFunctorClasses(std::string code,
                                       const std::vector<PredicateFunctor>& functors,
                                       std::vector<ConversionChange>& changes)
{
    bool removed = true;
    while (removed) {
        removed = false;
        const std::vector<ClassBlock> refreshedClasses = ClassResourceAnalyzer().analyzeClasses(code);
        for (const PredicateFunctor& functor : functors) {
            const auto functorClass = std::find_if(refreshedClasses.begin(), refreshedClasses.end(), [&functor](const ClassBlock& block) {
                return block.name == functor.block.name;
            });
            if (functorClass == refreshedClasses.end()) {
                continue;
            }

            std::string withoutClass = code;
            withoutClass.replace(functorClass->start, functorClass->end - functorClass->start, "");
            if (std::regex_search(withoutClass, std::regex("\\b" + escapeRegex(functor.block.name) + "\\b"))) {
                continue;
            }

            addAppliedChange(changes,
                             "Remove obsolete predicate functor",
                             trim(functorClass->text),
                             "removed",
                             "Removed a predicate functor after all local/algorithm uses were converted to lambdas.");
            code = std::move(withoutClass);
            removed = true;
            break;
        }
    }
    return code;
}

std::string convertLocalFunctorObjects(std::string code,
                                       const std::vector<PredicateFunctor>& functors,
                                       int& candidates,
                                       int& converted,
                                       int& skipped,
                                       std::vector<ConversionChange>& changes)
{
    for (const PredicateFunctor& functor : functors) {
        const std::regex declarationPattern(
            R"((^[ \t]*)\b)" + escapeRegex(functor.block.name)
                + R"(\s+([A-Za-z_]\w*)\s*(?:(?:\(\s*([^;\n]*)\s*\))|(?:\{\s*([^;\n]*)\s*\}))?\s*;\s*$)",
            std::regex::ECMAScript | std::regex::multiline);
        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, declarationPattern)) {
            ++candidates;
            const std::size_t position = consumed + static_cast<std::size_t>(match.position());
            const std::size_t declarationEnd = position + static_cast<std::size_t>(match.length());
            const std::string variableName = match[2].str();
            if (!variableIsUsedAsCallableInScope(code, variableName, position, declarationEnd)) {
                ++skipped;
                addSkippedDiagnostic(changes, "Local functor object was not converted because no callable use was visible in the same lexical block.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string constructorArguments = match[3].matched ? match[3].str() : match[4].str();
            const std::string lambda = lambdaForLocalPredicate(functor, constructorArguments);
            if (lambda.empty()) {
                ++skipped;
                addSkippedDiagnostic(changes, "Local functor object was not converted because constructor arguments did not map cleanly to captured fields.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string replacement = match[1].str() + "auto " + variableName + " = " + lambda + ";";
            code.replace(position, static_cast<std::size_t>(match.length()), replacement);
            addAppliedChange(changes,
                             "Functor object to lambda",
                             trim(match[0].str()),
                             trim(replacement),
                             "Converted a local predicate functor object into a lambda while preserving constructor-captured state.");
            ++converted;
            consumed = position + replacement.size();
            search = code.substr(consumed);
        }

        const std::regex autoDeclarationPattern(
            R"((^[ \t]*)\b(?:const\s+)?auto\s+([A-Za-z_]\w*)\s*=\s*)"
                + escapeRegex(functor.block.name)
                + R"(\s*(?:(?:\(\s*([^;\n]*)\s*\))|(?:\{\s*([^;\n]*)\s*\}))\s*;\s*$)",
            std::regex::ECMAScript | std::regex::multiline);
        search = code;
        consumed = 0;
        while (std::regex_search(search, match, autoDeclarationPattern)) {
            ++candidates;
            const std::size_t position = consumed + static_cast<std::size_t>(match.position());
            const std::size_t declarationEnd = position + static_cast<std::size_t>(match.length());
            const std::string variableName = match[2].str();
            if (!variableIsUsedAsCallableInScope(code, variableName, position, declarationEnd)) {
                ++skipped;
                addSkippedDiagnostic(changes, "Local auto functor object was not converted because no callable use was visible in the same lexical block.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string constructorArguments = match[3].matched ? match[3].str() : match[4].str();
            const std::string lambda = lambdaForLocalPredicate(functor, constructorArguments);
            if (lambda.empty()) {
                ++skipped;
                addSkippedDiagnostic(changes, "Local auto functor object was not converted because constructor arguments did not map cleanly to captured fields.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string replacement = match[1].str() + "auto " + variableName + " = " + lambda + ";";
            code.replace(position, static_cast<std::size_t>(match.length()), replacement);
            addAppliedChange(changes,
                             "Functor object to lambda",
                             trim(match[0].str()),
                             trim(replacement),
                             "Converted a local auto predicate functor object into a lambda while preserving constructor-captured state.");
            ++converted;
            consumed = position + replacement.size();
            search = code.substr(consumed);
        }
    }
    return code;
}
} // namespace

std::string FunctorToLambdaPass::rewrite(const std::string& code,
                                         std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = false;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    const std::size_t changeCountBefore = changes.size();
    updated = repairGeneratedLambdaPredicateUses(std::move(updated), changed, changes);

    std::vector<PredicateFunctor> functors = collectPredicateFunctors(updated);
    if (functors.empty()) {
        addDiagnostic(changes, candidates, converted, skipped);
        if (!changed) {
            return code;
        }
        const IncludeManager includeManager;
        return includeManager.ensureInclude(updated, "#include <algorithm>");
    }

    updated = convertLocalFunctorObjects(std::move(updated), functors, candidates, converted, skipped, changes);
    if (updated != code) {
        changed = true;
    }

    for (const PredicateFunctor& functor : functors) {
        ++candidates;
        if (countAlgorithmPredicateUses(updated, functor.block.name) != 1U) {
            ++skipped;
            addSkippedDiagnostic(changes, "Functor algorithm conversion was skipped because the functor was not used exactly once in direct algorithm construction form.");
            continue;
        }

        const std::regex algorithmUse(
            R"(()" + algorithmPattern() + R"(\s*\([\s\S]*?,\s*))"
            + escapeRegex(functor.block.name)
            + R"(\s*(?:\{\s*([^{}]*)\s*\}|\(\s*([^()]*)\s*\)))");
        std::smatch match;
        if (!std::regex_search(updated, match, algorithmUse)) {
            continue;
        }

        const std::string constructorArguments = match[2].matched ? match[2].str() : match[3].str();
        const std::string lambda = lambdaForCondition(functor, constructorArguments);
        if (lambda.empty()) {
            ++skipped;
            addSkippedDiagnostic(changes, "Functor algorithm conversion was skipped because constructor arguments did not map cleanly to captured fields.");
            continue;
        }
        const std::string before = match[0].str();
        const std::string after = match[1].str() + lambda;
        updated.replace(static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        after);
        ++converted;
        addAppliedChange(changes,
                         "Functor predicate to lambda",
                         trim(before),
                         trim(after),
                         "Converted a stateless predicate functor used once by a standard algorithm into an inline lambda.");

        changed = true;
    }

    if (changed) {
        updated = removeUnusedFunctorClasses(std::move(updated), functors, changes);
    }
    addDiagnostic(changes, candidates, converted, skipped);

    if (!changed) {
        return code;
    }

    std::string validationReason;
    if (!FunctorToLambdaValidationPass().isValid(updated, validationReason)) {
        changes.resize(changeCountBefore);
        changes.push_back(ConversionChange{
            "FunctorToLambdaValidationPass",
            "functor-to-lambda candidate",
            {},
            "Rolled back functor-to-lambda modernization because validation failed: " + validationReason,
            false,
            true,
        });
        return code;
    }

    const IncludeManager includeManager;
    return includeManager.ensureInclude(updated, "#include <algorithm>");
}
