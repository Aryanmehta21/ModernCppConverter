#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class ImpactCascadingCleanupPass
{
public:
    [[nodiscard]] std::string run(const std::string& code,
                                  const TransformationContext& context,
                                  std::vector<ConversionChange>& changes) const;
};
