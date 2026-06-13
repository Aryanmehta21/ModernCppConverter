#include "converter/PassByValueToConstRefPass.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct ParameterRewrite
{
    std::string original;
    std::string replacement;
    std::string name;
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
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        "PassByValueToConstRefPass",
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

std::vector<std::string> splitParameters(const std::string& parameters)
{
    std::vector<std::string> result;
    std::string current;
    int angleDepth = 0;
    int parenDepth = 0;
    for (const char character : parameters) {
        if (character == '<') {
            ++angleDepth;
        } else if (character == '>' && angleDepth > 0) {
            --angleDepth;
        } else if (character == '(') {
            ++parenDepth;
        } else if (character == ')' && parenDepth > 0) {
            --parenDepth;
        }

        if (character == ',' && angleDepth == 0 && parenDepth == 0) {
            result.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!trim(current).empty()) {
        result.push_back(trim(current));
    }
    return result;
}

bool isPrimitiveValueType(std::string type)
{
    type = trim(type);
    type = std::regex_replace(type, std::regex(R"(\bconst\b)"), "");
    type = trim(type);
    static const std::vector<std::string> primitives{
        "bool", "char", "signed char", "unsigned char", "short", "unsigned short",
        "int", "unsigned", "unsigned int", "long", "unsigned long", "long long",
        "unsigned long long", "float", "double", "long double", "std::size_t",
        "size_t", "std::ptrdiff_t", "ptrdiff_t",
    };
    return std::find(primitives.begin(), primitives.end(), type) != primitives.end();
}

bool isSmartPointerType(const std::string& type)
{
    return type.find("std::unique_ptr") != std::string::npos
        || type.find("std::shared_ptr") != std::string::npos;
}

bool isNonTrivialReadOnlyCandidate(const std::string& type)
{
    if (isPrimitiveValueType(type)
        || isSmartPointerType(type)
        || type.find("std::string_view") != std::string::npos
        || type.find("std::span") != std::string::npos) {
        return false;
    }
    if (type.find("std::string") != std::string::npos
        || type.find("std::vector") != std::string::npos
        || type.find("std::map") != std::string::npos
        || type.find("std::unordered_map") != std::string::npos
        || type.find("std::set") != std::string::npos
        || type.find("std::unordered_set") != std::string::npos
        || type.find("std::list") != std::string::npos
        || type.find("std::deque") != std::string::npos
        || type.find("std::array") != std::string::npos
        || type.find("std::optional") != std::string::npos) {
        return true;
    }
    return !type.empty() && std::isupper(static_cast<unsigned char>(type.front()));
}

bool parameterIsReadOnlyBorrow(const std::string& body, const std::string& name)
{
    const std::string escaped = escapeRegex(name);
    if (std::regex_search(body, std::regex(R"(\bstd::move\s*\(\s*)" + escaped + R"(\s*\))"))
        || std::regex_search(body, std::regex(R"(\b)" + escaped + R"(\s*=)"))
        || std::regex_search(body, std::regex(R"((\+\+|--)\s*)" + escaped + R"(\b|\b)" + escaped + R"(\s*(\+\+|--))"))
        || std::regex_search(body, std::regex(R"(\b)" + escaped + R"(\s*\[[^\]]+\]\s*=)"))) {
        return false;
    }

    if (std::regex_search(body, std::regex(R"(\b)" + escaped + R"(\s*\.\s*(push_back|emplace_back|insert|erase|clear|resize|assign|reserve)\s*\()"))
        || std::regex_search(body, std::regex(R"(\breturn\s+)" + escaped + R"(\s*;)"))
        || std::regex_search(body, std::regex(R"(=\s*)" + escaped + R"(\s*;)"))
        || std::regex_search(body, std::regex(R"(\b(push_back|emplace_back|insert|assign)\s*\([^;\n]*\b)" + escaped + R"(\b)"))) {
        return false;
    }

    return true;
}

bool parseValueParameter(const std::string& parameter, std::string& type, std::string& name, std::string& defaultSuffix)
{
    std::string candidate = trim(parameter);
    const std::size_t defaultPosition = candidate.find('=');
    if (defaultPosition != std::string::npos) {
        defaultSuffix = " " + trim(candidate.substr(defaultPosition));
        candidate = trim(candidate.substr(0, defaultPosition));
    } else {
        defaultSuffix.clear();
    }

    if (candidate.find('&') != std::string::npos
        || candidate.find('*') != std::string::npos
        || candidate.find("&&") != std::string::npos
        || std::regex_search(candidate, std::regex(R"(\bconst\b)"))) {
        return false;
    }

    std::smatch match;
    if (!std::regex_match(candidate, match, std::regex(R"((.+?)\s+([A-Za-z_]\w*)$)"))) {
        return false;
    }

    type = trim(match[1].str());
    name = match[2].str();
    return !type.empty() && !name.empty();
}

std::string rewriteHeaderParameters(const std::string& header,
                                    const std::string& body,
                                    std::vector<ParameterRewrite>& rewrites)
{
    const std::size_t open = header.find('(');
    const std::size_t close = header.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return header;
    }

    const std::string parameters = header.substr(open + 1, close - open - 1);
    std::string rewrittenParameters = parameters;
    for (const std::string& originalParameter : splitParameters(parameters)) {
        std::string type;
        std::string name;
        std::string defaultSuffix;
        if (!parseValueParameter(originalParameter, type, name, defaultSuffix)) {
            continue;
        }
        if (!isNonTrivialReadOnlyCandidate(type) || !parameterIsReadOnlyBorrow(body, name)) {
            continue;
        }

        const std::string replacement = "const " + type + "& " + name + defaultSuffix;
        rewrittenParameters = std::regex_replace(rewrittenParameters,
                                                 std::regex("(^|,\\s*)" + escapeRegex(originalParameter) + "(?=\\s*(,|$))"),
                                                 "$1" + replacement,
                                                 std::regex_constants::format_first_only);
        rewrites.push_back(ParameterRewrite{originalParameter, replacement, name});
    }

    if (rewrites.empty()) {
        return header;
    }
    return header.substr(0, open + 1) + rewrittenParameters + header.substr(close);
}
} // namespace

std::string PassByValueToConstRefPass::rewrite(const std::string& code,
                                               std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const std::regex functionHeader(
        R"((^[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+[A-Za-z_]\w*\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{))",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, functionHeader)) {
        const std::size_t headerStart = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = headerStart + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(updated, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string header = updated.substr(headerStart, openBrace - headerStart);
        const std::string body = updated.substr(openBrace + 1, closeBrace - openBrace - 1);
        std::vector<ParameterRewrite> rewrites;
        const std::string rewrittenHeader = rewriteHeaderParameters(header, body, rewrites);
        if (rewrites.empty()) {
            consumed = closeBrace + 1;
            search = updated.substr(consumed);
            continue;
        }

        updated.replace(headerStart, openBrace - headerStart, rewrittenHeader);
        for (const ParameterRewrite& rewrite : rewrites) {
            addAppliedChange(changes,
                             rewrite.original,
                             rewrite.replacement,
                             "Changed a read-only non-trivial by-value parameter to const reference to avoid an unnecessary copy.");
        }
        consumed = headerStart + rewrittenHeader.size() + (closeBrace - openBrace + 1);
        search = consumed < updated.size() ? updated.substr(consumed) : std::string{};
    }

    return updated;
}
