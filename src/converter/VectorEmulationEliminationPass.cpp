#include "converter/VectorEmulationEliminationPass.h"

#include "converter/VectorGrowthEmulationCleanupPass.h"

std::string VectorEmulationEliminationPass::rewrite(const std::string& code,
                                                    const TransformationContext& context,
                                                    std::vector<ConversionChange>& changes) const
{
    const VectorGrowthEmulationCleanupPass growthCleanupPass;
    return growthCleanupPass.rewrite(code, context, changes);
}
