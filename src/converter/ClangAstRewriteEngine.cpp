#include "converter/ClangAstRewriteEngine.h"

#include "converter/RewriteCoordinator.h"
#include "frontend/FrontendFactory.h"
#include "frontend/IModernizationFrontend.h"
#include "parser/ParsedEntity.h"
#include "utils/CrashBreadcrumb.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace
{
constexpr const char* passName = "ClangAstRewriteEngine";
constexpr const char* typedefRuleName = "Clang AST typedef alias modernization";

std::string collapseWhitespace(std::string value)
{
    std::string output;
    bool lastWasSpace = false;
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            if (!lastWasSpace) {
                output.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        output.push_back(static_cast<char>(character));
        lastWasSpace = false;
    }

    const auto first = output.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const auto last = output.find_last_not_of(' ');
    return output.substr(first, last - first + 1);
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string sourceSlice(const std::string& source, const SourceRange& range)
{
    if (!range.isValidFor(source.size())) {
        return {};
    }
    return source.substr(range.start.offset, range.length());
}

bool diagnosticsContainText(const std::vector<std::string>& diagnostics, const std::string& needle)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&needle](const std::string& diagnostic) {
        return diagnostic.find(needle) != std::string::npos;
    });
}

ClangParseConfig clangParseConfigForOptions(const ModernizationOptions& options)
{
    ClangParseConfig config;
    config.languageStandard = options.targetStandard == CppStandard::Cpp17 ? "c++17" : "c++20";
    config.virtualFileName = "input.cpp";
    config.compileArguments = {"-std=" + config.languageStandard, "-fsyntax-only", "-x", "c++"};
    return config;
}

std::string joinStrings(const std::vector<std::string>& values, const char separator)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            output << separator;
        }
        output << values[index];
    }
    return output.str();
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
    output << " include_paths=" << joinStrings(config.includePaths, ',')
           << " resource_dir=" << config.resourceDir
           << " system_root=" << config.systemRoot;
    return output.str();
}

std::optional<std::string> clangParseConfigDiagnostic(const std::vector<std::string>& diagnostics)
{
    constexpr std::string_view prefix = "FRONTEND clang_parse_config=\"";
    for (const std::string& diagnostic : diagnostics) {
        if (diagnostic.rfind(std::string(prefix), 0) != 0 || diagnostic.size() <= prefix.size()) {
            continue;
        }
        std::string config = diagnostic.substr(prefix.size());
        if (!config.empty() && config.back() == '"') {
            config.pop_back();
        }
        return config;
    }
    return std::nullopt;
}

bool rangeIntersects(const SourceRange& left, const SourceRange& right)
{
    return left.start.offset < right.end.offset && right.start.offset < left.end.offset;
}

bool overlapsMacroDirective(const ParsedDocument& document, const SourceRange& range)
{
    return std::any_of(document.macros.begin(), document.macros.end(), [&range](const ParsedMacroDirective& macro) {
        return macro.range.isValid() && rangeIntersects(range, macro.range);
    });
}

SourceRange extendToSemicolon(const std::string& source, SourceRange range)
{
    if (!range.isValidFor(source.size())) {
        return range;
    }

    const std::size_t semicolon = source.find(';', range.end.offset);
    if (semicolon == std::string::npos) {
        return range;
    }

    const std::size_t newline = source.find('\n', range.end.offset);
    if (newline != std::string::npos && newline < semicolon) {
        return range;
    }

    range.end.offset = semicolon + 1;
    return range;
}

bool looksLikeSimpleAliasTypedef(const std::string& declarationText)
{
    const std::string trimmed = trim(declarationText);
    if (trimmed.rfind("typedef ", 0) != 0) {
        return false;
    }
    return trimmed.find('{') == std::string::npos
        && trimmed.find('}') == std::string::npos
        && trimmed.find("(*") == std::string::npos;
}

bool lineIsPreprocessorDirective(const std::string& source, const SourceRange& range)
{
    if (!range.isValidFor(source.size())) {
        return false;
    }
    const std::size_t lineStart = source.rfind('\n', range.start.offset);
    const std::size_t begin = lineStart == std::string::npos ? 0 : lineStart + 1;
    const std::size_t firstNonSpace = source.find_first_not_of(" \t", begin);
    return firstNonSpace != std::string::npos
        && firstNonSpace < range.start.offset
        && source[firstNonSpace] == '#';
}

RewriteEdit rewriteEditFromAstEdit(const ASTRewriteEdit& astEdit)
{
    RewriteEdit edit;
    edit.range = astEdit.range;
    edit.replacementText = astEdit.replacementText;
    edit.passName = astEdit.passName;
    edit.reason = astEdit.rewriteReason + "; safety=" + astEdit.safetyStatus;
    edit.affectedSymbol = astEdit.symbolId;
    return edit;
}
} // namespace

