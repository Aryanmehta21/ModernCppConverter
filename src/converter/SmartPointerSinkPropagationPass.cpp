#include "converter/SmartPointerSinkPropagationPass.h"

#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct SmartPointerExpression
{
    std::string expression;
    std::string pointeeType;
    bool moveOnly = true;
};

struct RawPointerSink
{
    std::string functionName;
    std::string pointeeType;
};

struct RawPointerContainer
{
    std::string name;
    std::string pointeeType;
};

struct RawPointerVariable
{
    std::string name;
    std::string pointeeType;
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

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
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

bool isOwnershipTransferName(const std::string& functionName)
{
    const std::string lowered = lowercase(functionName);
    return lowered.find("own") != std::string::npos
        || lowered.find("take") != std::string::npos
        || lowered.find("adopt") != std::string::npos
        || lowered.find("release") != std::string::npos
        || lowered.find("destroy") != std::string::npos
        || lowered.find("delete") != std::string::npos
        || lowered.find("free") != std::string::npos;
}

std::map<std::string, std::string> collectInheritance(const std::string& code)
{
    std::map<std::string, std::string> inheritance;
    const std::regex inheritancePattern(
        R"(\b(?:class|struct)\s+([A-Za-z_]\w*)\s*:\s*(?:public|protected|private)?\s*([A-Za-z_]\w*)\s*\{)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), inheritancePattern), end; iterator != end; ++iterator) {
        inheritance[(*iterator)[1].str()] = (*iterator)[2].str();
    }
    return inheritance;
}

bool typeMatches(const std::string& sinkType,
                 const std::string& expressionType,
                 const std::map<std::string, std::string>& inheritance)
{
    if (sinkType == expressionType) {
        return true;
    }
    std::string current = expressionType;
    for (int depth = 0; depth < 8; ++depth) {
        const auto found = inheritance.find(current);
        if (found == inheritance.end()) {
            return false;
        }
        if (found->second == sinkType) {
            return true;
        }
        current = found->second;
    }
    return false;
}

std::vector<RawPointerSink> collectRawPointerSinks(const std::string& code)
{
    std::vector<RawPointerSink> sinks;
    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?(?:\{|;))",
        std::regex::ECMAScript);
    const std::regex rawPointerParameter(R"((?:^|,)\s*(?:const\s+)?([A-Za-z_:][A-Za-z0-9_:]*)\s*\*\s*(?:[A-Za-z_]\w*)?)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        const std::string functionName = (*iterator)[1].str();
        if (isOwnershipTransferName(functionName)) {
            continue;
        }
        const std::string parameters = (*iterator)[2].str();
        for (std::sregex_iterator parameter(parameters.begin(), parameters.end(), rawPointerParameter), paramEnd; parameter != paramEnd; ++parameter) {
            sinks.push_back(RawPointerSink{functionName, (*parameter)[1].str()});
        }
    }
    return sinks;
}

std::set<std::string> collectSmartPointerSinkNames(const std::string& code)
{
    std::set<std::string> names;
    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?(?:\{|;))",
        std::regex::ECMAScript);
    const std::regex smartPointerParameter(R"(\bstd::(?:unique_ptr|shared_ptr)\s*<)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        if (std::regex_search((*iterator)[2].str(), smartPointerParameter)) {
            names.insert((*iterator)[1].str());
        }
    }
    return names;
}

void addExpression(std::vector<SmartPointerExpression>& expressions, SmartPointerExpression expression)
{
    if (expression.expression.empty() || expression.pointeeType.empty()) {
        return;
    }
    const auto duplicate = std::find_if(expressions.begin(), expressions.end(), [&expression](const SmartPointerExpression& existing) {
        return existing.expression == expression.expression;
    });
    if (duplicate == expressions.end()) {
        expressions.push_back(std::move(expression));
    }
}

