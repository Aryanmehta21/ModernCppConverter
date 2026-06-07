#include "converter/OfflineModernizationPipeline.h"

#include "converter/AggressiveRewriteEngine.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/StructuralModernizationEngine.h"
#include "converter/TransformationContext.h"
#include "converter/VectorParadigmRewritePass.h"

#include <regex>
#include <sstream>

namespace
{
bool isAggressiveAiLike(const ModernizationOptions& options)
{
    return options.offlineRewriteStyle == OfflineRewriteStyle::AggressiveAiLikeRewrite
        || options.offlineModernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
}

bool shouldRunOwnershipConsistencyPass(const ModernizationOptions& options)
{
    return options.offlineModernizationLevel != OfflineModernizationLevel::Conservative
        && (options.applySafeOwnershipModernization || (options.useStringView && options.applyStringViewWhenSafe));
}

bool shouldRunStructuralPass(const ModernizationOptions& options)
{
    return options.offlineModernizationLevel != OfflineModernizationLevel::Conservative;
}

std::string ensureInclude(std::string code, const std::string& includeLine)
{
    if (code.find(includeLine) != std::string::npos) {
        return code;
    }

    static const std::regex includePattern(R"(^#include\s+<[^>]+>\s*$)");
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool inserted = false;
    bool sawInclude = false;
    bool firstLine = true;

    while (std::getline(input, line)) {
        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;

        if (!inserted && sawInclude && !std::regex_match(line, includePattern)) {
            output << includeLine << '\n';
            inserted = true;
        }

        output << line;
        if (std::regex_match(line, includePattern)) {
            sawInclude = true;
        }
    }

    if (!inserted) {
        if (sawInclude) {
            output << '\n' << includeLine;
        } else {
            output.str({});
            output.clear();
            output << includeLine << '\n' << code;
        }
    }
    return output.str();
}
} // namespace

OfflineModernizationPipelineResult OfflineModernizationPipeline::runAfterSafeRules(const std::string& normalizedCode,
                                                                                   const ModernizationOptions& options,
                                                                                   std::vector<ConversionChange>& changes) const
{
    OfflineModernizationPipelineResult result;
    result.modernCode = normalizedCode;

    const bool aggressiveAiLike = isAggressiveAiLike(options);
    TransformationContext transformationContext;

    if (shouldRunStructuralPass(options)) {
        const StructuralModernizationEngine structuralEngine;
        result.modernCode = structuralEngine.modernize(result.modernCode, options, changes, transformationContext);
    }

    if (aggressiveAiLike) {
        AggressiveRewriteEngine aggressiveRewriteEngine;
        result.modernCode = aggressiveRewriteEngine.rewrite(result.modernCode, options, changes);
        result.rewriteLevel = "Offline Aggressive AI-like Rewrite";
    } else if (shouldRunOwnershipConsistencyPass(options)) {
        AggressiveRewriteEngine ownershipRewriteEngine;
        const std::string beforeOwnershipPass = result.modernCode;
        result.modernCode = ownershipRewriteEngine.rewriteOwnershipModernizations(result.modernCode, changes);
        if (result.modernCode != beforeOwnershipPass) {
            result.modernCode = ownershipRewriteEngine.ensureModernIncludes(result.modernCode, options, &changes);
        }
    }

    if (!transformationContext.empty()) {
        const ImpactCascadingCleanupPass impactCascadingCleanupPass;
        result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
        const VectorParadigmRewritePass vectorParadigmRewritePass;
        result.modernCode = vectorParadigmRewritePass.rewrite(result.modernCode, transformationContext, changes);
        result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
    }

    if (options.compileVerificationEnabled || aggressiveAiLike) {
        CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
        result.compileVerificationEnabled = verification.verificationEnabled;
        result.compileVerificationPassed = verification.passed;
        result.compilerUsed = verification.compilerUsed;
        result.compilerOutput = verification.output;

        if (verification.compilerFound && !verification.passed && !transformationContext.empty()) {
            const std::string beforeCleanup = result.modernCode;
            const CompilerDiagnosticCleanupPass compilerDiagnosticCleanupPass;
            result.modernCode = compilerDiagnosticCleanupPass.run(result.modernCode, transformationContext, verification.output, changes);

            const ImpactCascadingCleanupPass impactCascadingCleanupPass;
            result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
            const VectorParadigmRewritePass vectorParadigmRewritePass;
            result.modernCode = vectorParadigmRewritePass.rewrite(result.modernCode, transformationContext, changes);
            result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);

            if (result.modernCode != beforeCleanup) {
                result.compileVerificationAutoFixAttempted = true;
                const CompileVerificationResult secondVerification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
                result.compileVerificationPassed = secondVerification.passed;
                result.compilerUsed = secondVerification.compilerUsed;
                result.compilerOutput = "Initial compile output:\n" + verification.output
                    + "\n\nAfter dependent cleanup retry:\n" + secondVerification.output;
                verification = secondVerification;
            }
        }

        if (aggressiveAiLike && verification.compilerFound && !verification.passed) {
            AggressiveRewriteEngine aggressiveRewriteEngine;
            const std::string beforeAutoFix = result.modernCode;
            result.modernCode = aggressiveRewriteEngine.ensureModernIncludes(result.modernCode, options, nullptr);
            if (result.modernCode.find("std::cout") != std::string::npos) {
                result.modernCode = ensureInclude(result.modernCode, "#include <iostream>");
            }

            result.compileVerificationAutoFixAttempted = true;
            const CompileVerificationResult secondVerification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
            result.compileVerificationPassed = secondVerification.passed;
            result.compilerUsed = secondVerification.compilerUsed;
            result.compilerOutput = "Initial compile output:\n" + verification.output
                + "\n\nAfter include auto-fix:\n" + secondVerification.output;

            if (result.modernCode != beforeAutoFix) {
                changes.push_back(ConversionChange{
                    "Aggressive include auto-fix",
                    "",
                    "Added missing standard library include(s).",
                    "Syntax verification failed, so the offline pipeline attempted one include-only auto-fix pass.",
                    true,
                    false,
                });
            }
        }
    }

    return result;
}
