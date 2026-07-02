#include "converter/ClangSemanticValidationPass.h"

#include "frontend/FrontendFactory.h"
#include "utils/CrashBreadcrumb.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
bool diagnosticsContain(const std::vector<std::string>& diagnostics, const std::string& needle)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&needle](const std::string& diagnostic) {
        return diagnostic.find(needle) != std::string::npos;
    });
}

bool clangParseSucceeded(const ModernizationFrontendResult& result)
{
    return result.kind == ModernizationFrontendKind::ClangExperimental
        && result.parseSucceeded
        && !diagnosticsContain(result.diagnostics, "clang_parse=failure");
}

std::string frontendSelectionName(ModernizationFrontendSelection selection)
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

bool shouldRunForSelection(ModernizationFrontendSelection selection)
{
    return selection == ModernizationFrontendSelection::ClangExperimental
        || selection == ModernizationFrontendSelection::Auto;
}

std::string severityForCompileState(bool compileVerificationEnabled, bool compileVerificationPassed)
{
    return compileVerificationEnabled && !compileVerificationPassed ? "Error" : "Warning";
}

bool containsCaseSensitive(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

bool changeMentions(const ConversionChange& change, const std::string& needle)
{
    if (needle.empty()) {
        return false;
    }
    return containsCaseSensitive(change.ruleName, needle)
        || containsCaseSensitive(change.before, needle)
        || containsCaseSensitive(change.after, needle)
        || containsCaseSensitive(change.reason, needle);
}

bool changeMentionsAny(const ConversionChange& change, const std::initializer_list<std::string>& needles)
{
    return std::any_of(needles.begin(), needles.end(), [&change](const std::string& needle) {
        return changeMentions(change, needle);
    });
}

bool hasRecordedSignatureChange(const std::vector<ConversionChange>& changes,
                                const ParsedFunction& function)
{
    return std::any_of(changes.begin(), changes.end(), [&function](const ConversionChange& change) {
        return (changeMentions(change, function.name) || changeMentions(change, function.parentName))
            && changeMentionsAny(change, {
                "signature",
                "return type",
                "Return type",
                "parameter",
                "const",
                "API",
                "api",
                "propagation",
                "getter",
                "setter",
            });
    });
}

bool hasRecordedRuleOfZeroRemoval(const std::vector<ConversionChange>& changes,
                                  const ParsedFunction& function)
{
    return std::any_of(changes.begin(), changes.end(), [&function](const ConversionChange& change) {
        return (changeMentions(change, function.name) || changeMentions(change, function.parentName))
            && changeMentionsAny(change, {
                "Rule of Zero",
                "RuleOfZero",
                "cleanup-only",
                "destructor",
                "copy constructor",
                "copy assignment",
                "move constructor",
                "move assignment",
                "special member",
            });
    });
}

bool isSpecialMember(const ParsedFunction& function)
{
    return function.isMember
        && (function.name == function.parentName
            || function.name == "~" + function.parentName
            || function.name == "operator=");
}

std::string functionSignature(const ParsedFunction& function)
{
    std::ostringstream output;
    output << function.returnType << ' ' << function.parentName << "::" << function.name << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << function.parameters[index].type;
    }
    output << ')';
    if (function.isConst) {
        output << " const";
    }
    return output.str();
}

std::string functionKey(const ParsedFunction& function)
{
    return function.parentName + "::" + function.name;
}

std::map<std::string, std::vector<ParsedFunction>> functionsByParentAndName(const ParsedDocument& document)
{
    std::map<std::string, std::vector<ParsedFunction>> result;
    for (const ParsedFunction& function : document.functions) {
        result[functionKey(function)].push_back(function);
    }
    return result;
}

std::map<std::string, std::set<std::string>> enumConstantsByEnum(const ParsedDocument& document)
{
    std::map<std::string, std::set<std::string>> result;
    for (const ParsedEnum& parsedEnum : document.enums) {
        result[parsedEnum.name] = std::set<std::string>(parsedEnum.enumerators.begin(), parsedEnum.enumerators.end());
    }
    return result;
}

