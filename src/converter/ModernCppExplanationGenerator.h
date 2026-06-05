#pragma once

#include "converter/IExplanationGenerator.h"

class ModernCppExplanationGenerator final : public IExplanationGenerator
{
public:
    [[nodiscard]] std::string generate(const std::string& modernCode,
                                       const std::vector<ConversionChange>& changes,
                                       const ModernizationOptions& options) const override;
};
