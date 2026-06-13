#pragma once

#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class QualityModernizationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      std::vector<ConversionChange>& changes) const;
};
