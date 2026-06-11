#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class ClassResourceAnalyzerPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      TransformationContext& context,
                                      std::vector<ConversionChange>& changes) const;
};
