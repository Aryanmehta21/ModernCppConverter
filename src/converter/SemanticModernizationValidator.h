#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class SemanticModernizationValidator
{
public:
    [[nodiscard]] std::string validateAndRepair(const std::string& code,
                                                const ModernizationOptions& options,
                                                const TransformationContext& context,
                                                const std::string& compilerOutput,
                                                std::vector<ConversionChange>& changes) const;
};