std::vector<SmartPointerExpression> collectSmartPointerExpressions(const std::string& code)
{
    std::vector<SmartPointerExpression> expressions;

    const std::regex uniquePtrVariable(
        R"((?:const\s+)?std::(unique_ptr|shared_ptr)\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), uniquePtrVariable), end; iterator != end; ++iterator) {
        addExpression(expressions,
                      SmartPointerExpression{
                          (*iterator)[3].str(),
                          (*iterator)[2].str(),
                          (*iterator)[1].str() == "unique_ptr",
                      });
    }

    const std::regex autoMakeUnique(
        R"(\bauto\s+([A-Za-z_]\w*)\s*=\s*std::make_(unique|shared)\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*\()");
    for (std::sregex_iterator iterator(code.begin(), code.end(), autoMakeUnique), end; iterator != end; ++iterator) {
        addExpression(expressions,
                      SmartPointerExpression{
                          (*iterator)[1].str(),
                          (*iterator)[3].str(),
                          (*iterator)[2].str() == "unique",
                      });
    }

    const std::regex vectorUniquePtr(
        R"((?:const\s+)?std::vector\s*<\s*std::(unique_ptr|shared_ptr)\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), vectorUniquePtr), end; iterator != end; ++iterator) {
        addExpression(expressions,
                      SmartPointerExpression{
                          (*iterator)[3].str() + R"(\s*\[[^\]]+\])",
                          (*iterator)[2].str(),
                          (*iterator)[1].str() == "unique_ptr",
                      });
    }

    const std::regex arrayUniquePtr(
        R"((?:const\s+)?std::array\s*<\s*std::(unique_ptr|shared_ptr)\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*,\s*[^>]+>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), arrayUniquePtr), end; iterator != end; ++iterator) {
        addExpression(expressions,
                      SmartPointerExpression{
                          (*iterator)[3].str() + R"(\s*\[[^\]]+\])",
                          (*iterator)[2].str(),
                          (*iterator)[1].str() == "unique_ptr",
                      });
    }

    return expressions;
}

std::vector<RawPointerContainer> collectRawPointerContainers(const std::string& code)
{
    std::vector<RawPointerContainer> containers;
    const std::regex vectorRawPointer(
        R"(std::vector\s*<\s*(?:const\s+)?([A-Za-z_:][A-Za-z0-9_:]*)\s*\*\s*>\s+([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), vectorRawPointer), end; iterator != end; ++iterator) {
        containers.push_back(RawPointerContainer{
            (*iterator)[2].str(),
            (*iterator)[1].str(),
        });
    }
    return containers;
}

std::vector<RawPointerVariable> collectRawPointerVariables(const std::string& code)
{
    std::vector<RawPointerVariable> variables;
    const std::regex rawPointerDeclaration(
        R"(\b(?:const\s+)?([A-Za-z_:][A-Za-z0-9_:]*)\s*\*\s*([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), rawPointerDeclaration), end; iterator != end; ++iterator) {
        variables.push_back(RawPointerVariable{
            (*iterator)[2].str(),
            (*iterator)[1].str(),
        });
    }
    return variables;
}

std::string expressionRegex(const SmartPointerExpression& expression)
{
    const bool expressionIsPattern = expression.expression.find('\\') != std::string::npos
        || expression.expression.find('[') != std::string::npos;
    return expressionIsPattern ? R"(\b)" + expression.expression : R"(\b)" + escapeRegex(expression.expression) + R"(\b)";
}

bool expressionMatchesWhole(const std::string& value,
                            const SmartPointerExpression& expression,
                            std::string& matchedExpression)
{
    std::smatch match;
    const std::regex wholePattern("^\\s*(" + expressionRegex(expression) + ")\\s*$");
    if (!std::regex_match(value, match, wholePattern)) {
        return false;
    }
    matchedExpression = trim(match[1].str());
    return !matchedExpression.empty();
}

bool isSimpleIdentifier(const std::string& expression)
{
    return std::regex_match(expression, std::regex(R"([A-Za-z_]\w*)"));
}

bool isRawPointerSinkSignatureLine(const std::string& codePart, const std::string& functionName)
{
    const std::regex rawPointerSignature(
        R"(^[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+)"
            + escapeRegex(functionName)
            + R"(\s*\([^;\n{}]*\*\s*[A-Za-z_]\w*[^;\n{}]*\)\s*(?:const\s*)?(?:;|\{.*)?$)");
    return std::regex_match(codePart, rawPointerSignature);
}

std::string appendGetForExpression(std::string codePart,
                                   const SmartPointerExpression& expression,
                                   bool& changed)
{
    const std::regex expressionPattern(expressionRegex(expression));
    std::string updated;
    std::size_t last = 0;
    for (std::sregex_iterator iterator(codePart.begin(), codePart.end(), expressionPattern), end; iterator != end; ++iterator) {
        const std::size_t position = static_cast<std::size_t>(iterator->position());
        const std::size_t length = static_cast<std::size_t>(iterator->length());
        std::size_t next = position + length;
        while (next < codePart.size() && std::isspace(static_cast<unsigned char>(codePart[next]))) {
            ++next;
        }
        if (next < codePart.size()
            && (codePart[next] == '.'
                || codePart[next] == '-'
                || codePart[next] == '[')) {
            continue;
        }
        updated.append(codePart, last, position - last);
        updated.append(iterator->str());
        updated.append(".get()");
        last = position + length;
        changed = true;
    }
    if (last == 0) {
        return codePart;
    }
    updated.append(codePart, last, std::string::npos);
    return updated;
}