std::size_t memberMethodCount(const ParsedDocument& document)
{
    return static_cast<std::size_t>(std::count_if(document.functions.begin(), document.functions.end(), [](const ParsedFunction& function) {
        return function.isMember && !isSpecialMember(function);
    }));
}

std::size_t invalidSourceRangeCount(const ParsedDocument& document, std::size_t sourceSize)
{
    std::size_t invalid = 0;
    auto addIfInvalid = [&](const SourceRange& range) {
        if (range.length() > 0U && !range.isValidFor(sourceSize)) {
            ++invalid;
        }
    };
    for (const ParsedAggregate& aggregate : document.aggregates) {
        addIfInvalid(aggregate.range);
        addIfInvalid(aggregate.nameRange);
        addIfInvalid(aggregate.bodyRange);
    }
    for (const ParsedEnum& parsedEnum : document.enums) {
        addIfInvalid(parsedEnum.range);
        addIfInvalid(parsedEnum.nameRange);
        addIfInvalid(parsedEnum.bodyRange);
    }
    for (const ParsedFunction& function : document.functions) {
        addIfInvalid(function.range);
        addIfInvalid(function.nameRange);
        addIfInvalid(function.bodyRange);
    }
    for (const ParsedVariable& variable : document.memberVariables) {
        addIfInvalid(variable.range);
        addIfInvalid(variable.nameRange);
    }
    for (const ParsedVariable& variable : document.globalVariables) {
        addIfInvalid(variable.range);
        addIfInvalid(variable.nameRange);
    }
    for (const ParsedVariable& variable : document.localVariables) {
        addIfInvalid(variable.range);
        addIfInvalid(variable.nameRange);
    }
    return invalid;
}

std::string quoted(const std::string& value)
{
    return "\"" + value + "\"";
}

std::string parseConfigSummary(const ClangParseConfig& config)
{
    std::ostringstream output;
    output << "standard=" << config.languageStandard << " args=";
    for (std::size_t index = 0; index < config.compileArguments.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << config.compileArguments[index];
    }
    return output.str();
}

