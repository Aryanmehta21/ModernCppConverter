#include "converter/ClassResourceAnalyzerPass.h"

#include "converter/StructuralModernizationEngine.h"

std::string ClassResourceAnalyzerPass::rewrite(const std::string& code,
                                               const ModernizationOptions& options,
                                               TransformationContext& context,
                                               std::vector<ConversionChange>& changes) const
{
    const StructuralModernizationEngine structuralEngine;
    return structuralEngine.modernize(code, options, changes, context);
}
