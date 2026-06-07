#pragma once

#include "models/ModernizationOptions.h"

#include <string>

struct CompileVerificationResult
{
    bool verificationEnabled = false;
    bool compilerFound = false;
    bool passed = false;
    std::string compilerUsed;
    std::string output;
};

class CompileVerifier
{
public:
    [[nodiscard]] static CompileVerificationResult verifySyntaxOnly(const std::string& code);
    [[nodiscard]] static CompileVerificationResult verifySyntaxOnly(const std::string& code, CppStandard standard);
};
