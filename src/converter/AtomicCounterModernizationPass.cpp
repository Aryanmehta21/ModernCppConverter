#include "converter/AtomicCounterModernizationPass.h"

#include "converter/IncludeManager.h"

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
struct GlobalVariable
{
    std::string type;
    std::string name;
    std::string initializer;
    std::string declaration;
    std::size_t start = 0;
    std::size_t end = 0;
    bool integral = false;
    bool alreadyAtomic = false;
};

struct FunctionBody
{
    std::string name;
    std::string body;
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

std::string normalizeSpaces(std::string value)
{
    value = trim(std::move(value));
    std::string normalized;
    bool previousSpace = false;
    for (const char character : value) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!previousSpace) {
                normalized.push_back(' ');
            }
            previousSpace = true;
        } else {
            normalized.push_back(character);
            previousSpace = false;
        }
    }
    return normalized;
}

std::string regexEscape(const std::string& text)
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

std::string maskCommentsAndLiterals(const std::string& code)
{
    std::string masked = code;
    enum class State {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharLiteral,
        RawStringLiteral,
    };

    State state = State::Code;
    std::string rawStringTerminator;
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
            } else if (current == 'R' && next == '"') {
                const std::size_t delimiterStart = index + 2;
                const std::size_t openingParen = code.find('(', delimiterStart);
                if (openingParen != std::string::npos) {
                    rawStringTerminator = ")" + code.substr(delimiterStart, openingParen - delimiterStart) + "\"";
                    std::fill(masked.begin() + static_cast<std::ptrdiff_t>(index),
                              masked.begin() + static_cast<std::ptrdiff_t>(openingParen + 1),
                              ' ');
                    index = openingParen;
                    state = State::RawStringLiteral;
                }
            } else if (current == '"') {
                masked[index] = ' ';
                state = State::StringLiteral;
            } else if (current == '\'') {
                masked[index] = ' ';
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
            if (current == '\\' && next != '\0') {
                masked[index + 1] = ' ';
                ++index;
            } else if (current == '"') {
                state = State::Code;
            }
            break;
        case State::CharLiteral:
            masked[index] = ' ';
            if (current == '\\' && next != '\0') {
                masked[index + 1] = ' ';
                ++index;
            } else if (current == '\'') {
                state = State::Code;
            }
            break;
        case State::RawStringLiteral:
            masked[index] = ' ';
            if (!rawStringTerminator.empty()
                && index + rawStringTerminator.size() <= code.size()
                && code.compare(index, rawStringTerminator.size(), rawStringTerminator) == 0) {
                std::fill(masked.begin() + static_cast<std::ptrdiff_t>(index),
                          masked.begin() + static_cast<std::ptrdiff_t>(index + rawStringTerminator.size()),
                          ' ');
                index += rawStringTerminator.size() - 1;
                rawStringTerminator.clear();
                state = State::Code;
            }
            break;
        }
    }
    return masked;
}

std::string removeAtomicSpacing(std::string value)
{
    value = normalizeSpaces(std::move(value));
    value = std::regex_replace(value, std::regex(R"(\s*<\s*)"), "<");
    value = std::regex_replace(value, std::regex(R"(\s*>\s*)"), ">");
    return value;
}

bool isSupportedIntegralType(const std::string& type)
{
    const std::string normalized = normalizeSpaces(type);
    static const std::set<std::string> supportedTypes{
        "char",
        "signed char",
        "unsigned char",
        "short",
        "short int",
        "signed short",
        "signed short int",
        "unsigned short",
        "unsigned short int",
        "int",
        "signed",
        "signed int",
        "unsigned",
        "unsigned int",
        "long",
        "long int",
        "signed long",
        "signed long int",
        "unsigned long",
        "unsigned long int",
        "long long",
        "long long int",
        "signed long long",
        "signed long long int",
        "unsigned long long",
        "unsigned long long int",
        "std::size_t",
        "size_t",
    };
    return supportedTypes.find(normalized) != supportedTypes.end();
}

