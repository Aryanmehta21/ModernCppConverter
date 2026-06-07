#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class OrphanedGrowthSymbolCleanupPass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const TransformationContext& context,
                                      const std::string& compilerDiagnostics,
                                      std::vector<ConversionChange>& changes) const;
};
