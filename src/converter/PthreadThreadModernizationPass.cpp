#include "converter/PthreadThreadModernizationPass.h"

#include "converter/RewriteCoordinator.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct LineInfo
{
    std::string text;
    std::size_t start = 0;
    std::size_t end = 0;
};

struct FunctionInfo
{
    std::string name;
    std::string header;
    std::string body;
    std::size_t headerStart = 0;
    std::size_t openParen = 0;
    std::size_t closeParen = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
};

struct WorkerInfo
{
    FunctionInfo function;
    std::string argumentName;
    std::string inferredType;
    bool safe = false;
    std::string skipReason;
};

struct PthreadCreateCall
{
    std::string indentation;
    std::string handleExpression;
    std::string handleBase;
    std::string handleIndex;
    std::string attributesArgument;
    std::string workerName;
    std::string workerArgument;
    std::string originalLine;
    std::size_t lineStart = 0;
    std::size_t lineEnd = 0;
};

struct PthreadJoinCall
{
    std::string indentation;
    std::string handleExpression;
    std::string joinResultArgument;
    std::string originalLine;
    std::size_t lineStart = 0;
    std::size_t lineEnd = 0;
};

struct PthreadDeclaration
{
    std::string handleBase;
    bool array = false;
    std::size_t lineStart = 0;
    std::size_t lineEnd = 0;
};

struct ConversionGroup
{
    PthreadCreateCall create;
    PthreadJoinCall join;
    WorkerInfo worker;
    std::string threadVariable;
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

std::vector<LineInfo> splitLinesWithOffsets(const std::string& code)
{
    std::vector<LineInfo> lines;
    std::size_t start = 0;
    while (start < code.size()) {
        const std::size_t newline = code.find('\n', start);
        const std::size_t end = newline == std::string::npos ? code.size() : newline + 1U;
        lines.push_back(LineInfo{code.substr(start, end - start), start, end});
        start = end;
    }
    if (code.empty()) {
        lines.push_back(LineInfo{});
    }
    return lines;
}

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;

    for (std::size_t index = openBrace; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if ((inString || inCharacter) && current == '\\') {
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
        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
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
        name = name.substr(scope + 2U);
    }
    return name;
}

bool isFunctionLikeKeyword(const std::string& name)
{
    static const std::set<std::string> keywords{
        "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof",
    };
    return keywords.contains(name);
}

std::map<std::string, FunctionInfo> collectFunctions(const std::string& code)
{
    std::map<std::string, FunctionInfo> functions;
    const std::string masked = maskCommentsLiteralsAndMacros(code);
    static const std::regex functionPattern(
        R"((?:^|\n)([ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:~][A-Za-z0-9_:<>,\s*&*]*\s+)*[A-Za-z_:~][A-Za-z0-9_:~]*\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*)?\s*)\{)",
        std::regex::ECMAScript);

    for (std::sregex_iterator iterator(masked.begin(), masked.end(), functionPattern), end; iterator != end; ++iterator) {
        const std::size_t headerStart = static_cast<std::size_t>(iterator->position(1));
        const std::string header = code.substr(headerStart, static_cast<std::size_t>((*iterator)[1].length()));
        const std::size_t openBrace = static_cast<std::size_t>(iterator->position(0) + iterator->length(0) - 1U);
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            continue;
        }

        FunctionInfo function;
        function.name = extractFunctionName(header);
        if (function.name.empty() || isFunctionLikeKeyword(function.name)) {
            continue;
        }
        function.header = header;
        function.headerStart = headerStart;
        function.openParen = headerStart + header.find('(');
        function.closeParen = headerStart + header.rfind(')');
        function.openBrace = openBrace;
        function.closeBrace = closeBrace;
        function.body = code.substr(openBrace + 1U, closeBrace - openBrace - 1U);
        functions[function.name] = std::move(function);
    }

    return functions;
}

std::string canonicalHandleExpression(std::string value)
{
    value = trim(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
                    return std::isspace(character) != 0;
                }),
                value.end());
    return value;
}

