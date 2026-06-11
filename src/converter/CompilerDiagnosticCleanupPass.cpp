#include "converter/CompilerDiagnosticCleanupPass.h"

#include "converter/DependentUsageRewritePass.h"
#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
#include "converter/SmartPointerTypePropagationPass.h"
#include "converter/VectorParadigmRewritePass.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace
{
std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string ruleName,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

bool isStringRecord(const TypeChangeRecord& record)
{
    return record.newType == "std::string";
}

bool isSmartPointerRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::unique_ptr<") || record.newType.starts_with("std::shared_ptr<");
}

bool hasKnownDiagnosticForRecord(const std::string& loweredCompilerOutput, const TypeChangeRecord& record)
{
    const std::string loweredSymbol = lowercase(record.symbolName);
    if (!loweredSymbol.empty() && loweredCompilerOutput.find(loweredSymbol) != std::string::npos) {
        return true;
    }

    if (isVectorRecord(record)) {
        return loweredCompilerOutput.find("nullptr") != std::string::npos
            || loweredCompilerOutput.find("delete[]") != std::string::npos
            || loweredCompilerOutput.find("delete []") != std::string::npos
            || loweredCompilerOutput.find("undeclared") != std::string::npos
            || loweredCompilerOutput.find("not declared") != std::string::npos
            || loweredCompilerOutput.find("std::vector") != std::string::npos
            || loweredCompilerOutput.find("no match for") != std::string::npos;
    }

    if (isStringRecord(record)) {
        return loweredCompilerOutput.find("strncpy") != std::string::npos
            || loweredCompilerOutput.find("strcpy") != std::string::npos
            || loweredCompilerOutput.find("strcmp") != std::string::npos
            || loweredCompilerOutput.find("strlen") != std::string::npos
            || loweredCompilerOutput.find("std::string") != std::string::npos
            || loweredCompilerOutput.find("basic_string") != std::string::npos
            || loweredCompilerOutput.find("char *") != std::string::npos
            || loweredCompilerOutput.find("char*") != std::string::npos;
    }

    if (isSmartPointerRecord(record)) {
        return loweredCompilerOutput.find("delete") != std::string::npos
            || loweredCompilerOutput.find("unique_ptr") != std::string::npos
            || loweredCompilerOutput.find("shared_ptr") != std::string::npos;
    }

    return false;
}

bool hasKnownDiagnostic(const std::string& compilerOutput, const TransformationContext& context)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    if (loweredCompilerOutput.empty()) {
        return false;
    }

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (hasKnownDiagnosticForRecord(loweredCompilerOutput, record)) {
            return true;
        }
    }

    return false;
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
} // namespace

std::string CompilerDiagnosticCleanupPass::run(const std::string& code,
                                               const TransformationContext& context,
                                               const std::string& compilerOutput,
                                               std::vector<ConversionChange>& changes) const
{
    const bool enumOutputDiagnostic = hasScopedEnumOutputDiagnostic(compilerOutput);
    if ((!enumOutputDiagnostic && context.empty()) || (!enumOutputDiagnostic && !hasKnownDiagnostic(compilerOutput, context))) {
        return code;
    }

    std::string updated = code;
    if (!context.empty()) {
        const DependentUsageRewritePass dependentUsageRewritePass;
        updated = dependentUsageRewritePass.rewrite(updated, context, changes);
    }
    const OrphanedGrowthSymbolCleanupPass orphanedGrowthSymbolCleanupPass;
    updated = orphanedGrowthSymbolCleanupPass.rewrite(updated, context, compilerOutput, changes);
    const OrphanedTempBufferLoopCleanupPass orphanedTempBufferLoopCleanupPass;
    updated = orphanedTempBufferLoopCleanupPass.rewrite(updated, context, compilerOutput, changes);
    const VectorParadigmRewritePass vectorParadigmRewritePass;
    updated = vectorParadigmRewritePass.rewrite(updated, context, changes);
    ModernizationOptions retryOptions;
    retryOptions.useSmartPointers = true;
    retryOptions.applySafeOwnershipModernization = true;
    retryOptions.useRangeBasedFor = true;
    retryOptions.useLambdas = true;
    const SmartPointerTypePropagationPass smartPointerTypePropagationPass;
    updated = smartPointerTypePropagationPass.rewrite(updated, retryOptions, context, changes);
    const OwnershipSanityScanner ownershipSanityScanner;
    updated = ownershipSanityScanner.rewrite(updated, context, changes);
    const ScopeLeakValidationPass scopeLeakValidationPass;
    updated = scopeLeakValidationPass.validate(updated, context, compilerOutput, changes);
    const SemanticConsistencyValidator semanticConsistencyValidator;
    updated = semanticConsistencyValidator.validateAndRepair(updated, retryOptions, context, compilerOutput, changes);
    const ScopedEnumCastValidationPass scopedEnumCastValidationPass;
    updated = scopedEnumCastValidationPass.validateAndNormalize(updated, changes);
    const ScopedEnumOutputPropagationPass scopedEnumOutputPropagationPass;
    updated = scopedEnumOutputPropagationPass.rewrite(updated, retryOptions, changes);
    const ScopedEnumOutputValidator scopedEnumOutputValidator;
    updated = scopedEnumOutputValidator.validateAndRepair(updated, retryOptions, changes);
    updated = orphanedGrowthSymbolCleanupPass.rewrite(updated, context, compilerOutput, changes);
    updated = orphanedTempBufferLoopCleanupPass.rewrite(updated, context, compilerOutput, changes);

    if (updated != code) {
        addAppliedChange(changes,
                         "Compiler diagnostic cleanup",
                         "syntax-only compiler diagnostics",
                         "known incompatible legacy usages cleaned",
                         "Compile verification reported a known fallout pattern from a type-changing modernization, so the pipeline ran a targeted dependency cleanup pass.");
    }

    return updated;
}
