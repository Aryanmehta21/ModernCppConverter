#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class IExplanationGenerator
{
public:
    virtual ~IExplanationGenerator() = default;

    [[nodiscard]] virtual std::string generate(const std::string& modernCode,
                                               const std::vector<ConversionChange>& changes,
                                               const ModernizationOptions& options) const = 0;
};
