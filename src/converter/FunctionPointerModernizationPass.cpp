#include "converter/FunctionPointerModernizationPass.h"

#include "converter/IncludeManager.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct ClassRange
{
    std::size_t bodyBegin = 0;
    std::size_t bodyEnd = 0;
    std::size_t declarationBegin = 0;
    std::string kind;
};

struct StoredCallback
{
    std::string returnType;
    std::string name;
    std::string args;
    std::string parameterName;
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

std::string collapseWhitespace(std::string value)
{
    value = std::regex_replace(std::move(value), std::regex(R"(\s+)"), " ");
    return trim(std::move(value));
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

std::vector<std::string> splitLines(const std::string& code)
{
    std::vector<std::string> lines;
    std::stringstream input(code);
    std::string line;
    while (std::getline(input, line)) {
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

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < code.size(); ++index) {
        const char character = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inChar)) {
            escaped = true;
            continue;
        }
        if (character == '"' && !inChar) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inChar = !inChar;
            continue;
        }
        if (inString || inChar) {
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

std::vector<ClassRange> findClassRanges(const std::string& code)
{
    std::vector<ClassRange> ranges;
    const std::regex classPattern(R"(\b(class|struct)\s+[A-Za-z_]\w*(?:\s*:[^{]+)?\s*\{)");
    for (std::sregex_iterator it(code.begin(), code.end(), classPattern), end; it != end; ++it) {
        const std::size_t declarationBegin = static_cast<std::size_t>(it->position());
        const std::size_t openBrace = declarationBegin + it->str().rfind('{');
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            continue;
        }
        ranges.push_back(ClassRange{openBrace + 1, closeBrace, declarationBegin, (*it)[1].str()});
    }
    return ranges;
}

int relativeBraceDepth(const std::string& text, const std::size_t begin, const std::size_t position)
{
    int depth = 1;
    for (std::size_t index = begin; index < position && index < text.size(); ++index) {
        if (text[index] == '{') {
            ++depth;
        } else if (text[index] == '}') {
            --depth;
        }
    }
    return depth;
}

const ClassRange* containingClassRange(const std::vector<ClassRange>& ranges, const std::size_t position)
{
    for (const ClassRange& range : ranges) {
        if (position > range.bodyBegin && position < range.bodyEnd) {
            return &range;
        }
    }
    return nullptr;
}

bool isTopLevelClassMember(const std::string& code,
                           const std::vector<ClassRange>& ranges,
                           const std::size_t position)
{
    const ClassRange* range = containingClassRange(ranges, position);
    return range != nullptr && relativeBraceDepth(code, range->bodyBegin, position) == 1;
}

bool isPrivateOrProtectedClassMember(const std::string& code,
                                     const ClassRange& range,
                                     const std::size_t position)
{
    std::string access = range.kind == "class" ? "private" : "public";
    const std::string before = code.substr(range.bodyBegin, position - range.bodyBegin);
    const std::regex accessPattern(R"((public|private|protected)\s*:)");
    for (std::sregex_iterator it(before.begin(), before.end(), accessPattern), end; it != end; ++it) {
        access = (*it)[1].str();
    }
    return access == "private" || access == "protected";
}

int braceDepthAt(const std::string& code, const std::size_t position)
{
    int depth = 0;
    for (std::size_t index = 0; index < position && index < code.size(); ++index) {
        if (code[index] == '{') {
            ++depth;
        } else if (code[index] == '}') {
            --depth;
        }
    }
    return depth;
}

bool isNullInitializer(const std::string& initializer)
{
    const std::string value = trim(initializer);
    return value == "nullptr" || value == "NULL" || value == "0";
}

std::string functionType(const std::string& returnType, const std::string& args)
{
    return collapseWhitespace(returnType) + "(" + collapseWhitespace(args) + ")";
}

std::string functionPointerType(const std::string& returnType, const std::string& args)
{
    return collapseWhitespace(returnType) + " (*)(" + collapseWhitespace(args) + ")";
}

std::string rewriteFunctionPointerTypedefs(const std::string& code,
                                           const ModernizationOptions& options,
                                           std::vector<ConversionChange>& changes)
{
    const std::regex typedefPattern(
        R"(^([ \t]*)typedef\s+(.+?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(([^;]*)\)\s*;\s*$)");
    std::vector<std::string> lines = splitLines(code);
    bool changed = false;
    for (std::string& line : lines) {
        std::smatch match;
        if (!std::regex_match(line, match, typedefPattern)) {
            continue;
        }
        const std::string before = trim(line);
        const std::string alias = match[3].str();
        const std::string after = match[1].str() + "using " + alias + " = "
            + functionPointerType(match[2].str(), match[4].str()) + ";";
        if (options.useUsingAliases) {
            line = after;
            changed = true;
            addAppliedChange(changes,
                             "Function pointer typedef to using",
                             before,
                             trim(after),
                             "Function pointer typedef aliases can be expressed more clearly with using while preserving pointer semantics.");
        } else {
            addSuggestion(changes,
                          "Function pointer typedef to using",
                          before,
                          "Function pointer typedef alias was preserved because using aliases are disabled.");
        }
    }
    return changed ? joinLines(lines) : code;
}

std::string rewriteLocalFunctionPointerVariables(const std::string& code,
                                                 const std::vector<ClassRange>& ranges,
                                                 std::vector<ConversionChange>& changes)
{
    const std::regex localPointerPattern(
        R"(^([ \t]*)(.+?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(([^;{}]*)\)\s*=\s*([^;]+)\s*;\s*$)");
    std::vector<std::string> lines = splitLines(code);
    std::size_t position = 0;
    bool changed = false;

    for (std::string& line : lines) {
        std::smatch match;
        const std::size_t linePosition = position;
        position += line.size() + 1;
        if (!std::regex_match(line, match, localPointerPattern)) {
            continue;
        }
        if (isTopLevelClassMember(code, ranges, linePosition)) {
            continue;
        }
        if (braceDepthAt(code, linePosition) == 0) {
            continue;
        }

        const std::string initializer = trim(match[5].str());
        if (isNullInitializer(initializer) || initializer.find("new ") != std::string::npos) {
            continue;
        }

        const std::string before = trim(line);
        const std::string after = match[1].str() + "auto " + match[3].str() + " = " + initializer + ";";
        line = after;
        changed = true;
        addAppliedChange(changes,
                         "Function pointer local callback to auto",
                         before,
                         trim(after),
                         "A local function pointer initialized from a visible callable can use auto without changing callable pointer semantics.");
    }

    return changed ? joinLines(lines) : code;
}

std::vector<StoredCallback> findStoredCallbacks(const std::string& code, const std::vector<ClassRange>& ranges)
{
    std::vector<StoredCallback> callbacks;
    const std::regex memberPattern(
        R"((^|\n)([ \t]*)(.+?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(([^;{}]*)\)\s*(?:=\s*(nullptr|NULL|0))?\s*;)",
        std::regex::ECMAScript);

    for (const ClassRange& range : ranges) {
        const std::string body = code.substr(range.bodyBegin, range.bodyEnd - range.bodyBegin);
        for (std::sregex_iterator it(body.begin(), body.end(), memberPattern), end; it != end; ++it) {
            const std::size_t declarationPosition = range.bodyBegin + static_cast<std::size_t>(it->position()) + (*it)[1].length();
            if (relativeBraceDepth(code, range.bodyBegin, declarationPosition) != 1) {
                continue;
            }
            if (!isPrivateOrProtectedClassMember(code, range, declarationPosition)) {
                continue;
            }

            const std::string returnType = collapseWhitespace((*it)[3].str());
            const std::string memberName = (*it)[4].str();
            const std::string args = collapseWhitespace((*it)[5].str());
            const std::regex assignmentPattern(R"(\b)" + escapeRegex(memberName) + R"(\s*=\s*([A-Za-z_]\w*)\s*;)");
            const std::regex invocationPattern(R"(\b)" + escapeRegex(memberName) + R"(\s*\()");
            std::smatch assignmentMatch;
            if (!std::regex_search(body, assignmentMatch, assignmentPattern) || !std::regex_search(body, invocationPattern)) {
                continue;
            }

            callbacks.push_back(StoredCallback{
                returnType,
                memberName,
                args,
                assignmentMatch[1].str(),
            });
        }
    }
    return callbacks;
}

std::string rewriteStoredCallbacks(std::string code,
                                   const std::vector<StoredCallback>& callbacks,
                                   std::vector<ConversionChange>& changes)
{
    bool changed = false;
    for (const StoredCallback& callback : callbacks) {
        const std::string escapedReturn = escapeRegex(callback.returnType);
        const std::string escapedArgs = escapeRegex(callback.args);
        const std::string escapedName = escapeRegex(callback.name);
        const std::string stdFunctionType = "std::function<" + functionType(callback.returnType, callback.args) + ">";

        const std::regex declarationPattern(
            R"((^|\n)([ \t]*))" + escapedReturn + R"(\s*\(\s*\*\s*)" + escapedName
                + R"(\s*\)\s*\(\s*)" + escapedArgs + R"(\s*\)\s*(?:=\s*(?:nullptr|NULL|0))?\s*;)");
        std::smatch declarationMatch;
        if (std::regex_search(code, declarationMatch, declarationPattern)) {
            const std::string before = trim(declarationMatch[0].str());
            const std::string after = declarationMatch[1].str() + declarationMatch[2].str() + stdFunctionType + " " + callback.name + ";";
            code.replace(static_cast<std::size_t>(declarationMatch.position()),
                         static_cast<std::size_t>(declarationMatch.length()),
                         after);
            changed = true;
            addAppliedChange(changes,
                             "Stored callback pointer to std::function",
                             before,
                             trim(after),
                             "A private callback stored for later invocation can use std::function to accept function pointers and lambdas while preserving call syntax.");
        }

        const std::string escapedParameter = escapeRegex(callback.parameterName);
        const std::regex parameterPattern(escapedReturn + R"(\s*\(\s*\*\s*)" + escapedParameter
                                          + R"(\s*\)\s*\(\s*)" + escapedArgs + R"(\s*\))");
        std::smatch parameterMatch;
        while (std::regex_search(code, parameterMatch, parameterPattern)) {
            const std::size_t absolutePosition = static_cast<std::size_t>(parameterMatch.position());
            const std::size_t functionStart = code.rfind('{', absolutePosition);
            const std::size_t assignmentPosition = code.find(callback.name + " = " + callback.parameterName + ";", absolutePosition);
            const std::size_t nextClose = code.find('}', absolutePosition);
            if (functionStart != std::string::npos && assignmentPosition != std::string::npos
                && nextClose != std::string::npos && assignmentPosition < nextClose) {
                const std::string before = parameterMatch[0].str();
                const std::string after = stdFunctionType + " " + callback.parameterName;
                code.replace(absolutePosition, before.size(), after);
                changed = true;
                addAppliedChange(changes,
                                 "Stored callback parameter to std::function",
                                 before,
                                 after,
                                 "The callback parameter feeds std::function storage, so the visible local API can accept function pointers and lambdas consistently.");
                continue;
            }
            break;
        }

        const std::regex notNullPattern(R"(\b)" + escapedName + R"(\s*!=\s*(?:nullptr|NULL|0))");
        const std::regex nullPattern(R"(\b)" + escapedName + R"(\s*==\s*(?:nullptr|NULL|0))");
        const std::string beforeNullChecks = code;
        code = std::regex_replace(code, notNullPattern, callback.name);
        code = std::regex_replace(code, nullPattern, "!" + callback.name);
        if (code != beforeNullChecks) {
            changed = true;
            addAppliedChange(changes,
                             "Stored callback nullptr check cleanup",
                             beforeNullChecks == code ? std::string{} : callback.name + " != nullptr",
                             callback.name,
                             "std::function exposes emptiness through its boolean conversion, so converted callback checks use that form.");
        }
    }

    if (changed) {
        code = IncludeManager{}.ensureInclude(std::move(code), "#include <functional>");
    }
    return code;
}

void suggestPreservedRawCallbackParameters(const std::string& before,
                                           const std::string& after,
                                           std::vector<ConversionChange>& changes)
{
    const std::regex rawParameterPattern(R"((.+?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(([^)]*)\))");
    for (std::sregex_iterator it(after.begin(), after.end(), rawParameterPattern), end; it != end; ++it) {
        const std::string text = trim(it->str());
        if (before.find(text) == std::string::npos) {
            continue;
        }
        addSuggestion(changes,
                      "Function pointer parameter preserved",
                      text,
                      "Raw function pointer parameter was preserved because it is an observing callback interface and std::function could change ABI, allocation, or performance semantics.");
    }
}
} // namespace

std::string FunctionPointerModernizationPass::rewrite(const std::string& code,
                                                      const ModernizationOptions& options,
                                                      std::vector<ConversionChange>& changes) const
{
    std::string updated = rewriteFunctionPointerTypedefs(code, options, changes);
    const std::vector<ClassRange> ranges = findClassRanges(updated);
    updated = rewriteLocalFunctionPointerVariables(updated, ranges, changes);
    const std::vector<StoredCallback> storedCallbacks = findStoredCallbacks(updated, findClassRanges(updated));
    updated = rewriteStoredCallbacks(std::move(updated), storedCallbacks, changes);
    suggestPreservedRawCallbackParameters(code, updated, changes);
    return updated;
}
