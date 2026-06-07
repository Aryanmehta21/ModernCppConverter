#include "repository/RepositoryVerificationService.h"

#include "converter/CompileVerifier.h"

#include <sstream>

void RepositoryVerificationService::verifyFile(FileModernizationResult& fileResult, const std::string& code) const
{
    const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(code);
    fileResult.compileVerificationEnabled = verification.verificationEnabled;
    fileResult.compileVerificationPassed = verification.passed;
    fileResult.compilerUsed = verification.compilerUsed;
    fileResult.compilerOutput = verification.output;
}

std::string RepositoryVerificationService::summarize(const RepositoryModernizationResult& result) const
{
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const FileModernizationResult& file : result.files) {
        if (!file.compileVerificationEnabled) {
            ++skipped;
        } else if (file.compileVerificationPassed) {
            ++passed;
        } else {
            ++failed;
        }
    }

    std::ostringstream output;
    output << "Syntax verification passed for " << passed << " file(s), failed or skipped for " << failed
           << " file(s), and was not run for " << skipped << " file(s). "
           << "Repository CMake build is skipped for safety; the tool does not run project scripts or binaries.";
    return output.str();
}
