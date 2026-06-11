#include "converter/ScopedEnumOutputValidator.h"

#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"

std::string ScopedEnumOutputValidator::validateAndRepair(const std::string& code,
                                                         const ModernizationOptions& options,
                                                         std::vector<ConversionChange>& changes) const
{
    const ScopedEnumCastValidationPass castValidationPass;
    std::string updated = castValidationPass.validateAndNormalize(code, changes);
    const ScopedEnumOutputPropagationPass propagationPass;
    updated = propagationPass.rewrite(updated, options, changes);
    return castValidationPass.validateAndNormalize(updated, changes);
}
