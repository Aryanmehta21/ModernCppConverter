#include "converter/AlgorithmPolishPass.h"

#include "converter/AlgorithmModernizationPass.h"

std::string AlgorithmPolishPass::rewrite(const std::string& code,
                                         const ModernizationOptions& options,
                                         const TransformationContext& context,
                                         std::vector<ConversionChange>& changes) const
{
    const AlgorithmModernizationPass pass;
    return pass.rewrite(code, options, context, changes);
}

