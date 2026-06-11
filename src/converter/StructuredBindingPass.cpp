#include "converter/StructuredBindingPass.h"

#include "converter/ModernizationPolishPass.h"

std::string StructuredBindingPass::rewrite(const std::string& code,
                                           const ModernizationOptions& options,
                                           const TransformationContext& context,
                                           std::vector<ConversionChange>& changes) const
{
    if (!options.useStructuredBindings) {
        return code;
    }
    const ModernizationPolishPass polishPass;
    return polishPass.rewrite(code, options, context, changes);
}