std::string threadVariableName(const std::string& handleExpression)
{
    std::string result;
    result.reserve(handleExpression.size());
    for (const char character : handleExpression) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_') {
            result.push_back(character);
        }
    }
    return result.empty() ? "thread" : result;
}

bool isNullArgument(const std::string& argument)
{
    const std::string value = trim(argument);
    return value == "nullptr" || value == "NULL" || value == "0";
}

bool parsePthreadCreateLine(const LineInfo& line, PthreadCreateCall& call)
{
    std::string text = line.text;
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }

    static const std::regex createPattern(
        R"(^([ \t]*)pthread_create\s*\(\s*&\s*([A-Za-z_]\w*)(?:\s*\[\s*([^\]]+)\s*\])?\s*,\s*([^,]+?)\s*,\s*&?\s*([A-Za-z_]\w*)\s*,\s*([^()]*)\)\s*;\s*(?://.*)?$)",
        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(text, match, createPattern)) {
        return false;
    }

    call.indentation = match[1].str();
    call.handleBase = match[2].str();
    call.handleIndex = match[3].matched ? trim(match[3].str()) : std::string{};
    call.handleExpression = call.handleBase + (call.handleIndex.empty() ? "" : "[" + call.handleIndex + "]");
    call.attributesArgument = trim(match[4].str());
    call.workerName = match[5].str();
    call.workerArgument = trim(match[6].str());
    call.originalLine = trim(text);
    call.lineStart = line.start;
    call.lineEnd = line.end;
    return true;
}

bool parsePthreadJoinLine(const LineInfo& line, PthreadJoinCall& join)
{
    std::string text = line.text;
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }

    static const std::regex joinPattern(
        R"(^([ \t]*)pthread_join\s*\(\s*([A-Za-z_]\w*)(?:\s*\[\s*([^\]]+)\s*\])?\s*,\s*([^()]*)\)\s*;\s*(?://.*)?$)",
        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(text, match, joinPattern)) {
        return false;
    }

    join.indentation = match[1].str();
    const std::string handleBase = match[2].str();
    const std::string handleIndex = match[3].matched ? trim(match[3].str()) : std::string{};
    join.handleExpression = handleBase + (handleIndex.empty() ? "" : "[" + handleIndex + "]");
    join.joinResultArgument = trim(match[4].str());
    join.originalLine = trim(text);
    join.lineStart = line.start;
    join.lineEnd = line.end;
    return true;
}

