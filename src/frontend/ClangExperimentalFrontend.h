#pragma once

#include "frontend/IModernizationFrontend.h"

class ClangExperimentalFrontend final : public IModernizationFrontend
{
public:
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] ModernizationFrontendKind kind() const override;
    [[nodiscard]] bool isExperimental() const override;
    [[nodiscard]] ModernizationFrontendResult analyze(const std::string& source) const override;
};
