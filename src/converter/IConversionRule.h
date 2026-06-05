#pragma once

#include "converter/CodeRepresentation.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class IConversionRule
{
public:
    virtual ~IConversionRule() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    virtual void apply(CodeRepresentation& representation,
                       const ModernizationOptions& options,
                       std::vector<ConversionChange>& changes) const = 0;
};
