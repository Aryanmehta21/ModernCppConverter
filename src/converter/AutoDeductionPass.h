#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class AutoDeductionPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      std::vector<ConversionChange>& changes) const;
};
