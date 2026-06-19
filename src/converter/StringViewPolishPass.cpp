#include "converter/StringViewPolishPass.h"

#include "converter/IncludeManager.h"

#include <regex>
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

void addChange(std::vector<ConversionChange>& changes,
               std::string ruleName,
               std::string before,
               std::string after,
               std::string reason,
               bool applied)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        applied,
        false,
    });
}

bool bodyStoresOrEscapesParameter(const std::string& body, const std::string& parameter)
{
    const std::string escaped = escapeRegex(parameter);
    return std::regex_search(body, std::regex(R"(\breturn\s+)" + escaped + R"(\b(?!\s*(?:\.|->)))"))
        || std::regex_search(body, std::regex(R"(\b[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*\s*=\s*)" + escaped + R"(\b)"))
        || std::regex_search(body, std::regex(R"(\b(?:push_back|emplace_back|insert|assign)\s*\([^;\n]*\b)" + escaped + R"(\b)"));
}

bool bodyRequiresNullTerminatedString(const std::string& body, const std::string& parameter)
{
    const std::string escaped = escapeRegex(parameter);
    return std::regex_search(body, std::regex(R"(\b)" + escaped + R"(\s*\.\s*c_str\s*\()"))
        || std::regex_search(body, std::regex(R"(\b(?:std::)?(?:fopen|system)\s*\(\s*)" + escaped + R"(\b)"))
        || std::regex_search(body, std::regex(R"(\b(?:std::)?(?:strcpy|strncpy|strcat|strcmp)\s*\([^;\n]*\b)" + escaped + R"(\b)"))
        || std::regex_search(body, std::regex(R"(\b(?:std::)?(?:printf|fprintf|sprintf|snprintf)\s*\(\s*"[^"\n]*%s[^"\n]*"[^;\n]*\b)" + escaped + R"(\b)"));
}
} // namespace

std::string StringViewPolishPass::rewrite(const std::string& code,
                                          const ModernizationOptions& options,
                                          std::vector<ConversionChange>& changes) const
{
    if (!options.useStringView) {
        return code;
    }

    const std::regex functionPattern(
        R"((^[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+[A-Za-z_]\w*\s*\([^)]*?)const\s+std::string\s*&\s+([A-Za-z_]\w*)([^)]*\)\s*(?:const\s*)?\{([\s\S]*?)^\s*\}))",
        std::regex::ECMAScript | std::regex::multiline);

    std::string updated = code;
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    bool changed = false;
    while (std::regex_search(search, match, functionPattern)) {
        const std::string parameterName = match[2].str();
        const std::string body = match[4].str();
        const std::string before = match[0].str();
        if (bodyStoresOrEscapesParameter(body, parameterName) || bodyRequiresNullTerminatedString(body, parameterName)) {
            addChange(changes,
                      "std::string_view polish",
                      trim(match[1].str() + "const std::string& " + parameterName),
                      {},
                      bodyRequiresNullTerminatedString(body, parameterName)
                          ? "The string parameter is used with a null-terminated C API, so std::string_view conversion was left as a suggestion only."
                          : "The string parameter appears to escape or be stored, so std::string_view conversion was left as a suggestion only.",
                      false);
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        if (!options.applyStringViewWhenSafe) {
            addChange(changes,
                      "std::string_view polish",
                      trim(match[1].str() + "const std::string& " + parameterName),
                      {},
                      "Read-only string parameter is a safe std::string_view candidate, but automatic string_view modernization is disabled.",
                      false);
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string replacement = before;
        replacement.replace(replacement.find("const std::string& " + parameterName),
                            std::string("const std::string& " + parameterName).size(),
                            "std::string_view " + parameterName);
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        addChange(changes,
                  "std::string_view polish",
                  trim(before.substr(0, before.find('{'))),
                  trim(replacement.substr(0, replacement.find('{'))),
                  "Converted a read-only string parameter to std::string_view because it is not stored, returned, or otherwise escaped.",
                  true);
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = updated.substr(consumed);
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <string_view>");
    }
    return updated;
}