std::string propagateRawPointerAssignments(std::string codePart,
                                           const std::vector<SmartPointerExpression>& expressions,
                                           const std::vector<RawPointerVariable>& rawPointerVariables,
                                           const std::map<std::string, std::string>& inheritance,
                                           bool& changed)
{
    std::smatch match;
    const std::regex typedRawPointerAssignment(
        R"(^([ \t]*(?:const\s+)?([A-Za-z_:][A-Za-z0-9_:]*)\s*\*\s+[A-Za-z_]\w*\s*=\s*)([^;]+?)(\s*;\s*)$)");
    if (std::regex_match(codePart, match, typedRawPointerAssignment)) {
        const std::string rawType = match[2].str();
        const std::string rhs = trim(match[3].str());
        if (rhs.find(".get()") == std::string::npos
            && rhs.find("std::move") == std::string::npos
            && rhs.find(".release()") == std::string::npos) {
            for (const SmartPointerExpression& expression : expressions) {
                if (!typeMatches(rawType, expression.pointeeType, inheritance)) {
                    continue;
                }
                std::string matchedExpression;
                if (!expressionMatchesWhole(rhs, expression, matchedExpression)) {
                    continue;
                }
                changed = true;
                return match[1].str() + matchedExpression + ".get()" + match[4].str();
            }
        }
    }

    const std::regex autoRawPointerAssignment(R"(^([ \t]*(?:const\s+)?auto\s*\*\s+[A-Za-z_]\w*\s*=\s*)([^;]+?)(\s*;\s*)$)");
    if (std::regex_match(codePart, match, autoRawPointerAssignment)) {
        const std::string rhs = trim(match[2].str());
        if (rhs.find(".get()") == std::string::npos
            && rhs.find("std::move") == std::string::npos
            && rhs.find(".release()") == std::string::npos) {
            for (const SmartPointerExpression& expression : expressions) {
                std::string matchedExpression;
                if (!expressionMatchesWhole(rhs, expression, matchedExpression)) {
                    continue;
                }
                changed = true;
                return match[1].str() + matchedExpression + ".get()" + match[3].str();
            }
        }
    }

    const std::regex rawPointerReassignment(R"(^([ \t]*)([A-Za-z_]\w*)\s*=\s*([^;]+?)(\s*;\s*)$)");
    if (std::regex_match(codePart, match, rawPointerReassignment)) {
        const std::string destination = match[2].str();
        const std::string rhs = trim(match[3].str());
        if (rhs.find(".get()") == std::string::npos
            && rhs.find("std::move") == std::string::npos
            && rhs.find(".release()") == std::string::npos) {
            const auto rawVariable = std::find_if(rawPointerVariables.begin(),
                                                  rawPointerVariables.end(),
                                                  [&destination](const RawPointerVariable& variable) {
                                                      return variable.name == destination;
                                                  });
            if (rawVariable != rawPointerVariables.end()) {
                for (const SmartPointerExpression& expression : expressions) {
                    if (!typeMatches(rawVariable->pointeeType, expression.pointeeType, inheritance)) {
                        continue;
                    }
                    std::string matchedExpression;
                    if (!expressionMatchesWhole(rhs, expression, matchedExpression)) {
                        continue;
                    }
                    changed = true;
                    return match[1].str() + destination + " = " + matchedExpression + ".get()" + match[4].str();
                }
            }
        }
    }

    const std::regex copiedSmartPointerAssignment(R"(^([ \t]*)auto\s+([A-Za-z_]\w*)\s*=\s*([^;]+?)(\s*;\s*)$)");
    if (std::regex_match(codePart, match, copiedSmartPointerAssignment)) {
        const std::string rhs = trim(match[3].str());
        if (rhs.find(".get()") == std::string::npos
            && rhs.find("std::move") == std::string::npos
            && rhs.find(".release()") == std::string::npos
            && rhs.find("std::make_") == std::string::npos) {
            for (const SmartPointerExpression& expression : expressions) {
                if (!expression.moveOnly) {
                    continue;
                }
                std::string matchedExpression;
                if (!expressionMatchesWhole(rhs, expression, matchedExpression)) {
                    continue;
                }
                changed = true;
                return match[1].str() + "auto* " + match[2].str() + " = " + matchedExpression + ".get()" + match[4].str();
            }
        }
    }

    return codePart;
}

std::string removeSmartOwnedDeletes(std::string codePart,
                                    const std::vector<SmartPointerExpression>& expressions,
                                    bool& changed)
{
    std::smatch match;
    const std::regex deletePattern(R"(^[ \t]*delete(?:\s*\[\s*\])?\s+([^;]+?)\s*;\s*$)");
    if (!std::regex_match(codePart, match, deletePattern)) {
        return codePart;
    }

    const std::string deletedExpression = trim(match[1].str());
    if (deletedExpression.find(".get()") != std::string::npos) {
        return codePart;
    }
    for (const SmartPointerExpression& expression : expressions) {
        std::string matchedExpression;
        if (!expressionMatchesWhole(deletedExpression, expression, matchedExpression)) {
            continue;
        }
        changed = true;
        return {};
    }
    return codePart;
}

