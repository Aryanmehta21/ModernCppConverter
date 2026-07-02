#include "converter/IncludeCleanupPass.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace
{
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

struct ParsedInclude
{
    std::string indentation;
    std::string opener;
    std::string target;
    std::string closer;
    std::string trailingComment;
    std::string key;
    std::string normalizedLine;
};

bool parseIncludeLine(const std::string& line, ParsedInclude& include)
{
    static const std::regex includePattern(
        R"(^([ \t]*)#\s*include\s*([<"])\s*([^>"]+?)\s*([>"])([ \t]*(?://.*)?)\s*$)",
        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(line, match, includePattern)) {
        return false;
    }

    include.indentation = match[1].str();
    include.opener = match[2].str();
    include.target = trim(match[3].str());
    include.closer = include.opener == "<" ? ">" : "\"";
    include.trailingComment = match[5].matched ? match[5].str() : std::string{};
    if (!include.trailingComment.empty()) {
        include.trailingComment = " " + trim(include.trailingComment);
    }
    include.key = include.opener + include.target + include.closer;
    include.normalizedLine = include.indentation + "#include " + include.opener + include.target + include.closer
        + include.trailingComment;
    return !include.target.empty();
}

bool isStandardInclude(const ParsedInclude& include)
{
    return include.opener == "<" && include.closer == ">";
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

    for (std::size_t i = 0; i < code.size(); ++i) {
        const char current = code[i];
        const char next = i + 1 < code.size() ? code[i + 1] : '\0';

        switch (state) {
        case State::Code:
            if (current == '/' && next == '/') {
                masked[i] = ' ';
                masked[i + 1] = ' ';
                ++i;
                state = State::LineComment;
            } else if (current == '/' && next == '*') {
                masked[i] = ' ';
                masked[i + 1] = ' ';
                ++i;
                state = State::BlockComment;
            } else if (current == 'R' && next == '"') {
                const std::size_t delimiterStart = i + 2;
                const std::size_t openingParen = code.find('(', delimiterStart);
                if (openingParen != std::string::npos) {
                    rawStringTerminator = ")" + code.substr(delimiterStart, openingParen - delimiterStart) + "\"";
                    std::fill(masked.begin() + static_cast<std::ptrdiff_t>(i),
                              masked.begin() + static_cast<std::ptrdiff_t>(openingParen + 1),
                              ' ');
                    i = openingParen;
                    state = State::RawStringLiteral;
                }
            } else if (current == '"') {
                masked[i] = ' ';
                state = State::StringLiteral;
            } else if (current == '\'') {
                masked[i] = ' ';
                state = State::CharLiteral;
            }
            break;
        case State::LineComment:
            if (current == '\n') {
                state = State::Code;
            } else {
                masked[i] = ' ';
            }
            break;
        case State::BlockComment:
            masked[i] = ' ';
            if (current == '*' && next == '/') {
                masked[i + 1] = ' ';
                ++i;
                state = State::Code;
            }
            break;
        case State::StringLiteral:
            masked[i] = ' ';
            if (current == '\\' && next != '\0') {
                masked[i + 1] = ' ';
                ++i;
            } else if (current == '"') {
                state = State::Code;
            }
            break;
        case State::CharLiteral:
            masked[i] = ' ';
            if (current == '\\' && next != '\0') {
                masked[i + 1] = ' ';
                ++i;
            } else if (current == '\'') {
                state = State::Code;
            }
            break;
        case State::RawStringLiteral:
            masked[i] = ' ';
            if (!rawStringTerminator.empty()
                && i + rawStringTerminator.size() <= code.size()
                && code.compare(i, rawStringTerminator.size(), rawStringTerminator) == 0) {
                std::fill(masked.begin() + static_cast<std::ptrdiff_t>(i),
                          masked.begin() + static_cast<std::ptrdiff_t>(i + rawStringTerminator.size()),
                          ' ');
                i += rawStringTerminator.size() - 1;
                rawStringTerminator.clear();
                state = State::Code;
            }
            break;
        }
    }

    return masked;
}

bool containsPattern(const std::string& text, const std::string& pattern)
{
    return std::regex_search(text, std::regex(pattern, std::regex::ECMAScript));
}

std::vector<std::string> detectRequiredStandardIncludes(const std::string& code)
{
    const std::string searchable = maskCommentsAndLiterals(code);
    const std::vector<std::pair<std::string, std::string>> rules{
        {"<atomic>", R"(\bstd::atomic\b)"},
        {"<vector>", R"(\bstd::vector\b)"},
        {"<string>", R"(\bstd::string\b)"},
        {"<string_view>", R"(\bstd::string_view\b)"},
        {"<memory>", R"(\bstd::(?:unique_ptr|shared_ptr|make_unique|make_shared)\b)"},
        {"<format>", R"(\bstd::format\b)"},
        {"<type_traits>", R"(\bstd::underlying_type_t\b)"},
        {"<cstddef>", R"(\bstd::size_t\b)"},
        {"<fstream>", R"(\bstd::(?:ofstream|ifstream)\b)"},
        {"<map>", R"(\bstd::map\b)"},
        {"<unordered_map>", R"(\bstd::unordered_map\b)"},
        {"<functional>", R"(\bstd::function\b)"},
        {"<array>", R"(\bstd::array\b)"},
        {"<optional>", R"(\bstd::optional\b)"},
        {"<mutex>", R"(\bstd::(?:lock_guard|mutex)\b)"},
    };

    std::vector<std::string> includes;
    for (const auto& [include, pattern] : rules) {
        if (containsPattern(searchable, pattern)
            && std::find(includes.begin(), includes.end(), include) == includes.end()) {
            includes.push_back(include);
        }
    }
    return includes;
}

bool containsAnyPattern(const std::string& text, const std::vector<std::string>& patterns)
{
    return std::any_of(patterns.begin(), patterns.end(), [&text](const std::string& pattern) {
        return containsPattern(text, pattern);
    });
}

std::vector<std::pair<std::string, std::vector<std::string>>> obsoleteIncludeRules()
{
    return {
        {"<cstring>",
         {
             R"(\b(?:std::)?strcpy\s*\()",
             R"(\b(?:std::)?strncpy\s*\()",
             R"(\b(?:std::)?strcat\s*\()",
             R"(\b(?:std::)?strcmp\s*\()",
             R"(\b(?:std::)?strlen\s*\()",
             R"(\b(?:std::)?memcpy\s*\()",
             R"(\b(?:std::)?memset\s*\()",
             R"(\b(?:std::)?memmove\s*\()",
         }},
        {"<cstdio>",
         {
             R"(\bFILE\b)",
             R"(\b(?:std::)?fopen\s*\()",
             R"(\b(?:std::)?fprintf\s*\()",
             R"(\b(?:std::)?printf\s*\()",
             R"(\b(?:std::)?fclose\s*\()",
             R"(\b(?:std::)?fscanf\s*\()",
             R"(\b(?:std::)?scanf\s*\()",
             R"(\b(?:std::)?sprintf\s*\()",
             R"(\b(?:std::)?snprintf\s*\()",
             R"(\b(?:std::)?fputs\s*\()",
             R"(\b(?:std::)?puts\s*\()",
             R"(\b(?:std::)?fread\s*\()",
             R"(\b(?:std::)?fwrite\s*\()",
             R"(\b(?:stdin|stdout|stderr)\b)",
         }},
        {"<cstdlib>",
         {
             R"(\b(?:std::)?malloc\s*\()",
             R"(\b(?:std::)?free\s*\()",
             R"(\b(?:std::)?calloc\s*\()",
             R"(\b(?:std::)?realloc\s*\()",
             R"(\b(?:std::)?aligned_alloc\s*\()",
             R"(\b(?:std::)?exit\s*\()",
             R"(\b(?:EXIT_SUCCESS|EXIT_FAILURE)\b)",
         }},
        {"<pthread.h>",
         {
             R"(\bpthread_[A-Za-z_]\w*\b)",
         }},
        {"<unistd.h>",
         {
             R"(\b(?:sleep|usleep|read|write|close)\s*\()",
         }},
    };
}

const std::vector<std::string>* obsoleteUsagePatternsForInclude(const std::string& includeKey)
{
    static const std::vector<std::pair<std::string, std::vector<std::string>>> rules = obsoleteIncludeRules();
    const auto found = std::find_if(rules.begin(), rules.end(), [&includeKey](const auto& rule) {
        return rule.first == includeKey;
    });
    return found == rules.end() ? nullptr : &found->second;
}

std::size_t requiredIncludeInsertionIndex(const std::vector<std::string>& lines)
{
    std::size_t firstInclude = std::string::npos;
    std::size_t lastStandardInclude = std::string::npos;
    std::size_t pragmaOnceIndex = std::string::npos;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string trimmed = trim(lines[index]);
        if (trimmed == "#pragma once") {
            pragmaOnceIndex = index;
        }

        ParsedInclude include;
        if (!parseIncludeLine(lines[index], include)) {
            continue;
        }
        if (firstInclude == std::string::npos) {
            firstInclude = index;
        }
        if (isStandardInclude(include)) {
            lastStandardInclude = index;
        }
    }

    if (lastStandardInclude != std::string::npos) {
        return lastStandardInclude + 1;
    }
    if (firstInclude != std::string::npos) {
        return firstInclude;
    }
    if (pragmaOnceIndex != std::string::npos) {
        return pragmaOnceIndex + 1;
    }
    return 0;
}

