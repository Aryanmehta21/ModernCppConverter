#include "converter/ConstPointerParameterModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct ParameterInfo
{
    std::string original;
    std::string type;
    std::string name;
    std::size_t relativeStart = 0;
    std::size_t relativeEnd = 0;
    bool pointer = false;
    bool constPointer = false;
};

struct FunctionInfo
{
    std::string name;
    std::string header;
    std::string body;
    std::size_t headerStart = 0;
    std::size_t openParen = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
    std::vector<ParameterInfo> parameters;
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
    escaped.reserve(text.size() * 2U);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string maskCommentsAndLiterals(const std::string& code)
{
    std::string masked = code;
    enum class State {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharLiteral,
    };

    State state = State::Code;
    bool escaped = false;
    for (std::size_t index = 0; index < code.size(); ++index) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';

        switch (state) {
        case State::Code:
            if (current == '/' && next == '/') {
                masked[index] = ' ';
                masked[index + 1] = ' ';
                ++index;
                state = State::LineComment;
            } else if (current == '/' && next == '*') {
                masked[index] = ' ';
                masked[index + 1] = ' ';
                ++index;
                state = State::BlockComment;
            } else if (current == '"') {
                masked[index] = ' ';
                escaped = false;
                state = State::StringLiteral;
            } else if (current == '\'') {
                masked[index] = ' ';
                escaped = false;
                state = State::CharLiteral;
            }
            break;
        case State::LineComment:
            if (current == '\n') {
                state = State::Code;
            } else {
                masked[index] = ' ';
            }
            break;
        case State::BlockComment:
            masked[index] = ' ';
            if (current == '*' && next == '/') {
                masked[index + 1] = ' ';
                ++index;
                state = State::Code;
            }
            break;
        case State::StringLiteral:
            masked[index] = ' ';
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                state = State::Code;
            }
            break;
        case State::CharLiteral:
            masked[index] = ' ';
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '\'') {
                state = State::Code;
            }
            break;
        }
    }
    return masked;
}

std::size_t findMatching(const std::string& code,
                         const std::size_t openPosition,
                         const char openCharacter,
                         const char closeCharacter)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openPosition; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (current == openCharacter) {
            ++depth;
        } else if (current == closeCharacter) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

std::vector<ParameterInfo> splitParametersWithOffsets(const std::string& parameters)
{
    std::vector<ParameterInfo> result;
    int angleDepth = 0;
    int parenDepth = 0;
    std::size_t tokenStart = 0;

    auto appendParameter = [&](const std::size_t end) {
        std::string original = parameters.substr(tokenStart, end - tokenStart);
        const std::size_t leading = original.find_first_not_of(" \t\r\n");
        const std::size_t trailing = original.find_last_not_of(" \t\r\n");
        if (leading == std::string::npos || trailing == std::string::npos) {
            return;
        }
        const std::size_t absoluteStart = tokenStart + leading;
        const std::size_t absoluteEnd = tokenStart + trailing + 1;
        result.push_back(ParameterInfo{
            parameters.substr(absoluteStart, absoluteEnd - absoluteStart),
            {},
            {},
            absoluteStart,
            absoluteEnd,
            false,
            false,
        });
    };

    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const char character = parameters[index];
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
            appendParameter(index);
            tokenStart = index + 1;
        }
    }
    appendParameter(parameters.size());
    return result;
}

bool parsePointerParameter(ParameterInfo& parameter)
{
    std::string candidate = parameter.original;
    const std::size_t defaultPosition = candidate.find('=');
    if (defaultPosition != std::string::npos) {
        candidate = trim(candidate.substr(0, defaultPosition));
    }
    if (candidate.find("...") != std::string::npos || candidate.find("&&") != std::string::npos) {
        return false;
    }

    std::smatch match;
    static const std::regex pointerParameterPattern(R"(^(.+?)\s*\*\s*([A-Za-z_]\w*)$)",
                                                    std::regex::ECMAScript);
    if (!std::regex_match(candidate, match, pointerParameterPattern)) {
        return false;
    }

    parameter.type = trim(match[1].str());
    parameter.name = match[2].str();
    parameter.pointer = true;
    parameter.constPointer = std::regex_search(parameter.type, std::regex(R"(\bconst\b)"));
    return !parameter.type.empty() && !parameter.name.empty();
}

