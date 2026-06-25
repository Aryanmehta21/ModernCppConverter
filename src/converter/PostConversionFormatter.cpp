#include "converter/PostConversionFormatter.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <set>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

void trimTrailingWhitespace(std::string& line)
{
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.pop_back();
    }
}

bool startsWithPreprocessorDirective(const std::string& line)
{
    const std::string stripped = trim(line);
    return !stripped.empty() && stripped.front() == '#';
}

bool endsWithMacroContinuation(const std::string& line)
{
    std::string stripped = line;
    trimTrailingWhitespace(stripped);
    return !stripped.empty() && stripped.back() == '\\';
}

struct IncludeLine
{
    std::string key;
    std::string normalized;
    bool local = false;
};

bool parseIncludeLine(const std::string& line, IncludeLine& include)
{
    static const std::regex includePattern(R"(^\s*#\s*include\s*([<"])\s*([^>"]+?)\s*([>"])\s*(//.*)?\s*$)",
                                           std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_match(line, match, includePattern)) {
        return false;
    }

    const std::string opener = match[1].str();
    const std::string target = trim(match[2].str());
    const std::string closer = opener == "<" ? ">" : "\"";
    const std::string comment = match[4].matched ? " " + trim(match[4].str()) : std::string{};
    include.key = opener + target + closer;
    include.normalized = "#include " + opener + target + closer + comment;
    include.local = opener == "\"";
    return !target.empty();
}

std::string normalizeAndSortIncludes(const std::string& code, const bool ensureMemory)
{
    std::stringstream input(code);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }

    std::size_t cursor = 0;
    while (cursor < lines.size() && trim(lines[cursor]).empty()) {
        ++cursor;
    }

    std::vector<IncludeLine> includes;
    std::size_t consumed = cursor;
    while (consumed < lines.size()) {
        IncludeLine include;
        if (parseIncludeLine(lines[consumed], include)) {
            includes.push_back(std::move(include));
            ++consumed;
            continue;
        }
        if (trim(lines[consumed]).empty()) {
            ++consumed;
            continue;
        }
        break;
    }

    if (includes.empty() && !ensureMemory) {
        return code;
    }

    if (ensureMemory) {
        includes.push_back(IncludeLine{"<memory>", "#include <memory>", false});
    }

    std::set<std::string> seen;
    std::vector<IncludeLine> uniqueIncludes;
    for (const IncludeLine& include : includes) {
        if (seen.insert(include.key).second) {
            uniqueIncludes.push_back(include);
        }
    }
    std::sort(uniqueIncludes.begin(), uniqueIncludes.end(), [](const IncludeLine& left, const IncludeLine& right) {
        if (left.local != right.local) {
            return !left.local && right.local;
        }
        return left.key < right.key;
    });

    std::vector<std::string> outputLines;
    for (std::size_t index = 0; index < cursor; ++index) {
        outputLines.push_back(lines[index]);
    }
    bool emittedLocalSeparator = false;
    bool emittedAnyInclude = false;
    for (const IncludeLine& include : uniqueIncludes) {
        if (include.local && emittedAnyInclude && !emittedLocalSeparator) {
            outputLines.emplace_back();
            emittedLocalSeparator = true;
        }
        outputLines.push_back(include.normalized);
        emittedAnyInclude = true;
    }
    if (emittedAnyInclude && consumed < lines.size() && !trim(lines[consumed]).empty()) {
        outputLines.emplace_back();
    }
    for (std::size_t index = consumed; index < lines.size(); ++index) {
        outputLines.push_back(lines[index]);
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < outputLines.size(); ++index) {
        if (index != 0U) {
            output << '\n';
        }
        output << outputLines[index];
    }
    std::string normalized = output.str();
    if (!code.empty() && code.back() == '\n' && (normalized.empty() || normalized.back() != '\n')) {
        normalized.push_back('\n');
    }
    return normalized;
}

