#include "converter/ClassStringBufferModernizationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
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

std::vector<std::string> charPointerMembers(const std::string& classText)
{
    std::vector<std::string> members;
    const std::regex memberPattern(R"(\bchar\s*\*\s*([A-Za-z_]\w*)\s*;)");
    for (std::sregex_iterator it(classText.begin(), classText.end(), memberPattern), end; it != end; ++it) {
        members.push_back((*it)[1].str());
    }
    return members;
}

bool hasManualTextCopySemantics(const std::string& classText, const std::string& className)
{
    const std::string escapedClassName = escapeRegex(className);
    return std::regex_search(classText,
                             std::regex("\\b" + escapedClassName + "\\s*\\(\\s*const\\s+" + escapedClassName + "\\s*&\\s*[A-Za-z_]\\w*\\s*\\)"))
        || std::regex_search(classText,
                             std::regex(escapedClassName + "\\s*&\\s*operator\\s*=\\s*\\(\\s*const\\s+" + escapedClassName + "\\s*&\\s*[A-Za-z_]\\w*\\s*\\)"));
}

bool memberHasNewCharAllocation(const std::string& classText, const std::string& member)
{
    const std::string escaped = escapeRegex(member);
    return std::regex_search(classText, std::regex(R"(\b)" + escaped + R"(\s*=\s*new\s+char\s*\[)"))
        || std::regex_search(classText, std::regex(R"(\b)" + escaped + R"(\s*\(\s*new\s+char\s*\[)"));
}

bool memberHasDeleteArrayCleanup(const std::string& classText, const std::string& member)
{
    return std::regex_search(classText, std::regex(R"(delete\s*\[\s*\]\s*)" + escapeRegex(member) + R"(\b)"));
}

bool memberHasTextApiUsage(const std::string& classText, const std::string& member)
{
    const std::string escaped = escapeRegex(member);
    return std::regex_search(classText, std::regex(R"(\b(?:std::)?(?:strcpy|strncpy|strcat|strlen|strcmp)\s*\(\s*)" + escaped + R"(\b)"))
        || std::regex_search(classText, std::regex(R"(\b(?:std::)?(?:strcpy|strncpy|strcat|strlen|strcmp)\s*\([^;\n]*,\s*)" + escaped + R"(\b)"));
}

bool hasClearTextOwnership(const std::string& classText, const std::string& className, const std::string& member)
{
    return memberHasNewCharAllocation(classText, member)
        && memberHasDeleteArrayCleanup(classText, member)
        && memberHasTextApiUsage(classText, member)
        && (hasManualTextCopySemantics(classText, className)
            || std::regex_search(classText, std::regex(R"(~)" + escapeRegex(className) + R"(\s*\(\s*\))")));
}

std::string removeConvertedMemberInitializers(std::string classText, const std::string& member)
{
    const std::string escaped = escapeRegex(member);
    bool changed = true;
    while (changed) {
        const std::string before = classText;
        classText = std::regex_replace(classText,
                                       std::regex(R"(:\s*)" + escaped + R"(\s*\(\s*(?:nullptr|NULL|0|new\s+char\s*\[[^)]*\])\s*\)\s*,\s*)"),
                                       ": ");
        classText = std::regex_replace(classText,
                                       std::regex(R"(,\s*)" + escaped + R"(\s*\(\s*(?:nullptr|NULL|0|new\s+char\s*\[[^)]*\])\s*\))"),
                                       "");
        classText = std::regex_replace(classText,
                                       std::regex(R"(:\s*)" + escaped + R"(\s*\(\s*(?:nullptr|NULL|0|new\s+char\s*\[[^)]*\])\s*\)\s*)"),
                                       "");
        changed = classText != before;
    }

    classText = std::regex_replace(classText,
                                   std::regex(R"(\n[ \t]*:\s*\n(?=[ \t]*\{))"),
                                   "\n");
    classText = std::regex_replace(classText,
                                   std::regex(R"([ \t]+:\s*(?=\{))"),
                                   " ");
    return classText;
}

