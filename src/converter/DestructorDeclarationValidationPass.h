#pragma once

#include "models/ConversionChange.h"

#include <string>
#include <vector>

class DestructorDeclarationValidationPass
{
public:
    [[nodiscard]] std::string validateAndRepair(const std::string& code,
                                                std::vector<ConversionChange>& changes) const;
};
