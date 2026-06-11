#include "converter/SmartPointerCollectionPropagationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct SmartPointerCollection
{
    std::string name;
    std::string pointeeType;
    bool isArray = false;
};

struct RawPointerFunction
{
    std::string name;
    std::string pointeeType;
};

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

bool sameTypeName(const std::string& left, const std::string& right)
{
    return trim(left) == trim(right);
}

std::string parseUniquePtrPointee(const std::string& typeText)
{
    std::smatch match;
    const std::regex uniquePtrPattern(R"(std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>)");
    if (std::regex_search(typeText, match, uniquePtrPattern)) {
        return match[1].str();
    }
    return {};
}

void addCollection(std::vector<SmartPointerCollection>& collections, SmartPointerCollection collection)
{
    if (collection.name.empty() || collection.pointeeType.empty()) {
        return;
    }
    const auto duplicate = std::find_if(collections.begin(), collections.end(), [&collection](const SmartPointerCollection& existing) {
        return existing.name == collection.name;
    });
    if (duplicate == collections.end()) {
        collections.push_back(std::move(collection));
    }
}

std::vector<SmartPointerCollection> collectSmartPointerCollections(const std::string& code,
                                                                   const TransformationContext& context)
{
    std::vector<SmartPointerCollection> collections;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (record.newType.starts_with("std::vector<std::unique_ptr<")
            || record.newType.starts_with("std::array<std::unique_ptr<")) {
            addCollection(collections,
                          SmartPointerCollection{
                              record.symbolName,
                              parseUniquePtrPointee(record.newType),
                              record.newType.starts_with("std::array<std::unique_ptr<"),
                          });
        }
    }

    const std::regex vectorDeclaration(
        R"((?:const\s+)?std::vector\s*<\s*std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), vectorDeclaration), end; iterator != end; ++iterator) {
        addCollection(collections,
                      SmartPointerCollection{
                          (*iterator)[2].str(),
                          (*iterator)[1].str(),
                          false,
                      });
    }

    const std::regex constVectorReferenceParameter(
        R"(const\s+std::vector\s*<\s*std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*>\s*&\s*([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), constVectorReferenceParameter), end; iterator != end; ++iterator) {
        addCollection(collections,
                      SmartPointerCollection{
                          (*iterator)[2].str(),
                          (*iterator)[1].str(),
                          false,
                      });
    }

    const std::regex arrayDeclaration(
        R"((?:const\s+)?std::array\s*<\s*std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*,\s*[^>]+>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), arrayDeclaration), end; iterator != end; ++iterator) {
        addCollection(collections,
                      SmartPointerCollection{
                          (*iterator)[2].str(),
                          (*iterator)[1].str(),
                          true,
                      });
    }

    std::stringstream stream(code);
    std::string line;
    const std::regex pointeePattern(R"(std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>)");
    const std::regex nameAfterTemplatePattern(R"((?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)");
    while (std::getline(stream, line)) {
        if (line.find("std::unique_ptr") == std::string::npos
            || (line.find("std::vector") == std::string::npos && line.find("std::array") == std::string::npos)) {
            continue;
        }
        std::smatch pointeeMatch;
        if (!std::regex_search(line, pointeeMatch, pointeePattern)) {
            continue;
        }
        const std::size_t lastTemplateClose = line.rfind('>');
        if (lastTemplateClose == std::string::npos || lastTemplateClose + 1 >= line.size()) {
            continue;
        }
        std::smatch nameMatch;
        const std::string tail = line.substr(lastTemplateClose + 1);
        if (!std::regex_search(tail, nameMatch, nameAfterTemplatePattern)) {
            continue;
        }
        addCollection(collections,
                      SmartPointerCollection{
                          nameMatch[1].str(),
                          pointeeMatch[1].str(),
                          line.find("std::array") != std::string::npos,
                      });
    }

    return collections;
}

