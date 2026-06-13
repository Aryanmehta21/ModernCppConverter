#pragma once

#include "models/ConversionChange.h"

#include <string>
#include <vector>

class NsdmiScopeSafetyPass
{
public:
    [[nodiscard]] std::string validateAndRepair(const std::string& code,
                                                std::vector<ConversionChange>& changes) const;
};