std::vector<std::string> compareClangGraphs(const ModernizationFrontendResult& original,
                                            const ModernizationFrontendResult& converted,
                                            const std::string& convertedSource,
                                            const ModernizationOptions& options,
                                            const std::vector<ConversionChange>& changes,
                                            const bool compileVerificationEnabled,
                                            const bool compileVerificationPassed,
                                            const ClangParseConfig& config,
                                            const bool originalGraphReused)
{
    CrashBreadcrumb::ScopedStage stage("ClangSemanticValidationPass selected frontend");
    std::vector<std::string> diagnostics;
    const bool convertedParseSucceeded = clangParseSucceeded(converted);
    const std::string failureSeverity = severityForCompileState(compileVerificationEnabled, compileVerificationPassed);
    if (!convertedParseSucceeded) {
        diagnostics.push_back("CLANG VALIDATION enabled=true requested="
                              + frontendSelectionName(options.frontendSelection)
                              + " original_parse=success converted_parse=failure severity="
                              + failureSeverity
                              + " reason=\"converted source failed Clang parse\"");
    }

    std::size_t suspiciousLosses = 0;
    std::size_t acceptedIntentionalChanges = 0;
    std::size_t enumConstantsPreserved = 0;

    auto warnLoss = [&](const std::string& reason) {
        ++suspiciousLosses;
        diagnostics.push_back("CLANG VALIDATION warning reason=" + quoted(reason));
    };

    if (convertedParseSucceeded) {
        if (converted.entityCounts.classes < original.entityCounts.classes) {
            warnLoss("class count decreased before="
                     + std::to_string(original.entityCounts.classes)
                     + " after=" + std::to_string(converted.entityCounts.classes));
        }
        if (converted.entityCounts.enums < original.entityCounts.enums) {
            warnLoss("enum count decreased before="
                     + std::to_string(original.entityCounts.enums)
                     + " after=" + std::to_string(converted.entityCounts.enums));
        }

        const auto originalFunctions = functionsByParentAndName(original.document);
        const auto convertedFunctions = functionsByParentAndName(converted.document);
        for (const ParsedFunction& originalFunction : original.document.functions) {
            const auto convertedMatch = convertedFunctions.find(functionKey(originalFunction));
            if (convertedMatch == convertedFunctions.end()) {
                if (isSpecialMember(originalFunction) && hasRecordedRuleOfZeroRemoval(changes, originalFunction)) {
                    ++acceptedIntentionalChanges;
                    diagnostics.push_back("CLANG VALIDATION accepted intentional_change=\"method removal\" entity=\""
                                          + functionKey(originalFunction)
                                          + "\" reason=\"Rule of Zero recorded\"");
                } else if (originalFunction.isMember && !isSpecialMember(originalFunction)) {
                    warnLoss("member method missing entity=" + functionKey(originalFunction));
                }
                continue;
            }

            const ParsedFunction& convertedFunction = convertedMatch->second.front();
            if (functionSignature(originalFunction) != functionSignature(convertedFunction)
                && !hasRecordedSignatureChange(changes, originalFunction)) {
                warnLoss("function signature changed without recorded modernization entity="
                         + functionKey(originalFunction));
            }
        }

        const auto originalEnums = enumConstantsByEnum(original.document);
        const auto convertedEnums = enumConstantsByEnum(converted.document);
        for (const auto& [enumName, originalConstants] : originalEnums) {
            const auto convertedEnum = convertedEnums.find(enumName);
            if (convertedEnum == convertedEnums.end()) {
                warnLoss("enum missing entity=" + enumName);
                continue;
            }
            for (const std::string& enumerator : originalConstants) {
                if (convertedEnum->second.contains(enumerator)) {
                    ++enumConstantsPreserved;
                } else {
                    warnLoss("enum constant missing entity=" + enumName + "::" + enumerator);
                }
            }
        }

        const std::size_t invalidRanges = invalidSourceRangeCount(converted.document, convertedSource.size());
        if (invalidRanges > 0U) {
            warnLoss("invalid source ranges count=" + std::to_string(invalidRanges));
        }
    }

    diagnostics.push_back("CLANG VALIDATION enabled=true requested="
                          + frontendSelectionName(options.frontendSelection)
                          + " original_parse=success converted_parse="
                          + std::string(convertedParseSucceeded ? "success" : "failure")
                          + " classes_before=" + std::to_string(original.entityCounts.classes)
                          + " classes_after=" + std::to_string(converted.entityCounts.classes)
                          + " functions_before=" + std::to_string(original.entityCounts.functions)
                          + " functions_after=" + std::to_string(converted.entityCounts.functions)
                          + " enums_before=" + std::to_string(original.entityCounts.enums)
                          + " enums_after=" + std::to_string(converted.entityCounts.enums)
                          + " methods_before=" + std::to_string(memberMethodCount(original.document))
                          + " methods_after=" + std::to_string(memberMethodCount(converted.document))
                          + " enum_constants_preserved=" + std::to_string(enumConstantsPreserved)
                          + " suspicious_losses=" + std::to_string(suspiciousLosses)
                          + " accepted_intentional=" + std::to_string(acceptedIntentionalChanges)
                          + " original_graph_reused=" + (originalGraphReused ? "true" : "false")
                          + " converted_parse_count=1"
                          + " parse_config=\"" + parseConfigSummary(config) + "\""
                          + " severity=" + (suspiciousLosses == 0U && convertedParseSucceeded ? "Info" : failureSeverity));

    return diagnostics;
}
} // namespace

