#include "converter/SleepModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Replacement
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
    std::string original;
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

std::string maskCommentsLiteralsAndMacros(const std::string& code)
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
    bool lineStart = true;
    bool preprocessorLine = false;
    bool macroContinuation = false;

    for (std::size_t index = 0; index < code.size(); ++index) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';

        if (lineStart) {
            std::size_t cursor = index;
            while (cursor < code.size() && (code[cursor] == ' ' || code[cursor] == '\t')) {
                ++cursor;
            }
            preprocessorLine = macroContinuation || (cursor < code.size() && code[cursor] == '#');
            lineStart = false;
        }

        if (preprocessorLine) {
            if (current != '\n') {
                masked[index] = ' ';
            } else {
                std::size_t previous = index;
                while (previous > 0 && (code[previous - 1] == ' ' || code[previous - 1] == '\t' || code[previous - 1] == '\r')) {
                    --previous;
                }
                macroContinuation = previous > 0 && code[previous - 1] == '\\';
                preprocessorLine = false;
                lineStart = true;
            }
            continue;
        }

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
            } else if (current == '\n') {
                lineStart = true;
                macroContinuation = false;
            }
            break;
        case State::LineComment:
            if (current == '\n') {
                lineStart = true;
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

bool isSimpleArgument(const std::string& argument)
{
    static const std::regex simpleArgumentPattern(
        R"(^\s*(?:[0-9]+[uUlL]*|[A-Za-z_]\w*)\s*$)",
        std::regex::ECMAScript);
    return std::regex_match(argument, simpleArgumentPattern);
}

bool hasUserDefinedFunction(const std::string& maskedCode, const std::string& functionName)
{
    std::istringstream input(maskedCode);
    std::string line;
    const std::regex declarationPattern(
        "^\\s*(?:[A-Za-z_:][A-Za-z0-9_:<>~*&]*\\s+)+"
        + functionName
        + R"(\s*\([^;{}]*\)\s*(?:;|\{|$))",
        std::regex::ECMAScript);

    while (std::getline(input, line)) {
        if (std::regex_search(line, declarationPattern)) {
            return true;
        }
    }
    return false;
}

bool hasUnsafePrefix(const std::string& maskedCode, const std::size_t start)
{
    if (start == 0) {
        return false;
    }
    std::size_t cursor = start;
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(maskedCode[cursor - 1])) != 0) {
        --cursor;
    }
    if (cursor == 0) {
        return false;
    }
    const char previous = maskedCode[cursor - 1];
    return previous == '.' || previous == ':' || previous == '>' || previous == '_'
        || std::isalnum(static_cast<unsigned char>(previous)) != 0;
}

std::string replacementFor(const std::string& functionName, const std::string& argument)
{
    const std::string duration = functionName == "sleep" ? "seconds" : "microseconds";
    return "std::this_thread::sleep_for(std::chrono::" + duration + "(" + trim(argument) + "))";
}

std::string applyReplacements(const std::string& code, std::vector<Replacement> replacements)
{
    std::sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        return left.start > right.start;
    });

    std::string updated = code;
    for (const Replacement& replacement : replacements) {
        updated.replace(replacement.start, replacement.end - replacement.start, replacement.text);
    }
    return updated;
}

void addSkippedChange(std::vector<ConversionChange>& changes,
                      const std::string& original,
                      const std::string& reason)
{
    changes.push_back(ConversionChange{
        "POSIX sleep to std::this_thread::sleep_for",
        original,
        original,
        reason,
        false,
        true,
    });
}
} // namespace

std::string SleepModernizationPass::rewrite(const std::string& code,
                                            std::vector<ConversionChange>& changes) const
{
    const std::string maskedCode = maskCommentsLiteralsAndMacros(code);
    const bool userSleep = hasUserDefinedFunction(maskedCode, "sleep");
    const bool userUsleep = hasUserDefinedFunction(maskedCode, "usleep");
    static const std::regex callPattern(R"((::)?\b(sleep|usleep)\s*\(\s*([^()]*)\s*\))", std::regex::ECMAScript);

    std::vector<Replacement> replacements;
    auto searchBegin = maskedCode.cbegin();
    std::smatch match;
    while (std::regex_search(searchBegin, maskedCode.cend(), match, callPattern)) {
        const std::size_t start = static_cast<std::size_t>(std::distance(maskedCode.cbegin(), match[0].first));
        const std::size_t end = static_cast<std::size_t>(std::distance(maskedCode.cbegin(), match[0].second));
        const std::string functionName = match[2].str();
        const std::string argument = match[3].str();
        const bool globalQualified = match[1].matched && match[1].str() == "::";
        const std::string original = code.substr(start, end - start);

        if (hasUnsafePrefix(maskedCode, start)) {
            searchBegin = match[0].second;
            continue;
        }
        if (!globalQualified
            && ((functionName == "sleep" && userSleep) || (functionName == "usleep" && userUsleep))) {
            addSkippedChange(changes, original, "Skipped POSIX sleep modernization because a user-defined function with the same name is visible.");
            searchBegin = match[0].second;
            continue;
        }
        if (!isSimpleArgument(argument)) {
            addSkippedChange(changes, original, "Skipped POSIX sleep modernization because the argument is not a simple literal or variable.");
            searchBegin = match[0].second;
            continue;
        }

        replacements.push_back(Replacement{start, end, replacementFor(functionName, argument), original});
        changes.push_back(ConversionChange{
            "POSIX sleep to std::this_thread::sleep_for",
            original,
            replacements.back().text,
            functionName == "sleep"
                ? "Converted POSIX sleep(seconds) to std::this_thread::sleep_for(std::chrono::seconds(...))."
                : "Converted POSIX usleep(microseconds) to std::this_thread::sleep_for(std::chrono::microseconds(...)).",
            true,
            false,
        });

        searchBegin = match[0].second;
    }

    if (replacements.empty()) {
        return code;
    }
    return applyReplacements(code, std::move(replacements));
}
