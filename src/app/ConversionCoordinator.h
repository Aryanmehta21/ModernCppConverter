#pragma once

#include "backend/IBackendClient.h"
#include "converter/IConverterEngine.h"

#include <memory>

struct CoordinatedConversionResult
{
    ConversionResult result;
    ConversionMode effectiveMode = ConversionMode::OfflineRuleBased;
    bool backendUnavailable = false;
    std::string warning;
};

class ConversionCoordinator
{
public:
    ConversionCoordinator(std::unique_ptr<IConverterEngine> localEngine,
                          std::unique_ptr<IBackendClient> backendClient);

    [[nodiscard]] CoordinatedConversionResult convert(const std::string& code,
                                                      const ModernizationOptions& options,
                                                      ConversionMode requestedMode) const;
    [[nodiscard]] bool backendAvailable() const;

private:
    std::unique_ptr<IConverterEngine> localEngine_;
    std::unique_ptr<IBackendClient> backendClient_;
};