std::string joinLines(const std::vector<std::string>& lines, const bool finalNewline)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            output << '\n';
        }
        output << lines[index];
    }
    if (finalNewline && (lines.empty() || output.str().empty() || output.str().back() != '\n')) {
        output << '\n';
    }
    return output.str();
}

std::string joinIncludeNames(const std::vector<std::string>& includes)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < includes.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << includes[index];
    }
    return output.str();
}

void addIncludeCleanupChange(std::vector<ConversionChange>& changes,
                             const IncludeCleanupResult& result)
{
    changes.push_back(ConversionChange{
        "Include cleanup",
        "include directives",
        "normalized include directives",
        "Normalized include syntax, removed exact duplicate includes, and added required standard includes. syntax_normalized="
            + std::to_string(result.syntaxNormalizedCount)
            + " duplicates_removed=" + std::to_string(result.duplicateIncludesRemovedCount)
            + " preserved=" + std::to_string(result.includesPreservedCount)
            + " required_added=" + std::to_string(result.requiredIncludesAddedCount)
            + " required_already_present=" + std::to_string(result.requiredIncludesAlreadyPresentCount)
            + " added=[" + joinIncludeNames(result.requiredIncludeNamesAdded) + "]"
            + " obsolete_removed=" + std::to_string(result.obsoleteIncludesRemovedCount)
            + " obsolete_kept=" + std::to_string(result.obsoleteIncludesKeptCount)
            + " obsolete_skipped=" + std::to_string(result.obsoleteIncludesSkippedCount)
            + " removed_obsolete=[" + joinIncludeNames(result.obsoleteIncludeNamesRemoved) + "]"
            + " kept_obsolete=[" + joinIncludeNames(result.obsoleteIncludeNamesKept) + "]"
            + " skipped_obsolete=[" + joinIncludeNames(result.obsoleteIncludeNamesSkipped) + "]",
        true,
        false,
    });
}
} // namespace

