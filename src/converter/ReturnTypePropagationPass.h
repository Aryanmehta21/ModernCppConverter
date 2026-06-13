#pragma once

#include "converter/ConversionRule.h"

#include <string>
#include <vector>

class ReturnTypePropagationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      std::vector<ConversionChange>& changes) const;
};