std::vector<std::pair<std::string, std::string>> collectUniquePtrVariables(const std::string& code)
{
    std::vector<std::pair<std::string, std::string>> variables;
    const std::regex declarationPattern(
        R"(std::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s+([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        variables.emplace_back((*iterator)[2].str(), (*iterator)[1].str());
    }

    const std::regex autoMakeUniquePattern(
        R"(\bauto\s+([A-Za-z_]\w*)\s*=\s*std::make_unique\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*\()");
    for (std::sregex_iterator iterator(code.begin(), code.end(), autoMakeUniquePattern), end; iterator != end; ++iterator) {
        variables.emplace_back((*iterator)[1].str(), (*iterator)[2].str());
    }
    return variables;
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

std::string normalizeWhitespace(std::string value)
{
    value = trim(value);
    value = std::regex_replace(value, std::regex(R"(\s+)"), " ");
    value = std::regex_replace(value, std::regex(R"(\s*([*&])\s*)"), "$1");
    return value;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
}

std::vector<std::string> splitParameters(const std::string& parameterList)
{
    std::vector<std::string> parameters;
    std::string current;
    int angleDepth = 0;
    int parenDepth = 0;
    for (char character : parameterList) {
        if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (character == '(') {
            ++parenDepth;
        } else if (character == ')' && parenDepth > 0) {
            --parenDepth;
        }

        if (character == ',' && angleDepth == 0 && parenDepth == 0) {
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
    std::vector<std::string> normalized;
    for (std::string parameter : splitParameters(parameterList)) {
        parameter = std::regex_replace(parameter, std::regex(R"(\s*=\s*.*$)"), "");
        parameter = trim(parameter);
        if (parameter == "void") {
            continue;
        }
        parameter = std::regex_replace(parameter, std::regex(R"((.+[&*\w:>])\s+[A-Za-z_]\w*$)"), "$1");
        normalized.push_back(normalizeWhitespace(parameter));
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        if (index > 0) {
            output << ",";
        }
        output << normalized[index];
    }
    return output.str();
}

std::vector<RawPointerFunction> collectRawPointerFunctions(const std::string& code)
{
    std::vector<RawPointerFunction> functions;
    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?(?:\{|;))",
        std::regex::ECMAScript);

    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        const std::string name = (*iterator)[1].str();
        if (isOwnershipTransferName(name)) {
            continue;
        }
        const std::string parameters = (*iterator)[2].str();
        const std::regex rawPointerParameter(R"((?:^|,)\s*(?:const\s+)?([A-Za-z_:][A-Za-z0-9_:]*)\s*\*\s*[A-Za-z_]\w*)");
        for (std::sregex_iterator param(parameters.begin(), parameters.end(), rawPointerParameter), paramEnd; param != paramEnd; ++param) {
            functions.push_back(RawPointerFunction{name, (*param)[1].str()});
        }
    }

    return functions;
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

bool typeMatchesRawPointerParameter(const std::string& functionPointee,
                                    const std::string& smartPointee,
                                    const std::map<std::string, std::string>& inheritance)
{
    if (sameTypeName(functionPointee, smartPointee)) {
        return true;
    }

    std::string current = smartPointee;
    for (int depth = 0; depth < 8; ++depth) {
        const auto found = inheritance.find(current);
        if (found == inheritance.end()) {
            return false;
        }
        if (sameTypeName(found->second, functionPointee)) {
            return true;
        }
        current = found->second;
    }
    return false;
}

std::string makeUniqueExpression(const std::string& allocatedType, const std::string& arguments)
{
    return "std::make_unique<" + allocatedType + ">(" + trim(arguments) + ")";
}

std::string rewriteRawNewAssignments(std::string code,
                                     const std::vector<SmartPointerCollection>& collections,
                                     std::set<std::pair<std::string, std::string>>& polymorphicUses,
                                     std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        for (const SmartPointerCollection& collection : collections) {
            const std::string escapedName = escapeRegex(collection.name);
            std::smatch match;

            const std::regex elementAssignment(
                R"(^([ \t]*))" + escapedName
                    + R"(\s*\[\s*([^\]]+)\s*\]\s*=\s*new\s+([A-Za-z_:][A-Za-z0-9_:]*)\s*(?:\(([^;]*)\))?\s*;\s*$)");
            if (std::regex_match(codePart, match, elementAssignment)) {
                const std::string allocatedType = match[3].str();
                const std::string replacement = match[1].str() + collection.name + "[" + trim(match[2].str()) + "] = "
                    + makeUniqueExpression(allocatedType, match[4].matched ? match[4].str() : "") + ";";
                if (!sameTypeName(allocatedType, collection.pointeeType)) {
                    polymorphicUses.emplace(collection.pointeeType, allocatedType);
                }
                addAppliedChange(changes,
                                 "Smart pointer collection raw allocation to make_unique",
                                 trim(codePart),
                                 trim(replacement),
                                 "Propagated smart-pointer collection ownership by replacing raw new assignment with std::make_unique().");
                changed = true;
                return replacement + trailingComment;
            }

            const std::regex resetAssignment(
                R"(^([ \t]*))" + escapedName
                    + R"(\s*\[\s*([^\]]+)\s*\]\s*\.\s*reset\s*\(\s*new\s+([A-Za-z_:][A-Za-z0-9_:]*)\s*(?:\(([^;]*)\))?\s*\)\s*;\s*$)");
            if (std::regex_match(codePart, match, resetAssignment)) {
                const std::string allocatedType = match[3].str();
                const std::string replacement = match[1].str() + collection.name + "[" + trim(match[2].str()) + "] = "
                    + makeUniqueExpression(allocatedType, match[4].matched ? match[4].str() : "") + ";";
                if (!sameTypeName(allocatedType, collection.pointeeType)) {
                    polymorphicUses.emplace(collection.pointeeType, allocatedType);
                }
                addAppliedChange(changes,
                                 "Smart pointer collection raw allocation to make_unique",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced reset(new T(...)) with assignment from std::make_unique().");
                changed = true;
                return replacement + trailingComment;
            }

            const std::regex pushNew(
                R"(^([ \t]*))" + escapedName
                    + R"(\s*\.\s*(push_back|emplace_back)\s*\(\s*new\s+([A-Za-z_:][A-Za-z0-9_:]*)\s*(?:\(([^;]*)\))?\s*\)\s*;\s*$)");
            if (std::regex_match(codePart, match, pushNew)) {
                const std::string allocatedType = match[3].str();
                const std::string replacement = match[1].str() + collection.name + ".push_back("
                    + makeUniqueExpression(allocatedType, match[4].matched ? match[4].str() : "") + ");";
                if (!sameTypeName(allocatedType, collection.pointeeType)) {
                    polymorphicUses.emplace(collection.pointeeType, allocatedType);
                }
                addAppliedChange(changes,
                                 "Smart pointer collection raw allocation to make_unique",
                                 trim(codePart),
                                 trim(replacement),
                                 "Converted raw pointer insertion into push_back(std::make_unique<T>()).");
                changed = true;
                return replacement + trailingComment;
            }
        }
        return line;
    });

    return changed ? code : code;
}

std::string appendGetToCollectionArguments(std::string line,
                                           const SmartPointerCollection& collection,
                                           bool& changed)
{
    const std::regex indexedExpression(escapeRegex(collection.name) + R"(\s*\[\s*[^\]]+\s*\])");
    std::string updated;
    std::size_t last = 0;
    for (std::sregex_iterator iterator(line.begin(), line.end(), indexedExpression), end; iterator != end; ++iterator) {
        const std::size_t position = static_cast<std::size_t>(iterator->position());
        const std::size_t length = static_cast<std::size_t>(iterator->length());
        std::size_t next = position + length;
        while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next]))) {
            ++next;
        }
        if (next < line.size() && (line[next] == '.' || line[next] == '-' || line[next] == '[')) {
            continue;
        }
        updated.append(line, last, position - last);
        updated.append(iterator->str());
        updated.append(".get()");
        last = position + length;
        changed = true;
    }
    if (last == 0) {
        return line;
    }
    updated.append(line, last, std::string::npos);
    return updated;
}

