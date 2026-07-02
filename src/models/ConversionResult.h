#pragma once

#include "ConversionChange.h"
#include "ModernizationOptions.h"

#include <string>
#include <vector>

struct ConversionResult
{
    std::string modernCode;
    std::vector<ConversionChange> changes;
    std::string explanation;
    std::string conversionSource;
    std::string backendStatus;
    std::string aiProvider;
    std::string aiModel;
    bool fallbackUsed = false;
    std::string convertedAt;
    std::vector<std::string> diagnosticMessages;
    bool compileVerificationEnabled = false;
    bool compileVerificationPassed = false;
    bool compileVerificationAutoFixAttempted = false;
    DiagnosticVerbosity diagnosticVerbosity = DiagnosticVerbosity::Normal;
    bool debugRawDiagnosticsEnabled = true;
    std::string compilerUsed;
    std::string compilerOutput;
    std::string rewriteLevel;
};