std::vector<ParameterInfo> parseParameters(const std::string& header)
{
    const std::size_t open = header.find('(');
    const std::size_t close = header.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return {};
    }

    std::vector<ParameterInfo> parameters = splitParametersWithOffsets(header.substr(open + 1, close - open - 1));
    for (ParameterInfo& parameter : parameters) {
        (void)parsePointerParameter(parameter);
    }
    return parameters;
}

bool isSkippedPointerType(std::string type)
{
    type = trim(std::regex_replace(type, std::regex(R"(\bconst\b)"), ""));
    static const std::set<std::string> skippedTypes{
        "void", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t", "char32_t", "std::byte",
        "FILE", "std::FILE",
    };
    return skippedTypes.find(type) != skippedTypes.end();
}

std::string canonicalType(std::string type)
{
    type = trim(std::regex_replace(type, std::regex(R"(\bconst\b)"), ""));
    type = std::regex_replace(type, std::regex(R"(\s+)"), " ");
    return trim(type);
}

std::string extractFunctionName(const std::string& header)
{
    const std::size_t open = header.find('(');
    if (open == std::string::npos) {
        return {};
    }
    std::size_t cursor = open;
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(header[cursor - 1])) != 0) {
        --cursor;
    }
    const std::size_t end = cursor;
    while (cursor > 0) {
        const char previous = header[cursor - 1];
        if (std::isalnum(static_cast<unsigned char>(previous)) != 0 || previous == '_' || previous == ':' || previous == '~') {
            --cursor;
            continue;
        }
        break;
    }
    std::string name = header.substr(cursor, end - cursor);
    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos) {
        name = name.substr(scope + 2);
    }
    return name;
}

bool isFunctionLikeKeyword(const std::string& name)
{
    static const std::set<std::string> keywords{
        "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof",
    };
    return keywords.find(name) != keywords.end();
}

std::vector<FunctionInfo> collectFunctions(const std::string& code)
{
    std::vector<FunctionInfo> functions;
    const std::string masked = maskCommentsAndLiterals(code);
    static const std::regex functionPattern(
        R"((?:^|\n)([ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:~][A-Za-z0-9_:<>,\s*&*]*\s+)*[A-Za-z_:~][A-Za-z0-9_:~]*\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*)?\s*)\{)",
        std::regex::ECMAScript);

    for (std::sregex_iterator iterator(masked.begin(), masked.end(), functionPattern), end; iterator != end; ++iterator) {
        std::size_t headerStart = static_cast<std::size_t>(iterator->position(1));
        std::string header = code.substr(headerStart, static_cast<std::size_t>((*iterator)[1].length()));
        const std::size_t openBrace = static_cast<std::size_t>(iterator->position(0) + iterator->length(0) - 1);
        const std::size_t closeBrace = findMatching(code, openBrace, '{', '}');
        if (closeBrace == std::string::npos) {
            continue;
        }
        FunctionInfo function;
        function.headerStart = headerStart;
        function.header = std::move(header);
        function.name = extractFunctionName(function.header);
        if (function.name.empty() || isFunctionLikeKeyword(function.name)) {
            continue;
        }
        function.openParen = function.headerStart + function.header.find('(');
        function.openBrace = openBrace;
        function.closeBrace = closeBrace;
        function.body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        function.parameters = parseParameters(function.header);
        functions.push_back(std::move(function));
    }
    return functions;
}

std::map<std::string, std::set<std::string>> collectConstMethods(const std::string& code)
{
    std::map<std::string, std::set<std::string>> methods;
    static const std::regex classPattern(R"(\b(?:class|struct)\s+([A-Za-z_]\w*)[^{;]*\{)",
                                         std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), classPattern), end; iterator != end; ++iterator) {
        const std::string className = (*iterator)[1].str();
        const std::size_t openBrace = static_cast<std::size_t>(iterator->position() + iterator->length() - 1);
        const std::size_t closeBrace = findMatching(code, openBrace, '{', '}');
        if (closeBrace == std::string::npos) {
            continue;
        }
        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        static const std::regex constMethodPattern(
            R"(\b([A-Za-z_~]\w*)\s*\([^;{}]*\)\s*const\b)",
            std::regex::ECMAScript);
        for (std::sregex_iterator method(body.begin(), body.end(), constMethodPattern), methodEnd; method != methodEnd; ++method) {
            methods[className].insert((*method)[1].str());
        }
    }
    return methods;
}

struct KnownFunctionParameter
{
    std::string type;
    bool pointer = false;
    bool constPointer = false;
};

