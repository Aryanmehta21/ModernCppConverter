#include "converter/CompilerDiagnosticCleanupPass.h"

#include "converter/DependentUsageRewritePass.h"
#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/RangeForModernizationPass.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/ScopedEnumUsagePropagationPass.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SemanticTypeValidationPass.h"
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
            || loweredCompilerOutput.find("strcat") != std::string::npos
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

bool hasScopedEnumUsageDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("use of undeclared identifier") != std::string::npos
        || loweredCompilerOutput.find("was not declared in this scope") != std::string::npos
        || loweredCompilerOutput.find("not declared in this scope") != std::string::npos
        || loweredCompilerOutput.find("invalid case") != std::string::npos
        || loweredCompilerOutput.find("case label") != std::string::npos;
}

bool hasStringCapiDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("strcpy") != std::string::npos
        || loweredCompilerOutput.find("strncpy") != std::string::npos
        || loweredCompilerOutput.find("strcat") != std::string::npos
        || loweredCompilerOutput.find("strcmp") != std::string::npos
        || loweredCompilerOutput.find("strlen") != std::string::npos
        || loweredCompilerOutput.find("no matching function") != std::string::npos
        || loweredCompilerOutput.find("no viable conversion") != std::string::npos
        || loweredCompilerOutput.find("cannot convert") != std::string::npos
        || loweredCompilerOutput.find("no member named 'c_str'") != std::string::npos
        || loweredCompilerOutput.find("has no member named 'c_str'") != std::string::npos
        || loweredCompilerOutput.find("no member named c_str") != std::string::npos
        || loweredCompilerOutput.find("string_view") != std::string::npos
        || loweredCompilerOutput.find("basic_string") != std::string::npos
        || loweredCompilerOutput.find("std::string") != std::string::npos
        || loweredCompilerOutput.find("invalid array subscript") != std::string::npos
        || loweredCompilerOutput.find("subscripted value") != std::string::npos;
}

bool hasSmartPointerDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("unique_ptr") != std::string::npos
        || loweredCompilerOutput.find("shared_ptr") != std::string::npos
        || loweredCompilerOutput.find("std::unique_ptr") != std::string::npos
        || loweredCompilerOutput.find("std::shared_ptr") != std::string::npos
        || loweredCompilerOutput.find("deleted copy constructor") != std::string::npos
        || loweredCompilerOutput.find("call to deleted constructor") != std::string::npos
        || loweredCompilerOutput.find("use of deleted function") != std::string::npos
        || loweredCompilerOutput.find("cannot convert") != std::string::npos
        || loweredCompilerOutput.find("no viable conversion") != std::string::npos
        || loweredCompilerOutput.find("no known conversion") != std::string::npos
        || loweredCompilerOutput.find("could not convert") != std::string::npos;
}

bool hasPairStreamDiagnostic(const std::string& compilerOutput)
{
    const std::string loweredCompilerOutput = lowercase(compilerOutput);
    return loweredCompilerOutput.find("std::pair") != std::string::npos
        && (loweredCompilerOutput.find("operator<<") != std::string::npos
            || loweredCompilerOutput.find("invalid operands") != std::string::npos
            || loweredCompilerOutput.find("no match") != std::string::npos
            || loweredCompilerOutput.find("no viable") != std::string::npos);
}
} // namespace

std::string CompilerDiagnosticCleanupPass::run(const std::string& code,
                                               const TransformationContext& context,
                                               const std::string& compilerOutput,
                                               std::vector<ConversionChange>& changes) const
{
    const bool enumOutputDiagnostic = hasScopedEnumOutputDiagnostic(compilerOutput)
        || (code.find("enum class") != std::string::npos && hasScopedEnumUsageDiagnostic(compilerOutput));
    const bool stringCapiDiagnostic = hasStringCapiDiagnostic(compilerOutput);
    const bool smartPointerDiagnostic = hasSmartPointerDiagnostic(compilerOutput);
    const bool pairStreamDiagnostic = hasPairStreamDiagnostic(compilerOutput);
    if ((!enumOutputDiagnostic && !stringCapiDiagnostic && !smartPointerDiagnostic && !pairStreamDiagnostic && context.empty())
        || (!enumOutputDiagnostic && !stringCapiDiagnostic && !smartPointerDiagnostic && !pairStreamDiagnostic && !hasKnownDiagnostic(compilerOutput, context))) {
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
    const ScopedEnumUsagePropagationPass scopedEnumUsagePropagationPass;
    updated = scopedEnumUsagePropagationPass.rewrite(updated, changes);
    const ScopedEnumOutputPropagationPass scopedEnumOutputPropagationPass;
    updated = scopedEnumOutputPropagationPass.rewrite(updated, retryOptions, changes);
    const ScopedEnumOutputValidator scopedEnumOutputValidator;
    updated = scopedEnumOutputValidator.validateAndRepair(updated, retryOptions, changes);
    if (stringCapiDiagnostic) {
        const SemanticTypeValidationPass semanticTypeValidationPass;
        updated = semanticTypeValidationPass.validateAndRepair(updated, retryOptions, changes);
    }
    if (pairStreamDiagnostic) {
        retryOptions.useStructuredBindings = true;
        const RangeForModernizationPass rangeForModernizationPass;
        updated = rangeForModernizationPass.rewrite(updated, retryOptions, changes);
    }
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