std::string removeConvertedMemberAllocations(std::string classText, const std::string& member)
{
    const SafeReplacementEngine safeReplacement;
    const std::string escaped = escapeRegex(member);
    return safeReplacement.rewriteCodeLines(classText, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const std::vector<std::regex> obsoletePatterns{
            std::regex(R"(^[ \t]*)" + escaped + R"(\s*=\s*new\s+char\s*\[[^\n;]+\]\s*;\s*$)"),
            std::regex(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escaped + R"(\s*;\s*$)"),
            std::regex(R"(^[ \t]*)" + escaped + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*$)"),
        };
        for (const std::regex& pattern : obsoletePatterns) {
            if (std::regex_match(codePart, pattern)) {
                return trailingComment.empty() ? std::string{} : trailingComment;
            }
        }
        return line;
    });
}

bool lineUsesIdentifier(const std::string& line, const std::string& identifier)
{
    return std::regex_search(line, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::string rewriteTemporaryConcatBuffers(std::string classText,
                                          const std::string& member,
                                          std::vector<ConversionChange>& changes)
{
    const std::string escapedMember = escapeRegex(member);
    std::vector<std::string> lines;
    {
        std::stringstream input(classText);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    bool changed = false;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::string trailingComment;
        const std::string allocationLine = SafeReplacementEngine::splitTrailingLineComment(lines[index], trailingComment);
        std::smatch allocationMatch;
        const std::regex allocationPattern(
            "^([ \\t]*)char\\s*\\*\\s*([A-Za-z_]\\w*)\\s*=\\s*new\\s+char\\s*\\[[^\\]\\n;]*\\b"
            + escapedMember
            + R"((?:\b(?:\s*\.\s*size\s*\(\s*\))?)[^\]\n;]*\]\s*;\s*$)",
            std::regex::ECMAScript);
        if (!std::regex_match(allocationLine, allocationMatch, allocationPattern)) {
            continue;
        }

        const std::string indent = allocationMatch[1].str();
        const std::string tempName = allocationMatch[2].str();
        const std::string escapedTemp = escapeRegex(tempName);
        std::optional<std::size_t> strcpyLine;
        std::optional<std::size_t> strcatLine;
        std::optional<std::size_t> memberAssignmentLine;
        std::optional<std::size_t> tempDeleteLine;
        std::optional<std::size_t> convertedMemberDeleteLine;
        std::string suffixExpression;
        bool unsafeUse = false;

        const std::size_t scanEnd = std::min(lines.size(), index + 9);
        for (std::size_t scan = index + 1; scan < scanEnd; ++scan) {
            std::string scanComment;
            const std::string codePart = trim(SafeReplacementEngine::splitTrailingLineComment(lines[scan], scanComment));
            if (codePart.empty()) {
                continue;
            }

            std::smatch statementMatch;
            if (!strcpyLine
                && std::regex_match(codePart,
                                    std::regex(R"((?:std::)?strcpy\s*\(\s*)" + escapedTemp
                                                   + R"(\s*,\s*)" + escapedMember + R"(\s*\)\s*;)"))) {
                strcpyLine = scan;
                continue;
            }
            if (!strcatLine
                && std::regex_match(codePart,
                                    statementMatch,
                                    std::regex(R"((?:std::)?strcat\s*\(\s*)" + escapedTemp
                                                   + R"(\s*,\s*([^;]+?)\s*\)\s*;)"))) {
                suffixExpression = trim(statementMatch[1].str());
                strcatLine = scan;
                continue;
            }
            if (!memberAssignmentLine
                && std::regex_match(codePart,
                                    std::regex(escapedMember + R"(\s*=\s*)" + escapedTemp + R"(\s*;)"))) {
                memberAssignmentLine = scan;
                continue;
            }
            if (!tempDeleteLine
                && std::regex_match(codePart,
                                    std::regex(R"(delete\s*\[\s*\]\s*)" + escapedTemp + R"(\s*;)"))) {
                tempDeleteLine = scan;
                continue;
            }
            if (!convertedMemberDeleteLine
                && std::regex_match(codePart,
                                    std::regex(R"(delete\s*\[\s*\]\s*)" + escapedMember + R"(\s*;)"))) {
                convertedMemberDeleteLine = scan;
                continue;
            }
            if (lineUsesIdentifier(codePart, tempName)) {
                unsafeUse = true;
                break;
            }
        }

        if (unsafeUse || !strcpyLine || !strcatLine || !memberAssignmentLine || suffixExpression.empty()) {
            continue;
        }

        const std::size_t lastRemoval = std::max({index,
                                                  *strcpyLine,
                                                  *strcatLine,
                                                  *memberAssignmentLine,
                                                  tempDeleteLine.value_or(index),
                                                  convertedMemberDeleteLine.value_or(index)});
        std::string before;
        for (std::size_t lineIndex = index; lineIndex <= lastRemoval; ++lineIndex) {
            if (lineUsesIdentifier(lines[lineIndex], tempName)
                || lineUsesIdentifier(lines[lineIndex], member)
                || lineIndex == convertedMemberDeleteLine.value_or(lines.size())) {
                before += trim(lines[lineIndex]) + "\n";
            }
        }

        lines[index] = indent + member + " += " + suffixExpression + ";";
        lines[*strcpyLine].clear();
        lines[*strcatLine].clear();
        lines[*memberAssignmentLine].clear();
        if (tempDeleteLine) {
            lines[*tempDeleteLine].clear();
        }
        if (convertedMemberDeleteLine) {
            lines[*convertedMemberDeleteLine].clear();
        }

        addAppliedChange(changes,
                         "Temporary C-string concat buffer to std::string append",
                         trim(before),
                         trim(lines[index]),
                         "Replaced a temporary char buffer used only to concatenate into a converted std::string with operator+=.");
        changed = true;
    }

    if (!changed) {
        return classText;
    }

    std::ostringstream output;
    bool first = true;
    for (const std::string& line : lines) {
        if (line.empty()) {
            continue;
        }
        if (!first) {
            output << '\n';
        }
        first = false;
        output << line;
    }
    if (!classText.empty() && classText.back() == '\n') {
        output << '\n';
    }
    return output.str();
}

std::string cleanupOnlyBody(std::string body)
{
    body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), "");
    body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), "");
    body = std::regex_replace(body, std::regex(R"((?:this->)?[A-Za-z_]\w*\s*=\s*[A-Za-z_]\w*\.[A-Za-z_]\w*\s*;)"), "");
    body = std::regex_replace(body, std::regex(R"(\bif\s*\([^)]*\)\s*\{\s*\})"), "");
    body = std::regex_replace(body, std::regex(R"(\bif\s*\(\s*this\s*!=\s*&[A-Za-z_]\w*\s*\)\s*\{\s*\})"), "");
    body = std::regex_replace(body, std::regex(R"(return\s+\*this\s*;)"), "");
    body.erase(std::remove_if(body.begin(), body.end(), [](unsigned char character) {
                   return std::isspace(character) != 0 || character == '{' || character == '}';
               }),
               body.end());
    return body;
}