std::map<std::string, std::vector<KnownFunctionParameter>> collectKnownFunctionParameters(const std::vector<FunctionInfo>& functions)
{
    std::map<std::string, std::vector<KnownFunctionParameter>> known;
    for (const FunctionInfo& function : functions) {
        std::vector<KnownFunctionParameter> parameters;
        for (const ParameterInfo& parameter : function.parameters) {
            parameters.push_back(KnownFunctionParameter{
                canonicalType(parameter.type),
                parameter.pointer,
                parameter.constPointer,
            });
        }
        known[function.name] = std::move(parameters);
    }
    return known;
}

std::vector<std::string> splitArguments(const std::string& arguments)
{
    std::vector<std::string> result;
    std::string current;
    int angleDepth = 0;
    int parenDepth = 0;
    for (const char character : arguments) {
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

bool isKeywordLikeCall(const std::string& name)
{
    static const std::set<std::string> keywords{
        "if", "for", "while", "switch", "sizeof", "static_cast", "reinterpret_cast", "const_cast",
        "dynamic_cast", "alignof", "decltype",
    };
    return keywords.find(name) != keywords.end();
}

bool expressionIsParameterName(const std::string& expression, const std::string& name)
{
    return trim(expression) == name;
}

std::string findUnsafeReason(const std::string& body,
                             const ParameterInfo& parameter,
                             const std::map<std::string, std::set<std::string>>& constMethods,
                             const std::map<std::string, std::vector<KnownFunctionParameter>>& knownFunctions)
{
    const std::string maskedBody = maskCommentsAndLiterals(body);
    const std::string name = escapeRegex(parameter.name);

    if (std::regex_search(maskedBody, std::regex(R"(\bdelete\s+)" + name + R"(\b)"))) {
        return "pointer is deleted";
    }
    if (std::regex_search(maskedBody, std::regex(R"(\bnew\s*\(\s*)" + name + R"(\s*\))"))) {
        return "pointer is used for placement new";
    }
    if (std::regex_search(maskedBody, std::regex(R"(\b(?:const_cast|reinterpret_cast)\s*<[^>]+>\s*\(\s*)" + name + R"(\s*\))"))
        || std::regex_search(maskedBody, std::regex(R"(\([^)]+\*\)\s*)" + name + R"(\b)"))) {
        return "pointer is cast in a way that may remove const safety";
    }
    if (std::regex_search(maskedBody, std::regex(R"(\b)" + name + R"(\s*(?:\+\+|--|\+=|-=|\[[^\]]+\]))"))
        || std::regex_search(maskedBody, std::regex(R"((?:\+\+|--)\s*)" + name + R"(\b)"))
        || std::regex_search(maskedBody, std::regex(R"(\b)" + name + R"(\s*[+-]\s*\d+)"))) {
        return "pointer arithmetic or indexed pointer access detected";
    }
    if (std::regex_search(maskedBody, std::regex(R"(\b)" + name + R"(\s*->\s*[A-Za-z_]\w*\s*(?:[+\-*/%&|^]=|=(?!=)|\+\+|--))"))
        || std::regex_search(maskedBody, std::regex(R"(\(\s*\*\s*)" + name + R"(\s*\)\s*\.\s*[A-Za-z_]\w*\s*(?:[+\-*/%&|^]=|=(?!=)|\+\+|--))"))
        || std::regex_search(maskedBody, std::regex(R"(\*\s*)" + name + R"(\s*=)"))) {
        return "pointed object is mutated";
    }

    const std::string typeName = canonicalType(parameter.type);
    static const std::regex memberCallPattern(R"((?:\b([A-Za-z_]\w*)\s*->|\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\.)\s*([A-Za-z_]\w*)\s*\()",
                                              std::regex::ECMAScript);
    for (std::sregex_iterator call(maskedBody.begin(), maskedBody.end(), memberCallPattern), end; call != end; ++call) {
        const std::string owner = (*call)[1].matched ? (*call)[1].str() : (*call)[2].str();
        if (owner != parameter.name) {
            continue;
        }
        const std::string methodName = (*call)[3].str();
        const auto methodsForType = constMethods.find(typeName);
        if (methodsForType == constMethods.end() || methodsForType->second.find(methodName) == methodsForType->second.end()) {
            return "non-const or unknown method call on pointer";
        }
    }

    static const std::regex functionCallPattern(R"(\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\))",
                                                std::regex::ECMAScript);
    for (std::sregex_iterator call(maskedBody.begin(), maskedBody.end(), functionCallPattern), end; call != end; ++call) {
        const std::string functionName = (*call)[1].str();
        if (isKeywordLikeCall(functionName)) {
            continue;
        }
        const std::vector<std::string> arguments = splitArguments((*call)[2].str());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!expressionIsParameterName(arguments[index], parameter.name)) {
                continue;
            }
            const auto known = knownFunctions.find(functionName);
            if (known == knownFunctions.end() || index >= known->second.size()) {
                return "pointer is passed to a function with unknown parameter constness";
            }
            const KnownFunctionParameter& target = known->second[index];
            if (!target.pointer || canonicalType(target.type) != typeName || !target.constPointer) {
                return "pointer is passed to a function expecting mutable T*";
            }
        }
    }

    return {};
}

