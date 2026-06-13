#include "converter/OfflineModernizationPipeline.h"

#include "converter/AggressiveRewriteEngine.h"
#include "converter/AlgorithmPolishPass.h"
#include "converter/AlgorithmModernizationPass.h"
#include "converter/AutoPtrRemovalPass.h"
#include "converter/ClassResourceAnalyzerPass.h"
#include "converter/ClassStringBufferModernizationPass.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/ConcurrencyRaiiModernizationPass.h"
#include "converter/ContainerModernizationCleanupPass.h"
#include "converter/ContainerPolishPass.h"
#include "converter/CrossFunctionTypePropagationPass.h"
#include "converter/CrossScopeTypePropagationPass.h"
#include "converter/EnumToStringCandidatePass.h"
#include "converter/FileIoModernizationPass.h"
#include "converter/FunctionPointerModernizationPass.h"
#include "converter/FunctionalModernizationPass.h"
#include "converter/FunctorToLambdaPass.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/IteratorModernizationPass.h"
#include "converter/MallocFreeModernizationPass.h"
#include "converter/MemberApiCascadePass.h"
#include "converter/ModernizationPolishPass.h"
#include "converter/ModernizationPolishValidator.h"
#include "converter/NsdmiScopeSafetyPass.h"
#include "converter/OwnershipGraphModernizationPass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/OverrideEnforcementPass.h"
#include "converter/PassByValueToConstRefPass.h"
#include "converter/PolymorphicContractPolishPass.h"
#include "converter/PolymorphicSafetyPass.h"
#include "converter/PrintfModernizationPass.h"
#include "converter/QualityModernizationPass.h"
#include "converter/ReturnTypePropagationPass.h"
#include "converter/RuleOfZeroPass.h"
#include "converter/RuleOfZeroPolishPass.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SemanticModernizationValidator.h"
#include "converter/SemanticTypeValidationPass.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
#include "converter/SmartPointerTypePropagationPass.h"
#include "converter/StructuralModernizationEngine.h"
#include "converter/StringViewPolishPass.h"
#include "converter/StructuredBindingPass.h"
#include "converter/TransformationContext.h"
#include "converter/VectorParadigmRewritePass.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <regex>
#include <sstream>
#include <unordered_map>

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

