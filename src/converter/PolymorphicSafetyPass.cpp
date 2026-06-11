#include "converter/PolymorphicSafetyPass.h"

#include "converter/PolymorphicContractPass.h"
#include "converter/SmartPointerCollectionPropagationPass.h"

std::string PolymorphicSafetyPass::rewrite(const std::string& code,
                                           const ModernizationOptions& options,
                                           const TransformationContext& context,
                                           std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SmartPointerCollectionPropagationPass propagationPass;
    updated = propagationPass.rewrite(updated, options, context, changes);
    const PolymorphicContractPass contractPass;
    return contractPass.rewrite(updated, changes);
}