IncludeCleanupResult IncludeCleanupPass::run(const std::string& code,
                                             std::vector<ConversionChange>& changes) const
{
    IncludeCleanupResult result;
    result.code = code;

    std::stringstream input(code);
    std::vector<std::string> outputLines;
    std::string line;
    std::map<std::string, std::string> firstTrailingCommentByKey;
    std::set<std::string> existingIncludeKeys;
    const std::string searchableCode = maskCommentsAndLiterals(code);

    while (std::getline(input, line)) {
        ParsedInclude include;
        bool removeLine = false;
        std::string outputLine = line;

        if (parseIncludeLine(line, include)) {
            const auto [first, inserted] = firstTrailingCommentByKey.emplace(include.key, include.trailingComment);
            if (!inserted
                && (include.trailingComment.empty() || include.trailingComment == first->second)) {
                removeLine = true;
                ++result.duplicateIncludesRemovedCount;
            } else {
                const std::vector<std::string>* obsoletePatterns = obsoleteUsagePatternsForInclude(include.key);
                if (obsoletePatterns != nullptr) {
                    if (!include.trailingComment.empty()) {
                        ++result.obsoleteIncludesSkippedCount;
                        result.obsoleteIncludeNamesSkipped.push_back(include.key);
                    } else if (containsAnyPattern(searchableCode, *obsoletePatterns)) {
                        ++result.obsoleteIncludesKeptCount;
                        result.obsoleteIncludeNamesKept.push_back(include.key);
                    } else {
                        removeLine = true;
                        ++result.obsoleteIncludesRemovedCount;
                        result.obsoleteIncludeNamesRemoved.push_back(include.key);
                    }
                }

                if (!removeLine) {
                    outputLine = include.normalizedLine;
                    if (outputLine != line) {
                        ++result.syntaxNormalizedCount;
                    }
                    ++result.includesPreservedCount;
                    existingIncludeKeys.insert(include.key);
                }
            }
        }

        if (!removeLine) {
            outputLines.push_back(outputLine);
        }
    }

    const std::vector<std::string> requiredIncludes = detectRequiredStandardIncludes(code);
    std::vector<std::string> missingRequiredIncludes;
    for (const std::string& include : requiredIncludes) {
        if (existingIncludeKeys.find(include) != existingIncludeKeys.end()) {
            ++result.requiredIncludesAlreadyPresentCount;
        } else {
            missingRequiredIncludes.push_back(include);
        }
    }

    if (!missingRequiredIncludes.empty()) {
        const std::size_t insertionIndex = requiredIncludeInsertionIndex(outputLines);
        std::vector<std::string> includeLines;
        includeLines.reserve(missingRequiredIncludes.size());
        for (const std::string& include : missingRequiredIncludes) {
            includeLines.push_back("#include " + include);
            result.requiredIncludeNamesAdded.push_back(include);
        }
        outputLines.insert(outputLines.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
                           includeLines.begin(),
                           includeLines.end());
        result.requiredIncludesAddedCount = missingRequiredIncludes.size();
    }

    const bool finalNewline = !code.empty() && code.back() == '\n';
    result.code = joinLines(outputLines, finalNewline);
    result.modified = result.code != code;
    if (result.modified) {
        addIncludeCleanupChange(changes, result);
    }
    return result;
}
