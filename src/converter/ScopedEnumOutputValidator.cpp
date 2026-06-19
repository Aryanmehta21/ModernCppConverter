#include "converter/ScopedEnumOutputValidator.h"

#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumUsagePropagationPass.h"

std::string ScopedEnumOutputValidator::validateAndRepair(const std::string& code,
                                                         const ModernizationOptions& options,
                                                         std::vector<ConversionChange>& changes) const
{
    const ScopedEnumUsagePropagationPass usagePropagationPass;
    std::string updated = usagePropagationPass.rewrite(code, changes);
    const ScopedEnumCastValidationPass castValidationPass;
    updated = castValidationPass.validateAndNormalize(updated, changes);
    const ScopedEnumOutputPropagationPass propagationPass;
    updated = propagationPass.rewrite(updated, options, changes);
    return castValidationPass.validateAndNormalize(updated, changes);
}
