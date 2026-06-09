#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class ScopeLeakValidationPass
{
public:
    [[nodiscard]] std::string validate(const std::string& code,
                                       const TransformationContext& context,
                                       const std::string& compilerOutput,
                                       std::vector<ConversionChange>& changes) const;
};
