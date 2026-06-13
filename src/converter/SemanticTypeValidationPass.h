#pragma once

#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class SemanticTypeValidationPass
{
public:
    [[nodiscard]] std::string validateAndRepair(const std::string& code,
                                                const ModernizationOptions& options,
                                                std::vector<ConversionChange>& changes) const;
};
