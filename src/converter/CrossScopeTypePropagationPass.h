#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class CrossScopeTypePropagationPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      const TransformationContext& context,
                                      std::vector<ConversionChange>& changes) const;

    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      const TransformationContext& context,
                                      const std::string& externalContext,
                                      std::vector<ConversionChange>& changes) const;
};
