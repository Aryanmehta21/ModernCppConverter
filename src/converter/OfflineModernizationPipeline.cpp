#include "converter/OfflineModernizationPipeline.h"

#include "converter/AggressiveRewriteEngine.h"
#include "converter/AlgorithmPolishPass.h"
#include "converter/AlgorithmModernizationPass.h"
#include "converter/AtomicCounterModernizationPass.h"
#include "converter/AutoPtrRemovalPass.h"
#include "converter/ClassResourceAnalyzerPass.h"
#include "converter/ClassStringBufferModernizationPass.h"
#include "converter/ClangSemanticValidationPass.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/ConcurrencyRaiiModernizationPass.h"
#include "converter/ConstPointerParameterModernizationPass.h"
#include "converter/ContainerModernizationCleanupPass.h"
#include "converter/ContainerPolishPass.h"
#include "converter/CrossFunctionTypePropagationPass.h"
#include "converter/CrossScopeTypePropagationPass.h"
#include "converter/EnumToStringCandidatePass.h"
#include "converter/FileIoModernizationPass.h"
#include "converter/FunctionPointerModernizationPass.h"
#include "converter/FrontendCodeRepresentation.h"
#include "converter/FunctionalModernizationPass.h"
#include "converter/FunctorToLambdaPass.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/IncludeCleanupPass.h"
#include "converter/IteratorModernizationPass.h"
#include "converter/MallocFreeModernizationPass.h"
#include "converter/MakeUniqueModernizationPass.h"
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
#include "converter/PostConversionFormatter.h"
#include "converter/PrintfModernizationPass.h"
#include "converter/PthreadThreadModernizationPass.h"
#include "converter/QualityModernizationPass.h"
#include "converter/ReturnTypePropagationPass.h"
#include "converter/RuleOfZeroPass.h"
#include "converter/RuleOfZeroPolishPass.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/ScopedEnumUsagePropagationPass.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SemanticModernizationValidator.h"
#include "converter/SemanticTypeValidationPass.h"
#include "converter/SemanticValidationAndRepairPass.h"
#include "converter/SleepModernizationPass.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
#include "converter/SmartPointerTypePropagationPass.h"
#include "converter/StructuralAnalyzers.h"
#include "converter/StructuralModernizationEngine.h"
#include "converter/StringViewPolishPass.h"
#include "converter/StructuredBindingPass.h"
#include "converter/TransformationContext.h"
#include "converter/VectorParadigmRewritePass.h"
#include "frontend/FrontendFactory.h"
#include "utils/AppVersion.h"
#include "utils/CrashBreadcrumb.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

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

std::string modernizationLevelName(const OfflineModernizationLevel level)
{
    switch (level) {
    case OfflineModernizationLevel::Conservative:
        return "Conservative";
    case OfflineModernizationLevel::Balanced:
        return "Balanced";
    case OfflineModernizationLevel::AggressiveSafe:
        return "Aggressive Safe";
    case OfflineModernizationLevel::AiStyleAggressiveRewrite:
        return "AI-Style Aggressive Rewrite";
    }
    return "Balanced";
}

std::string targetStandardName(const CppStandard standard)
{
    return standard == CppStandard::Cpp17 ? "C++17" : "C++20";
}

std::string frontendSelectionName(const ModernizationFrontendSelection selection)
{
    switch (selection) {
    case ModernizationFrontendSelection::Lightweight:
        return "Lightweight";
    case ModernizationFrontendSelection::ClangExperimental:
        return "ClangExperimental";
    case ModernizationFrontendSelection::Auto:
        return "Auto";
    }
    return "Lightweight";
}

bool diagnosticsContainText(const std::vector<std::string>& diagnostics, const std::string& needle)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&needle](const std::string& diagnostic) {
        return diagnostic.find(needle) != std::string::npos;
    });
}

FrontendEntityCounts entityCountsForDocument(const ParsedDocument& document)
{
    return FrontendEntityCounts{
        document.aggregates.size(),
        document.functions.size(),
        document.enums.size(),
        document.memberVariables.size() + document.globalVariables.size() + document.localVariables.size(),
    };
}

ClangParseConfig clangParseConfigForOptions(const ModernizationOptions& options)
{
    ClangParseConfig config;
    config.languageStandard = options.targetStandard == CppStandard::Cpp17 ? "c++17" : "c++20";
    config.virtualFileName = "input.cpp";
    config.compileArguments = {"-std=" + config.languageStandard, "-fsyntax-only", "-x", "c++"};
    return config;
}

std::string clangParseConfigSummary(const ClangParseConfig& config)
{
    std::ostringstream output;
    output << "virtual_file=" << config.virtualFileName
           << " standard=" << config.languageStandard
           << " args=";
    for (std::size_t index = 0; index < config.compileArguments.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << config.compileArguments[index];
    }
    output << " include_paths=";
    for (std::size_t index = 0; index < config.includePaths.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << config.includePaths[index];
    }
    output << " resource_dir=" << config.resourceDir
           << " system_root=" << config.systemRoot;
    return output.str();
}

struct FrontendSelectionAnalysis
{
    ModernizationFrontendSelection requested = ModernizationFrontendSelection::Lightweight;
    std::string selectedFrontendName = "LightweightFrontend";
    bool clangEnabled = false;
    bool clangAvailable = false;
    bool fallbackUsed = false;
    std::string fallbackReason;
    ModernizationFrontendResult result;
    std::shared_ptr<CodeRepresentation> representation;
    ClangParseConfig clangConfig;
    std::vector<std::string> debugDiagnostics;
    std::string summaryDiagnostic;

    [[nodiscard]] bool selectedClang() const
    {
        return result.kind == ModernizationFrontendKind::ClangExperimental
            && selectedFrontendName == "ClangExperimentalFrontend";
    }
};

void appendClangAttemptDiagnostics(FrontendSelectionAnalysis& analysis,
                                   const ModernizationFrontendResult& clangAttempt,
                                   const bool fallbackUsed,
                                   const std::string& fallbackReason)
{
    const bool parseFailed = diagnosticsContainText(clangAttempt.diagnostics, "clang_parse=failure");
    const bool parseSucceeded = clangAttempt.parseSucceeded && !parseFailed;
    std::string configSummary = clangParseConfigSummary(analysis.clangConfig);
    for (const std::string& diagnostic : clangAttempt.diagnostics) {
        constexpr std::string_view prefix = "FRONTEND clang_parse_config=\"";
        if (diagnostic.rfind(std::string(prefix), 0) == 0 && diagnostic.size() > prefix.size()) {
            configSummary = diagnostic.substr(prefix.size());
            if (!configSummary.empty() && configSummary.back() == '"') {
                configSummary.pop_back();
            }
            break;
        }
    }
    analysis.debugDiagnostics.push_back("CLANG ATTEMPT config=\""
                                        + configSummary
                                        + "\"");
    analysis.debugDiagnostics.push_back("CLANG ATTEMPT initialization=success");
    analysis.debugDiagnostics.push_back(std::string("CLANG ATTEMPT parse=")
                                        + (parseSucceeded ? "success" : "failure"));
    analysis.debugDiagnostics.push_back(std::string("CLANG ATTEMPT entity_extraction=")
                                        + (parseSucceeded ? "success" : "skipped"));
    analysis.debugDiagnostics.push_back(std::string("CLANG ATTEMPT overall=")
                                        + (parseSucceeded ? "success" : "failure"));
    analysis.debugDiagnostics.push_back(std::string("CLANG ATTEMPT fallback_used=")
                                        + (fallbackUsed ? "true" : "false")
                                        + " fallback_reason=\"" + fallbackReason + "\"");
    for (const std::string& diagnostic : clangAttempt.diagnostics) {
        if (diagnostic.rfind("CLANG DIAGNOSTIC", 0) == 0) {
            analysis.debugDiagnostics.push_back(diagnostic);
        }
    }
}