bool hasSpecialMemberBusinessLogic(const std::string& body)
{
    std::string lowered = body;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lowered.find("std::cout") != std::string::npos
        || lowered.find("std::cerr") != std::string::npos
        || lowered.find("printf") != std::string::npos
        || lowered.find("fprintf") != std::string::npos
        || lowered.find("throw") != std::string::npos
        || lowered.find("log") != std::string::npos
        || lowered.find("telemetry") != std::string::npos
        || lowered.find("callback") != std::string::npos
        || lowered.find("lock") != std::string::npos
        || lowered.find("unlock") != std::string::npos
        || lowered.find("close") != std::string::npos;
}

std::string removeCleanupOnlyFunctionWithBody(std::string classText,
                                              const std::regex& headerPattern,
                                              std::vector<ConversionChange>& changes,
                                              const std::string& ruleName,
                                              const std::string& reason)
{
    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, headerPattern)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = classText.find('{', position);
        if (openBrace == std::string::npos) {
            break;
        }
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }
        const std::string body = classText.substr(openBrace + 1, closeBrace - openBrace - 1);
        if (hasSpecialMemberBusinessLogic(body) || !cleanupOnlyBody(body).empty()) {
            consumed = closeBrace + 1;
            search = classText.substr(consumed);
            continue;
        }
        const std::string before = classText.substr(position, closeBrace - position + 1);
        addAppliedChange(changes, ruleName, trim(before), "removed", reason);
        classText.erase(position, closeBrace - position + 1);
        consumed = position;
        search = classText.substr(consumed);
    }
    return classText;
}

