#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class AlgorithmModernizationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      const TransformationContext& context,
                                      std::vector<ConversionChange>& changes) const;
};