std::vector<PthreadDeclaration> collectPthreadDeclarations(const std::vector<LineInfo>& lines)
{
    std::vector<PthreadDeclaration> declarations;
    static const std::regex declarationPattern(R"(^[ \t]*pthread_t\s+([A-Za-z_]\w*)(?:\s*\[\s*[^\]]+\s*\])?\s*;\s*(?://.*)?$)",
                                               std::regex::ECMAScript);
    static const std::regex arrayPattern(R"(\[[^\]]+\])", std::regex::ECMAScript);

    for (const LineInfo& line : lines) {
        std::string text = line.text;
        if (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        std::smatch match;
        if (!std::regex_match(text, match, declarationPattern)) {
            continue;
        }
        declarations.push_back(PthreadDeclaration{
            match[1].str(),
            std::regex_search(text, arrayPattern),
            line.start,
            line.end,
        });
    }
    return declarations;
}

bool hasCheckedReturnCall(const std::string& maskedCode, const std::string& functionName)
{
    const std::regex statementPattern(R"((^|[;\{\}]\s*)pthread_)" + functionName.substr(std::string("pthread_").size())
                                      + R"(\s*\([^;]*\)\s*;)",
                                      std::regex::ECMAScript);
    std::smatch match;
    auto searchBegin = maskedCode.cbegin();
    while (std::regex_search(searchBegin, maskedCode.cend(), match, statementPattern)) {
        searchBegin = match[0].second;
    }

    const std::regex checkedPattern(R"((?:=|if\s*\(|while\s*\(|return\s+|&&|\|\||\!|==|!=|<|>)\s*)"
                                    + functionName + R"(\s*\()",
                                    std::regex::ECMAScript);
    return std::regex_search(maskedCode, checkedPattern);
}

bool handleHasUnsupportedLifecycleUse(const std::string& maskedCode, const std::string& handleExpression)
{
    const std::vector<std::string> unsupportedApis{
        "pthread_cancel", "pthread_detach", "pthread_kill", "pthread_setschedparam",
        "pthread_getschedparam", "pthread_join_np", "pthread_tryjoin_np", "pthread_timedjoin_np",
    };
    const std::string handle = escapeRegex(canonicalHandleExpression(handleExpression));
    std::string compact = maskedCode;
    compact.erase(std::remove_if(compact.begin(), compact.end(), [](unsigned char character) {
                      return std::isspace(character) != 0;
                  }),
                  compact.end());

    for (const std::string& api : unsupportedApis) {
        const std::regex pattern(R"(\b)" + api + R"(\s*\([^;]*\b)" + handle + R"(\b)");
        if (std::regex_search(compact, pattern)) {
            return true;
        }
    }
    return false;
}

bool workerHasOnlyNullReturns(const std::string& body)
{
    const std::string masked = maskCommentsLiteralsAndMacros(body);
    static const std::regex returnPattern(R"(\breturn\s+([^;]+)\s*;)", std::regex::ECMAScript);
    bool sawReturn = false;
    for (std::sregex_iterator iterator(masked.begin(), masked.end(), returnPattern), end; iterator != end; ++iterator) {
        sawReturn = true;
        if (!isNullArgument((*iterator)[1].str())) {
            return false;
        }
    }
    return sawReturn;
}

std::optional<std::string> inferWorkerArgumentType(const WorkerInfo& worker)
{
    const std::string argument = escapeRegex(worker.argumentName);
    const std::string body = maskCommentsLiteralsAndMacros(worker.function.body);

    const std::vector<std::regex> patterns{
        std::regex(R"(\b([A-Za-z_:]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?)\s+[A-Za-z_]\w*\s*=\s*\*\s*\(\s*\1\s*\*\s*\)\s*)" + argument + R"(\b)",
                   std::regex::ECMAScript),
        std::regex(R"(\b([A-Za-z_:]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?)\s+[A-Za-z_]\w*\s*=\s*\*\s*static_cast\s*<\s*\1\s*\*\s*>\s*\(\s*)" + argument + R"(\s*\))",
                   std::regex::ECMAScript),
        std::regex(R"(\b([A-Za-z_:]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?)\s*\*\s+[A-Za-z_]\w*\s*=\s*\(\s*\1\s*\*\s*\)\s*)" + argument + R"(\b)",
                   std::regex::ECMAScript),
        std::regex(R"(\b([A-Za-z_:]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?)\s*\*\s+[A-Za-z_]\w*\s*=\s*static_cast\s*<\s*\1\s*\*\s*>\s*\(\s*)" + argument + R"(\s*\))",
                   std::regex::ECMAScript),
    };

    std::set<std::string> inferredTypes;
    for (const std::regex& pattern : patterns) {
        for (std::sregex_iterator iterator(body.begin(), body.end(), pattern), end; iterator != end; ++iterator) {
            inferredTypes.insert(trim((*iterator)[1].str()));
        }
    }

    if (inferredTypes.size() != 1U) {
        return std::nullopt;
    }
    return *inferredTypes.begin();
}

std::optional<std::string> workerVoidPointerArgumentName(const FunctionInfo& function)
{
    const std::string parameters = function.header.substr(function.openParen - function.headerStart + 1U,
                                                         function.closeParen - function.openParen - 1U);
    static const std::regex voidPointerParameterPattern(R"(^\s*(?:const\s+)?void\s*\*\s*([A-Za-z_]\w*)\s*$)",
                                                        std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(parameters, match, voidPointerParameterPattern)) {
        return std::nullopt;
    }
    return match[1].str();
}

WorkerInfo analyzeWorker(const FunctionInfo& function)
{
    WorkerInfo worker;
    worker.function = function;

    if (!std::regex_search(function.header, std::regex(R"(\bvoid\s*\*)", std::regex::ECMAScript))) {
        worker.skipReason = "worker return type is not void*";
        return worker;
    }

    const std::optional<std::string> argumentName = workerVoidPointerArgumentName(function);
    if (!argumentName.has_value()) {
        worker.skipReason = "worker argument is not a single void* parameter";
        return worker;
    }
    worker.argumentName = *argumentName;

    if (!workerHasOnlyNullReturns(function.body)) {
        worker.skipReason = "worker returns a meaningful pointer";
        return worker;
    }

    const std::optional<std::string> inferredType = inferWorkerArgumentType(worker);
    if (!inferredType.has_value()) {
        worker.skipReason = "ambiguous worker argument type";
        return worker;
    }

    worker.inferredType = *inferredType;
    worker.safe = true;
    return worker;
}

std::string modernizeWorkerBody(std::string body, const WorkerInfo& worker)
{
    const std::string type = escapeRegex(worker.inferredType);
    const std::string argument = escapeRegex(worker.argumentName);

    body = std::regex_replace(body,
                              std::regex(R"(\*\s*\(\s*)" + type + R"(\s*\*\s*\)\s*)" + argument + R"(\b)",
                                         std::regex::ECMAScript),
                              "*" + worker.argumentName);
    body = std::regex_replace(body,
                              std::regex(R"(\*\s*static_cast\s*<\s*)" + type + R"(\s*\*\s*>\s*\(\s*)" + argument + R"(\s*\))",
                                         std::regex::ECMAScript),
                              "*" + worker.argumentName);
    body = std::regex_replace(body,
                              std::regex(R"(\(\s*)" + type + R"(\s*\*\s*\)\s*)" + argument + R"(\b)",
                                         std::regex::ECMAScript),
                              worker.argumentName);
    body = std::regex_replace(body,
                              std::regex(R"(static_cast\s*<\s*)" + type + R"(\s*\*\s*>\s*\(\s*)" + argument + R"(\s*\))",
                                         std::regex::ECMAScript),
                              worker.argumentName);

    const std::regex aliasDeclarationPattern(R"((^|\n)([ \t]*))" + type + R"(\s*\*\s+([A-Za-z_]\w*)\s*=\s*)"
                                             + argument + R"(\s*;\s*\n?)",
                                             std::regex::ECMAScript);
    std::smatch aliasMatch;
    while (std::regex_search(body, aliasMatch, aliasDeclarationPattern)) {
        const std::string alias = aliasMatch[3].str();
        body.replace(static_cast<std::size_t>(aliasMatch.position()),
                     static_cast<std::size_t>(aliasMatch.length()),
                     aliasMatch[1].str());
        if (alias != worker.argumentName) {
            body = std::regex_replace(body,
                                      std::regex(R"(\b)" + escapeRegex(alias) + R"(\b)", std::regex::ECMAScript),
                                      worker.argumentName);
        }
    }

    body = std::regex_replace(body,
                              std::regex(R"((^|\n)[ \t]*return\s+(?:nullptr|NULL|0)\s*;\s*)",
                                         std::regex::ECMAScript),
                              "$1");
    return body;
}

std::string modernizedWorkerFunction(const std::string& code, const WorkerInfo& worker)
{
    const FunctionInfo& function = worker.function;
    std::string header = code.substr(function.headerStart, function.openBrace - function.headerStart);
    const std::size_t open = header.find('(');
    std::string prefix = trim(header.substr(0, open));
    const std::regex returnAndNamePattern(R"((.*?)\bvoid\s*\*\s*([A-Za-z_]\w*)\s*$)",
                                          std::regex::ECMAScript);
    std::smatch match;
    if (std::regex_match(prefix, match, returnAndNamePattern)) {
        prefix = trim(match[1].str() + " void " + match[2].str());
    } else {
        prefix = "void " + function.name;
    }

    const std::string body = modernizeWorkerBody(function.body, worker);
    return prefix + "(" + worker.inferredType + "* " + worker.argumentName + ")\n{" + body + "}";
}

void addChange(std::vector<ConversionChange>& changes,
               std::string ruleName,
               std::string before,
               std::string after,
               std::string reason,
               const bool applied,
               const bool skipped)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        applied,
        skipped,
    });
}

