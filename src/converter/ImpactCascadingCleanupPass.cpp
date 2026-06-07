#include "converter/ImpactCascadingCleanupPass.h"

#include "converter/DependentUsageRewritePass.h"

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

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

bool isStringRecord(const TypeChangeRecord& record)
{
    return record.newType == "std::string";
}
} // namespace

std::string ImpactCascadingCleanupPass::run(const std::string& code,
                                            const TransformationContext& context,
                                            std::vector<ConversionChange>& changes) const
{
    if (context.empty()) {
        return code;
    }

    const std::string before = code;
    const DependentUsageRewritePass dependentUsageRewritePass;
    std::string updated = dependentUsageRewritePass.rewrite(code, context, changes);

    if (updated == before) {
        return updated;
    }

    bool sawVectorChange = false;
    bool sawStringChange = false;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        sawVectorChange = sawVectorChange || isVectorRecord(record);
        sawStringChange = sawStringChange || isStringRecord(record);
    }

    if (sawVectorChange) {
        addAppliedChange(changes,
                         "Vector cascade cleanup",
                         "vector-converted symbols",
                         "dependent pointer-style vector usages rewritten",
                         "After raw arrays became std::vector, dependent new[]/delete[]/nullptr and special-member cleanup was cascaded before verification.");
    }

    if (sawStringChange) {
        addAppliedChange(changes,
                         "String cascade cleanup",
                         "string-converted symbols",
                         "dependent C-string usages rewritten",
                         "After char buffers became std::string, dependent C-string API calls and manual terminators were cascaded before verification.");
    }

    return updated;
}
