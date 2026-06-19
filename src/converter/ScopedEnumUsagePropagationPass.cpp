#include "converter/ScopedEnumUsagePropagationPass.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct EnumInfo
{
    std::string name;
    std::vector<std::string> enumerators;
    std::size_t start = 0;
    std::size_t end = 0;
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

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string before,
                      std::string after)
{
    changes.push_back(ConversionChange{
        "Scoped enum usage propagation",
        std::move(before),
        std::move(after),
        "Scoped enum classes require enumerators to be qualified at use sites, so leftover unscoped labels were rewritten to Enum::Value.",
        true,
        false,
    });
}

std::vector<std::string> parseEnumeratorNames(const std::string& body)
{
    std::vector<std::string> names;
    std::stringstream stream(body);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        std::string name = trim(entry);
        const std::size_t assignment = name.find('=');
        if (assignment != std::string::npos) {
            name = trim(name.substr(0, assignment));
        }
        if (std::regex_match(name, std::regex(R"([A-Za-z_]\w*)"))) {
            names.push_back(name);
        }
    }
    return names;
}

std::vector<EnumInfo> collectEnumClasses(const std::string& code)
{
    std::vector<EnumInfo> enums;
    const std::regex enumPattern(R"(\benum\s+class\s+([A-Za-z_]\w*)\s*(?::\s*[^\{\n]+?)?\s*\{([^{}]*)\}\s*;)",
                                 std::regex::ECMAScript);
    for (std::sregex_iterator it(code.begin(), code.end(), enumPattern), end; it != end; ++it) {
        EnumInfo info;
        info.name = (*it)[1].str();
        info.enumerators = parseEnumeratorNames((*it)[2].str());
        info.start = static_cast<std::size_t>(it->position());
        info.end = info.start + static_cast<std::size_t>(it->length());
        if (!info.enumerators.empty()) {
            enums.push_back(std::move(info));
        }
    }
    return enums;
}

bool isIdentifierStart(const char character)
{
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
}

bool isIdentifierCharacter(const char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

bool isInsideEnumDeclaration(const std::vector<EnumInfo>& enums, const std::size_t position)
{
    return std::any_of(enums.begin(), enums.end(), [position](const EnumInfo& info) {
        return position >= info.start && position < info.end;
    });
}

std::size_t previousNonSpace(const std::string& code, std::size_t position)
{
    while (position > 0) {
        --position;
        if (!std::isspace(static_cast<unsigned char>(code[position]))) {
            return position;
        }
    }
    return std::string::npos;
}

std::size_t nextNonSpace(const std::string& code, std::size_t position)
{
    while (position < code.size()) {
        if (!std::isspace(static_cast<unsigned char>(code[position]))) {
            return position;
        }
        ++position;
    }
    return std::string::npos;
}

bool isAlreadyScoped(const std::string& code, const std::size_t tokenStart)
{
    const std::size_t previous = previousNonSpace(code, tokenStart);
    return previous != std::string::npos
        && code[previous] == ':'
        && previous > 0
        && code[previous - 1] == ':';
}

bool followsMemberAccess(const std::string& code, const std::size_t tokenStart)
{
    const std::size_t previous = previousNonSpace(code, tokenStart);
    if (previous == std::string::npos) {
        return false;
    }
    if (code[previous] == '.') {
        return true;
    }
    return code[previous] == '>' && previous > 0 && code[previous - 1] == '-';
}

bool isQualifiedPrefix(const std::string& code, const std::size_t tokenEnd)
{
    const std::size_t next = nextNonSpace(code, tokenEnd);
    return next != std::string::npos
        && next + 1 < code.size()
        && code[next] == ':'
        && code[next + 1] == ':';
}
} // namespace

std::string ScopedEnumUsagePropagationPass::rewrite(const std::string& code,
                                                    std::vector<ConversionChange>& changes) const
{
    const std::vector<EnumInfo> enums = collectEnumClasses(code);
    if (enums.empty()) {
        return code;
    }

    std::map<std::string, std::string> uniqueEnumeratorOwner;
    std::set<std::string> ambiguousEnumerators;
    for (const EnumInfo& info : enums) {
        for (const std::string& enumerator : info.enumerators) {
            const auto [it, inserted] = uniqueEnumeratorOwner.emplace(enumerator, info.name);
            if (!inserted && it->second != info.name) {
                ambiguousEnumerators.insert(enumerator);
            }
        }
    }
    for (const std::string& ambiguous : ambiguousEnumerators) {
        uniqueEnumeratorOwner.erase(ambiguous);
    }
    if (uniqueEnumeratorOwner.empty()) {
        return code;
    }

    std::string updated;
    updated.reserve(code.size());
    bool changed = false;
    bool inString = false;
    bool inCharacter = false;
    bool inBlockComment = false;
    bool inLineComment = false;
    bool escaped = false;

    for (std::size_t index = 0; index < code.size();) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';

        if (inLineComment) {
            updated.push_back(current);
            if (current == '\n') {
                inLineComment = false;
            }
            ++index;
            continue;
        }

        if (inBlockComment) {
            updated.push_back(current);
            if (current == '*' && next == '/') {
                updated.push_back(next);
                index += 2;
                inBlockComment = false;
            } else {
                ++index;
            }
            continue;
        }

        if (inString || inCharacter) {
            updated.push_back(current);
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (inString && current == '"') {
                inString = false;
            } else if (inCharacter && current == '\'') {
                inCharacter = false;
            }
            ++index;
            continue;
        }

        if (current == '/' && next == '/') {
            updated.push_back(current);
            updated.push_back(next);
            index += 2;
            inLineComment = true;
            continue;
        }
        if (current == '/' && next == '*') {
            updated.push_back(current);
            updated.push_back(next);
            index += 2;
            inBlockComment = true;
            continue;
        }
        if (current == '"') {
            updated.push_back(current);
            inString = true;
            ++index;
            continue;
        }
        if (current == '\'') {
            updated.push_back(current);
            inCharacter = true;
            ++index;
            continue;
        }

        if (!isIdentifierStart(current) || isInsideEnumDeclaration(enums, index)) {
            updated.push_back(current);
            ++index;
            continue;
        }

        const std::size_t tokenStart = index;
        while (index < code.size() && isIdentifierCharacter(code[index])) {
            ++index;
        }
        const std::string token = code.substr(tokenStart, index - tokenStart);
        const auto owner = uniqueEnumeratorOwner.find(token);
        if (owner == uniqueEnumeratorOwner.end()
            || isAlreadyScoped(code, tokenStart)
            || followsMemberAccess(code, tokenStart)
            || isQualifiedPrefix(code, index)) {
            updated.append(token);
            continue;
        }

        updated.append(owner->second);
        updated.append("::");
        updated.append(token);
        changed = true;
    }

    if (changed) {
        addAppliedChange(changes, "unscoped enum class labels", "scoped enum class labels");
    }
    return changed ? updated : code;
}

