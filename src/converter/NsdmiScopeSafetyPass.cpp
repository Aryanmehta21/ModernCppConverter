#include "converter/NsdmiScopeSafetyPass.h"

#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct ConstructorInfo
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t openBrace = 0;
    std::string text;
    std::vector<std::string> parameters;
    std::vector<std::string> locals;
};

struct MemberInitializer
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string indentation;
    std::string type;
    std::string name;
    std::string initializer;
};

struct Replacement
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
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

void addChange(std::vector<ConversionChange>& changes,
               std::string ruleName,
               std::string before,
               std::string after,
               std::string reason,
               bool applied)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        applied,
        false,
    });
}

std::size_t findMatchingBrace(const std::string& text, std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < text.size(); ++index) {
        const char character = text[index];
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
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

bool referencesIdentifier(const std::string& expression, const std::string& identifier)
{
    return std::regex_search(expression, std::regex("\\b" + escapeRegex(identifier) + "\\b"));
}

std::vector<std::string> splitParameters(const std::string& parameters)
{
    std::vector<std::string> result;
    std::string current;
    int angleDepth = 0;
    int parenDepth = 0;
    for (const char character : parameters) {
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
            result.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!trim(current).empty()) {
        result.push_back(trim(current));
    }
    return result;
}

std::vector<std::string> parameterNames(const std::string& header)
{
    const std::size_t open = header.find('(');
    const std::size_t close = header.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return {};
    }

    std::vector<std::string> names;
    for (std::string parameter : splitParameters(header.substr(open + 1, close - open - 1))) {
        const std::size_t defaultPosition = parameter.find('=');
        if (defaultPosition != std::string::npos) {
            parameter = trim(parameter.substr(0, defaultPosition));
        }
        if (parameter.empty() || parameter == "void") {
            continue;
        }
        std::smatch match;
        if (std::regex_search(parameter, match, std::regex(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$)"))) {
            names.push_back(match[1].str());
        }
    }
    return names;
}

std::vector<std::string> localSymbols(const std::string& body)
{
    std::vector<std::string> symbols;
    const std::regex declarationPattern(
        R"((?:^|[;\n]\s*)(?:auto|const\s+auto|std::[A-Za-z_][A-Za-z0-9_:<> ,&*]*|[A-Za-z_:][A-Za-z0-9_:<>]*)(?:\s+|\s*[*&]\s*)([A-Za-z_]\w*)\s*(?:=|\{|;|\)))",
        std::regex::ECMAScript);
    for (std::sregex_iterator it(body.begin(), body.end(), declarationPattern), end; it != end; ++it) {
        symbols.push_back((*it)[1].str());
    }
    return symbols;
}

std::vector<ConstructorInfo> constructorsForClass(const std::string& classText, const std::string& className)
{
    std::vector<ConstructorInfo> constructors;
    const std::regex constructorHeader("\\b" + escapeRegex(className) + R"(\s*\([^;{}]*\)\s*(?::[^{]*)?\{)");
    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, constructorHeader)) {
        const std::size_t start = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = classText.find('{', start);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }
        const std::string constructorText = classText.substr(start, closeBrace - start + 1);
        const std::string header = classText.substr(start, openBrace - start);
        const std::string body = classText.substr(openBrace + 1, closeBrace - openBrace - 1);
        constructors.push_back(ConstructorInfo{
            start,
            closeBrace + 1,
            openBrace,
            constructorText,
            parameterNames(header),
            localSymbols(body),
        });
        consumed = closeBrace + 1;
        search = classText.substr(consumed);
    }
    return constructors;
}

void updateBraceDepthForLine(const std::string& line, int& depth)
{
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (const char character : line) {
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
        }
    }
}