FrontendSelectionAnalysis analyzeFrontendForOptions(const std::string& source,
                                                    const ModernizationOptions& options,
                                                    const ModernizationFrontendSelection requested)
{
    CrashBreadcrumb::ScopedStage stage("frontend selection");
    const bool compiledWithClang = clangExperimentsEnabled();
    const ClangParseConfig clangConfig = clangParseConfigForOptions(options);
    const bool clangAvailable = options.enableClangFrontend
        && createClangExperimentalFrontend(clangConfig) != nullptr;
    FrontendSelectionAnalysis analysis;
    analysis.requested = requested;
    analysis.clangEnabled = compiledWithClang;
    analysis.clangAvailable = clangAvailable;
    analysis.clangConfig = clangConfig;

    auto assignLightweight = [&](ModernizationFrontendResult lightweightResult,
                                 bool fallback,
                                 std::string reason) {
        analysis.selectedFrontendName = "LightweightFrontend";
        analysis.fallbackUsed = fallback;
        analysis.fallbackReason = std::move(reason);
        lightweightResult.kind = ModernizationFrontendKind::Lightweight;
        lightweightResult.frontendName = "LightweightFrontend";
        lightweightResult.entityCounts = entityCountsForDocument(lightweightResult.document);
        analysis.result = std::move(lightweightResult);
    };

    auto analyzeLightweight = [&]() {
        std::unique_ptr<IModernizationFrontend> lightweightFrontend = createDefaultModernizationFrontend();
        return lightweightFrontend->analyze(source);
    };

    if (requested == ModernizationFrontendSelection::Lightweight) {
        assignLightweight(analyzeLightweight(), false, {});
    } else if (requested == ModernizationFrontendSelection::ClangExperimental) {
        if (!options.enableClangFrontend) {
            assignLightweight(analyzeLightweight(),
                              true,
                              "Internal Clang frontend flag disabled");
        } else if (clangAvailable) {
            std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend(clangConfig);
            CrashBreadcrumb::ScopedStage clangStage("Clang parse");
            ModernizationFrontendResult clangAttempt = clangFrontend->analyze(source);
            if (diagnosticsContainText(clangAttempt.diagnostics, "clang_parse=failure")) {
                appendClangAttemptDiagnostics(analysis,
                                              clangAttempt,
                                              true,
                                              "Clang parse failed; LightweightFrontend fallback used");
                assignLightweight(ModernizationFrontendResult{
                                      ModernizationFrontendKind::Lightweight,
                                      "LightweightFrontend",
                                      clangAttempt.document,
                                      entityCountsForDocument(clangAttempt.document),
                                      {},
                                      clangAttempt.document.parseSucceeded,
                                  },
                                  true,
                                  "Clang parse failed; LightweightFrontend fallback used");
            } else {
                appendClangAttemptDiagnostics(analysis, clangAttempt, false, {});
                analysis.selectedFrontendName = "ClangExperimentalFrontend";
                analysis.result = std::move(clangAttempt);
            }
        } else {
            assignLightweight(analyzeLightweight(),
                              true,
                              compiledWithClang ? "Clang frontend unavailable" : "Clang support not compiled");
        }
    } else {
        if (!options.enableClangFrontend) {
            assignLightweight(analyzeLightweight(),
                              true,
                              "Internal Clang frontend flag disabled");
        } else if (clangAvailable) {
            std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend(clangConfig);
            CrashBreadcrumb::ScopedStage clangStage("Clang parse");
            ModernizationFrontendResult clangAttempt = clangFrontend->analyze(source);
            if (diagnosticsContainText(clangAttempt.diagnostics, "clang_parse=failure")) {
                appendClangAttemptDiagnostics(analysis,
                                              clangAttempt,
                                              true,
                                              "Clang parse failed; LightweightFrontend fallback used");
                assignLightweight(ModernizationFrontendResult{
                                      ModernizationFrontendKind::Lightweight,
                                      "LightweightFrontend",
                                      clangAttempt.document,
                                      entityCountsForDocument(clangAttempt.document),
                                      {},
                                      clangAttempt.document.parseSucceeded,
                                  },
                                  true,
                                  "Clang parse failed; LightweightFrontend fallback used");
            } else {
                appendClangAttemptDiagnostics(analysis, clangAttempt, false, {});
                analysis.selectedFrontendName = "ClangExperimentalFrontend";
                analysis.result = std::move(clangAttempt);
            }
        } else {
            assignLightweight(analyzeLightweight(),
                              true,
                              compiledWithClang ? "Clang frontend unavailable" : "Clang support not compiled");
        }
    }

    if (analysis.result.frontendName.empty()) {
        assignLightweight(analyzeLightweight(), true, "Requested frontend could not be constructed");
    }

    analysis.representation = std::make_shared<FrontendCodeRepresentation>(source, analysis.result.document);

    const bool clangParseRequested = requested != ModernizationFrontendSelection::Lightweight
        && options.enableClangFrontend
        && compiledWithClang;
    const bool isolatedClangParse = diagnosticsContainText(analysis.result.diagnostics, "isolated_process=true");
    std::ostringstream output;
    output << "FRONTEND requested=" << frontendSelectionName(requested)
           << " selected=" << analysis.selectedFrontendName
           << " clang_enabled=" << (compiledWithClang ? "true" : "false")
           << " clang_available=" << (clangAvailable ? "true" : "false")
           << " clang_parse_enabled=" << (clangParseRequested ? "true" : "false")
           << " isolated_process=" << (isolatedClangParse ? "true" : "false")
           << " ast_rewrite_enabled=" << (options.enableClangAstRewrite ? "true" : "false")
           << " clang_validation_enabled=" << (options.enableClangValidation ? "true" : "false")
           << " shared_ast_reuse_enabled=" << (options.enableSharedAstReuse ? "true" : "false")
           << " fallback=" << (analysis.fallbackUsed ? "true" : "false")
           << " reason=\"" << analysis.fallbackReason << "\""
           << " parse=" << (analysis.result.parseSucceeded ? "success" : "failure")
           << " classes=" << analysis.result.entityCounts.classes
           << " functions=" << analysis.result.entityCounts.functions
           << " enums=" << analysis.result.entityCounts.enums
           << " variables=" << analysis.result.entityCounts.variables;
    analysis.summaryDiagnostic = output.str();
    return analysis;
}

bool containsAnyLowered(const std::string& loweredText, const std::initializer_list<std::string_view> needles)
{
    return std::any_of(needles.begin(), needles.end(), [&loweredText](const std::string_view needle) {
        return loweredText.find(needle) != std::string::npos;
    });
}

std::string lowercaseCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

struct RollbackDiagnostic
{
    bool isRollback = false;
    std::string category = "Semantic";
    std::string reason = "semantic validator failure";
    std::string affectedPass;
    std::string affectedEntity = "unavailable";
    std::string severity = "Warning";
};

struct SkippedRiskDiagnostic
{
    bool isRisk = false;
    std::string category = "Semantic";
    std::string reason = "transformation skipped for safety";
    std::string affectedPass;
    std::string affectedEntity = "unavailable";
    std::string severity = "Warning";
    std::string suggestedAction = "Review the preserved code manually before applying this modernization.";
    bool codeLeftUnchanged = true;
};

struct ClassBoundaryValidationResult
{
    bool valid = true;
    std::string reason;
    std::string entity = "translation-unit";
};

std::string diagnosticField(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    std::replace(value.begin(), value.end(), '"', '\'');
    if (value.empty()) {
        return "unavailable";
    }
    if (value.size() > 96U) {
        value.resize(96U);
        value += "...";
    }
    return value;
}

std::string affectedEntityFromChange(const ConversionChange& change)
{
    const std::string combined = change.before + "\n" + change.after + "\n" + change.ruleName;
    const std::vector<std::regex> patterns = {
        std::regex(R"(#\s*define\s+([A-Za-z_][A-Za-z0-9_]*))"),
        std::regex(R"(\b(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*))"),
        std::regex(R"(\benum\s+(?:class\s+)?([A-Za-z_][A-Za-z0-9_]*))"),
        std::regex(R"(\b(?:auto|int|long|short|char|bool|float|double|void|std::[A-Za-z_][A-Za-z0-9_:<>]*)\s*\*?\s*&?\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\(|;|\[))"),
    };
    for (const std::regex& pattern : patterns) {
        std::smatch match;
        if (std::regex_search(combined, match, pattern) && match.size() > 1U) {
            return diagnosticField(match[1].str());
        }
    }
    return "unavailable";
}

