#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"
#include "frontend/IModernizationFrontend.h"

#include <string>
#include <vector>

class ClangSemanticValidationPass
{
public:
    [[nodiscard]] std::vector<std::string> validate(const std::string& originalSource,
                                                    const std::string& convertedSource,
                                                    const ModernizationOptions& options,
                                                    const std::vector<ConversionChange>& changes,
                                                    bool compileVerificationEnabled,
                                                    bool compileVerificationPassed) const;
    [[nodiscard]] std::vector<std::string> validateWithSelectedFrontend(const ModernizationFrontendResult& selectedOriginal,
                                                                        bool selectedFrontendIsClang,
                                                                        bool frontendFallbackUsed,
                                                                        const std::string& frontendFallbackReason,
                                                                        const ClangParseConfig& config,
                                                                        const std::string& convertedSource,
                                                                        const ModernizationOptions& options,
                                                                        const std::vector<ConversionChange>& changes,
                                                                        bool compileVerificationEnabled,
                                                                        bool compileVerificationPassed) const;
};
