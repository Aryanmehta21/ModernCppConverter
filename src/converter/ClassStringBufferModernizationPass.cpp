#include "converter/ClassStringBufferModernizationPass.h"

#include "converter/IncludeManager.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <regex>
#include <set>
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

bool hasClearTextOwnership(const std::string& classText, const std::string& className, const std::string& member)
{
    const std::string escaped = escapeRegex(member);
    return classText.find("new char[") != std::string::npos
        && classText.find("delete[] " + member) != std::string::npos
        && hasManualTextCopySemantics(classText, className)
        && (std::regex_search(classText, std::regex(R"(std::str(cpy|ncpy)\s*\(\s*)" + escaped + R"(\b)"))
            || std::regex_search(classText, std::regex(R"(str(cpy|ncpy)\s*\(\s*)" + escaped + R"(\b)")))
        && (classText.find("std::strlen") != std::string::npos || classText.find("strlen") != std::string::npos);
}

std::string removeFunctionWithBody(std::string classText,
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
        const std::string before = classText.substr(position, closeBrace - position + 1);
        addAppliedChange(changes, ruleName, trim(before), "removed", reason);
        classText.erase(position, closeBrace - position + 1);
        consumed = position;
        search = classText.substr(consumed);
    }
    return classText;
}

std::string rewriteMemberUsage(std::string classText,
                               const std::string& className,
                               const std::string& member,
                               std::vector<ConversionChange>& changes)
{
    const std::string escaped = escapeRegex(member);
    classText = std::regex_replace(classText,
                                   std::regex(R"(\bchar\s*\*\s*)" + escaped + R"(\s*;)"),
                                   "std::string " + member + ";");
    classText = std::regex_replace(classText,
                                   std::regex(R"(^[ \t]*)" + escaped + R"(\s*=\s*new\s+char\s*\[[^\n;]+\]\s*;\s*\n?)",
                                   std::regex::ECMAScript | std::regex::multiline),
                                   "");
    classText = std::regex_replace(classText,
                                   std::regex(R"(std::strcpy\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+)\)\s*;)"),
                                   member + " = $1;");
    classText = std::regex_replace(classText,
                                   std::regex(R"(strcpy\s*\(\s*)" + escaped + R"(\s*,\s*([^)]+)\)\s*;)"),
                                   member + " = $1;");
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
                                   std::regex(R"(other\.)" + escaped),
                                   "other." + member);
    classText = std::regex_replace(classText,
                                   std::regex(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escaped + R"(\s*;\s*\n?)",
                                   std::regex::ECMAScript | std::regex::multiline),
                                   "");

    classText = removeFunctionWithBody(classText,
                                       std::regex(R"((^[ \t]*)~)" + escapeRegex(className) + R"(\s*\(\s*\)\s*\{)",
                                                  std::regex::ECMAScript | std::regex::multiline),
                                       changes,
                                       "Rule of Zero after string buffer modernization",
                                       "Removed a cleanup-only destructor after converting an owned char buffer to std::string.");
    classText = removeFunctionWithBody(classText,
                                       std::regex(std::string("(^[ \\t]*)") + escapeRegex(className) + R"(\s*\(\s*const\s+)" + escapeRegex(className) + R"(\s*&\s*[A-Za-z_]\w*\s*\)\s*\{)",
                                                  std::regex::ECMAScript | std::regex::multiline),
                                       changes,
                                       "Rule of Zero after string buffer modernization",
                                       "Removed a manual copy constructor after std::string took over text ownership.");
    classText = removeFunctionWithBody(classText,
                                       std::regex(std::string("(^[ \\t]*)") + escapeRegex(className) + R"(\s*&\s*operator\s*=\s*\(\s*const\s+)" + escapeRegex(className) + R"(\s*&\s*[A-Za-z_]\w*\s*\)\s*\{)",
                                                  std::regex::ECMAScript | std::regex::multiline),
                                       changes,
                                       "Rule of Zero after string buffer modernization",
                                       "Removed a manual copy assignment operator after std::string took over text ownership.");
    return classText;
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
        bool classChanged = false;
        for (const std::string& member : charPointerMembers(classText)) {
            if (!hasClearTextOwnership(classText, block.name, member)) {
                continue;
            }
            const std::string before = classText;
            classText = rewriteMemberUsage(std::move(classText), block.name, member, changes);
            if (classText != before) {
                addAppliedChange(changes,
                                 "Class raw char buffer to std::string",
                                 "char* " + member,
                                 "std::string " + member,
                                 "Converted an internally owned text buffer to std::string and rewrote C-string ownership operations.");
                classChanged = true;
            }
        }
        if (classChanged) {
            updated.replace(block.start, block.end - block.start, classText);
            changed = true;
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <string>");
    }
    return changed ? updated : code;
}