std::string appendGetToUniquePtrVariables(std::string line,
                                          const std::vector<std::pair<std::string, std::string>>& uniquePtrVariables,
                                          const RawPointerFunction& function,
                                          const std::map<std::string, std::string>& inheritance,
                                          bool& changed)
{
    for (const auto& [variableName, pointeeType] : uniquePtrVariables) {
        if (!typeMatchesRawPointerParameter(function.pointeeType, pointeeType, inheritance)) {
            continue;
        }

        const std::regex variablePattern(R"(\b)" + escapeRegex(variableName) + R"(\b)");
        std::string updated;
        std::size_t last = 0;
        bool lineChanged = false;
        for (std::sregex_iterator iterator(line.begin(), line.end(), variablePattern), end; iterator != end; ++iterator) {
            const std::size_t position = static_cast<std::size_t>(iterator->position());
            const std::size_t length = static_cast<std::size_t>(iterator->length());
            std::size_t next = position + length;
            while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next]))) {
                ++next;
            }
            if (next < line.size() && (line[next] == '.' || line[next] == '-' || line[next] == '[')) {
                continue;
            }
            updated.append(line, last, position - last);
            updated.append(variableName);
            updated.append(".get()");
            last = position + length;
            lineChanged = true;
        }
        if (lineChanged) {
            updated.append(line, last, std::string::npos);
            line = updated;
            changed = true;
        }
    }
    return line;
}

