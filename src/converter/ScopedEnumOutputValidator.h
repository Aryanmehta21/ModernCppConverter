#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class ScopedEnumOutputValidator
{
public:
    [[nodiscard]] std::string validateAndRepair(const std::string& code,
                                                const ModernizationOptions& options,
                                                std::vector<ConversionChange>& changes) const;
};

