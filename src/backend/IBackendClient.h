#pragma once

#include "models/ConversionMode.h"
#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <string>

struct BackendConversionResponse
{
    bool ok = false;
    ConversionResult result;
    std::string errorMessage;
};

class IBackendClient
{
public:
    virtual ~IBackendClient() = default;

    [[nodiscard]] virtual bool isAvailable() const = 0;
    [[nodiscard]] virtual BackendConversionResponse convert(const std::string& code,
                                                            const ModernizationOptions& options,
                                                            ConversionMode mode,
                                                            const ConversionResult* localResult) const = 0;
};