std::string rewriteRawPointerCallsites(std::string code,
                                       const std::vector<SmartPointerCollection>& collections,
                                       const std::map<std::string, std::string>& inheritance,
                                       std::vector<ConversionChange>& changes)
{
    const std::vector<RawPointerFunction> functions = collectRawPointerFunctions(code);
    if (functions.empty()) {
        return code;
    }
    const std::vector<std::pair<std::string, std::string>> uniquePtrVariables = collectUniquePtrVariables(code);
    const SafeReplacementEngine safeReplacement;
    bool changed = false;

    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::string before = codePart;

        for (const RawPointerFunction& function : functions) {
            if (codePart.find(function.name + "(") == std::string::npos) {
                continue;
            }

            for (const SmartPointerCollection& collection : collections) {
                if (!typeMatchesRawPointerParameter(function.pointeeType, collection.pointeeType, inheritance)) {
                    continue;
                }
                codePart = appendGetToCollectionArguments(std::move(codePart), collection, changed);
            }
            codePart = appendGetToUniquePtrVariables(std::move(codePart), uniquePtrVariables, function, inheritance, changed);
        }

        if (codePart != before) {
            addAppliedChange(changes,
                             "Smart pointer collection raw pointer callsite to get",
                             trim(before),
                             trim(codePart),
                             "Passed a non-owning raw pointer view with .get() when calling APIs that still accept T*.");
        }
        return codePart + trailingComment;
    });

    return changed ? updated : code;
}

std::string preventUniquePtrCopies(std::string code,
                                   const std::vector<SmartPointerCollection>& collections,
                                   std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        for (const SmartPointerCollection& collection : collections) {
            std::smatch match;
            const std::regex rangeLoopPattern(R"(^([ \t]*)for\s*\(\s*auto\s+([A-Za-z_]\w*)\s*:\s*)"
                                              + escapeRegex(collection.name)
                                              + R"(\s*\)\s*$)");
            if (std::regex_match(codePart, match, rangeLoopPattern)) {
                const std::string replacement = match[1].str() + "for (const auto& " + match[2].str() + " : " + collection.name + ")";
                addAppliedChange(changes,
                                 "Unique_ptr collection copy prevention",
                                 trim(codePart),
                                 trim(replacement),
                                 "Avoided copying std::unique_ptr elements while traversing an owning collection.");
                changed = true;
                return replacement + trailingComment;
            }

            const std::regex indexedCopyPattern(R"(^([ \t]*)auto\s+([A-Za-z_]\w*)\s*=\s*)"
                                                + escapeRegex(collection.name)
                                                + R"(\s*\[\s*([^\]]+)\s*\]\s*;\s*$)");
            if (std::regex_match(codePart, match, indexedCopyPattern)) {
                const std::string replacement = match[1].str() + "auto* " + match[2].str() + " = "
                    + collection.name + "[" + trim(match[3].str()) + "].get();";
                addAppliedChange(changes,
                                 "Unique_ptr collection copy prevention",
                                 trim(codePart),
                                 trim(replacement),
                                 "Replaced accidental unique_ptr element copy with a non-owning raw pointer observation.");
                changed = true;
                return replacement + trailingComment;
            }
        }
        return line;
    });

    return changed ? code : code;
}

