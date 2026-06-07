#include "converter/IncludeManager.h"

#include <regex>
#include <sstream>

std::string IncludeManager::ensureInclude(std::string code, const std::string& includeLine) const
{
    if (code.find(includeLine) != std::string::npos) {
        return code;
    }

    static const std::regex includePattern(R"(^#include\s+<[^>]+>\s*$)");
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool inserted = false;
    bool sawInclude = false;
    bool firstLine = true;

    while (std::getline(input, line)) {
        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;

        if (!inserted && sawInclude && !std::regex_match(line, includePattern)) {
            output << includeLine << '\n';
            inserted = true;
        }

        output << line;
        if (std::regex_match(line, includePattern)) {
            sawInclude = true;
        }
    }

    if (!inserted) {
        if (sawInclude) {
            output << '\n' << includeLine;
        } else {
            output.str({});
            output.clear();
            output << includeLine << '\n' << code;
        }
    }

    return output.str();
}

std::string IncludeManager::removeIncludeIfUnused(std::string code,
                                                  const std::string& includeLine,
                                                  const std::vector<std::string>& usageNeedles) const
{
    for (const std::string& needle : usageNeedles) {
        if (code.find(needle) != std::string::npos) {
            return code;
        }
    }

    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool firstLine = true;

    while (std::getline(input, line)) {
        if (line == includeLine) {
            continue;
        }
        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;
        output << line;
    }

    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }
    return output.str();
}
