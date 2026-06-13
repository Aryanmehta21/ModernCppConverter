#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class RangeForModernizationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      std::vector<ConversionChange>& changes) const;
};