bool isIntegerLiteral(const std::string& expression)
{
    const std::string trimmed = trim(expression);
    return std::regex_match(trimmed, std::regex(R"([+-]?(?:0[xX][0-9A-Fa-f]+|\d+)(?:[uUlL]*)?)"));
}

std::vector<GlobalVariable> findGlobalVariables(const std::string& code)
{
    const std::string masked = maskCommentsAndLiterals(code);
    std::vector<GlobalVariable> globals;
    std::size_t lineStart = 0;
    int braceDepth = 0;

    while (lineStart <= masked.size()) {
        const std::size_t lineEnd = masked.find('\n', lineStart);
        const std::size_t effectiveLineEnd = lineEnd == std::string::npos ? masked.size() : lineEnd;
        const std::string maskedLine = masked.substr(lineStart, effectiveLineEnd - lineStart);
        const std::string originalLine = code.substr(lineStart, effectiveLineEnd - lineStart);

        if (braceDepth == 0) {
            std::smatch match;
            static const std::regex atomicPattern(
                R"(^([ \t]*(?:static\s+)?)(std::atomic\s*<\s*([^>]+?)\s*>)\s+([A-Za-z_]\w*)\s*(?:\{([^}]*)\}|=\s*([^;]+))?\s*;[ \t]*$)",
                std::regex::ECMAScript);
            static const std::regex variablePattern(
                R"(^([ \t]*(?:static\s+)?)([A-Za-z_:]\w*(?:\s+[A-Za-z_:]\w*)*)\s+([A-Za-z_]\w*)\s*(?:=\s*([^;]+))?\s*;[ \t]*$)",
                std::regex::ECMAScript);

            if (std::regex_match(maskedLine, match, atomicPattern)) {
                globals.push_back(GlobalVariable{
                    removeAtomicSpacing(match[2].str()),
                    match[4].str(),
                    trim(match[5].matched ? match[5].str() : match[6].str()),
                    originalLine,
                    lineStart,
                    effectiveLineEnd,
                    false,
                    true,
                });
            } else if (std::regex_match(maskedLine, match, variablePattern)) {
                const std::string type = normalizeSpaces(match[2].str());
                if (originalLine.find('*') == std::string::npos && originalLine.find('&') == std::string::npos) {
                    globals.push_back(GlobalVariable{
                        type,
                        match[3].str(),
                        trim(match[4].matched ? match[4].str() : std::string{}),
                        originalLine,
                        lineStart,
                        effectiveLineEnd,
                        isSupportedIntegralType(type),
                        false,
                    });
                }
            }
        }

        for (std::size_t index = lineStart; index < effectiveLineEnd; ++index) {
            if (masked[index] == '{') {
                ++braceDepth;
            } else if (masked[index] == '}') {
                braceDepth = std::max(0, braceDepth - 1);
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return globals;
}

std::size_t matchingBrace(const std::string& masked, const std::size_t openBrace)
{
    int depth = 0;
    for (std::size_t index = openBrace; index < masked.size(); ++index) {
        if (masked[index] == '{') {
            ++depth;
        } else if (masked[index] == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

std::map<std::string, FunctionBody> findFunctions(const std::string& code)
{
    const std::string masked = maskCommentsAndLiterals(code);
    std::map<std::string, FunctionBody> functions;
    static const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:[\w:<>,~*&]+\s+)+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{)",
        std::regex::ECMAScript);

    std::string::const_iterator searchBegin = masked.begin();
    std::smatch match;
    while (std::regex_search(searchBegin, masked.cend(), match, functionPattern)) {
        const std::size_t matchStart = static_cast<std::size_t>(std::distance(masked.cbegin(), searchBegin) + match.position());
        const std::size_t openBrace = masked.find('{', matchStart);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t closeBrace = matchingBrace(masked, openBrace);
        if (closeBrace == std::string::npos) {
            searchBegin = masked.cbegin() + static_cast<std::ptrdiff_t>(openBrace + 1);
            continue;
        }
        const std::string name = match[1].str();
        if (name != "if" && name != "for" && name != "while" && name != "switch") {
            functions[name] = FunctionBody{
                name,
                code.substr(openBrace + 1, closeBrace - openBrace - 1),
            };
        }
        searchBegin = masked.cbegin() + static_cast<std::ptrdiff_t>(closeBrace + 1);
    }
    return functions;
}

std::set<std::string> findThreadEntryFunctionNames(const std::string& code)
{
    const std::string masked = maskCommentsAndLiterals(code);
    std::set<std::string> names;

    const std::vector<std::regex> patterns{
        std::regex(R"(\bpthread_create\s*\(\s*[^,]+,\s*[^,]+,\s*&?\s*([A-Za-z_]\w*))", std::regex::ECMAScript),
        std::regex(R"(\bstd::j?thread\s+[A-Za-z_]\w*\s*[\(\{]\s*&?\s*([A-Za-z_]\w*))", std::regex::ECMAScript),
    };

    for (const std::regex& pattern : patterns) {
        std::string::const_iterator searchBegin = masked.begin();
        std::smatch match;
        while (std::regex_search(searchBegin, masked.cend(), match, pattern)) {
            const std::string name = match[1].str();
            if (name != "nullptr" && name != "NULL" && name != "0") {
                names.insert(name);
            }
            searchBegin = match.suffix().first;
        }
    }

    return names;
}

std::vector<std::string> findThreadLambdaBodies(const std::string& code)
{
    const std::string masked = maskCommentsAndLiterals(code);
    std::vector<std::string> bodies;
    static const std::regex threadLambdaPattern(
        R"(\bstd::j?thread\s+[A-Za-z_]\w*\s*[\(\{]\s*\[[^\]]*\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?\{)",
        std::regex::ECMAScript);
    std::string::const_iterator searchBegin = masked.begin();
    std::smatch match;
    while (std::regex_search(searchBegin, masked.cend(), match, threadLambdaPattern)) {
        const std::size_t matchStart = static_cast<std::size_t>(std::distance(masked.cbegin(), searchBegin) + match.position());
        const std::size_t openBrace = masked.find('{', matchStart);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t closeBrace = matchingBrace(masked, openBrace);
        if (closeBrace == std::string::npos) {
            searchBegin = masked.cbegin() + static_cast<std::ptrdiff_t>(openBrace + 1);
            continue;
        }
        bodies.push_back(code.substr(openBrace + 1, closeBrace - openBrace - 1));
        searchBegin = masked.cbegin() + static_cast<std::ptrdiff_t>(closeBrace + 1);
    }
    return bodies;
}

bool containsAllowedMutation(const std::string& code, const std::string& name)
{
    const std::string escapedName = regexEscape(name);
    const std::string searchable = maskCommentsAndLiterals(code);
    const std::vector<std::regex> allowedPatterns{
        std::regex(R"(\+\+\s*\b)" + escapedName + R"(\b)", std::regex::ECMAScript),
        std::regex(R"(\b)" + escapedName + R"(\b\s*\+\+)", std::regex::ECMAScript),
        std::regex(R"(--\s*\b)" + escapedName + R"(\b)", std::regex::ECMAScript),
        std::regex(R"(\b)" + escapedName + R"(\b\s*--)", std::regex::ECMAScript),
        std::regex(R"(\b)" + escapedName + R"(\b\s*[+\-]=\s*[+-]?(?:0[xX][0-9A-Fa-f]+|\d+)(?:[uUlL]*)?\s*;)",
                   std::regex::ECMAScript),
    };
    return std::any_of(allowedPatterns.begin(), allowedPatterns.end(), [&searchable](const std::regex& pattern) {
        return std::regex_search(searchable, pattern);
    });
}

struct Replacement
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

bool rangeOverlaps(const Replacement& left, const Replacement& right)
{
    return left.start < right.end && right.start < left.end;
}

void addReplacement(std::vector<Replacement>& replacements,
                    const std::size_t start,
                    const std::size_t end,
                    std::string text)
{
    if (start >= end) {
        return;
    }
    const Replacement candidate{start, end, std::move(text)};
    const auto duplicate = std::find_if(replacements.begin(), replacements.end(), [&candidate](const Replacement& existing) {
        return existing.start == candidate.start && existing.end == candidate.end && existing.text == candidate.text;
    });
    if (duplicate != replacements.end()) {
        return;
    }
    const auto overlap = std::find_if(replacements.begin(), replacements.end(), [&candidate](const Replacement& existing) {
        return rangeOverlaps(existing, candidate);
    });
    if (overlap == replacements.end()) {
        replacements.push_back(candidate);
    }
}

std::size_t skipWhitespace(const std::string& text, std::size_t position)
{
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    return position;
}

std::size_t skipWhitespaceBackward(const std::string& text, std::size_t position)
{
    while (position > 0 && std::isspace(static_cast<unsigned char>(text[position - 1])) != 0) {
        --position;
    }
    return position;
}

std::size_t matchingParen(const std::string& masked, const std::size_t openParen)
{
    int depth = 0;
    for (std::size_t index = openParen; index < masked.size(); ++index) {
        if (masked[index] == '(') {
            ++depth;
        } else if (masked[index] == ')') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

std::vector<std::pair<std::size_t, std::size_t>> splitTopLevelArguments(const std::string& masked,
                                                                        const std::size_t begin,
                                                                        const std::size_t end)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::size_t argumentBegin = begin;
    int parenDepth = 0;
    int braceDepth = 0;
    int bracketDepth = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const char character = masked[index];
        if (character == '(') {
            ++parenDepth;
        } else if (character == ')') {
            parenDepth = std::max(0, parenDepth - 1);
        } else if (character == '{') {
            ++braceDepth;
        } else if (character == '}') {
            braceDepth = std::max(0, braceDepth - 1);
        } else if (character == '[') {
            ++bracketDepth;
        } else if (character == ']') {
            bracketDepth = std::max(0, bracketDepth - 1);
        } else if (character == ',' && parenDepth == 0 && braceDepth == 0 && bracketDepth == 0) {
            ranges.emplace_back(argumentBegin, index);
            argumentBegin = index + 1;
        }
    }
    if (argumentBegin <= end) {
        ranges.emplace_back(argumentBegin, end);
    }
    return ranges;
}

bool trimmedRangeEquals(const std::string& code,
                        std::size_t begin,
                        std::size_t end,
                        const std::string& expected,
                        std::size_t& trimmedBegin,
                        std::size_t& trimmedEnd)
{
    begin = skipWhitespace(code, begin);
    end = skipWhitespaceBackward(code, end);
    trimmedBegin = begin;
    trimmedEnd = end;
    return begin <= end && code.substr(begin, end - begin) == expected;
}

bool isAtomicDeclarationOccurrence(const std::string& code, const std::size_t nameStart)
{
    const std::size_t lineStart = code.rfind('\n', nameStart);
    const std::size_t prefixStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    const std::string prefix = code.substr(prefixStart, nameStart - prefixStart);
    return prefix.find("std::atomic") != std::string::npos;
}

std::string applyReplacements(std::string code, std::vector<Replacement> replacements)
{
    std::sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        return left.start > right.start;
    });
    for (const Replacement& replacement : replacements) {
        if (replacement.end <= code.size() && replacement.start <= replacement.end) {
            code.replace(replacement.start, replacement.end - replacement.start, replacement.text);
        }
    }
    return code;
}

void collectFormatReadRepairs(const std::string& code,
                              const std::string& name,
                              std::vector<Replacement>& replacements)
{
    const std::string masked = maskCommentsAndLiterals(code);
    std::size_t searchPosition = 0;
    while ((searchPosition = masked.find("std::format", searchPosition)) != std::string::npos) {
        const std::size_t openParen = masked.find('(', searchPosition + std::string("std::format").size());
        if (openParen == std::string::npos) {
            break;
        }
        const std::size_t closeParen = matchingParen(masked, openParen);
        if (closeParen == std::string::npos) {
            searchPosition = openParen + 1;
            continue;
        }
        const auto arguments = splitTopLevelArguments(masked, openParen + 1, closeParen);
        for (std::size_t argumentIndex = 1; argumentIndex < arguments.size(); ++argumentIndex) {
            std::size_t trimmedBegin = 0;
            std::size_t trimmedEnd = 0;
            if (trimmedRangeEquals(code, arguments[argumentIndex].first, arguments[argumentIndex].second, name, trimmedBegin, trimmedEnd)) {
                addReplacement(replacements, trimmedBegin, trimmedEnd, name + ".load()");
            }
        }
        searchPosition = closeParen + 1;
    }
}

void collectStreamReadRepairs(const std::string& code,
                              const std::string& name,
                              std::vector<Replacement>& replacements)
{
    const std::string masked = maskCommentsAndLiterals(code);
    const std::regex pattern(R"(<<\s*)" + regexEscape(name) + R"(\b)", std::regex::ECMAScript);
    std::string::const_iterator searchBegin = masked.begin();
    std::smatch match;
    while (std::regex_search(searchBegin, masked.cend(), match, pattern)) {
        const std::size_t matchStart = static_cast<std::size_t>(std::distance(masked.cbegin(), searchBegin) + match.position());
        const std::size_t nameStart = matchStart + match.str().rfind(name);
        const std::size_t nameEnd = nameStart + name.size();
        const std::size_t next = skipWhitespace(masked, nameEnd);
        if (next < masked.size()
            && (masked[next] == '.' || masked[next] == '+' || masked[next] == '-' || masked[next] == '=')) {
            searchBegin = match.suffix().first;
            continue;
        }
        addReplacement(replacements, nameStart, nameEnd, name + ".load()");
        searchBegin = match.suffix().first;
    }
}

void collectCopyReadRepairs(const std::string& code,
                            const std::string& name,
                            std::vector<Replacement>& replacements)
{
    const std::string masked = maskCommentsAndLiterals(code);
    const std::regex pattern(
        R"((?:^|[;\n])\s*(?:const\s+)?(?:auto|[A-Za-z_:]\w*(?:\s*<[^;=]+>)?(?:\s+[*&]?\s*[A-Za-z_:]\w*)*)\s+([A-Za-z_]\w*)\s*=\s*)"
            + regexEscape(name) + R"(\s*;)",
        std::regex::ECMAScript);
    std::string::const_iterator searchBegin = masked.begin();
    std::smatch match;
    while (std::regex_search(searchBegin, masked.cend(), match, pattern)) {
        const std::size_t matchStart = static_cast<std::size_t>(std::distance(masked.cbegin(), searchBegin) + match.position());
        const std::string matchedText = match.str();
        const std::size_t localNameOffset = matchedText.rfind(name);
        if (localNameOffset == std::string::npos) {
            searchBegin = match.suffix().first;
            continue;
        }
        const std::size_t nameStart = matchStart + localNameOffset;
        addReplacement(replacements, nameStart, nameStart + name.size(), name + ".load()");
        searchBegin = match.suffix().first;
    }
}

void collectComparisonReadRepairs(const std::string& code,
                                  const std::string& name,
                                  std::vector<Replacement>& replacements)
{
    const std::string masked = maskCommentsAndLiterals(code);
    const std::string escapedName = regexEscape(name);
    const std::vector<std::regex> patterns{
        std::regex(R"(\b)" + escapedName + R"(\b\s*(?:==|!=|<=|>=|<|>))", std::regex::ECMAScript),
        std::regex(R"((?:==|!=|<=|>=|<|>)\s*\b)" + escapedName + R"(\b)", std::regex::ECMAScript),
    };

    for (const std::regex& pattern : patterns) {
        std::string::const_iterator searchBegin = masked.begin();
        std::smatch match;
        while (std::regex_search(searchBegin, masked.cend(), match, pattern)) {
            const std::size_t matchStart = static_cast<std::size_t>(std::distance(masked.cbegin(), searchBegin) + match.position());
            const std::size_t localNameOffset = match.str().find(name);
            if (localNameOffset == std::string::npos) {
                searchBegin = match.suffix().first;
                continue;
            }
            const std::size_t nameStart = matchStart + localNameOffset;
            if (isAtomicDeclarationOccurrence(code, nameStart)) {
                searchBegin = match.suffix().first;
                continue;
            }
            addReplacement(replacements, nameStart, nameStart + name.size(), name + ".load()");
            searchBegin = match.suffix().first;
        }
    }
}

std::string repairAtomicReadContexts(std::string code,
                                     const std::set<std::string>& atomicNames,
                                     std::vector<ConversionChange>& changes)
{
    for (const std::string& name : atomicNames) {
        std::vector<Replacement> replacements;
        collectFormatReadRepairs(code, name, replacements);
        collectStreamReadRepairs(code, name, replacements);
        collectCopyReadRepairs(code, name, replacements);
        collectComparisonReadRepairs(code, name, replacements);
        if (replacements.empty()) {
            continue;
        }
        const std::string before = code;
        code = applyReplacements(std::move(code), replacements);
        if (code != before) {
            changes.push_back(ConversionChange{
                "Atomic value read to load",
                name,
                name + ".load()",
                "Rewrote value-read contexts for atomic counter '" + name + "' to use .load() while preserving mutation operations.",
                true,
                false,
            });
        }
    }
    return code;
}

bool bodyHasMutexProtection(const std::string& body)
{
    const std::string searchable = maskCommentsAndLiterals(body);
    return searchable.find("std::lock_guard") != std::string::npos
        || searchable.find("std::scoped_lock") != std::string::npos
        || searchable.find("std::unique_lock") != std::string::npos
        || searchable.find("pthread_mutex_lock") != std::string::npos
        || searchable.find(".lock(") != std::string::npos
        || searchable.find("->lock(") != std::string::npos;
}

std::string codeWithoutRange(std::string code, const std::size_t start, const std::size_t end)
{
    if (start < end && end <= code.size()) {
        const std::size_t lineStart = code.rfind('\n', start);
        const std::size_t eraseStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        const std::size_t lineEnd = code.find('\n', end);
        const std::size_t eraseEnd = lineEnd == std::string::npos ? end : lineEnd;
        std::fill(code.begin() + static_cast<std::ptrdiff_t>(eraseStart),
                  code.begin() + static_cast<std::ptrdiff_t>(eraseEnd),
                  ' ');
    }
    return code;
}

bool hasDisallowedWrite(const std::string& codeWithoutDeclaration,
                        const std::string& name,
                        std::string& reason)
{
    const std::string escapedName = regexEscape(name);
    const std::string searchable = maskCommentsAndLiterals(codeWithoutDeclaration);
    const std::regex assignmentPattern(R"(\b)" + escapedName + R"(\b\s*(\+=|-=|\*=|/=|%=|=(?!=))\s*([^;]+);)",
                                       std::regex::ECMAScript);
    std::string::const_iterator searchBegin = searchable.begin();
    std::smatch match;
    while (std::regex_search(searchBegin, searchable.cend(), match, assignmentPattern)) {
        const std::string op = match[1].str();
        const std::string rhs = match[2].str();
        if ((op == "+=" || op == "-=") && isIntegerLiteral(rhs)) {
            searchBegin = match.suffix().first;
            continue;
        }
        reason = "unsupported write expression for shared counter: " + name + " " + op + " ...";
        return true;
    }

    if (std::regex_search(searchable, std::regex(R"(&\s*)" + escapedName + R"(\b)", std::regex::ECMAScript))) {
        reason = "address of counter is taken, so changing its type may break an external API";
        return true;
    }

    reason.clear();
    return false;
}

void addSkippedChange(std::vector<ConversionChange>& changes,
                      const std::string& variable,
                      const std::string& declaration,
                      const std::string& reason)
{
    changes.push_back(ConversionChange{
        "Atomic counter modernization skipped",
        trim(declaration),
        {},
        "Skipped shared counter candidate '" + variable + "': " + reason,
        false,
        true,
    });
}

void addAppliedChange(std::vector<ConversionChange>& changes,
                      const std::string& before,
                      const std::string& after,
                      const std::string& variable,
                      const std::size_t threadEntryCount)
{
    changes.push_back(ConversionChange{
        "Shared integral counter to std::atomic",
        trim(before),
        trim(after),
        "Converted shared integral counter '" + variable + "' to std::atomic after detecting "
            + std::to_string(threadEntryCount) + " thread entry function(s).",
        true,
        false,
    });
}
} // namespace

std::string AtomicCounterModernizationPass::rewrite(const std::string& code,
                                                    const ModernizationOptions&,
                                                    std::vector<ConversionChange>& changes) const
{
    const std::vector<GlobalVariable> globals = findGlobalVariables(code);
    if (globals.empty()) {
        return code;
    }

    const std::map<std::string, FunctionBody> functions = findFunctions(code);
    const std::set<std::string> threadEntryNames = findThreadEntryFunctionNames(code);
    std::vector<std::string> threadBodies = findThreadLambdaBodies(code);
    for (const std::string& name : threadEntryNames) {
        const auto found = functions.find(name);
        if (found != functions.end()) {
            threadBodies.push_back(found->second.body);
        }
    }

    if (threadBodies.empty()) {
        return code;
    }

    std::string updated = code;
    bool changed = false;
    std::vector<std::pair<std::size_t, std::string>> replacements;
    std::set<std::string> atomicNames;

    for (const GlobalVariable& global : globals) {
        const auto writesVariable = [&global](const std::string& body) {
            return containsAllowedMutation(body, global.name);
        };
        const std::size_t matchingThreadEntries = static_cast<std::size_t>(
            std::count_if(threadBodies.begin(), threadBodies.end(), writesVariable));
        if (matchingThreadEntries == 0) {
            continue;
        }

        if (global.alreadyAtomic) {
            atomicNames.insert(global.name);
            continue;
        }

        if (!global.integral) {
            addSkippedChange(changes,
                             global.name,
                             global.declaration,
                             "variable type '" + global.type + "' is not a supported integral counter type");
            continue;
        }

        const auto protectedThreadBody = [&global](const std::string& body) {
            return containsAllowedMutation(body, global.name) && bodyHasMutexProtection(body);
        };
        if (std::any_of(threadBodies.begin(), threadBodies.end(), protectedThreadBody)) {
            addSkippedChange(changes,
                             global.name,
                             global.declaration,
                             "counter appears to be protected by a mutex or lock guard");
            continue;
        }

        std::string disallowedReason;
        if (hasDisallowedWrite(codeWithoutRange(code, global.start, global.end), global.name, disallowedReason)) {
            addSkippedChange(changes, global.name, global.declaration, disallowedReason);
            continue;
        }

        const std::string initializer = global.initializer.empty() ? "0" : global.initializer;
        const bool isStatic = trim(global.declaration).find("static ") == 0;
        const std::string replacement = std::string(isStatic ? "static " : "")
            + "std::atomic<" + global.type + "> " + global.name + "{" + initializer + "};";
        replacements.emplace_back(global.start, replacement);
        atomicNames.insert(global.name);
        addAppliedChange(changes, global.declaration, replacement, global.name, matchingThreadEntries);
        changed = true;
    }

    if (!changed && atomicNames.empty()) {
        return code;
    }

    std::sort(replacements.begin(), replacements.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    for (const auto& [start, replacement] : replacements) {
        const auto found = std::find_if(globals.begin(), globals.end(), [start](const GlobalVariable& global) {
            return global.start == start;
        });
        if (found == globals.end()) {
            continue;
        }
        updated.replace(found->start, found->end - found->start, replacement);
    }

    updated = repairAtomicReadContexts(std::move(updated), atomicNames, changes);

    if (changed) {
        const IncludeManager includeManager;
        return includeManager.ensureInclude(updated, "#include <atomic>");
    }
    return updated;
}