std::vector<MemberInitializer> topLevelMemberInitializers(const std::string& classText)
{
    std::vector<MemberInitializer> initializers;
    std::size_t lineStart = 0;
    int depth = 0;
    const std::regex memberPattern(
        R"(^([ \t]*)([A-Za-z_:][A-Za-z0-9_:<> ,*&]*?)\s+([A-Za-z_]\w*)\s*=\s*([^;\n]+)\s*;\s*(?://.*)?$)",
        std::regex::ECMAScript);

    while (lineStart <= classText.size()) {
        const std::size_t lineEnd = classText.find('\n', lineStart);
        const std::size_t end = lineEnd == std::string::npos ? classText.size() : lineEnd;
        const std::string line = classText.substr(lineStart, end - lineStart);
        const int depthBeforeLine = depth;

        std::smatch match;
        if (depthBeforeLine == 1 && std::regex_match(line, match, memberPattern)) {
            const std::string type = trim(match[2].str());
            if (type.find("return") == std::string::npos
                && type.find("if ") == std::string::npos
                && type.find("for ") == std::string::npos) {
                initializers.push_back(MemberInitializer{
                    lineStart,
                    end,
                    match[1].str(),
                    type,
                    match[3].str(),
                    trim(match[4].str()),
                });
            }
        }

        updateBraceDepthForLine(line, depth);
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return initializers;
}

std::vector<std::string> referencedSymbols(const std::string& expression, const std::vector<std::string>& candidates)
{
    std::vector<std::string> result;
    for (const std::string& candidate : candidates) {
        if (referencesIdentifier(expression, candidate)) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::string bodyIndentForConstructor(const std::string& constructorText, const std::size_t relativeOpenBrace)
{
    const std::size_t nextLine = constructorText.find('\n', relativeOpenBrace);
    if (nextLine != std::string::npos) {
        const std::size_t firstNonSpace = constructorText.find_first_not_of(" \t\r\n", nextLine + 1);
        if (firstNonSpace != std::string::npos) {
            const std::size_t lineStart = constructorText.rfind('\n', firstNonSpace);
            if (lineStart != std::string::npos) {
                return constructorText.substr(lineStart + 1, firstNonSpace - lineStart - 1);
            }
        }
    }

    const std::size_t headerLineStart = constructorText.rfind('\n', relativeOpenBrace);
    std::string headerIndent;
    if (headerLineStart == std::string::npos) {
        const std::size_t firstNonSpace = constructorText.find_first_not_of(" \t");
        headerIndent = firstNonSpace == std::string::npos ? "" : constructorText.substr(0, firstNonSpace);
    } else {
        const std::size_t firstNonSpace = constructorText.find_first_not_of(" \t", headerLineStart + 1);
        headerIndent = firstNonSpace == std::string::npos ? "" : constructorText.substr(headerLineStart + 1, firstNonSpace - headerLineStart - 1);
    }
    return headerIndent + "    ";
}

std::size_t insertionAfterLocalDeclarations(const ConstructorInfo& constructor,
                                            const std::vector<std::string>& localDependencies)
{
    std::size_t insertion = constructor.openBrace + 1;
    for (const std::string& local : localDependencies) {
        const std::regex declarationLine(
            R"((^|[\n])([ \t]*(?:auto|const\s+auto|std::[A-Za-z_][A-Za-z0-9_:<> ,&*]*|[A-Za-z_:][A-Za-z0-9_:<>]*)(?:\s+|\s*[*&]\s*)"
            + escapeRegex(local) + R"(\s*(?:=|\{|;)[^\n;]*;))",
            std::regex::ECMAScript);
        std::smatch match;
        if (std::regex_search(constructor.text, match, declarationLine)) {
            const std::size_t relativeEnd = static_cast<std::size_t>(match.position()) + static_cast<std::size_t>(match.length());
            insertion = std::max(insertion, constructor.start + relativeEnd);
        }
    }
    return insertion;
}

bool constructorAlreadyAssigns(const ConstructorInfo& constructor, const std::string& memberName)
{
    return std::regex_search(constructor.text,
                             std::regex(R"(^[ \t]*)" + escapeRegex(memberName) + R"(\s*=)",
                                        std::regex::ECMAScript | std::regex::multiline));
}

std::string applyReplacements(std::string text, std::vector<Replacement> replacements)
{
    std::stable_sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        if (left.start == right.start) {
            return left.text < right.text;
        }
        return left.start > right.start;
    });
    for (const Replacement& replacement : replacements) {
        text.replace(replacement.start, replacement.end - replacement.start, replacement.text);
    }
    return text;
}
} // namespace

std::string NsdmiScopeSafetyPass::validateAndRepair(const std::string& code,
                                                    std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    std::vector<ClassBlock> classes = ClassResourceAnalyzer().analyzeClasses(updated);
    std::sort(classes.begin(), classes.end(), [](const ClassBlock& left, const ClassBlock& right) {
        return left.start > right.start;
    });

    for (const ClassBlock& block : classes) {
        std::string classText = updated.substr(block.start, block.end - block.start);
        const std::vector<ConstructorInfo> constructors = constructorsForClass(classText, block.name);
        const std::vector<MemberInitializer> initializers = topLevelMemberInitializers(classText);
        if (initializers.empty()) {
            continue;
        }

        std::vector<Replacement> replacements;
        bool classChanged = false;
        for (const MemberInitializer& initializer : initializers) {
            bool initializerInvalidInClassScope = false;
            bool repairedInConstructor = false;

            for (const ConstructorInfo& constructor : constructors) {
                const std::vector<std::string> parameterDependencies = referencedSymbols(initializer.initializer, constructor.parameters);
                const std::vector<std::string> localDependencies = referencedSymbols(initializer.initializer, constructor.locals);
                if (parameterDependencies.empty() && localDependencies.empty()) {
                    continue;
                }

                initializerInvalidInClassScope = true;
                if (constructorAlreadyAssigns(constructor, initializer.name)) {
                    repairedInConstructor = true;
                    continue;
                }

                const std::size_t relativeOpenBrace = constructor.openBrace - constructor.start;
                const std::string indent = bodyIndentForConstructor(constructor.text, relativeOpenBrace);
                const std::size_t insertion = localDependencies.empty()
                    ? constructor.openBrace + 1
                    : insertionAfterLocalDeclarations(constructor, localDependencies);
                replacements.push_back(Replacement{
                    insertion,
                    insertion,
                    "\n" + indent + initializer.name + " = " + initializer.initializer + ";",
                });
                repairedInConstructor = true;
            }

            if (!initializerInvalidInClassScope) {
                continue;
            }

            const std::string before = classText.substr(initializer.start, initializer.end - initializer.start);
            const std::string after = initializer.indentation + initializer.type + " " + initializer.name + ";";
            replacements.push_back(Replacement{initializer.start, initializer.end, after});
            classChanged = true;

            addChange(changes,
                      "NsdmiScopeSafetyPass",
                      trim(before),
                      trim(after),
                      repairedInConstructor
                          ? "Moved an inline member initializer back into constructor scope because it referenced constructor-scoped symbols."
                          : "Removed an inline member initializer that referenced symbols unavailable at class scope; manual initialization review is recommended.",
                      true);
        }

        if (!classChanged) {
            continue;
        }

        classText = applyReplacements(std::move(classText), std::move(replacements));
        updated.replace(block.start, block.end - block.start, classText);
    }

    return updated;
}
