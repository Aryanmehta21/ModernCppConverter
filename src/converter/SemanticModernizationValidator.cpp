#include "converter/SemanticModernizationValidator.h"

#include "converter/FunctorToLambdaPass.h"
#include "converter/IteratorModernizationPass.h"
#include "converter/PolymorphicContractPass.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SmartPointerSinkPropagationPass.h"

#include <regex>
#include <string_view>
#include <utility>

namespace
{
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

std::vector<std::pair<std::string, std::string>> collectUniquePtrVariables(const std::string& code)
{
    std::vector<std::pair<std::string, std::string>> variables;
    const std::regex explicitUniquePtr(
        R"(\bstd::unique_ptr\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s+([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), explicitUniquePtr), end; iterator != end; ++iterator) {
        variables.emplace_back((*iterator)[2].str(), (*iterator)[1].str());
    }

    const std::regex autoUniquePtr(
        R"(\bauto\s+([A-Za-z_]\w*)\s*=\s*std::make_unique\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*)\s*>\s*\()");
    for (std::sregex_iterator iterator(code.begin(), code.end(), autoUniquePtr), end; iterator != end; ++iterator) {
        variables.emplace_back((*iterator)[1].str(), (*iterator)[2].str());
    }
    return variables;
}
} // namespace

std::string SemanticModernizationValidator::validateAndRepair(const std::string& code,
                                                              const ModernizationOptions& options,
                                                              const TransformationContext& context,
                                                              const std::string& compilerOutput,
                                                              std::vector<ConversionChange>& changes) const
{
    std::string updated = code;

    const SmartPointerSinkPropagationPass sinkPropagationPass;
    updated = sinkPropagationPass.rewrite(updated, changes);
    const PolymorphicContractPass polymorphicContractPass;
    updated = polymorphicContractPass.rewrite(updated, changes);
    const FunctorToLambdaPass functorToLambdaPass;
    updated = functorToLambdaPass.rewrite(updated, changes);
    const IteratorModernizationPass iteratorModernizationPass;
    updated = iteratorModernizationPass.rewrite(updated, options, context, changes);

    const SemanticConsistencyValidator consistencyValidator;
    updated = consistencyValidator.validateAndRepair(updated, options, context, compilerOutput, changes);

    for (const auto& [name, pointeeType] : collectUniquePtrVariables(updated)) {
        (void)pointeeType;
        const std::regex directCallPattern(R"(\b[A-Za-z_]\w*\s*\([^;\n)]*\b)" + escapeRegex(name) + R"(\b)");
        if (std::regex_search(updated, directCallPattern)) {
            const std::regex alreadyPropagated("\\b" + escapeRegex(name) + R"(\s*\.\s*get\s*\()");
            if (std::regex_search(updated, alreadyPropagated)) {
                continue;
            }
            addSuggestion(changes,
                          "Semantic modernization validator",
                          name,
                          "A std::unique_ptr may still be passed to an API without .get(). The sink propagation pass only applies this automatically when a visible raw-pointer sink signature is known.");
        }
    }

    return updated;
}
