#include "converter/SmartPointerTypePropagationPass.h"

#include "converter/DependentUsageRewritePass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
#include "converter/SmartPointerSinkPropagationPass.h"

std::string SmartPointerTypePropagationPass::rewrite(const std::string& code,
                                                     const ModernizationOptions& options,
                                                     const TransformationContext& context,
                                                     std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const SmartPointerCollectionPropagationPass collectionPropagationPass;
    updated = collectionPropagationPass.rewrite(updated, options, context, changes);
    const SmartPointerSinkPropagationPass sinkPropagationPass;
    updated = sinkPropagationPass.rewrite(updated, changes);
    const DependentUsageRewritePass dependentUsageRewritePass;
    updated = dependentUsageRewritePass.rewrite(updated, context, changes);
    const OwnershipSanityScanner ownershipSanityScanner;
    return ownershipSanityScanner.rewrite(updated, context, changes);
}