std::string boolText(const bool value)
{
    return value ? "true" : "false";
}

ClassBoundaryValidationResult validateClassBoundaries(const std::string& code)
{
    const ClassResourceAnalyzer classAnalyzer;
    for (const ClassBlock& block : classAnalyzer.analyzeClasses(code)) {
        std::size_t position = block.closeBrace + 1;
        while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position])) != 0) {
            ++position;
        }
        if (position >= code.size() || code[position] != ';') {
            return ClassBoundaryValidationResult{
                false,
                "missing semicolon after class/struct body",
                block.name,
            };
        }
    }

    int braceDepth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inPreprocessorLine = false;
    bool escaped = false;
    bool atLineStart = true;
    for (std::size_t index = 0; index < code.size();) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';

        if (inPreprocessorLine) {
            if (current == '\n') {
                inPreprocessorLine = false;
                atLineStart = true;
            }
            ++index;
            continue;
        }
        if (inLineComment) {
            if (current == '\n') {
                inLineComment = false;
                atLineStart = true;
            }
            ++index;
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                index += 2;
                inBlockComment = false;
            } else {
                ++index;
            }
            continue;
        }
        if (escaped) {
            escaped = false;
            ++index;
            continue;
        }
        if (current == '\\' && (inString || inCharacter)) {
            escaped = true;
            ++index;
            continue;
        }
        if (inString || inCharacter) {
            if (inString && current == '"') {
                inString = false;
            } else if (inCharacter && current == '\'') {
                inCharacter = false;
            }
            ++index;
            continue;
        }
        if (current == '\n') {
            atLineStart = true;
            ++index;
            continue;
        }
        if (atLineStart && std::isspace(static_cast<unsigned char>(current)) != 0) {
            ++index;
            continue;
        }
        if (atLineStart && current == '#') {
            inPreprocessorLine = true;
            ++index;
            continue;
        }
        if (current == '/' && next == '/') {
            inLineComment = true;
            index += 2;
            continue;
        }
        if (current == '/' && next == '*') {
            inBlockComment = true;
            index += 2;
            continue;
        }
        if (current == '"') {
            inString = true;
            atLineStart = false;
            ++index;
            continue;
        }
        if (current == '\'') {
            inCharacter = true;
            atLineStart = false;
            ++index;
            continue;
        }

        if (current == '{') {
            ++braceDepth;
        } else if (current == '}' && braceDepth > 0) {
            --braceDepth;
        }

        if (atLineStart && braceDepth == 0) {
            const std::string_view remaining(code.data() + index, code.size() - index);
            for (const std::string_view access : {"public:", "private:", "protected:"}) {
                if (remaining.starts_with(access)) {
                    return ClassBoundaryValidationResult{
                        false,
                        "orphan access section outside class/struct body",
                        std::string(access.substr(0, access.size() - 1)),
                    };
                }
            }
        }

        atLineStart = false;
        ++index;
    }

    return {};
}

