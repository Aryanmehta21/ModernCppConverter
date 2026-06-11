#include "converter/AlgorithmModernizationPass.h"

#include "converter/IncludeManager.h"

#include <regex>
#include <sstream>
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

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (const char character : text) {
        if (std::string(R"(\.^$|()[]{}*+?)").find(character) != std::string::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
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

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string ruleName,
                      std::string before,
                      std::string after,
                      std::string reason);

std::string modernizeFindIfLoops(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex findLoop(
        R"((^[ \t]*)auto\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\.end\(\)\s*;\s*\n\1for\s*\(\s*auto\s+([A-Za-z_]\w*)\s*=\s*\3\.begin\(\)\s*;\s*\4\s*!=\s*\3\.end\(\)\s*;\s*(?:\+\+\4|\4\+\+)\s*\)\s*\n\1\{\s*\n\1[ \t]*if\s*\(([^{}\n;]+)\)\s*\n\1[ \t]*\{\s*\n\1[ \t]*\2\s*=\s*\4\s*;\s*\n\1[ \t]*break\s*;\s*\n\1[ \t]*\}\s*\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    bool changed = false;
    while (std::regex_search(search, match, findLoop)) {
        const std::string indent = match[1].str();
        const std::string foundName = match[2].str();
        const std::string collectionName = match[3].str();
        const std::string iteratorName = match[4].str();
        std::string condition = trim(match[5].str());
        if (condition.find(iteratorName) == std::string::npos
            || condition.find("++" + iteratorName) != std::string::npos
            || condition.find(iteratorName + "++") != std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        condition = std::regex_replace(condition, std::regex(R"(\*)" + escapeRegex(iteratorName) + R"(\b)"), "item");
        if (std::regex_search(condition, std::regex("\\b" + escapeRegex(iteratorName) + "\\b"))) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string replacement = indent + "auto " + foundName + " = std::find_if(" + collectionName + ".begin(), " + collectionName + ".end(), [&](const auto& item) {\n"
            + indent + "    return " + condition + ";\n"
            + indent + "});";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Manual search loop to std::find_if",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a simple iterator search loop with assignment and break to std::find_if.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    if (!changed) {
        return code;
    }
    const IncludeManager includeManager;
    return includeManager.ensureInclude(code, "#include <algorithm>");
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
} // namespace

std::string AlgorithmModernizationPass::rewrite(const std::string& code,
                                                const ModernizationOptions& options,
                                                const TransformationContext&,
                                                std::vector<ConversionChange>& changes) const
{
    if (!options.useLambdas && options.offlineModernizationLevel != OfflineModernizationLevel::AiStyleAggressiveRewrite) {
        return modernizeFindIfLoops(code, changes);
    }

    std::string algorithmUpdated = modernizeFindIfLoops(code, changes);
    std::vector<std::string> lines = splitLines(algorithmUpdated);
    bool changed = false;
    for (std::size_t index = 0; index + 7 < lines.size(); ++index) {
        std::smatch counterMatch;
        const std::regex counterPattern(R"(^([ \t]*)(?:int|auto|std::size_t|size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*$)");
        if (!std::regex_match(lines[index], counterMatch, counterPattern)) {
            continue;
        }

        const std::string indent = counterMatch[1].str();
        const std::string counterName = counterMatch[2].str();
        std::smatch loopMatch;
        const std::regex rangeLoopPattern("^" + escapeRegex(indent)
                                             + R"(for\s*\(\s*(?:const\s+auto&|auto&|auto|[A-Za-z_:][A-Za-z0-9_:<>,\s*&]+)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*$)");
        if (!std::regex_match(lines[index + 1], loopMatch, rangeLoopPattern)
            || trim(lines[index + 2]) != "{") {
            continue;
        }

        std::smatch ifMatch;
        const std::regex ifPattern("^" + escapeRegex(indent) + R"([ \t]*if\s*\((.+)\)\s*$)");
        if (!std::regex_match(lines[index + 3], ifMatch, ifPattern)
            || trim(lines[index + 4]) != "{") {
            continue;
        }

        const std::string increment = trim(lines[index + 5]);
        if ((increment != "++" + counterName + ";" && increment != counterName + "++;")
            || trim(lines[index + 6]) != "}"
            || trim(lines[index + 7]) != "}") {
            continue;
        }

        const std::string itemName = loopMatch[1].str();
        const std::string collectionName = loopMatch[2].str();
        const std::string condition = trim(ifMatch[1].str());
        if (condition.find(itemName) == std::string::npos) {
            continue;
        }

        std::ostringstream before;
        for (std::size_t lineIndex = index; lineIndex <= index + 7; ++lineIndex) {
            if (lineIndex > index) {
                before << '\n';
            }
            before << lines[lineIndex];
        }

        std::vector<std::string> replacement{
            indent + "auto " + counterName + " = std::count_if(" + collectionName + ".begin(), " + collectionName + ".end(), [&](const auto& " + itemName + ") {",
            indent + "    return " + condition + ";",
            indent + "});",
        };
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index),
                    lines.begin() + static_cast<std::ptrdiff_t>(index + 8));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), replacement.begin(), replacement.end());
        addAppliedChange(changes,
                         "Manual count loop to std::count_if",
                         trim(before.str()),
                         trim(joinLines(replacement)),
                         "Converted a simple read-only predicate count loop to std::count_if.");
        changed = true;
    }

    if (!changed) {
        return algorithmUpdated;
    }

    const IncludeManager includeManager;
    return includeManager.ensureInclude(joinLines(lines), "#include <algorithm>");
}