std::string rewriteUniquePtrNewConstruction(const std::string& code, bool& needsMemory)
{
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool first = true;
    bool inMacroContinuation = false;
    static const std::regex constructionPattern(
        R"(^([ \t]*)std::unique_ptr\s*<\s*([^>\n]+)\s*>\s+([A-Za-z_]\w*)\s*\(\s*new\s+([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n()]+>)?)\s*\(([^;\n]*)\)\s*\)\s*;\s*(//.*)?$)",
        std::regex::ECMAScript);

    while (std::getline(input, line)) {
        std::string rewritten = line;
        const bool macroLine = inMacroContinuation || startsWithPreprocessorDirective(line);
        inMacroContinuation = macroLine && endsWithMacroContinuation(line);
        if (!macroLine) {
            std::smatch match;
            if (std::regex_match(line, match, constructionPattern)) {
                const std::string trailingComment = match[6].matched ? " " + trim(match[6].str()) : std::string{};
                rewritten = match[1].str()
                    + "auto " + match[3].str()
                    + " = std::make_unique<" + trim(match[4].str()) + ">(" + trim(match[5].str()) + ");"
                    + trailingComment;
                needsMemory = true;
            }
        }
        if (!first) {
            output << '\n';
        }
        first = false;
        output << rewritten;
    }

    std::string rewritten = output.str();
    if (!code.empty() && code.back() == '\n' && (rewritten.empty() || rewritten.back() != '\n')) {
        rewritten.push_back('\n');
    }
    return rewritten;
}

std::string prepareFormattingInput(const std::string& code)
{
    bool needsMemory = false;
    std::string prepared = rewriteUniquePtrNewConstruction(code, needsMemory);
    prepared = normalizeAndSortIncludes(prepared, needsMemory);
    return prepared;
}

std::string normalizeStreamInsertionOutsideLiterals(const std::string& text)
{
    std::string output;
    output.reserve(text.size() + 8U);
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            output.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            output.push_back(character);
            escaped = true;
            continue;
        }
        if (!inCharacter && character == '"') {
            inString = !inString;
            output.push_back(character);
            continue;
        }
        if (!inString && character == '\'') {
            inCharacter = !inCharacter;
            output.push_back(character);
            continue;
        }
        if (!inString && !inCharacter && character == '<' && index + 1U < text.size() && text[index + 1U] == '<') {
            while (!output.empty() && output.back() == ' ') {
                output.pop_back();
            }
            if (!output.empty()) {
                output.push_back(' ');
            }
            output += "<<";
            ++index;
            while (index + 1U < text.size() && text[index + 1U] == ' ') {
                ++index;
            }
            if (index + 1U < text.size()) {
                output.push_back(' ');
            }
            continue;
        }
        output.push_back(character);
    }
    return output;
}

std::pair<int, int> countStructuralBraces(const std::string& text)
{
    int opens = 0;
    int closes = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (!inCharacter && character == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && character == '\'') {
            inCharacter = !inCharacter;
            continue;
        }
        if (!inString && !inCharacter && character == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            break;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '{') {
            ++opens;
        } else if (character == '}') {
            ++closes;
        }
    }
    return {opens, closes};
}