void addSkip(std::vector<ConversionChange>& changes,
             const std::string& subject,
             const std::string& reason)
{
    addChange(changes,
              "pthread_create/join to std::thread skipped",
              subject,
              subject,
              "Skipped pthread to std::thread modernization: " + reason + ".",
              false,
              true);
}

std::string joinReasons(const std::vector<std::string>& reasons)
{
    std::ostringstream joined;
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        if (index > 0U) {
            joined << "; ";
        }
        joined << reasons[index];
    }
    return joined.str();
}

const char* boolText(const bool value)
{
    return value ? "true" : "false";
}

std::string quoteValue(const std::string& value)
{
    std::string quoted = "\"";
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

std::size_t countCallMatches(const std::string& maskedCode, const std::string& functionName)
{
    const std::regex pattern(R"(\b)" + functionName + R"(\s*\()", std::regex::ECMAScript);
    return static_cast<std::size_t>(
        std::distance(std::sregex_iterator(maskedCode.begin(), maskedCode.end(), pattern),
                      std::sregex_iterator()));
}

std::optional<std::string> workerVoidPointerArgumentName(const FunctionInfo& function);

std::size_t countWorkerFunctions(const std::map<std::string, FunctionInfo>& functions)
{
    return static_cast<std::size_t>(std::count_if(functions.begin(), functions.end(), [](const auto& entry) {
        const FunctionInfo& function = entry.second;
        return std::regex_search(function.header, std::regex(R"(\bvoid\s*\*)", std::regex::ECMAScript))
            && workerVoidPointerArgumentName(function).has_value();
    }));
}

void addPthreadDebug(std::vector<ConversionChange>& changes,
                     const bool sourceContainsPthreadCreate,
                     const std::size_t rawCreateMatches,
                     const std::size_t rawJoinMatches,
                     const std::size_t functionWorkersFound,
                     const std::size_t candidateGroupsFound,
                     const std::size_t convertedGroups,
                     const std::vector<std::string>& skipReasons,
                     const std::vector<PthreadCreateCall>& creates,
                     const std::vector<PthreadJoinCall>& joins)
{
    std::ostringstream diagnostic;
    diagnostic << "PTHREAD MODERNIZATION DEBUG\n"
               << "- pass_registered=true\n"
               << "- pass_executed=true\n"
               << "- source_contains_pthread_create=" << boolText(sourceContainsPthreadCreate) << '\n'
               << "- raw_pthread_create_matches=" << rawCreateMatches << '\n'
               << "- raw_pthread_join_matches=" << rawJoinMatches << '\n'
               << "- function_workers_found=" << functionWorkersFound << '\n'
               << "- candidate_groups_found=" << candidateGroupsFound << '\n'
               << "- converted_groups=" << convertedGroups << '\n'
               << "- skipped_groups=" << skipReasons.size() << '\n'
               << "- skip_reasons=[" << joinReasons(skipReasons) << "]";

    for (std::size_t index = 0; index < creates.size(); ++index) {
        const PthreadCreateCall& create = creates[index];
        diagnostic << '\n'
                   << "- pthread_create[" << index << "]"
                   << " thread_handle_expression=" << quoteValue(create.handleExpression)
                   << " attributes_expression=" << quoteValue(create.attributesArgument)
                   << " worker_function=" << quoteValue(create.workerName)
                   << " argument_expression=" << quoteValue(create.workerArgument);
    }

    for (std::size_t index = 0; index < joins.size(); ++index) {
        const PthreadJoinCall& join = joins[index];
        diagnostic << '\n'
                   << "- pthread_join[" << index << "]"
                   << " thread_handle_expression=" << quoteValue(join.handleExpression)
                   << " result_expression=" << quoteValue(join.joinResultArgument);
    }

    addChange(changes,
              "PTHREAD MODERNIZATION DEBUG",
              "pthread modernization",
              diagnostic.str(),
              diagnostic.str(),
              false,
              false);
}

void addPthreadSummary(std::vector<ConversionChange>& changes,
                       const std::size_t createCount,
                       const std::size_t joinCount,
                       const std::size_t matchedGroups,
                       const std::size_t convertedGroups,
                       const std::vector<std::string>& skipReasons)
{
    std::ostringstream after;
    after << "pthread_create_detected_count=" << createCount
          << " pthread_join_detected_count=" << joinCount
          << " matched_groups_count=" << matchedGroups
          << " converted_groups_count=" << convertedGroups
          << " skipped_groups_count=" << skipReasons.size();
    if (!skipReasons.empty()) {
        after << " skip_reasons=\"" << joinReasons(skipReasons) << "\"";
    }

    addChange(changes,
              "PTHREAD MODERNIZATION summary",
              "pthread modernization",
              after.str(),
              after.str(),
              false,
              false);
}

SourceRange rangeFor(std::size_t start, std::size_t end, std::string name)
{
    SourceRange range;
    range.start.offset = start;
    range.end.offset = end;
    range.entityKind = SourceEntityKind::Statement;
    range.entityName = std::move(name);
    return range;
}

RewriteEdit editFor(std::size_t start,
                    std::size_t end,
                    std::string replacement,
                    std::string reason,
                    std::string symbol)
{
    RewriteEdit edit;
    edit.range = rangeFor(start, end, symbol);
    edit.replacementText = std::move(replacement);
    edit.passName = "PthreadThreadModernizationPass";
    edit.reason = std::move(reason);
    edit.affectedSymbol = std::move(symbol);
    return edit;
}

bool hasCreateOrJoinCallNotMatched(const std::string& maskedCode,
                                   const std::vector<PthreadCreateCall>& creates,
                                   const std::vector<PthreadJoinCall>& joins)
{
    const std::regex createPattern(R"(\bpthread_create\s*\()", std::regex::ECMAScript);
    const std::regex joinPattern(R"(\bpthread_join\s*\()", std::regex::ECMAScript);
    const std::size_t createTextCount = static_cast<std::size_t>(
        std::distance(std::sregex_iterator(maskedCode.begin(), maskedCode.end(), createPattern),
                      std::sregex_iterator()));
    const std::size_t joinTextCount = static_cast<std::size_t>(
        std::distance(std::sregex_iterator(maskedCode.begin(), maskedCode.end(), joinPattern),
                      std::sregex_iterator()));
    return createTextCount != creates.size() || joinTextCount != joins.size();
}
} // namespace

