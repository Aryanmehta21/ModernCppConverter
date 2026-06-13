#include "converter/RuleOfZeroPass.h"

#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
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

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
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

bool hasBusinessLogic(const std::string& body)
{
    const std::string lowered = lowercase(body);
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

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBrace)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBrace; index < code.size(); ++index) {
        const char character = code[index];
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

bool cleanupOnlyBody(std::string body)
{
    if (hasBusinessLogic(body)) {
        return false;
    }
    body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), "");
    body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), "");
    body = std::regex_replace(body, std::regex(R"(\bif\s*\([^)]*\)\s*\{\s*\})"), "");
    body = std::regex_replace(body, std::regex(R"(delete\s*(?:\[\s*\])?\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[[^\]]+\])?\s*;)"), "");
    body = std::regex_replace(body, std::regex(R"([A-Za-z_][A-Za-z0-9_]*(?:\s*->\s*|\s*\.\s*)?[A-Za-z_][A-Za-z0-9_]*\s*=\s*(?:nullptr|0)\s*;)"), "");
    body.erase(std::remove_if(body.begin(), body.end(), [](unsigned char character) {
                   return std::isspace(character) || character == '{' || character == '}';
               }),
               body.end());
    return body.empty();
}

bool classHasUnconvertedRawOwnedResource(const std::string& classText)
{
    const std::regex rawMemberPattern(R"(\b(?:char|wchar_t|unsigned\s+char|[A-Za-z_][A-Za-z0-9_:<>]*)\s*\*\s*([A-Za-z_]\w*)\s*(?:=\s*(?:nullptr|NULL|0))?\s*;)");
    for (std::sregex_iterator it(classText.begin(), classText.end(), rawMemberPattern), end; it != end; ++it) {
        const std::string member = (*it)[1].str();
        const std::string escaped = std::regex_replace(member, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
        const bool deleted = std::regex_search(classText, std::regex(R"(delete\s*(?:\[\s*\])?\s*)" + escaped + R"(\b)"));
        const bool allocated = std::regex_search(classText, std::regex(R"(\b)" + escaped + R"(\s*=\s*new\b)"))
            || std::regex_search(classText, std::regex(R"(\b)" + escaped + R"(\s*\(\s*new\b)"));
        if (deleted || allocated) {
            return true;
        }
    }
    return false;
}

bool classStillNeedsManualOwnershipCleanup(const std::string& code, const std::string& className)
{
    const ClassResourceAnalyzer analyzer;
    for (const ClassBlock& block : analyzer.analyzeClasses(code)) {
        if (block.name == className) {
            return classHasUnconvertedRawOwnedResource(block.text);
        }
    }
    return false;
}
} // namespace

std::string RuleOfZeroPass::rewrite(const std::string& code,
                                    const TransformationContext& context,
                                    std::vector<ConversionChange>& changes) const
{
    std::set<std::string> classesWithAutomaticOwnership;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (record.isClassMember && !record.scopeName.empty()) {
            classesWithAutomaticOwnership.insert(record.scopeName);
        }
    }
    const ClassResourceAnalyzer classAnalyzer;
    for (const ClassBlock& block : classAnalyzer.analyzeClasses(code)) {
        if (block.text.find("std::unique_ptr<") != std::string::npos
            || block.text.find("std::shared_ptr<") != std::string::npos
            || block.text.find("std::vector<") != std::string::npos
            || block.text.find("std::array<") != std::string::npos
            || block.text.find("std::string") != std::string::npos) {
            classesWithAutomaticOwnership.insert(block.name);
        }
    }
    if (classesWithAutomaticOwnership.empty()) {
        return code;
    }

    std::string updated = code;
    const std::regex destructorHeader(R"((^[ \t]*)(?:virtual\s+)?~([A-Za-z_]\w*)\s*\(\s*\)\s*\{)",
                                      std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, destructorHeader)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(updated, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string functionText = updated.substr(position, closeBrace - position + 1);
        const std::string className = match[2].str();
        if (!classesWithAutomaticOwnership.contains(className)) {
            consumed = closeBrace + 1;
            search = updated.substr(consumed);
            continue;
        }
        const std::string body = updated.substr(openBrace + 1, closeBrace - openBrace - 1);
        if (classStillNeedsManualOwnershipCleanup(updated, className)) {
            consumed = closeBrace + 1;
            search = updated.substr(consumed);
            continue;
        }
        if (!cleanupOnlyBody(body)) {
            consumed = closeBrace + 1;
            search = updated.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         "Rule of Zero special member removal",
                         trim(functionText),
                         "removed",
                         "Removed a cleanup-only destructor because ownership is now handled by standard library RAII types.");
        updated.replace(position, closeBrace - position + 1, "");
        consumed = position;
        search = updated.substr(consumed);
    }
    return updated;
}