std::string lightweightFormat(std::string code)
{
    code = std::regex_replace(code, std::regex(R"(\boverride\s*\{)"), "override {");

    std::stringstream input(code);
    std::vector<std::string> formattedLines;
    std::string line;
    int blankRun = 0;
    int indentLevel = 0;
    bool inMacroContinuation = false;
    while (std::getline(input, line)) {
        trimTrailingWhitespace(line);

        const bool macroLine = inMacroContinuation || startsWithPreprocessorDirective(line);
        if (macroLine) {
            formattedLines.push_back(line);
            inMacroContinuation = endsWithMacroContinuation(line);
            continue;
        }

        std::string trimmedLine = trim(line);
        if (trimmedLine.empty()) {
            ++blankRun;
            if (blankRun > 1) {
                continue;
            }
            formattedLines.emplace_back();
            continue;
        }
        blankRun = 0;

        const bool lineContainsLiteral = trimmedLine.find('"') != std::string::npos
            || trimmedLine.find('\'') != std::string::npos;
        const bool lineIsComment = trimmedLine.starts_with("//");
        if (!lineIsComment) {
            trimmedLine = normalizeStreamInsertionOutsideLiterals(trimmedLine);
        }
        if (!lineContainsLiteral && !lineIsComment) {
            trimmedLine = std::regex_replace(trimmedLine, std::regex(R"(,\s*)"), ", ");
            if (trimmedLine.find("operator") == std::string::npos) {
                std::string previous;
                do {
                    previous = trimmedLine;
                    trimmedLine = std::regex_replace(trimmedLine,
                                                     std::regex(R"(([^!<>=+\-*/%])\s*=\s*([^=]))"),
                                                     "$1 = $2");
                } while (trimmedLine != previous);
            }
            trimmedLine = std::regex_replace(trimmedLine,
                                             std::regex(R"(\b(if|for|while|switch)\s*\(([^{}\n]*)\)\s*\{)"),
                                             "$1 ($2) {");
            trimmedLine = std::regex_replace(trimmedLine,
                                             std::regex(R"(\b(class|struct)\s+([A-Za-z_]\w*)\s*\{)"),
                                             "$1 $2 {");
            trimmedLine = std::regex_replace(trimmedLine,
                                             std::regex(R"(\b(enum\s+class|enum)\s+([A-Za-z_]\w*)\s*\{)"),
                                             "$1 $2 {");
            trimmedLine = std::regex_replace(trimmedLine, std::regex(R"(\boverride\s*\{)"), "override {");
        }

        const auto [openCount, closeCount] = countStructuralBraces(trimmedLine);
        int lineIndent = indentLevel;
        if (!trimmedLine.empty() && trimmedLine.front() == '}') {
            lineIndent = std::max(0, lineIndent - 1);
        }
        if (std::regex_match(trimmedLine, std::regex(R"((?:public|protected|private)\s*:\s*)"))) {
            lineIndent = std::max(0, indentLevel - 1);
        } else if (std::regex_match(trimmedLine, std::regex(R"((?:case\b.*|default)\s*:\s*)"))) {
            lineIndent = std::max(0, indentLevel - 1);
        }

        formattedLines.push_back(std::string(static_cast<std::size_t>(lineIndent) * 4U, ' ') + trimmedLine);
        indentLevel = std::max(0, indentLevel + openCount - closeCount);
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < formattedLines.size(); ++index) {
        if (index != 0U) {
            output << '\n';
        }
        output << formattedLines[index];
    }
    std::string formatted = output.str();
    if (!code.empty() && code.back() == '\n' && (formatted.empty() || formatted.back() != '\n')) {
        formatted.push_back('\n');
    }
    return formatted;
}

QString detectClangFormat(const std::string& overridePath)
{
    if (!overridePath.empty()) {
        const QString candidate = QString::fromStdString(overridePath);
        return QFileInfo::exists(candidate) ? candidate : QString{};
    }
    return QStandardPaths::findExecutable(QStringLiteral("clang-format"));
}

bool runClangFormat(const QString& executable,
                    const std::string& code,
                    const int timeoutMs,
                    std::string& formatted)
{
    if (executable.isEmpty()) {
        return false;
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--style=LLVM")});
    process.start();
    if (!process.waitForStarted(timeoutMs)) {
        return false;
    }
    process.write(QByteArray::fromStdString(code));
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return false;
    }
    formatted = QString::fromUtf8(process.readAllStandardOutput()).toStdString();
    return !formatted.empty() || code.empty();
}
} // namespace

PostConversionFormatter::PostConversionFormatter(PostConversionFormatterConfig config)
    : config_(std::move(config))
{
}

PostConversionFormattingResult PostConversionFormatter::format(const std::string& code) const
{
    PostConversionFormattingResult result;
    result.code = code;
    result.attempted = true;

    std::string formatted;
    const QString clangFormat = detectClangFormat(config_.clangFormatPathOverride);
    if (!clangFormat.isEmpty() && runClangFormat(clangFormat, prepareFormattingInput(code), config_.timeoutMs, formatted)) {
        result.formatterName = "clang-format";
        result.code = std::move(formatted);
        result.applied = result.code != code;
        result.diagnostic = result.applied
            ? "formatting applied: clang-format"
            : "formatting skipped: already formatted (clang-format)";
        return result;
    }

    if (!config_.allowLightweightFallback) {
        result.diagnostic = "formatting skipped: formatter unavailable";
        return result;
    }

    result.formatterName = "lightweight formatter";
    result.code = lightweightFormat(prepareFormattingInput(code));
    result.applied = result.code != code;
    result.diagnostic = result.applied
        ? "formatting applied: lightweight formatter"
        : "formatting skipped: already formatted (lightweight formatter)";
    return result;
}
