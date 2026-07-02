#pragma once

#include "frontend/IModernizationFrontend.h"

class ClangExperimentalFrontend final : public IModernizationFrontend
{
public:
    ClangExperimentalFrontend();
    explicit ClangExperimentalFrontend(ClangParseConfig config);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] ModernizationFrontendKind kind() const override;
    [[nodiscard]] bool isExperimental() const override;
    [[nodiscard]] ModernizationFrontendResult analyze(const std::string& source) const override;
    [[nodiscard]] ModernizationFrontendResult analyzeInProcess(const std::string& source) const;

private:
    ClangParseConfig config_;
};