std::vector<std::string> ClangSemanticValidationPass::validateWithSelectedFrontend(
    const ModernizationFrontendResult& selectedOriginal,
    const bool selectedFrontendIsClang,
    const bool frontendFallbackUsed,
    const std::string& frontendFallbackReason,
    const ClangParseConfig& config,
    const std::string& convertedSource,
    const ModernizationOptions& options,
    const std::vector<ConversionChange>& changes,
    const bool compileVerificationEnabled,
    const bool compileVerificationPassed) const
{
    std::vector<std::string> diagnostics;
    if (!shouldRunForSelection(options.frontendSelection)) {
        diagnostics.push_back("CLANG VALIDATION enabled=false requested="
                              + frontendSelectionName(options.frontendSelection)
                              + " reason=\"frontend mode does not request Clang validation\"");
        return diagnostics;
    }

    if (!clangExperimentsEnabled()) {
        diagnostics.push_back("CLANG VALIDATION enabled=false requested="
                              + frontendSelectionName(options.frontendSelection)
                              + " reason=\"Clang support not compiled\"");
        return diagnostics;
    }

    if (!selectedFrontendIsClang || !clangParseSucceeded(selectedOriginal)) {
        const bool parseFailureFallback = frontendFallbackReason.find("parse failed") != std::string::npos
            || frontendFallbackReason.find("parse failure") != std::string::npos;
        const std::string reason = parseFailureFallback
            ? "Skipped because selected frontend fell back to LightweightFrontend after Clang parse failure."
            : (!frontendFallbackReason.empty()
                   ? "Skipped because selected frontend is LightweightFrontend. " + frontendFallbackReason
                   : "selected frontend is not ClangExperimentalFrontend");
        diagnostics.push_back("CLANG VALIDATION enabled=false requested="
                              + frontendSelectionName(options.frontendSelection)
                              + " selected_frontend=" + selectedOriginal.frontendName
                              + " fallback=" + (frontendFallbackUsed ? "true" : "false")
                              + " reason=\"" + reason + "\"");
        return diagnostics;
    }

    std::unique_ptr<IModernizationFrontend> frontend = createClangExperimentalFrontend(config);
    if (!frontend) {
        diagnostics.push_back("CLANG VALIDATION enabled=false requested="
                              + frontendSelectionName(options.frontendSelection)
                              + " reason=\"Clang frontend unavailable\"");
        return diagnostics;
    }

    const ModernizationFrontendResult converted = frontend->analyze(convertedSource);
    return compareClangGraphs(selectedOriginal,
                              converted,
                              convertedSource,
                              options,
                              changes,
                              compileVerificationEnabled,
                              compileVerificationPassed,
                              config,
                              true);
}

std::vector<std::string> ClangSemanticValidationPass::validate(const std::string& originalSource,
                                                               const std::string& convertedSource,
                                                               const ModernizationOptions& options,
                                                               const std::vector<ConversionChange>& changes,
                                                               bool compileVerificationEnabled,
                                                               bool compileVerificationPassed) const
{
    CrashBreadcrumb::ScopedStage stage("ClangSemanticValidationPass standalone");
    if (!shouldRunForSelection(options.frontendSelection)) {
        return {"CLANG VALIDATION enabled=false requested="
                + frontendSelectionName(options.frontendSelection)
                + " reason=\"frontend mode does not request Clang validation\""};
    }

    if (!clangExperimentsEnabled()) {
        return {"CLANG VALIDATION enabled=false requested="
                + frontendSelectionName(options.frontendSelection)
                + " reason=\"Clang support not compiled\""};
    }

    ClangParseConfig config;
    config.languageStandard = options.targetStandard == CppStandard::Cpp17 ? "c++17" : "c++20";
    config.compileArguments = {"-std=" + config.languageStandard, "-fsyntax-only"};
    std::unique_ptr<IModernizationFrontend> frontend = createClangExperimentalFrontend(config);
    if (!frontend) {
        return {"CLANG VALIDATION enabled=false requested="
                + frontendSelectionName(options.frontendSelection)
                + " reason=\"Clang frontend unavailable\""};
    }

    const ModernizationFrontendResult original = frontend->analyze(originalSource);
    if (!clangParseSucceeded(original)) {
        return {"CLANG VALIDATION enabled=false requested="
                + frontendSelectionName(options.frontendSelection)
                + " original_parse=failure reason=\"original source failed Clang parse\""};
    }

    return validateWithSelectedFrontend(original,
                                        true,
                                        false,
                                        {},
                                        config,
                                        convertedSource,
                                        options,
                                        changes,
                                        compileVerificationEnabled,
                                        compileVerificationPassed);
}
