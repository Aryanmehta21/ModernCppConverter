#pragma once

#include "converter/IConverterEngine.h"
#include "models/ConversionResult.h"
#include "models/ModernizationOptions.h"

#include <memory>
#include <string>

class ModernCppConverter
{
public:
    ModernCppConverter();
    explicit ModernCppConverter(std::unique_ptr<IConverterEngine> engine);

    [[nodiscard]] ConversionResult convert(const std::string& legacyCode) const;
    [[nodiscard]] ConversionResult convert(const std::string& legacyCode,
                                           const ModernizationOptions& options) const;

private:
    std::unique_ptr<IConverterEngine> engine_;
};
