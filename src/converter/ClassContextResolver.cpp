#include "converter/ClassContextResolver.h"

#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

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

std::size_t findMatchingBrace(const std::string& text, const std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < text.size(); ++index) {
        const char current = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
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

std::vector<std::string> splitBases(const std::string& baseList)
{
    std::vector<std::string> bases;
    std::stringstream stream(baseList);
    std::string part;
    while (std::getline(stream, part, ',')) {
        part = trim(std::regex_replace(part, std::regex(R"(\b(?:public|protected|private|virtual)\b)"), ""));
        std::smatch match;
        if (std::regex_search(part, match, std::regex(R"(([A-Za-z_:][A-Za-z0-9_:]*)\s*$)"))) {
            bases.push_back(match[1].str());
        }
    }
    return bases;
}

bool hasVirtualMethod(const std::string& classText)
{
    return std::regex_search(classText, std::regex(R"(\bvirtual\s+(?!~)[^;{}()]+\s+[A-Za-z_]\w*\s*\()"));
}

DestructorContext findDestructor(const ClassBlock& block, const std::string& classText)
{
    DestructorContext result;
    const std::regex destructorHeader(
        R"((^[ \t]*)(virtual\s+)?~([A-Za-z_]\w*)\s*\(\s*\)\s*(override)?\s*(?:=\s*default\s*)?([;{]))",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (!std::regex_search(classText, match, destructorHeader)) {
        return result;
    }

    result.exists = true;
    result.isVirtual = match[2].matched;
    result.hasOverride = match[4].matched;
    result.name = match[3].str();
    result.headerStart = block.start + static_cast<std::size_t>(match.position());
    result.headerEnd = result.headerStart + static_cast<std::size_t>(match.length());
    result.start = result.headerStart;
    result.end = result.headerEnd;

    if (match[5].str() == "{") {
        const std::size_t openBrace = static_cast<std::size_t>(match.position() + match.length() - 1);
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace != std::string::npos) {
            result.end = block.start + closeBrace + 1;
        }
    }
    result.text = classText.substr(result.start - block.start, result.end - result.start);
    return result;
}
} // namespace

std::vector<ClassContext> ClassContextResolver::resolve(const std::string& code) const
{
    const ClassResourceAnalyzer analyzer;
    std::vector<ClassContext> contexts;

    for (const ClassBlock& block : analyzer.analyzeClasses(code)) {
        if (block.openBrace <= block.start || block.openBrace > code.size()) {
            continue;
        }

        const std::string header = code.substr(block.start, block.openBrace - block.start);
        std::smatch headerMatch;
        const std::regex headerPattern(R"(^\s*(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::\s*([^{};]+))?\s*$)",
                                       std::regex::ECMAScript);
        if (!std::regex_match(header, headerMatch, headerPattern)) {
            contexts.push_back(ClassContext{block, block.name, {}, false, hasVirtualMethod(block.text), findDestructor(block, block.text)});
            continue;
        }

        ClassContext context;
        context.block = block;
        context.name = headerMatch[1].str();
        context.baseNames = headerMatch[2].matched ? splitBases(headerMatch[2].str()) : std::vector<std::string>{};
        context.confident = context.name == block.name && !context.name.empty();
        context.hasVirtualMethods = hasVirtualMethod(block.text);
        context.destructor = findDestructor(block, block.text);
        contexts.push_back(std::move(context));
    }

    return contexts;
}