std::string applyReplacements(std::string code, std::vector<Replacement> replacements)
{
    std::sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        return left.start > right.start;
    });
    for (const Replacement& replacement : replacements) {
        code.replace(replacement.start, replacement.end - replacement.start, replacement.text);
    }
    return code;
}

std::string rewriteForwardDeclarations(const std::string& code,
                                       const std::vector<std::pair<std::string, std::string>>& declarationRewrites)
{
    std::istringstream input(code);
    std::ostringstream output;
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        std::string rewritten = line;
        const std::string stripped = trim(line);
        if (!stripped.starts_with("//")
            && !stripped.starts_with("#")
            && stripped.ends_with(';')
            && stripped.find('(') != std::string::npos
            && stripped.find('{') == std::string::npos) {
            for (const auto& [before, after] : declarationRewrites) {
                const std::size_t position = rewritten.find(before);
                if (position != std::string::npos) {
                    rewritten.replace(position, before.size(), after);
                }
            }
        }
        if (!first) {
            output << '\n';
        }
        first = false;
        output << rewritten;
    }
    std::string rewrittenCode = output.str();
    if (!code.empty() && code.back() == '\n' && (rewrittenCode.empty() || rewrittenCode.back() != '\n')) {
        rewrittenCode.push_back('\n');
    }
    return rewrittenCode;
}

void addSkippedChange(std::vector<ConversionChange>& changes,
                      const std::string& parameter,
                      const std::string& reason)
{
    changes.push_back(ConversionChange{
        "Const pointer parameter modernization skipped",
        parameter,
        parameter,
        "Skipped raw pointer parameter const modernization: " + reason + ".",
        false,
        true,
    });
}
} // namespace

std::string ConstPointerParameterModernizationPass::rewrite(const std::string& code,
                                                            std::vector<ConversionChange>& changes) const
{
    const std::vector<FunctionInfo> functions = collectFunctions(code);
    const std::map<std::string, std::set<std::string>> constMethods = collectConstMethods(code);
    const std::map<std::string, std::vector<KnownFunctionParameter>> knownFunctions = collectKnownFunctionParameters(functions);
    std::vector<Replacement> replacements;
    std::vector<std::pair<std::string, std::string>> declarationRewrites;

    for (const FunctionInfo& function : functions) {
        for (const ParameterInfo& parameter : function.parameters) {
            if (!parameter.pointer || parameter.constPointer) {
                continue;
            }
            if (isSkippedPointerType(parameter.type)) {
                addSkippedChange(changes, parameter.original, "pointer type is excluded for this task");
                continue;
            }

            const std::string reason = findUnsafeReason(function.body, parameter, constMethods, knownFunctions);
            if (!reason.empty()) {
                addSkippedChange(changes, parameter.original, reason);
                continue;
            }

            const std::string replacement = "const " + canonicalType(parameter.type) + "* " + parameter.name;
            const std::size_t parameterStart = function.openParen + 1 + parameter.relativeStart;
            const std::size_t parameterEnd = function.openParen + 1 + parameter.relativeEnd;
            replacements.push_back(Replacement{parameterStart, parameterEnd, replacement});
            declarationRewrites.push_back({parameter.original, replacement});
            changes.push_back(ConversionChange{
                "Const pointer parameter modernization",
                parameter.original,
                replacement,
                "Made a raw pointer parameter const because the function only observes the pointed object.",
                true,
                false,
            });
        }
    }

    if (replacements.empty()) {
        return code;
    }

    std::string updated = applyReplacements(code, std::move(replacements));
    return rewriteForwardDeclarations(updated, declarationRewrites);
}