std::string rewriteCountLoops(std::string code,
                              const std::vector<SmartPointerCollection>& collections,
                              const ModernizationOptions& options,
                              std::vector<ConversionChange>& changes)
{
    if (!options.useLambdas && options.offlineModernizationLevel != OfflineModernizationLevel::AiStyleAggressiveRewrite) {
        return code;
    }

    bool changed = false;
    std::vector<std::string> lines = splitLines(code);
    for (std::size_t index = 0; index + 7 < lines.size(); ++index) {
        std::smatch counterMatch;
        const std::regex counterPattern(R"(^([ \t]*)(?:int|auto|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*$)");
        if (!std::regex_match(lines[index], counterMatch, counterPattern)) {
            continue;
        }

        const std::string indent = counterMatch[1].str();
        const std::string counterName = counterMatch[2].str();

        for (const SmartPointerCollection& collection : collections) {
            std::smatch loopMatch;
            const std::regex loopPattern("^" + escapeRegex(indent)
                                             + R"(for\s*\(\s*(?:int|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*)"
                                             + escapeRegex(collection.name)
                                             + R"(\.size\s*\(\s*\)\s*;\s*(?:\+\+\1|\1\+\+)\s*\)\s*$)");
            if (!std::regex_match(lines[index + 1], loopMatch, loopPattern)
                || trim(lines[index + 2]) != "{") {
                continue;
            }

            std::smatch ifMatch;
            const std::regex ifPattern("^" + escapeRegex(indent) + R"([ \t]*if\s*\((.+)\)\s*$)");
            if (!std::regex_match(lines[index + 3], ifMatch, ifPattern)
                || trim(lines[index + 4]) != "{") {
                continue;
            }

            const std::string increment = trim(lines[index + 5]);
            if ((increment != "++" + counterName + ";" && increment != counterName + "++;")
                || trim(lines[index + 6]) != "}"
                || trim(lines[index + 7]) != "}") {
                continue;
            }

            const std::string indexName = loopMatch[1].str();
            const std::string indexedExpression = escapeRegex(collection.name) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
            std::string condition = trim(ifMatch[1].str());
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*!=\s*nullptr)"), "item");
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*==\s*nullptr)"), "!item");
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*->)"), "item->");
            condition = std::regex_replace(condition,
                                           std::regex(R"(\b([A-Za-z_]\w*)\s*\(\s*)" + indexedExpression + R"((?:\.get\s*\(\s*\))?\s*\))"),
                                           "$1(item.get())");
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\.get\s*\(\s*\))"), "item.get()");
            condition = std::regex_replace(condition, std::regex(indexedExpression), "item");
            if (condition.find(indexName) != std::string::npos) {
                continue;
            }

            std::ostringstream before;
            for (std::size_t beforeIndex = index; beforeIndex <= index + 7; ++beforeIndex) {
                if (beforeIndex > index) {
                    before << '\n';
                }
                before << lines[beforeIndex];
            }

            std::vector<std::string> replacementLines{
                indent + "auto " + counterName + " = std::count_if(" + collection.name + ".begin(), " + collection.name + ".end(), [](const auto& item) {",
                indent + "    return " + condition + ";",
                indent + "});",
            };
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index),
                        lines.begin() + static_cast<std::ptrdiff_t>(index + 8));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index),
                         replacementLines.begin(),
                         replacementLines.end());

            addAppliedChange(changes,
                             "Smart pointer collection count loop to count_if",
                             trim(before.str()),
                             trim(joinLines(replacementLines)),
                             "Converted a read-only counting loop over a smart-pointer collection to std::count_if with a lambda.");
            changed = true;
            break;
        }
    }

    if (changed) {
        code = joinLines(lines);
        lines = splitLines(code);
    }

    for (std::size_t index = 0; index + 7 < lines.size(); ++index) {
        std::smatch counterMatch;
        const std::regex counterPattern(R"(^([ \t]*)(?:int|auto|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*$)");
        if (!std::regex_match(lines[index], counterMatch, counterPattern)) {
            continue;
        }

        const std::string indent = counterMatch[1].str();
        const std::string counterName = counterMatch[2].str();

        for (const SmartPointerCollection& collection : collections) {
            std::smatch loopMatch;
            const std::regex rangeLoopPattern("^" + escapeRegex(indent)
                                                 + R"(for\s*\(\s*(?:const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*)"
                                                 + escapeRegex(collection.name)
                                                 + R"(\s*\)\s*$)");
            if (!std::regex_match(lines[index + 1], loopMatch, rangeLoopPattern)
                || trim(lines[index + 2]) != "{") {
                continue;
            }

            std::smatch ifMatch;
            const std::regex ifPattern("^" + escapeRegex(indent) + R"([ \t]*if\s*\((.+)\)\s*$)");
            if (!std::regex_match(lines[index + 3], ifMatch, ifPattern)
                || trim(lines[index + 4]) != "{") {
                continue;
            }

            const std::string increment = trim(lines[index + 5]);
            if ((increment != "++" + counterName + ";" && increment != counterName + "++;")
                || trim(lines[index + 6]) != "}"
                || trim(lines[index + 7]) != "}") {
                continue;
            }

            const std::string itemName = loopMatch[1].str();
            std::string condition = trim(ifMatch[1].str());
            condition = std::regex_replace(condition,
                                           std::regex(R"(\b([A-Za-z_]\w*)\s*\(\s*)" + escapeRegex(itemName) + R"(\s*\))"),
                                           "$1(" + itemName + ".get())");

            std::ostringstream before;
            for (std::size_t beforeIndex = index; beforeIndex <= index + 7; ++beforeIndex) {
                if (beforeIndex > index) {
                    before << '\n';
                }
                before << lines[beforeIndex];
            }

            std::vector<std::string> replacementLines{
                indent + "auto " + counterName + " = std::count_if(" + collection.name + ".begin(), " + collection.name + ".end(), [](const auto& " + itemName + ") {",
                indent + "    return " + condition + ";",
                indent + "});",
            };
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index),
                        lines.begin() + static_cast<std::ptrdiff_t>(index + 8));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index),
                         replacementLines.begin(),
                         replacementLines.end());

            addAppliedChange(changes,
                             "Smart pointer collection count loop to count_if",
                             trim(before.str()),
                             trim(joinLines(replacementLines)),
                             "Converted a read-only counting range loop over a smart-pointer collection to std::count_if with a lambda.");
            changed = true;
            break;
        }
    }

    if (changed) {
        code = joinLines(lines);
    }

    for (const SmartPointerCollection& collection : collections) {
        const std::regex countLoopPattern(
            R"((^[ \t]*)(?:int|auto|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\n\1for\s*\(\s*(?:int|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\3\s*<\s*)"
                + escapeRegex(collection.name)
                + R"(\.size\s*\(\s*\)\s*;\s*(?:\+\+\3|\3\+\+)\s*\)\s*\n\1\{\s*\n\1[ \t]*if\s*\(([^\n{};]+)\)\s*\n\1[ \t]*\{\s*\n\1[ \t]*(?:\+\+\2|\2\+\+)\s*;\s*\n\1[ \t]*\}\s*\n\1\})",
            std::regex::ECMAScript | std::regex::multiline);

        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, countLoopPattern)) {
            std::string condition = trim(match[4].str());
            const std::string indexName = match[3].str();
            const std::string indexedExpression = escapeRegex(collection.name) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*!=\s*nullptr)"), "item");
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*==\s*nullptr)"), "!item");
            condition = std::regex_replace(condition, std::regex(indexedExpression + R"(\s*->)"), "item->");
            condition = std::regex_replace(condition,
                                           std::regex(R"(\b([A-Za-z_]\w*)\s*\(\s*)" + indexedExpression + R"(\s*\))"),
                                           "$1(item.get())");
            condition = std::regex_replace(condition, std::regex(indexedExpression), "item");

            if (condition.find(indexName) != std::string::npos) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            const std::string replacement = match[1].str() + "auto " + match[2].str()
                + " = std::count_if(" + collection.name + ".begin(), " + collection.name + ".end(), [](const auto& item) {\n"
                + match[1].str() + "    return " + condition + ";\n"
                + match[1].str() + "});";

            code.replace(consumed + static_cast<std::size_t>(match.position()),
                         static_cast<std::size_t>(match.length()),
                         replacement);
            addAppliedChange(changes,
                             "Smart pointer collection count loop to count_if",
                             trim(match[0].str()),
                             trim(replacement),
                             "Converted a read-only counting loop over a smart-pointer collection to std::count_if with a lambda.");
            changed = true;
            consumed += static_cast<std::size_t>(match.position()) + replacement.size();
            search = code.substr(consumed);
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        code = includeManager.ensureInclude(std::move(code), "#include <algorithm>");
    }
    return code;
}