RollbackDiagnostic classifyRollback(const ConversionChange& change, const std::string& passName)
{
    const std::string loweredReason = lowercaseCopy(change.reason);
    const std::string loweredRule = lowercaseCopy(change.ruleName);
    const std::string loweredPass = lowercaseCopy(passName);
    const std::string combined = loweredReason + " " + loweredRule + " " + loweredPass;

    RollbackDiagnostic diagnostic;
    diagnostic.affectedPass = passName;
    diagnostic.affectedEntity = affectedEntityFromChange(change);

    const auto containsAny = [&combined](const std::initializer_list<std::string_view> needles) {
        return containsAnyLowered(combined, needles);
    };

    diagnostic.isRollback = containsAny({"rollback",
                                         "transformation failed",
                                         "validation failed",
                                         "compile failed",
                                         "compile",
                                         "semantic validator",
                                         "unsafe",
                                         "risk",
                                         "scope leak",
                                         "string_view",
                                         "lifetime",
                                         "alias",
                                         "ambiguous",
                                         "unsupported macro",
                                         "missing build context",
                                         "timeout",
                                         "cancel",
                                         "iteration-limit",
                                         "convergence"});
    if (!diagnostic.isRollback) {
        return diagnostic;
    }

    if (containsAny({"transformation failed", "transformation convergence", "convergence failure"})) {
        diagnostic.category = "Infrastructure";
        diagnostic.reason = "transformation convergence failure";
        diagnostic.severity = "Error";
    } else if (containsAny({"iteration-limit", "iteration limit"})) {
        diagnostic.category = "Infrastructure";
        diagnostic.reason = "pass iteration limit reached";
        diagnostic.severity = "Error";
    } else if (containsAny({"timeout"})) {
        diagnostic.category = "Infrastructure";
        diagnostic.reason = "timeout reached";
        diagnostic.severity = "Error";
    } else if (containsAny({"cancel"})) {
        diagnostic.category = "Infrastructure";
        diagnostic.reason = "worker cancelled";
        diagnostic.severity = "Warning";
    } else if (containsAny({"compile_commands", "compile commands"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "compile_commands.json unavailable";
    } else if (containsAny({"include path", "include unresolved", "header unresolved"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "include path unresolved";
    } else if (containsAny({"cross-file", "cross file"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "cross-file propagation unavailable";
    } else if (containsAny({"build context", "repository context"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "missing build context";
    } else if (containsAny({"syntax verification", "syntax"})) {
        diagnostic.category = "Compilation";
        diagnostic.reason = "syntax verification failed";
        diagnostic.severity = "Error";
    } else if (containsAny({"compile", "compiler"})) {
        diagnostic.category = "Compilation";
        diagnostic.reason = "compile verification failed";
        diagnostic.severity = "Error";
    } else if (containsAny({"generated code", "invalid code"})) {
        diagnostic.category = "Compilation";
        diagnostic.reason = "generated code invalid";
        diagnostic.severity = "Error";
    } else if (containsAny({"dependency resolution"})) {
        diagnostic.category = "Compilation";
        diagnostic.reason = "dependency resolution unavailable";
    } else if (containsAny({"token pasting", "##"})) {
        diagnostic.category = "Macros";
        diagnostic.reason = "token pasting macro";
    } else if (containsAny({"stringification", "#"})) {
        diagnostic.category = "Macros";
        diagnostic.reason = "stringification macro";
    } else if (containsAny({"multi-statement", "multiple statement"})) {
        diagnostic.category = "Macros";
        diagnostic.reason = "multi-statement macro";
    } else if (containsAny({"platform", "compiler specific", "compiler-specific"})) {
        diagnostic.category = "Macros";
        diagnostic.reason = "platform/compiler specific macro";
    } else if (containsAny({"macro"})) {
        diagnostic.category = "Macros";
        diagnostic.reason = containsAny({"side effect", "side-effect"}) ? "side-effectful macro" : "unsupported macro pattern";
    } else if (containsAny({"string_view", "lifetime"})) {
        diagnostic.category = "String";
        diagnostic.reason = "string_view lifetime risk";
    } else if (containsAny({"null termination", "null-termination", "c_str"})) {
        diagnostic.category = "String";
        diagnostic.reason = "null termination required";
    } else if (containsAny({"c api", "c-api", "cstring"})) {
        diagnostic.category = "String";
        diagnostic.reason = "C API compatibility risk";
    } else if (containsAny({"string conversion", "std::string"})) {
        diagnostic.category = "String";
        diagnostic.reason = "string conversion uncertainty";
    } else if (containsAny({"vector"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "vector conversion dependency mismatch";
    } else if (containsAny({"iterator"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "iterator rewrite safety failure";
    } else if (containsAny({"structured binding"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "structured binding conversion unsafe";
    } else if (containsAny({"range-for", "range loop", "range-loop"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "range-loop conversion unsafe";
    } else if (containsAny({"type propagation"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "type propagation incomplete";
    } else if (containsAny({"getter", "setter"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "getter/setter mismatch";
    } else if (containsAny({"parameter"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "incompatible parameter conversion";
    } else if (containsAny({"return type"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "return type propagation failure";
    } else if (containsAny({"enum"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "enum modernization conflict";
    } else if (containsAny({"scope leak", "validation failed", "semantic validator", "validator"})) {
        diagnostic.category = "Semantic";
        diagnostic.reason = "type propagation incomplete";
    } else if (containsAny({"alias"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "raw pointer alias ambiguity";
    } else if (containsAny({"shared ownership", "shared_ptr"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "shared ownership suspected";
    } else if (containsAny({"borrowed", "observer"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "borrowed pointer detected";
    } else if (containsAny({"polymorphic"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "polymorphic ownership uncertainty";
    } else if (containsAny({"ownership", "owning", "ambiguous"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "unclear ownership model";
    } else {
        diagnostic.category = "Semantic";
        diagnostic.reason = "type propagation incomplete";
    }

    return diagnostic;
}

SkippedRiskDiagnostic classifySkippedRisk(const ConversionChange& change, const std::string& passName)
{
    const std::string loweredReason = lowercaseCopy(change.reason);
    const std::string loweredRule = lowercaseCopy(change.ruleName);
    const std::string loweredBefore = lowercaseCopy(change.before);
    const std::string loweredPass = lowercaseCopy(passName);
    const std::string combined = loweredReason + " " + loweredRule + " " + loweredBefore + " " + loweredPass;

    SkippedRiskDiagnostic diagnostic;
    diagnostic.affectedPass = passName;
    diagnostic.affectedEntity = affectedEntityFromChange(change);
    diagnostic.codeLeftUnchanged = !change.applied;

    const auto containsAny = [&combined](const std::initializer_list<std::string_view> needles) {
        return containsAnyLowered(combined, needles);
    };

    diagnostic.isRisk = !change.applied
        && containsAny({"preserved",
                        "manual review",
                        "unsafe",
                        "risk",
                        "ambiguous",
                        "unclear",
                        "borrowed",
                        "observer",
                        "lifetime",
                        "null-terminated",
                        "null termination",
                        "c api",
                        "c-api",
                        "file*",
                        "fopen",
                        "fprintf",
                        "binary",
                        "complex",
                        "macro",
                        "iterator",
                        "structured binding",
                        "range",
                        "polymorphic",
                        "compile_commands",
                        "build context",
                        "include path",
                        "format"});
    if (!diagnostic.isRisk) {
        return diagnostic;
    }

    if (containsAny({"token-pasting", "token pasting", "##"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "token pasting macro";
        diagnostic.suggestedAction = "Review manually. Token pasting depends on preprocessor semantics that constexpr functions cannot reproduce.";
    } else if (containsAny({"stringification", "#value", "# value"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "stringification macro";
        diagnostic.suggestedAction = "Review manually. Stringification depends on preprocessor text substitution.";
    } else if (containsAny({"multi-statement", "multiple statement", "control-flow", "control flow", "do {"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "multi-statement macro";
        diagnostic.suggestedAction = "Review manually. This macro contains control flow or multiple statements.";
    } else if (containsAny({"side effect", "side-effect", "evaluated multiple times"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "side-effectful macro";
        diagnostic.suggestedAction = "Review manually. Macro argument evaluation may have side effects or occur multiple times.";
    } else if (containsAny({"platform", "compiler extension", "compiler-specific"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "platform/compiler specific macro";
        diagnostic.suggestedAction = "Review manually. Preserve platform/compiler-specific macro behavior or replace it with a guarded abstraction.";
    } else if (containsAny({"macro"})) {
        diagnostic.category = "Macro";
        diagnostic.reason = "unsupported macro pattern";
        diagnostic.suggestedAction = "Review manually. Confirm the macro can be replaced without changing preprocessing semantics.";
    } else if (containsAny({"borrowed", "observer"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "borrowed pointer detected";
        diagnostic.suggestedAction = "Ownership is unclear. Consider documenting ownership or converting manually.";
    } else if (containsAny({"alias"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "raw pointer alias ambiguity";
        diagnostic.suggestedAction = "Review aliases manually before choosing unique_ptr, shared_ptr, or non-owning raw pointer semantics.";
    } else if (containsAny({"shared ownership", "shared_ptr"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = "shared ownership suspected";
        diagnostic.suggestedAction = "Confirm whether shared ownership is intentional before using std::shared_ptr.";
    } else if (containsAny({"polymorphic"})) {
        diagnostic.category = "Polymorphism";
        diagnostic.reason = "polymorphic ownership uncertainty";
        diagnostic.suggestedAction = "Check virtual destructors and ownership through base pointers before modernizing.";
    } else if (containsAny({"ownership", "owning", "malloc", "free", "new", "delete"})) {
        diagnostic.category = "Ownership";
        diagnostic.reason = containsAny({"unclear", "ambiguous"}) ? "unclear ownership model" : "raw pointer alias ambiguity";
        diagnostic.suggestedAction = "Ownership is unclear. Consider documenting ownership or converting manually.";
    } else if (containsAny({"string_view", "lifetime"})) {
        diagnostic.category = "String";
        diagnostic.reason = "string_view lifetime risk";
        diagnostic.suggestedAction = "Keep std::string or const char* unless the view lifetime is guaranteed.";
    } else if (containsAny({"null-terminated", "null termination", "c_str"})) {
        diagnostic.category = "String";
        diagnostic.reason = "null termination required";
        diagnostic.suggestedAction = "Null-terminated string required. Keep std::string or const char*.";
    } else if (containsAny({"c api", "c-api", "cstring"})) {
        diagnostic.category = "String";
        diagnostic.reason = "C API compatibility risk";
        diagnostic.suggestedAction = "Keep a compatible C-string boundary or introduce an owned std::string temporary with clear lifetime.";
    } else if (containsAny({"string"})) {
        diagnostic.category = "String";
        diagnostic.reason = "string conversion uncertainty";
        diagnostic.suggestedAction = "Review string ownership, lifetime, and C API boundaries before changing the interface.";
    } else if (containsAny({"file*", "fopen", "fclose", "fprintf", "fputs", "binary"})) {
        diagnostic.category = "File I/O";
        diagnostic.reason = containsAny({"binary"}) ? "binary file usage detected" : "complex FILE* usage detected";
        diagnostic.suggestedAction = "Complex or binary file usage detected. Consider std::ifstream/std::ofstream manually.";
    } else if (containsAny({"format", "printf", "specifier"})) {
        diagnostic.category = "Formatting";
        diagnostic.reason = "formatting conversion unsafe";
        diagnostic.suggestedAction = "Review format specifiers manually before replacing with streams or std::format.";
    } else if (containsAny({"iterator"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "iterator rewrite safety failure";
        diagnostic.suggestedAction = "Review manually. Iterator mutation, invalidation, or non-traversal use may make range-for unsafe.";
    } else if (containsAny({"structured binding"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "structured binding conversion unsafe";
        diagnostic.suggestedAction = "Review manually. Confirm key/value access is simple and iterator semantics are not needed.";
    } else if (containsAny({"range", "index loop"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "range-loop conversion unsafe";
        diagnostic.suggestedAction = "Review manually. Preserve index-based logic when the index has semantic meaning.";
    } else if (containsAny({"vector"})) {
        diagnostic.category = "Containers";
        diagnostic.reason = "vector conversion dependency mismatch";
        diagnostic.suggestedAction = "Review dependent getters, setters, and call sites before converting this container.";
    } else if (containsAny({"compile_commands", "compile commands"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "compile_commands.json unavailable";
        diagnostic.suggestedAction = "Provide compile_commands.json or configure a CMake build directory.";
    } else if (containsAny({"build context"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "missing build context";
        diagnostic.suggestedAction = "Provide compile_commands.json or configure CMake build directory.";
    } else if (containsAny({"include path"})) {
        diagnostic.category = "Repository";
        diagnostic.reason = "include path unresolved";
        diagnostic.suggestedAction = "Configure repository include paths before enabling project-level verification.";
    } else if (containsAny({"compile", "syntax", "generated code"})) {
        diagnostic.category = "Compilation";
        diagnostic.reason = containsAny({"syntax"}) ? "syntax verification failed" : "compile verification failed";
        diagnostic.severity = "Error";
        diagnostic.suggestedAction = "Inspect compiler diagnostics before accepting this transformation.";
    } else {
        diagnostic.category = "Semantic";
        diagnostic.reason = "transformation safety could not be proven";
        diagnostic.suggestedAction = "Review the preserved code manually before applying this modernization.";
    }

    return diagnostic;
}

std::string rollbackDetailMessage(const RollbackDiagnostic& diagnostic)
{
    std::ostringstream output;
    output << "ROLLBACK DETAIL"
           << " category=" << diagnostic.category
           << " reason=\"" << diagnosticField(diagnostic.reason) << "\""
           << " pass=\"" << diagnosticField(diagnostic.affectedPass) << "\""
           << " entity=\"" << diagnosticField(diagnostic.affectedEntity) << "\""
           << " severity=" << diagnostic.severity;
    return output.str();
}

std::string skippedRiskDetailMessage(const SkippedRiskDiagnostic& diagnostic)
{
    std::ostringstream output;
    output << "SKIPPED RISK"
           << " category=\"" << diagnosticField(diagnostic.category) << "\""
           << " pass=\"" << diagnosticField(diagnostic.affectedPass) << "\""
           << " entity=\"" << diagnosticField(diagnostic.affectedEntity) << "\""
           << " reason=\"" << diagnosticField(diagnostic.reason) << "\""
           << " severity=" << diagnostic.severity
           << " suggested_action=\"" << diagnosticField(diagnostic.suggestedAction) << "\""
           << " code_unchanged=" << boolText(diagnostic.codeLeftUnchanged);
    return output.str();
}

std::string rollbackSummaryMessage(const std::vector<RollbackDiagnostic>& diagnostics)
{
    const std::vector<std::string> categories = {
        "Ownership",
        "Semantic",
        "String",
        "Containers",
        "Macros",
        "Compilation",
        "Repository",
        "Infrastructure",
    };
    std::ostringstream output;
    output << "ROLLBACK SUMMARY";
    for (const std::string& category : categories) {
        const std::size_t count = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [&category](const RollbackDiagnostic& diagnostic) {
            return diagnostic.category == category;
        }));
        output << ' ' << category << '=' << count;
    }
    const std::size_t infoCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const RollbackDiagnostic& diagnostic) {
        return diagnostic.severity == "Info";
    }));
    const std::size_t warningCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const RollbackDiagnostic& diagnostic) {
        return diagnostic.severity == "Warning";
    }));
    const std::size_t errorCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const RollbackDiagnostic& diagnostic) {
        return diagnostic.severity == "Error";
    }));
    output << " Info=" << infoCount
           << " Warnings=" << warningCount
           << " Errors=" << errorCount;
    return output.str();
}

std::string skippedRiskSummaryMessage(const std::vector<SkippedRiskDiagnostic>& diagnostics)
{
    const std::vector<std::string> categories = {
        "Ownership",
        "String",
        "Macro",
        "Repository",
        "Semantic",
        "Containers",
        "Polymorphism",
        "File I/O",
        "Formatting",
        "Compilation",
    };
    std::ostringstream output;
    output << "SKIPPED RISK SUMMARY";
    for (const std::string& category : categories) {
        const std::size_t count = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [&category](const SkippedRiskDiagnostic& diagnostic) {
            return diagnostic.category == category;
        }));
        output << " \"" << category << "\"=" << count;
    }
    const std::size_t infoCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const SkippedRiskDiagnostic& diagnostic) {
        return diagnostic.severity == "Info";
    }));
    const std::size_t warningCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const SkippedRiskDiagnostic& diagnostic) {
        return diagnostic.severity == "Warning";
    }));
    const std::size_t errorCount = static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const SkippedRiskDiagnostic& diagnostic) {
        return diagnostic.severity == "Error";
    }));
    output << " Info=" << infoCount
           << " Warnings=" << warningCount
           << " Errors=" << errorCount;
    return output.str();
}

std::string passSummaryMessage(const std::string& passName,
                               const std::size_t appliedCount,
                               const std::size_t skippedCount,
                               const std::size_t warningCount,
                               const std::size_t rollbackCount,
                               const std::size_t nodesVisited,
                               const std::size_t nodesModified,
                               const std::size_t rewriteOperations,
                               const long long elapsedMilliseconds,
                               const std::string& status)
{
    std::ostringstream output;
    output << "PASS SUMMARY pass=\"" << passName << "\""
           << " applied=" << appliedCount
           << " skipped=" << skippedCount
           << " warnings=" << warningCount
           << " rollbacks=" << rollbackCount
           << " visited=" << nodesVisited
           << " modified=" << nodesModified
           << " rewrites=" << rewriteOperations
           << " time_ms=" << elapsedMilliseconds
           << " status=" << status;
    return output.str();
}

std::string finalStatusMessage(const OfflineModernizationPipelineResult& result,
                               const std::vector<ConversionChange>& changes)
{
    const std::size_t applied = static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.applied;
    }));
    const std::size_t skipped = static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.skipped;
    }));
    const std::size_t warnings = static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return !change.applied && !change.skipped;
    }));
    std::ostringstream output;
    output << "FINAL RESULT status=";
    if (result.compileVerificationEnabled) {
        output << (result.compileVerificationPassed ? "success" : "compile-verification-failed-or-skipped");
    } else {
        output << "success-without-compile-verification";
    }
    output << " applied=" << applied
           << " skipped=" << skipped
           << " warnings=" << warnings
           << " compile_verification=";
    if (!result.compileVerificationEnabled) {
        output << "not-run";
    } else {
        output << (result.compileVerificationPassed ? "passed" : "failed/skipped");
    }
    return output.str();
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
    std::vector<RollbackDiagnostic> rollbackDiagnostics;
    std::vector<SkippedRiskDiagnostic> skippedRiskDiagnostics;
    constexpr int maxModernizationIterations = 20;

    result.diagnosticMessages.push_back(AppVersion::diagnosticLine());
    result.diagnosticMessages.push_back("MODERNIZATION PROFILE level="
                                        + modernizationLevelName(options.offlineModernizationLevel)
                                        + " target=" + targetStandardName(options.targetStandard)
                                        + " structural_passes=" + (shouldRunStructuralPass(options) ? "enabled" : "disabled")
                                        + " ownership_consistency=" + (shouldRunOwnershipConsistencyPass(options) ? "enabled" : "disabled")
                                        + " compile_verification=" + (options.compileVerificationEnabled ? "requested" : "pipeline-default"));
    const FrontendSelectionAnalysis frontendAnalysis = analyzeFrontendForOptions(normalizedCode, options, options.frontendSelection);
    result.diagnosticMessages.push_back(frontendAnalysis.summaryDiagnostic);
    if (options.diagnosticVerbosity == DiagnosticVerbosity::Debug) {
        result.diagnosticMessages.insert(result.diagnosticMessages.end(),
                                         frontendAnalysis.debugDiagnostics.begin(),
                                         frontendAnalysis.debugDiagnostics.end());
    }
    if (!shouldRunStructuralPass(options)) {
        result.diagnosticMessages.push_back("SKIPPED PASS GROUP Structural modernization reason=offline modernization level is Conservative");
    }
    if (!shouldRunOwnershipConsistencyPass(options) && !isAggressiveAiLike(options)) {
        result.diagnosticMessages.push_back("SKIPPED PASS GROUP Ownership consistency reason=ownership/string-view consistency options are disabled for this profile");
    }

    auto appendRollbackDiagnostic = [&](RollbackDiagnostic diagnostic) {
        diagnostic.affectedPass = diagnostic.affectedPass.empty() ? "unavailable" : diagnostic.affectedPass;
        rollbackDiagnostics.push_back(diagnostic);
        result.diagnosticMessages.push_back(rollbackDetailMessage(diagnostic));
    };

    auto appendSkippedRiskDiagnostic = [&](SkippedRiskDiagnostic diagnostic) {
        diagnostic.affectedPass = diagnostic.affectedPass.empty() ? "unavailable" : diagnostic.affectedPass;
        skippedRiskDiagnostics.push_back(diagnostic);
        result.diagnosticMessages.push_back(skippedRiskDetailMessage(diagnostic));
    };

    for (const ConversionChange& change : changes) {
        RollbackDiagnostic rollbackDiagnostic = classifyRollback(change, "PrePipelineRules");
        if (rollbackDiagnostic.isRollback) {
            appendRollbackDiagnostic(std::move(rollbackDiagnostic));
        }
        SkippedRiskDiagnostic skippedRiskDiagnostic = classifySkippedRisk(change, "PrePipelineRules");
        if (skippedRiskDiagnostic.isRisk) {
            appendSkippedRiskDiagnostic(std::move(skippedRiskDiagnostic));
        }
    }

    if (options.useStringView && normalizedCode.find("const std::string&") != std::string::npos) {
        const std::regex stringReferencePattern(R"(\bconst\s+std::string\s*&\s*([A-Za-z_][A-Za-z0-9_]*))");
        for (std::sregex_iterator iterator(normalizedCode.begin(), normalizedCode.end(), stringReferencePattern), end;
             iterator != end;
             ++iterator) {
            const std::string parameterName = (*iterator)[1].str();
            const std::string escapedParameter = parameterName;
            const bool cStringRequired = normalizedCode.find(parameterName + ".c_str()") != std::string::npos
                || normalizedCode.find(parameterName + " .c_str()") != std::string::npos;
            const bool escapesOrStores = std::regex_search(normalizedCode, std::regex(R"(=\s*)" + escapedParameter + R"(\b)"))
                || std::regex_search(normalizedCode, std::regex(R"(\breturn\s+)" + escapedParameter + R"(\b)"))
                || std::regex_search(normalizedCode, std::regex(R"(\b(?:push_back|emplace_back|insert|assign)\s*\([^;\n]*\b)" + escapedParameter + R"(\b)"));
            if (!cStringRequired && !escapesOrStores) {
                continue;
            }
            appendSkippedRiskDiagnostic(SkippedRiskDiagnostic{
                true,
                "String",
                cStringRequired ? "null termination required" : "string_view lifetime risk",
                "StringViewSafetyDiagnostics",
                parameterName,
                "Warning",
                cStringRequired
                    ? "Null-terminated string required. Keep std::string or const char*."
                    : "Keep std::string or const char* unless the view lifetime is guaranteed.",
                true,
            });
        }
    }

    bool complexFileIoRisk = false;
    if (normalizedCode.find("FILE") != std::string::npos) {
        const std::regex fprintfTargetPattern(R"(\bfprintf\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
        for (std::sregex_iterator iterator(normalizedCode.begin(), normalizedCode.end(), fprintfTargetPattern), end;
             iterator != end;
             ++iterator) {
            const std::string target = (*iterator)[1].str();
            if (target != "stdout" && target != "stderr") {
                complexFileIoRisk = true;
                break;
            }
        }
        complexFileIoRisk = complexFileIoRisk
            || std::regex_search(normalizedCode, std::regex(R"(\bfopen\s*\([^,\n]+,\s*"[^"]*b[^"]*"\s*\))"));
    }
    if (complexFileIoRisk) {
        appendSkippedRiskDiagnostic(SkippedRiskDiagnostic{
            true,
            "File I/O",
            "complex FILE* usage detected",
            "FileIoSafetyDiagnostics",
            "FILE*",
            "Warning",
            "Complex or binary file usage detected. Consider std::ifstream/std::ofstream manually.",
            true,
        });
    }

    auto runTracedPass = [&](const std::string& passName, auto&& transform) {
        CrashBreadcrumb::enter(passName);
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
            appendRollbackDiagnostic(RollbackDiagnostic{
                true,
                "Infrastructure",
                "pass iteration limit reached",
                passName,
                "pipeline",
                "Error",
            });
            result.diagnosticMessages.push_back(passSummaryMessage(passName,
                                                                   0,
                                                                   0,
                                                                   1,
                                                                   1,
                                                                   nodesVisited,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   "iteration-limit-exceeded; returned-last-stable-version"));
            CrashBreadcrumb::fail(passName, "iteration limit exceeded");
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        std::string candidate;
        try {
            candidate = transform(before);
        } catch (const std::exception& exception) {
            CrashBreadcrumb::fail(passName, exception.what());
            throw;
        } catch (...) {
            CrashBreadcrumb::fail(passName, "unknown exception");
            throw;
        }
        const auto finished = std::chrono::steady_clock::now();
        const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
        std::string statusOverride;
        if (candidate != before) {
            const ClassBoundaryValidationResult beforeClassBoundaries = validateClassBoundaries(before);
            const ClassBoundaryValidationResult candidateClassBoundaries = validateClassBoundaries(candidate);
            if (beforeClassBoundaries.valid && !candidateClassBoundaries.valid) {
                changes.resize(changeCountBefore);
                changes.push_back(ConversionChange{
                    passName,
                    candidateClassBoundaries.entity,
                    {},
                    "Semantic validator failure: class boundary validation failed ("
                        + candidateClassBoundaries.reason
                        + "); rolled back this pass to preserve member function scope.",
                    false,
                    true,
                });
                candidate = before;
                statusOverride = "rolled-back-class-boundary-validation";
            }
        }

        const std::uint64_t hashAfter = stableHash(candidate);
        if (candidate == before && statusOverride.empty()) {
            removeNoOpAppliedChanges(changes, changeCountBefore);
        }

        const std::size_t rewriteOperations = countAppliedChangesSince(changes, changeCountBefore)
            + ((candidate != before && countAppliedChangesSince(changes, changeCountBefore) == 0U) ? 1U : 0U);
        const std::size_t nodesModified = candidate == before ? 0U : countChangedLines(before, candidate);
        const std::string status = !statusOverride.empty()
            ? statusOverride
            : (candidate == before ? "converged-no-change" : "changed");
        const auto changesBegin = changes.begin() + static_cast<std::ptrdiff_t>(changeCountBefore);
        const std::size_t appliedCount = static_cast<std::size_t>(std::count_if(changesBegin, changes.end(), [](const ConversionChange& change) {
            return change.applied;
        }));
        const std::size_t skippedCount = static_cast<std::size_t>(std::count_if(changesBegin, changes.end(), [](const ConversionChange& change) {
            return change.skipped;
        }));
        const std::size_t warningCount = static_cast<std::size_t>(std::count_if(changesBegin, changes.end(), [](const ConversionChange& change) {
            return !change.applied && !change.skipped;
        }));
        std::vector<RollbackDiagnostic> passRollbackDiagnostics;
        std::vector<SkippedRiskDiagnostic> passSkippedRiskDiagnostics;
        for (auto change = changesBegin; change != changes.end(); ++change) {
            if (passName == "PthreadThreadModernizationPass"
                && change->ruleName == "PTHREAD MODERNIZATION DEBUG") {
                result.diagnosticMessages.push_back(change->reason);
            }
            RollbackDiagnostic rollbackDiagnostic = classifyRollback(*change, passName);
            if (rollbackDiagnostic.isRollback) {
                passRollbackDiagnostics.push_back(std::move(rollbackDiagnostic));
            }
            SkippedRiskDiagnostic skippedRiskDiagnostic = classifySkippedRisk(*change, passName);
            if (skippedRiskDiagnostic.isRisk) {
                passSkippedRiskDiagnostics.push_back(std::move(skippedRiskDiagnostic));
            }
        }
        const std::size_t rollbackCount = passRollbackDiagnostics.size();
        result.diagnosticMessages.push_back(endPassTraceMessage(passName,
                                                                iteration,
                                                                hashBefore,
                                                                hashAfter,
                                                                nodesVisited,
                                                                nodesModified,
                                                                rewriteOperations,
                                                                elapsedMilliseconds,
                                                                status));
        for (RollbackDiagnostic& rollbackDiagnostic : passRollbackDiagnostics) {
            appendRollbackDiagnostic(std::move(rollbackDiagnostic));
        }
        for (SkippedRiskDiagnostic& skippedRiskDiagnostic : passSkippedRiskDiagnostics) {
            appendSkippedRiskDiagnostic(std::move(skippedRiskDiagnostic));
        }
        result.diagnosticMessages.push_back(passSummaryMessage(passName,
                                                               appliedCount,
                                                               skippedCount,
                                                               warningCount,
                                                               rollbackCount,
                                                               nodesVisited,
                                                               nodesModified,
                                                               rewriteOperations,
                                                               elapsedMilliseconds,
                                                               status));
        result.modernCode = std::move(candidate);
        CrashBreadcrumb::exit(passName);
    };

    auto runSemanticValidationAndRepairPass = [&]() {
        runTracedPass("SemanticValidationAndRepairPass", [&](const std::string& input) {
            const SemanticValidationAndRepairPass pass;
            const ParsedDocument* selectedDocument = frontendAnalysis.representation != nullptr
                ? frontendAnalysis.representation->parsedDocument()
                : nullptr;
            const bool selectedDocumentMatchesInput = selectedDocument != nullptr
                && selectedDocument->originalSource == input;
            const bool canReuseSelectedDocument = options.enableSharedAstReuse && selectedDocumentMatchesInput;
            if (selectedDocument != nullptr && !canReuseSelectedDocument) {
                result.diagnosticMessages.push_back(options.enableSharedAstReuse
                    ? "SEMANTIC REPAIR representation_reused=false reason=\"selected frontend source graph is stale after rewrites; reparsing current source\""
                    : "SEMANTIC REPAIR representation_reused=false reason=\"internal shared AST reuse flag disabled\"");
            }
            SemanticValidationAndRepairResult repairResult = canReuseSelectedDocument
                ? pass.validateAndRepair(input,
                                         options,
                                         transformationContext,
                                         *selectedDocument,
                                         frontendAnalysis.selectedFrontendName,
                                         true,
                                         changes)
                : pass.validateAndRepair(input, options, transformationContext, changes);
            result.diagnosticMessages.insert(result.diagnosticMessages.end(),
                                             repairResult.diagnostics.begin(),
                                             repairResult.diagnostics.end());
            return repairResult.code;
        });
    };

    auto runIncludeCleanupPass = [&]() {
        CrashBreadcrumb::ScopedStage stage("include cleanup");
        const IncludeCleanupPass includeCleanupPass;
        IncludeCleanupResult includeCleanup = includeCleanupPass.run(result.modernCode, changes);

        std::ostringstream addedIncludes;
        for (std::size_t index = 0; index < includeCleanup.requiredIncludeNamesAdded.size(); ++index) {
            if (index != 0) {
                addedIncludes << ", ";
            }
            addedIncludes << includeCleanup.requiredIncludeNamesAdded[index];
        }
        std::ostringstream removedObsoleteIncludes;
        for (std::size_t index = 0; index < includeCleanup.obsoleteIncludeNamesRemoved.size(); ++index) {
            if (index != 0) {
                removedObsoleteIncludes << ", ";
            }
            removedObsoleteIncludes << includeCleanup.obsoleteIncludeNamesRemoved[index];
        }
        std::ostringstream keptObsoleteIncludes;
        for (std::size_t index = 0; index < includeCleanup.obsoleteIncludeNamesKept.size(); ++index) {
            if (index != 0) {
                keptObsoleteIncludes << ", ";
            }
            keptObsoleteIncludes << includeCleanup.obsoleteIncludeNamesKept[index];
        }
        std::ostringstream skippedObsoleteIncludes;
        for (std::size_t index = 0; index < includeCleanup.obsoleteIncludeNamesSkipped.size(); ++index) {
            if (index != 0) {
                skippedObsoleteIncludes << ", ";
            }
            skippedObsoleteIncludes << includeCleanup.obsoleteIncludeNamesSkipped[index];
        }

        result.diagnosticMessages.push_back("INCLUDE CLEANUP syntax_normalized="
                                            + std::to_string(includeCleanup.syntaxNormalizedCount)
                                            + " duplicates_removed="
                                            + std::to_string(includeCleanup.duplicateIncludesRemovedCount)
                                            + " preserved="
                                            + std::to_string(includeCleanup.includesPreservedCount)
                                            + " required_added="
                                            + std::to_string(includeCleanup.requiredIncludesAddedCount)
                                            + " required_already_present="
                                            + std::to_string(includeCleanup.requiredIncludesAlreadyPresentCount)
                                            + " added=\"" + addedIncludes.str() + "\""
                                            + " obsolete_removed="
                                            + std::to_string(includeCleanup.obsoleteIncludesRemovedCount)
                                            + " obsolete_kept="
                                            + std::to_string(includeCleanup.obsoleteIncludesKeptCount)
                                            + " obsolete_skipped="
                                            + std::to_string(includeCleanup.obsoleteIncludesSkippedCount)
                                            + " removed_obsolete=\"" + removedObsoleteIncludes.str() + "\""
                                            + " kept_obsolete=\"" + keptObsoleteIncludes.str() + "\""
                                            + " skipped_obsolete=\"" + skippedObsoleteIncludes.str() + "\"");
        result.modernCode = std::move(includeCleanup.code);
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
        runTracedPass("MallocFreeModernizationPass::postStringFieldCleanup", [&](const std::string& input) {
            const MallocFreeModernizationPass pass;
            return pass.rewrite(input, options, transformationContext, changes);
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
        runTracedPass("ScopedEnumUsagePropagationPass", [&](const std::string& input) {
            const ScopedEnumUsagePropagationPass pass;
            return pass.rewrite(input, changes);
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
        runTracedPass("PthreadThreadModernizationPass", [&](const std::string& input) {
            const PthreadThreadModernizationPass pass;
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
        applyPolish("Atomic counter modernization", [&](const std::string& input) {
            const AtomicCounterModernizationPass pass;
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

    runTracedPass("MakeUniqueModernizationPass", [&](const std::string& input) {
        const MakeUniqueModernizationPass pass;
        return pass.rewrite(input, changes);
    });
    runTracedPass("SleepModernizationPass", [&](const std::string& input) {
        const SleepModernizationPass pass;
        return pass.rewrite(input, changes);
    });
    runTracedPass("ConstPointerParameterModernizationPass", [&](const std::string& input) {
        const ConstPointerParameterModernizationPass pass;
        return pass.rewrite(input, changes);
    });
    runSemanticValidationAndRepairPass();
    runIncludeCleanupPass();

    if (options.compileVerificationEnabled || aggressiveAiLike || shouldRunStructuralPass(options)) {
        CrashBreadcrumb::ScopedStage compileStage("compile verification");
        result.diagnosticMessages.push_back("COMPILE VERIFICATION status=started stage=initial");
        const auto compileStarted = std::chrono::steady_clock::now();
        CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
        const auto compileFinished = std::chrono::steady_clock::now();
        const auto compileElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(compileFinished - compileStarted).count();
        result.compileVerificationEnabled = verification.verificationEnabled;
        result.compileVerificationPassed = verification.passed;
        result.compilerUsed = verification.compilerUsed;
        result.compilerOutput = verification.output;
        result.diagnosticMessages.push_back("COMPILE VERIFICATION status="
                                            + std::string(verification.passed ? "passed" : "failed/skipped")
                                            + " stage=initial compiler="
                                            + (verification.compilerUsed.empty() ? "not-found" : verification.compilerUsed)
                                            + " time_ms=" + std::to_string(compileElapsedMilliseconds));

        if (verification.compilerFound
            && !verification.passed
            && (!transformationContext.empty()
                || hasScopedEnumOutputDiagnostic(verification.output)
                || (result.modernCode.find("enum class") != std::string::npos && hasScopedEnumUsageDiagnostic(verification.output))
                || hasStringCapiDiagnostic(verification.output)
                || hasSmartPointerDiagnostic(verification.output)
                || hasNsdmiScopeDiagnostic(verification.output))) {
            RollbackDiagnostic compileRollback;
            compileRollback.isRollback = true;
            compileRollback.category = "Compilation";
            compileRollback.reason = "compile verification failed";
            compileRollback.affectedPass = "CompileVerification";
            compileRollback.affectedEntity = "translation-unit";
            compileRollback.severity = "Error";
            appendRollbackDiagnostic(compileRollback);
            result.diagnosticMessages.push_back("ROLLBACK/REPAIR category=Compilation reason=\"compile verification failed\" pass=\"CompileVerification\" entity=\"translation-unit\" severity=Error action=\"targeted cleanup retry\"");
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
            runTracedPass("ScopedEnumUsagePropagationPass", [&](const std::string& input) {
                const ScopedEnumUsagePropagationPass pass;
                return pass.rewrite(input, changes);
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
            runSemanticValidationAndRepairPass();
            runIncludeCleanupPass();

            if (result.modernCode != beforeCleanup) {
                result.compileVerificationAutoFixAttempted = true;
                result.diagnosticMessages.push_back("COMPILE VERIFICATION status=started stage=dependent-cleanup-retry");
                const auto retryStarted = std::chrono::steady_clock::now();
                const CompileVerificationResult secondVerification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
                const auto retryFinished = std::chrono::steady_clock::now();
                const auto retryElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(retryFinished - retryStarted).count();
                result.compileVerificationPassed = secondVerification.passed;
                result.compilerUsed = secondVerification.compilerUsed;
                result.compilerOutput = "Initial compile output:\n" + verification.output
                    + "\n\nAfter dependent cleanup retry:\n" + secondVerification.output;
                result.diagnosticMessages.push_back("COMPILE VERIFICATION status="
                                                    + std::string(secondVerification.passed ? "passed" : "failed/skipped")
                                                    + " stage=dependent-cleanup-retry compiler="
                                                    + (secondVerification.compilerUsed.empty() ? "not-found" : secondVerification.compilerUsed)
                                                    + " time_ms=" + std::to_string(retryElapsedMilliseconds));
                verification = secondVerification;
            }
        }

        if (aggressiveAiLike && verification.compilerFound && !verification.passed) {
            RollbackDiagnostic includeRollback;
            includeRollback.isRollback = true;
            includeRollback.category = "Compilation";
            includeRollback.reason = "syntax verification failed";
            includeRollback.affectedPass = "CompileVerification";
            includeRollback.affectedEntity = "standard-library-includes";
            includeRollback.severity = "Error";
            appendRollbackDiagnostic(includeRollback);
            result.diagnosticMessages.push_back("ROLLBACK/REPAIR category=Compilation reason=\"syntax verification failed\" pass=\"CompileVerification\" entity=\"standard-library-includes\" severity=Error action=\"include auto-fix retry\"");
            AggressiveRewriteEngine aggressiveRewriteEngine;
            const std::string beforeAutoFix = result.modernCode;
            result.modernCode = aggressiveRewriteEngine.ensureModernIncludes(result.modernCode, options, nullptr);
            if (result.modernCode.find("std::cout") != std::string::npos) {
                result.modernCode = ensureInclude(result.modernCode, "#include <iostream>");
            }

            result.compileVerificationAutoFixAttempted = true;
            result.diagnosticMessages.push_back("COMPILE VERIFICATION status=started stage=include-auto-fix-retry");
            const auto includeRetryStarted = std::chrono::steady_clock::now();
            const CompileVerificationResult secondVerification = CompileVerifier::verifySyntaxOnly(result.modernCode, options.targetStandard);
            const auto includeRetryFinished = std::chrono::steady_clock::now();
            const auto includeRetryElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(includeRetryFinished - includeRetryStarted).count();
            result.compileVerificationPassed = secondVerification.passed;
            result.compilerUsed = secondVerification.compilerUsed;
            result.compilerOutput = "Initial compile output:\n" + verification.output
                + "\n\nAfter include auto-fix:\n" + secondVerification.output;
            result.diagnosticMessages.push_back("COMPILE VERIFICATION status="
                                                + std::string(secondVerification.passed ? "passed" : "failed/skipped")
                                                + " stage=include-auto-fix-retry compiler="
                                                + (secondVerification.compilerUsed.empty() ? "not-found" : secondVerification.compilerUsed)
                                                + " time_ms=" + std::to_string(includeRetryElapsedMilliseconds));

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
    } else {
        result.diagnosticMessages.push_back("SKIPPED PASS CompileVerification reason=compile verification disabled for selected options/profile");
    }

    auto runPostConversionFormatting = [&]() {
        CrashBreadcrumb::ScopedStage stage("post-conversion formatting");
        if (!options.enablePostConversionFormatting) {
            result.diagnosticMessages.push_back("POST FORMAT formatting skipped: disabled");
            return;
        }
        if (!result.compileVerificationEnabled || !result.compileVerificationPassed) {
            result.diagnosticMessages.push_back("POST FORMAT formatting skipped: compile failed");
            return;
        }

        const std::string beforeFormatting = result.modernCode;
        const PostConversionFormatter formatter;
        const PostConversionFormattingResult formattingResult = formatter.format(beforeFormatting);
        result.diagnosticMessages.push_back("POST FORMAT " + formattingResult.diagnostic);
        if (!formattingResult.applied || formattingResult.code == beforeFormatting) {
            return;
        }

        result.diagnosticMessages.push_back("COMPILE VERIFICATION status=started stage=post-format");
        const auto formatVerificationStarted = std::chrono::steady_clock::now();
        const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(formattingResult.code, options.targetStandard);
        const auto formatVerificationFinished = std::chrono::steady_clock::now();
        const auto formatVerificationElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            formatVerificationFinished - formatVerificationStarted).count();
        result.diagnosticMessages.push_back("COMPILE VERIFICATION status="
                                            + std::string(verification.passed ? "passed" : "failed/skipped")
                                            + " stage=post-format compiler="
                                            + (verification.compilerUsed.empty() ? "not-found" : verification.compilerUsed)
                                            + " time_ms=" + std::to_string(formatVerificationElapsedMilliseconds));
        if (!verification.compilerFound || !verification.passed) {
            result.diagnosticMessages.push_back("POST FORMAT formatting skipped: compile failed after formatting");
            return;
        }

        result.modernCode = formattingResult.code;
        result.compilerUsed = verification.compilerUsed;
        result.compilerOutput = verification.output;
        result.compileVerificationPassed = true;
        changes.push_back(ConversionChange{
            "Post-conversion formatting",
            "unformatted converted code",
            "formatted converted code",
            "Applied optional final formatting after semantic validation and compile verification passed using "
                + formattingResult.formatterName + ".",
            true,
            false,
        });
    };

    runPostConversionFormatting();

    {
        CrashBreadcrumb::ScopedStage stage("Clang semantic validation");
        if (options.enableClangValidation) {
            const ClangSemanticValidationPass clangValidationPass;
            const std::vector<std::string> clangValidationDiagnostics =
                clangValidationPass.validateWithSelectedFrontend(frontendAnalysis.result,
                                                                 frontendAnalysis.selectedClang(),
                                                                 frontendAnalysis.fallbackUsed,
                                                                 frontendAnalysis.fallbackReason,
                                                                 frontendAnalysis.clangConfig,
                                                                 result.modernCode,
                                                                 options,
                                                                 changes,
                                                                 result.compileVerificationEnabled,
                                                                 result.compileVerificationPassed);
            result.diagnosticMessages.insert(result.diagnosticMessages.end(),
                                             clangValidationDiagnostics.begin(),
                                             clangValidationDiagnostics.end());
        } else {
            result.diagnosticMessages.push_back("CLANG VALIDATION enabled=false reason=\"internal Clang validation flag disabled\"");
        }
    }

    result.diagnosticMessages.push_back(skippedRiskSummaryMessage(skippedRiskDiagnostics));
    result.diagnosticMessages.push_back(rollbackSummaryMessage(rollbackDiagnostics));
    result.diagnosticMessages.push_back(finalStatusMessage(result, changes));
    return result;
}
