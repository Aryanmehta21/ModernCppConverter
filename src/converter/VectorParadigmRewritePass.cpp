#include "converter/VectorParadigmRewritePass.h"

#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/VectorAppendMethodRewritePass.h"
#include "converter/VectorEmulationEliminationPass.h"

#include <utility>

namespace
{
void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string ruleName,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}
} // namespace

std::string VectorParadigmRewritePass::rewrite(const std::string& code,
                                               const TransformationContext& context,
                                               std::vector<ConversionChange>& changes) const
{
    if (context.empty()) {
        return code;
    }

    const VectorEmulationEliminationPass vectorEmulationEliminationPass;
    const VectorAppendMethodRewritePass vectorAppendMethodRewritePass;
    const OrphanedGrowthSymbolCleanupPass orphanedGrowthSymbolCleanupPass;
    const OrphanedTempBufferLoopCleanupPass orphanedTempBufferLoopCleanupPass;

    std::string updated = vectorEmulationEliminationPass.rewrite(code, context, changes);
    updated = orphanedGrowthSymbolCleanupPass.rewrite(updated, context, {}, changes);
    updated = orphanedTempBufferLoopCleanupPass.rewrite(updated, context, {}, changes);
    updated = vectorAppendMethodRewritePass.rewrite(updated, context, changes);
    updated = orphanedGrowthSymbolCleanupPass.rewrite(updated, context, {}, changes);
    updated = orphanedTempBufferLoopCleanupPass.rewrite(updated, context, {}, changes);

    if (updated != code) {
        addAppliedChange(changes,
                         "Vector paradigm rewrite",
                         "manual raw-array storage paradigm",
                         "std::vector-native storage and append operations",
                         "Rewrote legacy raw-array growth and insertion logic so std::vector owns allocation, growth, copying, and destruction.");
    }

    return updated;
}