std::string propagateRawPointerContainerPush(std::string codePart,
                                             const std::vector<RawPointerContainer>& containers,
                                             const std::vector<SmartPointerExpression>& expressions,
                                             const std::map<std::string, std::string>& inheritance,
                                             bool& changed)
{
    for (const RawPointerContainer& container : containers) {
        if (codePart.find(container.name) == std::string::npos
            || codePart.find("push_back") == std::string::npos) {
            continue;
        }

        for (const SmartPointerExpression& expression : expressions) {
            if (!typeMatches(container.pointeeType, expression.pointeeType, inheritance)) {
                continue;
            }
            if (isSimpleIdentifier(expression.expression)) {
                const std::regex simplePushPattern("(" + escapeRegex(container.name) + "\\s*\\.\\s*push_back\\s*\\(\\s*)"
                                                   + escapeRegex(expression.expression)
                                                   + "(\\s*\\))");
                if (std::regex_search(codePart, simplePushPattern)) {
                    const std::string before = codePart;
                    codePart = std::regex_replace(codePart,
                                                  simplePushPattern,
                                                  "$1" + expression.expression + ".get()$2");
                    changed = changed || codePart != before;
                    continue;
                }
            }
            const std::regex pushPattern("(" + escapeRegex(container.name) + "\\s*\\.\\s*push_back\\s*\\(\\s*)("
                                         + expressionRegex(expression)
                                         + ")(\\s*\\))");
            std::smatch match;
            if (!std::regex_search(codePart, match, pushPattern)) {
                continue;
            }
            const std::string before = codePart;
            const std::string replacement = match[1].str() + match[2].str() + ".get()" + match[3].str();
            codePart.replace(static_cast<std::size_t>(match.position()),
                             static_cast<std::size_t>(match.length()),
                             replacement);
            changed = codePart != before;
        }
    }
    return codePart;
}
} // namespace

std::string SmartPointerSinkPropagationPass::rewrite(const std::string& code,
                                                     std::vector<ConversionChange>& changes) const
{
    const std::vector<RawPointerSink> sinks = collectRawPointerSinks(code);
    const std::set<std::string> smartPointerSinkNames = collectSmartPointerSinkNames(code);
    const std::vector<SmartPointerExpression> expressions = collectSmartPointerExpressions(code);
    const std::vector<RawPointerContainer> rawPointerContainers = collectRawPointerContainers(code);
    const std::vector<RawPointerVariable> rawPointerVariables = collectRawPointerVariables(code);
    if (expressions.empty()) {
        return code;
    }

    const std::map<std::string, std::string> inheritance = collectInheritance(code);
    const SafeReplacementEngine safeReplacement;
    bool changed = false;

    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::string before = codePart;

        codePart = removeSmartOwnedDeletes(std::move(codePart), expressions, changed);
        if (codePart.empty()) {
            addAppliedChange(changes,
                             "Smart pointer sink propagation",
                             trim(before),
                             "removed",
                             "Removed leftover manual delete for an object now owned by std::unique_ptr or std::shared_ptr.");
            return trailingComment.empty() ? std::string{} : trailingComment;
        }

        codePart = propagateRawPointerAssignments(std::move(codePart),
                                                  expressions,
                                                  rawPointerVariables,
                                                  inheritance,
                                                  changed);

        codePart = propagateRawPointerContainerPush(std::move(codePart),
                                                    rawPointerContainers,
                                                    expressions,
                                                    inheritance,
                                                    changed);

        for (const RawPointerSink& sink : sinks) {
            if (codePart.find(sink.functionName) == std::string::npos) {
                continue;
            }
            if (smartPointerSinkNames.find(sink.functionName) != smartPointerSinkNames.end()) {
                continue;
            }
            if (isRawPointerSinkSignatureLine(codePart, sink.functionName)) {
                continue;
            }
            for (const SmartPointerExpression& expression : expressions) {
                if (!typeMatches(sink.pointeeType, expression.pointeeType, inheritance)) {
                    continue;
                }
                codePart = appendGetForExpression(std::move(codePart), expression, changed);
            }
        }

        if (codePart != before) {
            addAppliedChange(changes,
                             "Smart pointer sink propagation",
                             trim(before),
                             trim(codePart),
                             "Passed a non-owning raw pointer view with .get() when a raw-pointer sink API or observer container is used.");
        }
        return codePart + trailingComment;
    });

    return changed ? updated : code;
}