void collectExistingPolymorphicUses(const std::string& code,
                                    const std::vector<SmartPointerCollection>& collections,
                                    std::set<std::pair<std::string, std::string>>& polymorphicUses)
{
    for (const SmartPointerCollection& collection : collections) {
        std::smatch match;
        const std::regex assignmentPattern(
            escapeRegex(collection.name)
            + R"(\s*\[[^\]]+\]\s*=\s*std::make_unique\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*\()");
        for (std::sregex_iterator iterator(code.begin(), code.end(), assignmentPattern), end; iterator != end; ++iterator) {
            const std::string allocatedType = (*iterator)[1].str();
            if (!sameTypeName(allocatedType, collection.pointeeType)) {
                polymorphicUses.emplace(collection.pointeeType, allocatedType);
            }
        }

        const std::regex pushPattern(
            escapeRegex(collection.name)
            + R"(\s*\.\s*(?:push_back|emplace_back)\s*\(\s*std::make_unique\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*\()");
        for (std::sregex_iterator iterator(code.begin(), code.end(), pushPattern), end; iterator != end; ++iterator) {
            const std::string allocatedType = (*iterator)[1].str();
            if (!sameTypeName(allocatedType, collection.pointeeType)) {
                polymorphicUses.emplace(collection.pointeeType, allocatedType);
            }
        }
    }
}

