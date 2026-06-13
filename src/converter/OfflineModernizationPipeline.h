#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

struct OfflineModernizationPipelineResult
{
    std::string modernCode;
    bool compileVerificationEnabled = false;
    bool compileVerificationPassed = false;
    bool compileVerificationAutoFixAttempted = false;
    std::string compilerUsed;
    std::string compilerOutput;
    std::string rewriteLevel;
    std::vector<std::string> diagnosticMessages;
};

class OfflineModernizationPipeline
{
public:
    [[nodiscard]] OfflineModernizationPipelineResult runAfterSafeRules(const std::string& normalizedCode,
                                                                       const ModernizationOptions& options,
                                                                       std::vector<ConversionChange>& changes) const;
};
