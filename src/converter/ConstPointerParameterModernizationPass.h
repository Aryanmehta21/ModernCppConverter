#pragma once

#include "models/ConversionChange.h"

#include <string>
#include <vector>

class ConstPointerParameterModernizationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      std::vector<ConversionChange>& changes) const;
};
