#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class MemberApiCascadePass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const TransformationContext& context,
                                      std::vector<ConversionChange>& changes) const;
};
