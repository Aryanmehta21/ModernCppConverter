#include "converter/SemanticConsistencyValidator.h"

#include "converter/DependentUsageRewritePass.h"
#include "converter/CrossFunctionTypePropagationPass.h"
#include "converter/CrossScopeTypePropagationPass.h"
#include "converter/FunctorToLambdaPass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/PolymorphicContractPass.h"
#include "converter/RuleOfZeroPass.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/SemanticTypeValidationPass.h"
#include "converter/SmartPointerTypePropagationPass.h"
#include "converter/ValueTypePointerOperationScanner.h"

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

bool isSmartPointerCollection(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<std::unique_ptr<")
        || record.newType.starts_with("std::array<std::unique_ptr<");
}
} // namespace

std::string SemanticConsistencyValidator::validateAndRepair(const std::string& code,
                                                            const ModernizationOptions& options,
                                                            const TransformationContext& context,
                                                            const std::string& compilerOutput,
                                                            std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SmartPointerTypePropagationPass smartPointerTypePropagationPass;
    updated = smartPointerTypePropagationPass.rewrite(updated, options, context, changes);
    const CrossScopeTypePropagationPass crossScopeTypePropagationPass;
    updated = crossScopeTypePropagationPass.rewrite(updated, options, context, changes);
    const CrossFunctionTypePropagationPass crossFunctionTypePropagationPass;
    updated = crossFunctionTypePropagationPass.rewrite(updated, changes);
    const PolymorphicContractPass polymorphicContractPass;
    updated = polymorphicContractPass.rewrite(updated, changes);
    const FunctorToLambdaPass functorToLambdaPass;
    updated = functorToLambdaPass.rewrite(updated, changes);
    const ValueTypePointerOperationScanner valueTypePointerOperationScanner;
    updated = valueTypePointerOperationScanner.rewrite(updated, context, changes);
    const DependentUsageRewritePass dependentUsageRewritePass;
    updated = dependentUsageRewritePass.runConsistencyChecks(updated, context, changes);
    const OwnershipSanityScanner ownershipSanityScanner;
    updated = ownershipSanityScanner.rewrite(updated, context, changes);
    const ScopeLeakValidationPass scopeLeakValidationPass;
    updated = scopeLeakValidationPass.validate(updated, context, compilerOutput, changes);
    const RuleOfZeroPass ruleOfZeroPass;
    updated = ruleOfZeroPass.rewrite(updated, context, changes);
    const SemanticTypeValidationPass semanticTypeValidationPass;
    updated = semanticTypeValidationPass.validateAndRepair(updated, options, changes);

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (isSmartPointerCollection(record)) {
            const std::regex rawNewPattern(escapeRegex(record.symbolName) + R"((\s*\[[^\]]+\]|\s*\.\s*(push_back|emplace_back)\s*\()\s*(=)?\s*new\s+)");
            if (std::regex_search(updated, rawNewPattern)) {
                addSuggestion(changes,
                              "Semantic consistency validator",
                              record.symbolName,
                              "A smart-pointer collection still has a raw new interaction after propagation. The transformation requires manual ownership review before it should be accepted.");
            }
        }

        if (record.newType.starts_with("std::vector<") || record.newType == "std::string" || record.newType.starts_with("std::array<")) {
            const std::regex pointerOperationPattern(escapeRegex(record.symbolName) + R"(\s*(==|!=|=)\s*nullptr|delete\s*(\[\s*\])?\s*)" + escapeRegex(record.symbolName));
            if (std::regex_search(updated, pointerOperationPattern)) {
                addSuggestion(changes,
                              "Semantic consistency validator",
                              record.symbolName,
                              "Value-type modernization still has pointer-style operations. The invalid-leftover scanner must repair or block this output.");
            }
        }
    }

    return updated;
}