std::map<std::string, std::vector<VirtualMethod>> collectVirtualMethodsByClass(const std::string& code)
{
    std::map<std::string, std::vector<VirtualMethod>> methods;
    const ClassResourceAnalyzer analyzer;
    for (const ClassBlock& block : analyzer.analyzeClasses(code)) {
        const std::regex virtualMethodPattern(
            R"(\bvirtual\s+(?!~)(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(const)?\s*(?:=\s*0)?\s*[;{])");
        for (std::sregex_iterator iterator(block.text.begin(), block.text.end(), virtualMethodPattern), end; iterator != end; ++iterator) {
            methods[block.name].push_back(VirtualMethod{
                (*iterator)[1].str(),
                normalizeParameterList((*iterator)[2].str()),
                (*iterator)[3].matched,
            });
        }
    }
    return methods;
}

bool classHasVirtualMethod(const std::string& classText)
{
    return std::regex_search(classText, std::regex(R"(\bvirtual\s+(?!~)[^;{}()]+\s+[A-Za-z_]\w*\s*\()"));
}

std::string addVirtualDestructors(std::string code,
                                  const std::set<std::pair<std::string, std::string>>& polymorphicUses,
                                  std::vector<ConversionChange>& changes)
{
    if (polymorphicUses.empty()) {
        return code;
    }

    std::set<std::string> basesToReview;
    for (const auto& [base, derived] : polymorphicUses) {
        if (!sameTypeName(base, derived)) {
            basesToReview.insert(base);
        }
    }
    if (basesToReview.empty()) {
        return code;
    }

    const ClassResourceAnalyzer analyzer;
    std::vector<ClassBlock> classes = analyzer.analyzeClasses(code);
    std::sort(classes.begin(), classes.end(), [](const ClassBlock& left, const ClassBlock& right) {
        return left.start > right.start;
    });

    for (const ClassBlock& block : classes) {
        if (!basesToReview.contains(block.name) || !classHasVirtualMethod(block.text)) {
            continue;
        }

        const std::regex destructorPattern(R"((?:virtual\s+)?~)" + escapeRegex(block.name) + R"(\s*\()");
        std::smatch destructorMatch;
        std::string replacement = block.text;
        if (std::regex_search(block.text, destructorMatch, destructorPattern)) {
            const std::string destructorText = destructorMatch[0].str();
            if (destructorText.find("virtual") == std::string::npos) {
                replacement.replace(static_cast<std::size_t>(destructorMatch.position()),
                                    static_cast<std::size_t>(destructorMatch.length()),
                                    "virtual ~" + block.name + "(");
                addAppliedChange(changes,
                                 "Add virtual destructor for polymorphic base",
                                 trim(destructorText),
                                 "virtual ~" + block.name + "(",
                                 "Marked an existing base destructor virtual because the type is owned polymorphically.");
            }
        } else {
            const std::string destructorLine = "    virtual ~" + block.name + "() = default;\n";
            const std::regex publicPattern(R"((public\s*:\s*\n))");
            std::smatch publicMatch;
            if (std::regex_search(replacement, publicMatch, publicPattern)) {
                replacement.insert(static_cast<std::size_t>(publicMatch.position() + publicMatch.length()), destructorLine);
            } else if (block.keyword == "class") {
                const std::size_t brace = replacement.find('{');
                replacement.insert(brace + 1, "\npublic:\n" + destructorLine);
            } else {
                const std::size_t brace = replacement.find('{');
                replacement.insert(brace + 1, "\n" + destructorLine);
            }
            addAppliedChange(changes,
                             "Add virtual destructor for polymorphic base",
                             block.name,
                             "virtual ~" + block.name + "() = default;",
                             "Added a virtual default destructor because this base has virtual methods and is owned through std::unique_ptr<Base>.");
        }

        if (replacement != block.text) {
            code.replace(block.start, block.end - block.start, replacement);
        }
    }

    return code;
}