std::uint64_t stableHash(const std::string& text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::size_t countTextNodes(const std::string& code)
{
    std::stringstream input(code);
    std::string line;
    std::size_t count = 0;
    while (std::getline(input, line)) {
        if (std::any_of(line.begin(), line.end(), [](unsigned char character) {
                return std::isspace(character) == 0;
            })) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> linesOf(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::size_t countChangedLines(const std::string& before, const std::string& after)
{
    const std::vector<std::string> beforeLines = linesOf(before);
    const std::vector<std::string> afterLines = linesOf(after);
    const std::size_t maxSize = std::max(beforeLines.size(), afterLines.size());
    std::size_t changed = 0;
    for (std::size_t index = 0; index < maxSize; ++index) {
        const std::string beforeLine = index < beforeLines.size() ? beforeLines[index] : std::string{};
        const std::string afterLine = index < afterLines.size() ? afterLines[index] : std::string{};
        if (beforeLine != afterLine) {
            ++changed;
        }
    }
    return changed;
}

std::size_t countAppliedChangesSince(const std::vector<ConversionChange>& changes, const std::size_t start)
{
    return static_cast<std::size_t>(std::count_if(changes.begin() + static_cast<std::ptrdiff_t>(start),
                                                 changes.end(),
                                                 [](const ConversionChange& change) {
                                                     return change.applied;
                                                 }));
}

void removeNoOpAppliedChanges(std::vector<ConversionChange>& changes, const std::size_t start)
{
    changes.erase(std::remove_if(changes.begin() + static_cast<std::ptrdiff_t>(start),
                                 changes.end(),
                                 [](const ConversionChange& change) {
                                     return change.applied;
                                 }),
                  changes.end());
}

std::string passTraceMessage(const std::string& passName,
                             const int iteration,
                             const std::uint64_t hashBefore,
                             const std::uint64_t hashAfter,
                             const std::size_t nodesVisited,
                             const std::size_t nodesModified,
                             const std::size_t rewriteOperations,
                             const long long elapsedMilliseconds,
                             const std::string& status)
{
    std::ostringstream output;
    output << passName
           << " iteration=" << iteration
           << " hash_before=" << hashBefore
           << " hash_after=" << hashAfter
           << " visited=" << nodesVisited
           << " modified=" << nodesModified
           << " rewrites=" << rewriteOperations
           << " time_ms=" << elapsedMilliseconds
           << " status=" << status;
    return output.str();
}

std::string startPassTraceMessage(const std::string& passName,
                                  const int iteration,
                                  const std::uint64_t hashBefore,
                                  const std::size_t nodesVisited)
{
    std::ostringstream output;
    output << "START PASS " << passName
           << " iteration=" << iteration
           << " hash_before=" << hashBefore
           << " visited=" << nodesVisited;
    return output.str();
}

std::string endPassTraceMessage(const std::string& passName,
                                const int iteration,
                                const std::uint64_t hashBefore,
                                const std::uint64_t hashAfter,
                                const std::size_t nodesVisited,
                                const std::size_t nodesModified,
                                const std::size_t rewriteOperations,
                                const long long elapsedMilliseconds,
                                const std::string& status)
{
    return "END PASS " + passTraceMessage(passName,
                                          iteration,
                                          hashBefore,
                                          hashAfter,
                                          nodesVisited,
                                          nodesModified,
                                          rewriteOperations,
                                          elapsedMilliseconds,
                                          status);
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

bool hasNsdmiScopeDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("use of undeclared identifier") != std::string::npos
        || loweredCompilerOutput.find("not declared in this scope") != std::string::npos
        || loweredCompilerOutput.find("was not declared") != std::string::npos
        || loweredCompilerOutput.find("invalid use of non-static data member") != std::string::npos;
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
    std::unordered_map<std::string, int> passIterations;
    constexpr int maxModernizationIterations = 20;

    auto runTracedPass = [&](const std::string& passName, auto&& transform) {
        const int iteration = ++passIterations[passName];
        const std::string before = result.modernCode;
        const std::uint64_t hashBefore = stableHash(before);
        const std::size_t nodesVisited = countTextNodes(before);
        const std::size_t changeCountBefore = changes.size();
        result.diagnosticMessages.push_back(startPassTraceMessage(passName, iteration, hashBefore, nodesVisited));

        if (iteration > maxModernizationIterations) {
            result.diagnosticMessages.push_back(endPassTraceMessage(passName,
                                                                    iteration,
                                                                    hashBefore,
                                                                    hashBefore,
                                                                    nodesVisited,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    "iteration-limit-exceeded; returned-last-stable-version"));
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        std::string candidate = transform(before);
        const auto finished = std::chrono::steady_clock::now();
        const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

        const std::uint64_t hashAfter = stableHash(candidate);
        if (candidate == before) {
            removeNoOpAppliedChanges(changes, changeCountBefore);
        }

        const std::size_t rewriteOperations = countAppliedChangesSince(changes, changeCountBefore)
            + ((candidate != before && countAppliedChangesSince(changes, changeCountBefore) == 0U) ? 1U : 0U);
        const std::size_t nodesModified = candidate == before ? 0U : countChangedLines(before, candidate);
        const std::string status = candidate == before ? "converged-no-change" : "changed";
        result.diagnosticMessages.push_back(endPassTraceMessage(passName,
                                                                iteration,
                                                                hashBefore,
                                                                hashAfter,
                                                                nodesVisited,
                                                                nodesModified,
                                                                rewriteOperations,
                                                                elapsedMilliseconds,
                                                                status));
        result.modernCode = std::move(candidate);
    };

    if (shouldRunStructuralPass(options)) {
        runTracedPass("AutoPtrRemovalPass", [&](const std::string& input) {
            const AutoPtrRemovalPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("ClassResourceAnalyzerPass", [&](const std::string& input) {
            const ClassResourceAnalyzerPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("OwnershipGraphModernizationPass", [&](const std::string& input) {
            const OwnershipGraphModernizationPass pass;
            return pass.modernize(input, options, transformationContext, changes);
        });
        runTracedPass("MallocFreeModernizationPass", [&](const std::string& input) {
            const MallocFreeModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("ClassStringBufferModernizationPass", [&](const std::string& input) {
            const ClassStringBufferModernizationPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("CrossScopeTypePropagationPass", [&](const std::string& input) {
            const CrossScopeTypePropagationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
    }

    if (aggressiveAiLike) {
        runTracedPass("AggressiveRewriteEngine", [&](const std::string& input) {
            AggressiveRewriteEngine pass;
            return pass.rewrite(input, options, changes);
        });
        result.rewriteLevel = "Offline Aggressive AI-like Rewrite";
    } else if (shouldRunOwnershipConsistencyPass(options)) {
        runTracedPass("AggressiveRewriteEngine::rewriteOwnershipModernizations", [&](const std::string& input) {
            AggressiveRewriteEngine pass;
            std::string output = pass.rewriteOwnershipModernizations(input, changes);
            if (output != input) {
                output = pass.ensureModernIncludes(output, options, &changes);
            }
            return output;
        });
    }

    if (!transformationContext.empty()) {
        runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
            const ImpactCascadingCleanupPass pass;
            return pass.run(input, transformationContext, changes);
        });
        runTracedPass("ContainerModernizationCleanupPass", [&](const std::string& input) {
            const ContainerModernizationCleanupPass pass;
            return pass.rewrite(input, transformationContext, changes);
        });
        runTracedPass("VectorParadigmRewritePass", [&](const std::string& input) {
            const VectorParadigmRewritePass pass;
            return pass.rewrite(input, transformationContext, changes);
        });
        runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
            const ImpactCascadingCleanupPass pass;
            return pass.run(input, transformationContext, changes);
        });
        runTracedPass("SmartPointerTypePropagationPass", [&](const std::string& input) {
            const SmartPointerTypePropagationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("CrossScopeTypePropagationPass", [&](const std::string& input) {
            const CrossScopeTypePropagationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("CrossFunctionTypePropagationPass", [&](const std::string& input) {
            const CrossFunctionTypePropagationPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("PolymorphicSafetyPass", [&](const std::string& input) {
            const PolymorphicSafetyPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("MemberApiCascadePass", [&](const std::string& input) {
            const MemberApiCascadePass pass;
            return pass.rewrite(input, transformationContext, changes);
        });
        runTracedPass("ContainerModernizationCleanupPass", [&](const std::string& input) {
            const ContainerModernizationCleanupPass pass;
            return pass.rewrite(input, transformationContext, changes);
        });
        runTracedPass("ReturnTypePropagationPass", [&](const std::string& input) {
            const ReturnTypePropagationPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
            const ImpactCascadingCleanupPass pass;
            return pass.run(input, transformationContext, changes);
        });
        runTracedPass("OwnershipSanityScanner", [&](const std::string& input) {
            const OwnershipSanityScanner pass;
            return pass.rewrite(input, transformationContext, changes);
        });
        runTracedPass("ScopeLeakValidationPass", [&](const std::string& input) {
            const ScopeLeakValidationPass pass;
            return pass.validate(input, transformationContext, {}, changes);
        });
        runTracedPass("RuleOfZeroPass", [&](const std::string& input) {
            const RuleOfZeroPass pass;
            return pass.rewrite(input, transformationContext, changes);
        });
    }

    if (shouldRunStructuralPass(options)) {
        runTracedPass("SmartPointerTypePropagationPass", [&](const std::string& input) {
            const SmartPointerTypePropagationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("CrossScopeTypePropagationPass", [&](const std::string& input) {
            const CrossScopeTypePropagationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("CrossFunctionTypePropagationPass", [&](const std::string& input) {
            const CrossFunctionTypePropagationPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("PolymorphicSafetyPass", [&](const std::string& input) {
            const PolymorphicSafetyPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("FileIoModernizationPass", [&](const std::string& input) {
            const FileIoModernizationPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("PrintfModernizationPass", [&](const std::string& input) {
            const PrintfModernizationPass pass;
            return pass.rewrite(input, options, changes);
        });
        runTracedPass("FunctionPointerModernizationPass", [&](const std::string& input) {
            const FunctionPointerModernizationPass pass;
            return pass.rewrite(input, options, changes);
        });
        runTracedPass("FunctorToLambdaPass", [&](const std::string& input) {
            const FunctorToLambdaPass pass;
            return pass.rewrite(input, changes);
        });
        runTracedPass("FunctionalModernizationPass", [&](const std::string& input) {
            const FunctionalModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("AlgorithmModernizationPass", [&](const std::string& input) {
            const AlgorithmModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("IteratorModernizationPass", [&](const std::string& input) {
            const IteratorModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        runTracedPass("ScopedEnumCastValidationPass", [&](const std::string& input) {
            const ScopedEnumCastValidationPass pass;
            return pass.validateAndNormalize(input, changes);
        });
        runTracedPass("ScopedEnumOutputPropagationPass", [&](const std::string& input) {
            const ScopedEnumOutputPropagationPass pass;
            return pass.rewrite(input, options, changes);
        });
        runTracedPass("ScopedEnumOutputValidator", [&](const std::string& input) {
            const ScopedEnumOutputValidator pass;
            return pass.validateAndRepair(input, options, changes);
        });
        runTracedPass("EnumToStringCandidatePass", [&](const std::string& input) {
            const EnumToStringCandidatePass pass;
            pass.suggest(input, changes);
            return input;
        });
        runTracedPass("SemanticConsistencyValidator", [&](const std::string& input) {
            const SemanticConsistencyValidator pass;
            return pass.validateAndRepair(input, options, transformationContext, {}, changes);
        });
        runTracedPass("SemanticModernizationValidator", [&](const std::string& input) {
            const SemanticModernizationValidator pass;
            return pass.validateAndRepair(input, options, transformationContext, {}, changes);
        });
        runTracedPass("ReturnTypePropagationPass", [&](const std::string& input) {
            const ReturnTypePropagationPass pass;
            return pass.rewrite(input, changes);
        });

        const ModernizationPolishValidator polishValidator;
        auto applyPolish = [&](const std::string& passName, auto&& transform) {
            runTracedPass(passName, [&](const std::string& input) {
                const std::size_t changeCountBefore = changes.size();
                const std::string candidate = transform(input);
                if (candidate == input) {
                    return input;
                }
                std::string reason;
                if (polishValidator.isValid(candidate, reason)) {
                    return candidate;
                }
                changes.resize(changeCountBefore);
                changes.push_back(ConversionChange{
                    passName,
                    input,
                    {},
                    "Skipped polish candidate because validation failed: " + reason,
                    false,
                    false,
                });
                return input;
            });
        };

        applyPolish("Polymorphic contract polish", [&](const std::string& input) {
            const PolymorphicContractPolishPass pass;
            return pass.rewrite(input, changes);
        });
        applyPolish("Override enforcement", [&](const std::string& input) {
            const OverrideEnforcementPass pass;
            return pass.rewrite(input, changes);
        });
        applyPolish("Iterator modernization polish", [&](const std::string& input) {
            const IteratorModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        applyPolish("Structured binding polish", [&](const std::string& input) {
            const StructuredBindingPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        applyPolish("Algorithm polish", [&](const std::string& input) {
            const AlgorithmPolishPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
        });
        applyPolish("Container polish", [&](const std::string& input) {
            const ContainerPolishPass pass;
            return pass.rewrite(input, changes);
        });
        applyPolish("Concurrency RAII modernization", [&](const std::string& input) {
            const ConcurrencyRaiiModernizationPass pass;
            return pass.rewrite(input, options, changes);
        });
        applyPolish("Rule of Zero polish", [&](const std::string& input) {
            const RuleOfZeroPolishPass pass;
            return pass.rewrite(input, changes);
        });
        applyPolish("String view polish", [&](const std::string& input) {
            const StringViewPolishPass pass;
            return pass.rewrite(input, options, changes);
        });
        applyPolish("Quality modernization sprint", [&](const std::string& input) {
            const QualityModernizationPass pass;
            return pass.rewrite(input, options, changes);
        });
        applyPolish("NSDMI scope safety", [&](const std::string& input) {
            const NsdmiScopeSafetyPass pass;
            return pass.validateAndRepair(input, changes);
        });
        applyPolish("Pass-by-value to const reference", [&](const std::string& input) {
            const PassByValueToConstRefPass pass;
            return pass.rewrite(input, changes);
        });
        applyPolish("Semantic type validation", [&](const std::string& input) {
            const SemanticTypeValidationPass pass;
            return pass.validateAndRepair(input, options, changes);
        });
    }

    if (options.compileVerificationEnabled || aggressiveAiLike || shouldRunStructuralPass(options)) {
        CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
        result.compileVerificationEnabled = verification.verificationEnabled;
        result.compileVerificationPassed = verification.passed;
        result.compilerUsed = verification.compilerUsed;
        result.compilerOutput = verification.output;

        if (verification.compilerFound
            && !verification.passed
            && (!transformationContext.empty()
                || hasScopedEnumOutputDiagnostic(verification.output)
                || hasNsdmiScopeDiagnostic(verification.output))) {
            const std::string beforeCleanup = result.modernCode;
            runTracedPass("CompilerDiagnosticCleanupPass", [&](const std::string& input) {
                const CompilerDiagnosticCleanupPass pass;
                return pass.run(input, transformationContext, verification.output, changes);
            });
            runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
                const ImpactCascadingCleanupPass pass;
                return pass.run(input, transformationContext, changes);
            });
            runTracedPass("ContainerModernizationCleanupPass", [&](const std::string& input) {
                const ContainerModernizationCleanupPass pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("VectorParadigmRewritePass", [&](const std::string& input) {
                const VectorParadigmRewritePass pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
                const ImpactCascadingCleanupPass pass;
                return pass.run(input, transformationContext, changes);
            });
            runTracedPass("SmartPointerTypePropagationPass", [&](const std::string& input) {
                const SmartPointerTypePropagationPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("CrossScopeTypePropagationPass", [&](const std::string& input) {
                const CrossScopeTypePropagationPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("PolymorphicSafetyPass", [&](const std::string& input) {
                const PolymorphicSafetyPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("MemberApiCascadePass", [&](const std::string& input) {
                const MemberApiCascadePass pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("ContainerModernizationCleanupPass", [&](const std::string& input) {
                const ContainerModernizationCleanupPass pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("ReturnTypePropagationPass", [&](const std::string& input) {
                const ReturnTypePropagationPass pass;
                return pass.rewrite(input, changes);
            });
            runTracedPass("OwnershipSanityScanner", [&](const std::string& input) {
                const OwnershipSanityScanner pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("ScopeLeakValidationPass", [&](const std::string& input) {
                const ScopeLeakValidationPass pass;
                return pass.validate(input, transformationContext, verification.output, changes);
            });
            runTracedPass("NsdmiScopeSafetyPass", [&](const std::string& input) {
                const NsdmiScopeSafetyPass pass;
                return pass.validateAndRepair(input, changes);
            });
            runTracedPass("RuleOfZeroPass", [&](const std::string& input) {
                const RuleOfZeroPass pass;
                return pass.rewrite(input, transformationContext, changes);
            });
            runTracedPass("FileIoModernizationPass", [&](const std::string& input) {
                const FileIoModernizationPass pass;
                return pass.rewrite(input, changes);
            });
            runTracedPass("FunctorToLambdaPass", [&](const std::string& input) {
                const FunctorToLambdaPass pass;
                return pass.rewrite(input, changes);
            });
            runTracedPass("FunctionalModernizationPass", [&](const std::string& input) {
                const FunctionalModernizationPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("AlgorithmModernizationPass", [&](const std::string& input) {
                const AlgorithmModernizationPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("IteratorModernizationPass", [&](const std::string& input) {
                const IteratorModernizationPass pass;
                return pass.rewrite(input, options, transformationContext, changes);
            });
            runTracedPass("ScopedEnumCastValidationPass", [&](const std::string& input) {
                const ScopedEnumCastValidationPass pass;
                return pass.validateAndNormalize(input, changes);
            });
            runTracedPass("ScopedEnumOutputPropagationPass", [&](const std::string& input) {
                const ScopedEnumOutputPropagationPass pass;
                return pass.rewrite(input, options, changes);
            });
            runTracedPass("ScopedEnumOutputValidator", [&](const std::string& input) {
                const ScopedEnumOutputValidator pass;
                return pass.validateAndRepair(input, options, changes);
            });
            runTracedPass("ImpactCascadingCleanupPass", [&](const std::string& input) {
                const ImpactCascadingCleanupPass pass;
                return pass.run(input, transformationContext, changes);
            });
            runTracedPass("SemanticConsistencyValidator", [&](const std::string& input) {
                const SemanticConsistencyValidator pass;
                return pass.validateAndRepair(input, options, transformationContext, verification.output, changes);
            });
            runTracedPass("SemanticModernizationValidator", [&](const std::string& input) {
                const SemanticModernizationValidator pass;
                return pass.validateAndRepair(input, options, transformationContext, verification.output, changes);
            });
            runTracedPass("SemanticTypeValidationPass", [&](const std::string& input) {
                const SemanticTypeValidationPass pass;
                return pass.validateAndRepair(input, options, changes);
            });

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
