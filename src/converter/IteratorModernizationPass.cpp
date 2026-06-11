#include "converter/IteratorModernizationPass.h"

#include "converter/ModernizationPolishPass.h"

std::string IteratorModernizationPass::rewrite(const std::string& code,
                                               const ModernizationOptions& options,
                                               const TransformationContext& context,
                                               std::vector<ConversionChange>& changes) const
{
    const ModernizationPolishPass polishPass;
    return polishPass.rewrite(code, options, context, changes);
}
