#include "converter/AutoPtrRemovalPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

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

std::string AutoPtrRemovalPass::rewrite(const std::string& code,
                                        std::vector<ConversionChange>& changes) const
{
    if (code.find("std::auto_ptr") == std::string::npos) {
        return code;
    }

    std::set<std::string> convertedVariables;
    const SafeReplacementEngine safeReplacement;
    bool changed = false;

    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::string rewritten = codePart;
        std::smatch match;

        const std::regex directNewPattern(
            R"(^([ \t]*)std::auto_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s+([A-Za-z_]\w*)\s*\(\s*new\s+\2\s*(?:\(([^;]*)\))?\s*\)\s*;\s*$)");
        if (std::regex_match(codePart, match, directNewPattern)) {
            convertedVariables.insert(match[3].str());
            rewritten = match[1].str() + "auto " + match[3].str() + " = std::make_unique<" + match[2].str() + ">("
                + trim(match[4].matched ? match[4].str() : "") + ");";
            addAppliedChange(changes,
                             "std::auto_ptr to std::unique_ptr",
                             trim(codePart),
                             trim(rewritten),
                             "Replaced deprecated std::auto_ptr construction with std::make_unique and unique ownership.");
            changed = true;
            return rewritten + trailingComment;
        }

        const std::regex declarationPattern(
            R"(^([ \t]*)std::auto_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s+([A-Za-z_]\w*)(.*;\s*)$)");
        if (std::regex_match(codePart, match, declarationPattern)) {
            convertedVariables.insert(match[3].str());
            rewritten = match[1].str() + "std::unique_ptr<" + match[2].str() + "> " + match[3].str() + match[4].str();
            rewritten = std::regex_replace(rewritten,
                                           std::regex(R"(std::auto_ptr\s*<\s*)" + escapeRegex(match[2].str()) + R"(\s*>\s*\(\s*new\s+)" + escapeRegex(match[2].str()) + R"(\s*(?:\(([^;]*)\))?\s*\))"),
                                           "std::make_unique<" + match[2].str() + ">($1)");
            addAppliedChange(changes,
                             "std::auto_ptr to std::unique_ptr",
                             trim(codePart),
                             trim(rewritten),
                             "Converted deprecated std::auto_ptr type to std::unique_ptr.");
            changed = true;
            return rewritten + trailingComment;
        }

        rewritten = std::regex_replace(rewritten, std::regex(R"(\bstd::auto_ptr\s*<)"), "std::unique_ptr<");
        if (rewritten != codePart) {
            addAppliedChange(changes,
                             "std::auto_ptr to std::unique_ptr",
                             trim(codePart),
                             trim(rewritten),
                             "Converted deprecated std::auto_ptr spelling to std::unique_ptr.");
            changed = true;
        }
        return rewritten + trailingComment;
    });

    if (!convertedVariables.empty()) {
        updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
            std::string trailingComment;
            std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
            std::smatch match;
            const std::regex assignmentPattern(R"(^([ \t]*)([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*;\s*$)");
            if (std::regex_match(codePart, match, assignmentPattern)
                && convertedVariables.contains(match[2].str())
                && convertedVariables.contains(match[3].str())) {
                const std::string replacement = match[1].str() + match[2].str() + " = std::move(" + match[3].str() + ");";
                addAppliedChange(changes,
                                 "std::auto_ptr transfer to std::move",
                                 trim(codePart),
                                 trim(replacement),
                                 "Preserved auto_ptr transfer semantics using explicit std::move with std::unique_ptr.");
                changed = true;
                return replacement + trailingComment;
            }
            return line;
        });
    }

    if (!changed) {
        return code;
    }

    const IncludeManager includeManager;
    updated = includeManager.ensureInclude(std::move(updated), "#include <memory>");
    if (updated.find("std::move(") != std::string::npos) {
        updated = includeManager.ensureInclude(std::move(updated), "#include <utility>");
    }
    return updated;
}