std::string rewriteMemberUsage(std::string classText,
                               const std::string& member,
                               std::vector<ConversionChange>& changes)
{
    const std::string escaped = escapeRegex(member);
    classText = std::regex_replace(classText,
                                   std::regex(R"(\bchar\s*\*\s*)" + escaped + R"(\s*;)"),
                                   "std::string " + member + ";");
    classText = removeConvertedMemberInitializers(std::move(classText), member);
    classText = removeConvertedMemberAllocations(std::move(classText), member);
    classText = std::regex_replace(classText,
                                   std::regex(R"((?:std::)?strncpy\s*\(\s*)" + escaped + R"(\s*,\s*([^,;]+?)\s*,\s*[^;]+?\)\s*;)"),
                                   member + " = $1;");
    classText = std::regex_replace(classText,
                                   std::regex(R"((?:std::)?strcpy\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+)\)\s*;)"),
                                   member + " = $1;");
    classText = std::regex_replace(classText,
                                   std::regex(R"((?:std::)?strcat\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+)\)\s*;)"),
                                   member + " += $1;");
    classText = std::regex_replace(classText,
                                   std::regex(R"(return\s+)" + escaped + R"(\s*;)"),
                                   "return " + member + ".c_str();");
    classText = std::regex_replace(classText,
                                   std::regex(R"(std::strlen\s*\(\s*)" + escaped + R"(\s*\))"),
                                   member + ".size()");
    classText = std::regex_replace(classText,
                                   std::regex(R"(strlen\s*\(\s*)" + escaped + R"(\s*\))"),
                                   member + ".size()");
    classText = std::regex_replace(classText,
                                   std::regex(R"(std::strcmp\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+?)\s*\)\s*==\s*0)"),
                                   member + " == $1");
    classText = std::regex_replace(classText,
                                   std::regex(R"(strcmp\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+?)\s*\)\s*==\s*0)"),
                                   member + " == $1");
    classText = std::regex_replace(classText,
                                   std::regex(R"(std::strcmp\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+?)\s*\)\s*!=\s*0)"),
                                   member + " != $1");
    classText = std::regex_replace(classText,
                                   std::regex(R"(strcmp\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+?)\s*\)\s*!=\s*0)"),
                                   member + " != $1");
    classText = rewriteTemporaryConcatBuffers(std::move(classText), member, changes);

    return classText;
}

bool hasOwnedRawCharMember(const std::string& classText)
{
    for (const std::string& member : charPointerMembers(classText)) {
        if (memberHasDeleteArrayCleanup(classText, member) || memberHasNewCharAllocation(classText, member)) {
            return true;
        }
    }
    return false;
}

