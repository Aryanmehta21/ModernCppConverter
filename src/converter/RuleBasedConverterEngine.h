#pragma once

#include "converter/IConverterEngine.h"
#include "converter/IConversionRule.h"
#include "converter/IExplanationGenerator.h"

#include <memory>
#include <vector>

class RuleBasedConverterEngine final : public IConverterEngine
{
public:
    RuleBasedConverterEngine();
    RuleBasedConverterEngine(std::vector<std::unique_ptr<IConversionRule>> rules,
                             std::unique_ptr<IExplanationGenerator> explanationGenerator);

    [[nodiscard]] ConversionResult convert(const std::string& legacyCode) const override;
    [[nodiscard]] ConversionResult convert(const std::string& legacyCode,
                                           const ModernizationOptions& options) const override;

private:
    std::vector<std::unique_ptr<IConversionRule>> rules_;
    std::unique_ptr<IExplanationGenerator> explanationGenerator_;
};
