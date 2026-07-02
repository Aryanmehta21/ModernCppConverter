#include "frontend/LightweightFrontend.h"

#include "parser/LightweightCppParser.h"

#include <sstream>
#include <utility>

namespace
{
FrontendEntityCounts entityCountsFor(const ParsedDocument& document)
{
    return FrontendEntityCounts{
        document.aggregates.size(),
        document.functions.size(),
        document.enums.size(),
        document.memberVariables.size() + document.globalVariables.size() + document.localVariables.size(),
    };
}

std::string frontendSummaryMessage(const ModernizationFrontendResult& result)
{
    std::ostringstream output;
    output << "FRONTEND used=" << result.frontendName
           << " experimental=" << (result.kind == ModernizationFrontendKind::ClangExperimental ? "true" : "false")
           << " parse=" << (result.parseSucceeded ? "success" : "fallback")
           << " classes=" << result.entityCounts.classes
           << " functions=" << result.entityCounts.functions
           << " enums=" << result.entityCounts.enums
           << " variables=" << result.entityCounts.variables;
    return output.str();
}

std::string clangExperimentStateMessage()
{
#if defined(MODERNCPP_ENABLE_CLANG_EXPERIMENTS)
    return "FRONTEND clang_experiment=enabled default=LightweightFrontend";
#else
    return "FRONTEND clang_experiment=disabled default=LightweightFrontend";
#endif
}
} // namespace

std::string LightweightFrontend::name() const
{
    return "LightweightFrontend";
}

ModernizationFrontendKind LightweightFrontend::kind() const
{
    return ModernizationFrontendKind::Lightweight;
}

bool LightweightFrontend::isExperimental() const
{
    return false;
}

ModernizationFrontendResult LightweightFrontend::analyze(const std::string& source) const
{
    ModernizationFrontendResult result;
    result.kind = kind();
    result.frontendName = name();
    result.document = LightweightCppParser{}.parse(source);
    result.entityCounts = entityCountsFor(result.document);
    result.parseSucceeded = result.document.parseSucceeded;
    result.diagnostics = result.document.warnings;
    result.diagnostics.insert(result.diagnostics.begin(), frontendSummaryMessage(result));
    result.diagnostics.insert(result.diagnostics.begin(), clangExperimentStateMessage());
    return result;
}