std::string addOverrideAnnotations(std::string code,
                                   const std::map<std::string, std::string>& inheritance,
                                   std::vector<ConversionChange>& changes)
{
    if (inheritance.empty()) {
        return code;
    }

    const std::map<std::string, std::vector<VirtualMethod>> virtualMethodsByClass = collectVirtualMethodsByClass(code);
    if (virtualMethodsByClass.empty()) {
        return code;
    }

    const ClassResourceAnalyzer analyzer;
    std::vector<ClassBlock> classes = analyzer.analyzeClasses(code);
    std::sort(classes.begin(), classes.end(), [](const ClassBlock& left, const ClassBlock& right) {
        return left.start > right.start;
    });

    for (const ClassBlock& block : classes) {
        const auto baseIt = inheritance.find(block.name);
        if (baseIt == inheritance.end()) {
            continue;
        }
        const auto virtualIt = virtualMethodsByClass.find(baseIt->second);
        if (virtualIt == virtualMethodsByClass.end()) {
            continue;
        }

        std::string replacement = block.text;
        for (const VirtualMethod& method : virtualIt->second) {
            const std::regex methodHeader(
                R"((^[ \t]*)(virtual\s+)?((?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+))"
                    + escapeRegex(method.name)
                    + R"(\s*\(([^)]*)\)\s*(const)?\s*(?![^;\n{]*\boverride\b)([;{]))",
                std::regex::ECMAScript | std::regex::multiline);

            std::smatch match;
            std::string search = replacement;
            std::size_t consumed = 0;
            while (std::regex_search(search, match, methodHeader)) {
                if (normalizeParameterList(match[4].str()) != method.normalizedParameters
                    || static_cast<bool>(match[5].matched) != method.isConst) {
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }

                const std::string before = match[0].str();
                const std::string after = match[1].str()
                    + match[3].str()
                    + method.name + "(" + match[4].str() + ")"
                    + (method.isConst ? " const" : "")
                    + " override" + match[6].str();
                replacement.replace(consumed + static_cast<std::size_t>(match.position()),
                                    static_cast<std::size_t>(match.length()),
                                    after);
                addAppliedChange(changes,
                                 "Add override to derived virtual method",
                                 trim(before),
                                 trim(after),
                                 "Added override where a visible derived method exactly matches a base virtual signature.");
                consumed += static_cast<std::size_t>(match.position()) + after.size();
                search = replacement.substr(consumed);
            }
        }

        if (replacement != block.text) {
            code.replace(block.start, block.end - block.start, replacement);
        }
    }

    return code;
}

std::string reportRemainingRawNew(const std::string& code,
                                  const std::vector<SmartPointerCollection>& collections,
                                  std::vector<ConversionChange>& changes)
{
    for (const SmartPointerCollection& collection : collections) {
        const std::regex rawNewPattern(escapeRegex(collection.name) + R"((?:\s*\[[^\]]+\]|\s*\.\s*(?:push_back|emplace_back)\s*\()\s*(?:=)?\s*new\s+)");
        if (std::regex_search(code, rawNewPattern)) {
            addSuggestion(changes,
                          "Smart pointer collection propagation",
                          collection.name,
                          "Raw new still appears near a smart-pointer collection. The converter preserved the code because the ownership interaction was not simple enough to rewrite safely.");
        }
    }
    return code;
}
} // namespace

std::string SmartPointerCollectionPropagationPass::rewrite(const std::string& code,
                                                           const ModernizationOptions& options,
                                                           const TransformationContext& context,
                                                           std::vector<ConversionChange>& changes) const
{
    if (!options.useSmartPointers || !options.applySafeOwnershipModernization) {
        return code;
    }

    const std::vector<SmartPointerCollection> collections = collectSmartPointerCollections(code, context);
    if (collections.empty()) {
        return code;
    }

    std::set<std::pair<std::string, std::string>> polymorphicUses;
    const std::map<std::string, std::string> inheritance = collectInheritance(code);

    std::string updated = rewriteRawNewAssignments(code, collections, polymorphicUses, changes);
    collectExistingPolymorphicUses(updated, collections, polymorphicUses);
    updated = rewriteRawPointerCallsites(std::move(updated), collections, inheritance, changes);
    updated = preventUniquePtrCopies(std::move(updated), collections, changes);
    updated = rewriteCountLoops(std::move(updated), collections, options, changes);
    updated = addVirtualDestructors(std::move(updated), polymorphicUses, changes);
    updated = addOverrideAnnotations(std::move(updated), inheritance, changes);
    updated = reportRemainingRawNew(std::move(updated), collections, changes);

    if (updated.find("std::make_unique") != std::string::npos || updated.find("std::unique_ptr") != std::string::npos) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(std::move(updated), "#include <memory>");
    }
    if (updated.find("std::count_if") != std::string::npos) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(std::move(updated), "#include <algorithm>");
    }

    return updated;
}