std::string PthreadThreadModernizationPass::rewrite(const std::string& code,
                                                    std::vector<ConversionChange>& changes) const
{
    const std::string maskedCode = maskCommentsLiteralsAndMacros(code);
    if (maskedCode.find("pthread") == std::string::npos) {
        return code;
    }

    const bool sourceContainsPthreadCreate = maskedCode.find("pthread_create") != std::string::npos;
    const std::size_t rawCreateMatches = countCallMatches(maskedCode, "pthread_create");
    const std::size_t rawJoinMatches = countCallMatches(maskedCode, "pthread_join");
    addChange(changes,
              "PTHREAD MODERNIZATION started",
              "pthread modernization",
              "started",
              "PTHREAD MODERNIZATION started.",
              false,
              false);

    const std::vector<LineInfo> lines = splitLinesWithOffsets(code);

    std::vector<PthreadCreateCall> creates;
    std::vector<PthreadJoinCall> joins;
    std::vector<std::string> skipReasons;
    for (const LineInfo& line : lines) {
        const LineInfo maskedLine{maskedCode.substr(line.start, line.end - line.start), line.start, line.end};
        PthreadCreateCall create;
        if (parsePthreadCreateLine(maskedLine, create)) {
            create.originalLine = trim(code.substr(line.start, line.end - line.start));
            creates.push_back(std::move(create));
            continue;
        }

        PthreadJoinCall join;
        if (parsePthreadJoinLine(maskedLine, join)) {
            join.originalLine = trim(code.substr(line.start, line.end - line.start));
            joins.push_back(std::move(join));
        }
    }

    const std::map<std::string, FunctionInfo> functions = collectFunctions(code);
    const std::size_t functionWorkersFound = countWorkerFunctions(functions);
    std::map<std::string, PthreadJoinCall> joinsByHandle;
    for (const PthreadJoinCall& join : joins) {
        joinsByHandle[canonicalHandleExpression(join.handleExpression)] = join;
    }
    std::size_t candidateGroupsFound = 0;
    for (const PthreadCreateCall& create : creates) {
        if (joinsByHandle.contains(canonicalHandleExpression(create.handleExpression))) {
            ++candidateGroupsFound;
        }
    }

    auto addDebug = [&](const std::size_t convertedGroups) {
        addPthreadDebug(changes,
                        sourceContainsPthreadCreate,
                        rawCreateMatches,
                        rawJoinMatches,
                        functionWorkersFound,
                        candidateGroupsFound,
                        convertedGroups,
                        skipReasons,
                        creates,
                        joins);
    };

    if (creates.empty() && joins.empty()) {
        skipReasons.push_back("no pthread_create/pthread_join statement candidates detected");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }

    addChange(changes,
              "pthread_create/join to std::thread diagnostics",
              "pthread groups",
              "detected=" + std::to_string(creates.size()),
              "Detected " + std::to_string(creates.size()) + " pthread create group(s).",
              false,
              false);
    for (const PthreadCreateCall& create : creates) {
        addChange(changes,
                  "pthread_create detected",
                  create.originalLine,
                  create.handleExpression,
                  "Detected pthread_create for handle '" + create.handleExpression + "' and worker '"
                      + create.workerName + "'.",
                  false,
                  false);
    }
    for (const PthreadJoinCall& join : joins) {
        addChange(changes,
                  "pthread_join detected",
                  join.originalLine,
                  join.handleExpression,
                  "Detected pthread_join for handle '" + join.handleExpression + "'.",
                  false,
                  false);
    }

    if (hasCheckedReturnCall(maskedCode, "pthread_create")) {
        skipReasons.push_back("create return value is used");
        addSkip(changes, "pthread_create", "create return value is used");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }
    if (hasCheckedReturnCall(maskedCode, "pthread_join")) {
        skipReasons.push_back("join return value is used");
        addSkip(changes, "pthread_join", "join return value is used");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }
    if (std::regex_search(maskedCode, std::regex(R"(\bpthread_detach\s*\()"))) {
        skipReasons.push_back("detached thread");
        addSkip(changes, "pthread_detach", "detached thread");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }
    if (hasCreateOrJoinCallNotMatched(maskedCode, creates, joins)) {
        skipReasons.push_back("unsupported or non-statement pthread call shape");
        addSkip(changes, "pthread_create/pthread_join", "unsupported or non-statement pthread call shape");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }

    std::vector<ConversionGroup> groups;
    groups.reserve(creates.size());
    std::set<std::string> convertedHandles;
    std::map<std::string, WorkerInfo> workerCache;

    for (const PthreadCreateCall& create : creates) {
        const std::string handle = canonicalHandleExpression(create.handleExpression);
        if (convertedHandles.contains(handle)) {
            skipReasons.push_back("duplicate pthread_create for the same thread handle");
            addSkip(changes, create.originalLine, "duplicate pthread_create for the same thread handle");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }
        if (!isNullArgument(create.attributesArgument)) {
            skipReasons.push_back("non-null thread attributes");
            addSkip(changes, create.originalLine, "non-null thread attributes");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }

        const auto join = joinsByHandle.find(handle);
        if (join == joinsByHandle.end()) {
            skipReasons.push_back("incomplete create/join lifecycle");
            addSkip(changes, create.originalLine, "incomplete create/join lifecycle");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }
        addChange(changes,
                  "pthread_join matched",
                  create.handleExpression,
                  join->second.originalLine,
                  "Matched pthread_create with pthread_join for handle '" + create.handleExpression + "'.",
                  false,
                  false);
        if (!isNullArgument(join->second.joinResultArgument)) {
            skipReasons.push_back("join return value is requested");
            addSkip(changes, join->second.originalLine, "join return value is requested");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }
        if (handleHasUnsupportedLifecycleUse(maskedCode, handle)) {
            skipReasons.push_back("unsupported pthread lifecycle API affects the handle");
            addSkip(changes, create.originalLine, "unsupported pthread lifecycle API affects the handle");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }

        const auto function = functions.find(create.workerName);
        if (function == functions.end()) {
            skipReasons.push_back("worker function is not directly identifiable");
            addSkip(changes, create.workerName, "worker function is not directly identifiable");
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }

        auto [workerIterator, inserted] = workerCache.emplace(create.workerName, WorkerInfo{});
        if (inserted) {
            workerIterator->second = analyzeWorker(function->second);
        }
        if (!workerIterator->second.safe) {
            skipReasons.push_back(workerIterator->second.skipReason);
            addSkip(changes, create.workerName, workerIterator->second.skipReason);
            addDebug(0);
            addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
            return code;
        }
        addChange(changes,
                  "pthread worker argument type inferred",
                  create.workerName,
                  workerIterator->second.inferredType,
                  "Inferred pthread worker argument type '" + workerIterator->second.inferredType
                      + "' for worker '" + create.workerName + "'.",
                  false,
                  false);

        groups.push_back(ConversionGroup{
            create,
            join->second,
            workerIterator->second,
            threadVariableName(handle),
        });
        convertedHandles.insert(handle);
    }

    if (groups.empty()) {
        skipReasons.push_back("no complete safe pthread groups were formed");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }

    std::vector<RewriteEdit> edits;
    for (const ConversionGroup& group : groups) {
        edits.push_back(editFor(group.create.lineStart,
                                group.create.lineEnd,
                                group.create.indentation + "std::thread " + group.threadVariable + "("
                                    + group.create.workerName + ", " + group.create.workerArgument + ");\n",
                                "Converted ignored pthread_create to std::thread construction.",
                                group.threadVariable));
        edits.push_back(editFor(group.join.lineStart,
                                group.join.lineEnd,
                                group.join.indentation + group.threadVariable + ".join();\n",
                                "Converted ignored pthread_join to std::thread::join.",
                                group.threadVariable));
    }

    const std::vector<PthreadDeclaration> declarations = collectPthreadDeclarations(lines);
    for (const PthreadDeclaration& declaration : declarations) {
        const bool usedByConvertedGroup = std::any_of(groups.begin(), groups.end(), [&](const ConversionGroup& group) {
            return group.create.handleBase == declaration.handleBase;
        });
        if (usedByConvertedGroup) {
            edits.push_back(editFor(declaration.lineStart,
                                    declaration.lineEnd,
                                    "",
                                    "Removed pthread_t handle declaration replaced by std::thread.",
                                    declaration.handleBase));
        }
    }

    std::set<std::string> rewrittenWorkers;
    for (const ConversionGroup& group : groups) {
        if (!rewrittenWorkers.insert(group.worker.function.name).second) {
            continue;
        }
        edits.push_back(editFor(group.worker.function.headerStart,
                                group.worker.function.closeBrace + 1U,
                                modernizedWorkerFunction(code, group.worker),
                                "Modernized pthread worker signature after argument type inference.",
                                group.worker.function.name));
    }

    const RewriteApplicationResult rewriteResult = RewriteCoordinator{}.apply(code, edits);
    if (!rewriteResult.skippedEdits.empty()) {
        skipReasons.push_back("rewrite edit conflict prevented atomic group conversion");
        addSkip(changes, "pthread conversion group", "rewrite edit conflict prevented atomic group conversion");
        addDebug(0);
        addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, 0, skipReasons);
        return code;
    }

    for (const ConversionGroup& group : groups) {
        addChange(changes,
                  "pthread_create/join to std::thread",
                  group.create.originalLine + " / " + group.join.originalLine,
                  "std::thread " + group.threadVariable + "(...); " + group.threadVariable + ".join();",
                  "Converted a complete pthread_create/pthread_join group to std::thread.",
                  true,
                  false);
        addChange(changes,
                  "pthread worker signature modernization",
                  group.worker.function.name + "(void*)",
                  group.worker.function.name + "(" + group.worker.inferredType + "*)",
                  "Modernized pthread worker signature after safe argument type inference.",
                  true,
                  false);
    }

    addDebug(groups.size());
    addPthreadSummary(changes, creates.size(), joins.size(), candidateGroupsFound, groups.size(), skipReasons);
    return rewriteResult.code;
}
