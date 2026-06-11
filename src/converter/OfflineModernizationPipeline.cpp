#include "converter/OfflineModernizationPipeline.h"

#include "converter/AggressiveRewriteEngine.h"
#include "converter/AlgorithmModernizationPass.h"
#include "converter/AutoPtrRemovalPass.h"
#include "converter/ClassResourceAnalyzerPass.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/EnumToStringCandidatePass.h"
#include "converter/FileIoModernizationPass.h"
#include "converter/FunctorToLambdaPass.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/IteratorModernizationPass.h"
#include "converter/MemberApiCascadePass.h"
#include "converter/ModernizationPolishPass.h"
#include "converter/OwnershipGraphModernizationPass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/PolymorphicSafetyPass.h"
#include "converter/RuleOfZeroPass.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SemanticModernizationValidator.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
#include "converter/SmartPointerTypePropagationPass.h"
#include "converter/StructuralModernizationEngine.h"
#include "converter/TransformationContext.h"
#include "converter/VectorParadigmRewritePass.h"

#include <algorithm>
#include <cctype>
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

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool hasScopedEnumOutputDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("no viable overloaded 'operator<<'") != std::string::npos
        || loweredCompilerOutput.find("no match for 'operator<<'") != std::string::npos
        || loweredCompilerOutput.find("invalid operands to binary expression") != std::string::npos
        || loweredCompilerOutput.find("formatter") != std::string::npos
        || loweredCompilerOutput.find("std::format") != std::string::npos
        || loweredCompilerOutput.find("fmt::format") != std::string::npos
        || loweredCompilerOutput.find("cannot format") != std::string::npos;
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
        const AutoPtrRemovalPass autoPtrRemovalPass;
        result.modernCode = autoPtrRemovalPass.rewrite(result.modernCode, changes);
        const ClassResourceAnalyzerPass classResourceAnalyzerPass;
        result.modernCode = classResourceAnalyzerPass.rewrite(result.modernCode, options, transformationContext, changes);
        const OwnershipGraphModernizationPass ownershipGraphModernizationPass;
        result.modernCode = ownershipGraphModernizationPass.modernize(result.modernCode, options, transformationContext, changes);
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
        const SmartPointerTypePropagationPass smartPointerTypePropagationPass;
        result.modernCode = smartPointerTypePropagationPass.rewrite(result.modernCode, options, transformationContext, changes);
        const PolymorphicSafetyPass polymorphicSafetyPass;
        result.modernCode = polymorphicSafetyPass.rewrite(result.modernCode, options, transformationContext, changes);
        const MemberApiCascadePass memberApiCascadePass;
        result.modernCode = memberApiCascadePass.rewrite(result.modernCode, transformationContext, changes);
        result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
        const OwnershipSanityScanner ownershipSanityScanner;
        result.modernCode = ownershipSanityScanner.rewrite(result.modernCode, transformationContext, changes);
        const ScopeLeakValidationPass scopeLeakValidationPass;
        result.modernCode = scopeLeakValidationPass.validate(result.modernCode, transformationContext, {}, changes);
        const RuleOfZeroPass ruleOfZeroPass;
        result.modernCode = ruleOfZeroPass.rewrite(result.modernCode, transformationContext, changes);
    }

    if (shouldRunStructuralPass(options)) {
        const SmartPointerTypePropagationPass smartPointerTypePropagationPass;
        result.modernCode = smartPointerTypePropagationPass.rewrite(result.modernCode, options, transformationContext, changes);
        const PolymorphicSafetyPass polymorphicSafetyPass;
        result.modernCode = polymorphicSafetyPass.rewrite(result.modernCode, options, transformationContext, changes);
        const FileIoModernizationPass fileIoModernizationPass;
        result.modernCode = fileIoModernizationPass.rewrite(result.modernCode, changes);
        const FunctorToLambdaPass functorToLambdaPass;
        result.modernCode = functorToLambdaPass.rewrite(result.modernCode, changes);
        const AlgorithmModernizationPass algorithmModernizationPass;
        result.modernCode = algorithmModernizationPass.rewrite(result.modernCode, options, transformationContext, changes);
        const IteratorModernizationPass iteratorModernizationPass;
        result.modernCode = iteratorModernizationPass.rewrite(result.modernCode, options, transformationContext, changes);
        const ScopedEnumCastValidationPass scopedEnumCastValidationPass;
        result.modernCode = scopedEnumCastValidationPass.validateAndNormalize(result.modernCode, changes);
        const ScopedEnumOutputPropagationPass scopedEnumOutputPropagationPass;
        result.modernCode = scopedEnumOutputPropagationPass.rewrite(result.modernCode, options, changes);
        const ScopedEnumOutputValidator scopedEnumOutputValidator;
        result.modernCode = scopedEnumOutputValidator.validateAndRepair(result.modernCode, options, changes);
        const EnumToStringCandidatePass enumToStringCandidatePass;
        enumToStringCandidatePass.suggest(result.modernCode, changes);
        const SemanticConsistencyValidator semanticConsistencyValidator;
        result.modernCode = semanticConsistencyValidator.validateAndRepair(result.modernCode, options, transformationContext, {}, changes);
        const SemanticModernizationValidator semanticModernizationValidator;
        result.modernCode = semanticModernizationValidator.validateAndRepair(result.modernCode, options, transformationContext, {}, changes);
    }

    if (options.compileVerificationEnabled || aggressiveAiLike || shouldRunStructuralPass(options)) {
        CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
        result.compileVerificationEnabled = verification.verificationEnabled;
        result.compileVerificationPassed = verification.passed;
        result.compilerUsed = verification.compilerUsed;
        result.compilerOutput = verification.output;

        if (verification.compilerFound
            && !verification.passed
            && (!transformationContext.empty() || hasScopedEnumOutputDiagnostic(verification.output))) {
            const std::string beforeCleanup = result.modernCode;
            const CompilerDiagnosticCleanupPass compilerDiagnosticCleanupPass;
            result.modernCode = compilerDiagnosticCleanupPass.run(result.modernCode, transformationContext, verification.output, changes);

            const ImpactCascadingCleanupPass impactCascadingCleanupPass;
            result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
            const VectorParadigmRewritePass vectorParadigmRewritePass;
            result.modernCode = vectorParadigmRewritePass.rewrite(result.modernCode, transformationContext, changes);
            result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
            const SmartPointerTypePropagationPass smartPointerTypePropagationPass;
            result.modernCode = smartPointerTypePropagationPass.rewrite(result.modernCode, options, transformationContext, changes);
            const PolymorphicSafetyPass polymorphicSafetyPass;
            result.modernCode = polymorphicSafetyPass.rewrite(result.modernCode, options, transformationContext, changes);
            const MemberApiCascadePass memberApiCascadePass;
            result.modernCode = memberApiCascadePass.rewrite(result.modernCode, transformationContext, changes);
            const OwnershipSanityScanner ownershipSanityScanner;
            result.modernCode = ownershipSanityScanner.rewrite(result.modernCode, transformationContext, changes);
            const ScopeLeakValidationPass scopeLeakValidationPass;
            result.modernCode = scopeLeakValidationPass.validate(result.modernCode, transformationContext, verification.output, changes);
            const RuleOfZeroPass ruleOfZeroPass;
            result.modernCode = ruleOfZeroPass.rewrite(result.modernCode, transformationContext, changes);
            const FileIoModernizationPass fileIoModernizationPass;
            result.modernCode = fileIoModernizationPass.rewrite(result.modernCode, changes);
            const FunctorToLambdaPass functorToLambdaPass;
            result.modernCode = functorToLambdaPass.rewrite(result.modernCode, changes);
            const AlgorithmModernizationPass algorithmModernizationPass;
            result.modernCode = algorithmModernizationPass.rewrite(result.modernCode, options, transformationContext, changes);
            const IteratorModernizationPass iteratorModernizationPass;
            result.modernCode = iteratorModernizationPass.rewrite(result.modernCode, options, transformationContext, changes);
            const ScopedEnumCastValidationPass scopedEnumCastValidationPass;
            result.modernCode = scopedEnumCastValidationPass.validateAndNormalize(result.modernCode, changes);
            const ScopedEnumOutputPropagationPass scopedEnumOutputPropagationPass;
            result.modernCode = scopedEnumOutputPropagationPass.rewrite(result.modernCode, options, changes);
            const ScopedEnumOutputValidator scopedEnumOutputValidator;
            result.modernCode = scopedEnumOutputValidator.validateAndRepair(result.modernCode, options, changes);
            result.modernCode = impactCascadingCleanupPass.run(result.modernCode, transformationContext, changes);
            const SemanticConsistencyValidator semanticConsistencyValidator;
            result.modernCode = semanticConsistencyValidator.validateAndRepair(result.modernCode, options, transformationContext, verification.output, changes);
            const SemanticModernizationValidator semanticModernizationValidator;
            result.modernCode = semanticModernizationValidator.validateAndRepair(result.modernCode, options, transformationContext, verification.output, changes);

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
