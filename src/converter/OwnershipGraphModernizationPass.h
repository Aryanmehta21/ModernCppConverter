#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class OwnershipGraphModernizationPass
{
public:
    [[nodiscard]] std::string modernize(const std::string& code,
                                        const ModernizationOptions& options,
                                        TransformationContext& context,
                                        std::vector<ConversionChange>& changes) const;
};
