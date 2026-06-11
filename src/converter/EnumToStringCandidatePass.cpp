#include "converter/EnumToStringCandidatePass.h"

#include <regex>
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

std::vector<std::string> parseEnumeratorNames(const std::string& body)
{
    std::vector<std::string> enumerators;
    std::stringstream stream(body);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        std::string name = trim(entry);
        const std::size_t assignment = name.find('=');
        if (assignment != std::string::npos) {
            name = trim(name.substr(0, assignment));
        }
        if (std::regex_match(name, std::regex(R"([A-Za-z_]\w*)"))) {
            enumerators.push_back(name);
        }
    }
    return enumerators;
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
} // namespace

void EnumToStringCandidatePass::suggest(const std::string& code, std::vector<ConversionChange>& changes) const
{
    const std::regex enumPattern(R"(\benum\s+class\s+([A-Za-z_]\w*)\s*(?::\s*[^\{\n]+?)?\s*\{([^{}]*)\}\s*;)",
                                 std::regex::ECMAScript);
    for (std::sregex_iterator it(code.begin(), code.end(), enumPattern), end; it != end; ++it) {
        const std::vector<std::string> enumerators = parseEnumeratorNames((*it)[2].str());
        if (enumerators.size() < 2 || enumerators.size() > 8) {
            continue;
        }
        addSuggestion(changes,
                      "Enum-to-string helper candidate",
                      trim(it->str()),
                      "This small scoped enum may benefit from a switch-based to_string helper or formatter specialization. The rule-based converter records this as a suggestion only.");
    }
}

