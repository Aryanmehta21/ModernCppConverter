#include "converter/AutoDeductionPass.h"

#include "converter/SafeReplacementEngine.h"

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

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        "Auto iterator type deduction",
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: AutoDeductionPass",
        "pass started",
        {},
        "candidates found: " + std::to_string(candidates)
            + ", candidates converted: " + std::to_string(converted)
            + ", candidates skipped: " + std::to_string(skipped),
        false,
        true,
    });
}

void addSkippedDiagnostic(std::vector<ConversionChange>& changes, std::string reason)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: AutoDeductionPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}
} // namespace

std::string AutoDeductionPass::rewrite(const std::string& code,
                                       const ModernizationOptions& options,
                                       std::vector<ConversionChange>& changes) const
{
    if (!options.useAuto) {
        return code;
    }

    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        if (line.find("for") != std::string::npos
            || line.find("reverse_iterator") != std::string::npos
            || line.find("rbegin") != std::string::npos
            || line.find("rend") != std::string::npos) {
            if (line.find("::iterator") != std::string::npos || line.find("::const_iterator") != std::string::npos) {
                ++candidates;
                ++skipped;
                addSkippedDiagnostic(changes, "Iterator declaration was preserved because it appears in loop control or reverse iteration.");
            }
            return line;
        }

        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;
        const std::regex iteratorDeclaration(
            R"(^([ \t]*)(?:typename\s+)?[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator\s+([A-Za-z_]\w*)\s*=\s*([^;\n]*(?:c?begin|c?end)\s*\([^;\n]*\))\s*;\s*$)");
        if (!std::regex_match(codePart, match, iteratorDeclaration)) {
            return line;
        }

        ++candidates;
        const std::string replacement = match[1].str() + "auto " + match[2].str() + " = " + trim(match[3].str()) + ";";
        addAppliedChange(changes,
                         trim(codePart),
                         trim(replacement),
                         "Replaced a verbose iterator declaration with auto because the initializer exposes the iterator source.");
        changed = true;
        ++converted;
        return replacement + trailingComment;
    });

    addDiagnostic(changes, candidates, converted, skipped);
    return changed ? updated : code;
}
