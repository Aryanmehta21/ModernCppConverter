#pragma once

#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <string>

class IConverterEngine
{
public:
    virtual ~IConverterEngine() = default;

    [[nodiscard]] virtual ConversionResult convert(const std::string& legacyCode) const = 0;
    [[nodiscard]] virtual ConversionResult convert(const std::string& legacyCode,
                                                   const ModernizationOptions& options) const = 0;
};
