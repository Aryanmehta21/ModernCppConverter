#include "converter/FunctionalModernizationPass.h"

#include "converter/AutoDeductionPass.h"
#include "converter/FunctionalModernizationValidator.h"
#include "converter/FunctorToLambdaPass.h"
#include "converter/IndexLoopModernizationPass.h"
#include "converter/RangeForModernizationPass.h"

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
} // namespace

std::string FunctionalModernizationPass::rewrite(const std::string& code,
                                                 const ModernizationOptions& options,
                                                 const TransformationContext&,
                                                 std::vector<ConversionChange>& changes) const
{
    std::string candidate = code;
    const std::size_t changeCountBefore = changes.size();

    const FunctorToLambdaPass functorToLambdaPass;
    candidate = functorToLambdaPass.rewrite(candidate, changes);

    const RangeForModernizationPass rangeForModernizationPass;
    candidate = rangeForModernizationPass.rewrite(candidate, options, changes);

    const IndexLoopModernizationPass indexLoopModernizationPass;
    candidate = indexLoopModernizationPass.rewrite(candidate, options, changes);

    const AutoDeductionPass autoDeductionPass;
    candidate = autoDeductionPass.rewrite(candidate, options, changes);

    if (candidate == code) {
        return code;
    }

    std::string validationReason;
    if (FunctionalModernizationValidator().isValid(candidate, validationReason)) {
        return candidate;
    }

    changes.resize(changeCountBefore);
    addSuggestion(changes,
                  "FunctionalModernizationValidator",
                  "functional modernization candidate",
                  "Rolled back functional modernization because validation failed: " + validationReason);
    return code;
}