ClangAstRewriteResult ClangAstRewriteEngine::rewriteTypedefAliases(const std::string& source,
                                                                   const ModernizationOptions& options,
                                                                   std::vector<ConversionChange>& changes) const
{
    CrashBreadcrumb::ScopedStage stage("ClangAstRewriteEngine typedef aliases");
    ClangAstRewriteResult result;
    result.code = source;

    if (!options.useUsingAliases) {
        result.diagnostics.push_back("AST REWRITE enabled=false selected_rule=\"typedef alias modernization\" reason=\"using aliases option disabled\"");
        return result;
    }

    if (options.frontendSelection == ModernizationFrontendSelection::Lightweight) {
        result.diagnostics.push_back("AST REWRITE enabled=false selected_rule=\"typedef alias modernization\" reason=\"selected frontend is LightweightFrontend\"");
        return result;
    }

    result.attempted = true;
    if (!clangExperimentsEnabled()) {
        result.diagnostics.push_back("AST REWRITE enabled=false selected_rule=\"typedef alias modernization\" fallback_reason=\"Clang support not compiled\"");
        return result;
    }

    const ClangParseConfig config = clangParseConfigForOptions(options);
    std::unique_ptr<IModernizationFrontend> frontend = createClangExperimentalFrontend(config);
    if (frontend == nullptr) {
        result.diagnostics.push_back("AST REWRITE enabled=false selected_rule=\"typedef alias modernization\" fallback_reason=\"Clang frontend unavailable\"");
        return result;
    }

    ModernizationFrontendResult frontendResult = frontend->analyze(source);
    const bool parseFailed = !frontendResult.parseSucceeded
        || diagnosticsContainText(frontendResult.diagnostics, "clang_parse=failure");
    if (parseFailed) {
        result.diagnostics.push_back("AST REWRITE enabled=false selected_rule=\"typedef alias modernization\" fallback_reason=\"Clang parse failed; rule-based typedef modernization remains available\"");
        if (const std::optional<std::string> parsedConfig = clangParseConfigDiagnostic(frontendResult.diagnostics)) {
            result.diagnostics.push_back("AST REWRITE parse_config=\"" + *parsedConfig + "\"");
        }
        for (const std::string& diagnostic : frontendResult.diagnostics) {
            if (diagnostic.rfind("CLANG DIAGNOSTIC", 0) == 0) {
                result.diagnostics.push_back(diagnostic);
            }
        }
        return result;
    }

    result.enabled = true;
    const std::string configSummary = clangParseConfigDiagnostic(frontendResult.diagnostics)
        .value_or(clangParseConfigSummary(config));
    result.diagnostics.push_back("AST REWRITE enabled=true selected_rule=\"typedef alias modernization\" frontend=ClangExperimentalFrontend config=\""
                                 + configSummary
                                 + "\"");

    std::vector<ASTRewriteEdit> astEdits;
    for (const ParsedSymbol& symbol : frontendResult.document.symbols) {
        if (symbol.kind != ParsedSymbolKind::Typedef) {
            continue;
        }

        SourceRange editRange = extendToSemicolon(source, symbol.range);
        if (!editRange.isValidFor(source.size())) {
            ++result.editsSkipped;
            result.diagnostics.push_back("AST REWRITE skipped rule=\"typedef alias modernization\" symbol=\""
                                         + symbol.name
                                         + "\" reason=\"source range invalid\"");
            continue;
        }
        if (overlapsMacroDirective(frontendResult.document, editRange)) {
            ++result.editsSkipped;
            result.diagnostics.push_back("AST REWRITE skipped rule=\"typedef alias modernization\" symbol=\""
                                         + symbol.name
                                         + "\" reason=\"typedef inside macro body\"");
            continue;
        }

        const std::string before = sourceSlice(source, editRange);
        if (lineIsPreprocessorDirective(source, editRange) || before.find(symbol.name) == std::string::npos) {
            ++result.editsSkipped;
            result.diagnostics.push_back("AST REWRITE skipped rule=\"typedef alias modernization\" symbol=\""
                                         + symbol.name
                                         + "\" reason=\"typedef inside macro body\"");
            continue;
        }
        if (!looksLikeSimpleAliasTypedef(before)) {
            ++result.editsSkipped;
            result.diagnostics.push_back("AST REWRITE skipped rule=\"typedef alias modernization\" symbol=\""
                                         + symbol.name
                                         + "\" reason=\"unsupported typedef shape\"");
            continue;
        }

        const std::string replacement = "using " + symbol.name + " = " + collapseWhitespace(symbol.type) + ";";
        ASTRewriteEdit astEdit;
        astEdit.range = editRange;
        astEdit.replacementText = replacement;
        astEdit.symbolId = std::to_string(symbol.id);
        astEdit.rewriteReason = "Converted Clang TypedefDecl source range to a using alias";
        astEdit.passName = passName;
        astEdit.safetyStatus = "safe";
        astEdits.push_back(std::move(astEdit));
    }

    result.editsProposed = astEdits.size();
    std::vector<RewriteEdit> edits;
    edits.reserve(astEdits.size());
    for (const ASTRewriteEdit& astEdit : astEdits) {
        edits.push_back(rewriteEditFromAstEdit(astEdit));
    }

    const RewriteApplicationResult rewriteResult = RewriteCoordinator{}.apply(source, edits);
    result.code = rewriteResult.code;
    result.editsApplied = rewriteResult.appliedEdits.size();
    result.editsSkipped += rewriteResult.skippedEdits.size();

    for (const RewriteEdit& edit : rewriteResult.appliedEdits) {
        changes.push_back(ConversionChange{
            typedefRuleName,
            trim(sourceSlice(source, edit.range)),
            trim(edit.replacementText),
            "Applied Clang AST-backed source-range rewrite for a simple typedef alias.",
            true,
            false,
        });
    }
    for (const SkippedRewriteEdit& skipped : rewriteResult.skippedEdits) {
        result.diagnostics.push_back("AST REWRITE skipped rule=\"typedef alias modernization\" symbol=\""
                                     + skipped.edit.affectedSymbol
                                     + "\" reason=\"" + skipped.reason + "\"");
    }

    result.diagnostics.push_back("AST REWRITE rule=\"typedef alias modernization\" edits_proposed="
                                 + std::to_string(result.editsProposed)
                                 + " edits_applied=" + std::to_string(result.editsApplied)
                                 + " edits_skipped=" + std::to_string(result.editsSkipped));
    return result;
}