std::string removeRuleOfZeroTextSpecialMembers(std::string classText,
                                               const std::string& className,
                                               std::vector<ConversionChange>& changes)
{
    if (hasOwnedRawCharMember(classText)) {
        return classText;
    }

    classText = removeCleanupOnlyFunctionWithBody(classText,
                                                  std::regex(R"((^[ \t]*)~)" + escapeRegex(className) + R"(\s*\(\s*\)\s*\{)",
                                                             std::regex::ECMAScript | std::regex::multiline),
                                                  changes,
                                                  "Rule of Zero after string buffer modernization",
                                                  "Removed a cleanup-only destructor after all owned char text buffers were converted to std::string.");
    classText = removeCleanupOnlyFunctionWithBody(classText,
                                                  std::regex(std::string("(^[ \\t]*)") + escapeRegex(className) + R"(\s*\(\s*const\s+)" + escapeRegex(className) + R"(\s*&\s*[A-Za-z_]\w*\s*\)\s*\{)",
                                                             std::regex::ECMAScript | std::regex::multiline),
                                                  changes,
                                                  "Rule of Zero after string buffer modernization",
                                                  "Removed a cleanup-only copy constructor after all owned char text buffers were converted to std::string.");
    classText = removeCleanupOnlyFunctionWithBody(classText,
                                                  std::regex(std::string("(^[ \\t]*)") + escapeRegex(className) + R"(\s*&\s*operator\s*=\s*\(\s*const\s+)" + escapeRegex(className) + R"(\s*&\s*[A-Za-z_]\w*\s*\)\s*\{)",
                                                             std::regex::ECMAScript | std::regex::multiline),
                                                  changes,
                                                  "Rule of Zero after string buffer modernization",
                                                  "Removed a cleanup-only copy assignment operator after all owned char text buffers were converted to std::string.");
    return classText;
}

std::string removeCstringIfUnused(const std::string& code)
{
    const IncludeManager includeManager;
    return includeManager.removeIncludeIfUnused(code,
                                                "#include <cstring>",
                                                {"std::strcpy", "std::strncpy", "std::strcat", "std::strcmp", "std::strlen",
                                                 "strcpy(", "strncpy(", "strcat(", "strcmp(", "strlen("});
}
} // namespace

std::string ClassStringBufferModernizationPass::rewrite(const std::string& code,
                                                        std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = false;
    std::vector<ClassBlock> classes = ClassResourceAnalyzer().analyzeClasses(updated);
    std::sort(classes.begin(), classes.end(), [](const ClassBlock& left, const ClassBlock& right) {
        return left.start > right.start;
    });

    for (const ClassBlock& block : classes) {
        std::string classText = updated.substr(block.start, block.end - block.start);
        const std::string originalClassText = classText;
        bool classChanged = false;
        std::vector<std::string> safeTextMembers;
        for (const std::string& member : charPointerMembers(originalClassText)) {
            if (hasClearTextOwnership(originalClassText, block.name, member)) {
                safeTextMembers.push_back(member);
            } else if (memberHasDeleteArrayCleanup(originalClassText, member) || memberHasNewCharAllocation(originalClassText, member)) {
                changes.push_back(ConversionChange{
                    "Class raw char buffer modernization preserved",
                    "char* " + member,
                    {},
                    "This raw char* member appears to own memory but was not clearly text-only, so Rule of Zero cleanup is not applied until ownership can be modernized safely.",
                    false,
                    false,
                });
            }
        }

        if (safeTextMembers.empty()) {
            continue;
        }

        for (const std::string& member : safeTextMembers) {
            const std::vector<std::string> remainingCharMembers = charPointerMembers(classText);
            if (std::find(remainingCharMembers.begin(), remainingCharMembers.end(), member)
                == remainingCharMembers.end()) {
                continue;
            }
            const std::string before = classText;
            classText = rewriteMemberUsage(std::move(classText), member, changes);
            if (classText != before) {
                addAppliedChange(changes,
                                 "Class raw char buffer to std::string",
                                 "char* " + member,
                                 "std::string " + member,
                                 "Converted an internally owned text buffer to std::string and rewrote C-string ownership operations.");
                classChanged = true;
            }
        }

        classText = removeRuleOfZeroTextSpecialMembers(std::move(classText), block.name, changes);
        classChanged = classChanged || classText != originalClassText;
        if (classChanged) {
            updated.replace(block.start, block.end - block.start, classText);
            changed = true;
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <string>");
        updated = removeCstringIfUnused(updated);
    }
    return changed ? updated : code;
}
