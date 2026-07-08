#include "converter/SemanticValidationAndRepairPass.h"

#include "converter/ContainerModernizationCleanupPass.h"
#include "converter/FileIoModernizationPass.h"
#include "converter/RangeForModernizationPass.h"
#include "converter/ReturnTypePropagationPass.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "converter/ScopedEnumUsagePropagationPass.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/SemanticConsistencyValidator.h"
#include "converter/SemanticModernizationValidator.h"
#include "converter/SemanticTypeValidationPass.h"
#include "converter/SmartPointerSinkPropagationPass.h"
#include "converter/ValueTypePointerOperationScanner.h"
#include "converter/VectorParadigmRewritePass.h"
#include "parser/LightweightCppParser.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2U);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
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

void addSuggestion(std::vector<ConversionChange>& changes,
                   std::string ruleName,
                   std::string before,
                   std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        {},
        std::move(reason),
        false,
        false,
    });
}

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

std::string categoryForChange(const ConversionChange& change)
{
    const std::string combined = change.ruleName + " " + change.reason + " " + change.before + " " + change.after;
    std::string lowered = combined;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (lowered.find("enum") != std::string::npos) {
        return "Enum";
    }
    if (lowered.find("smart pointer") != std::string::npos
        || lowered.find("unique_ptr") != std::string::npos
        || lowered.find("shared_ptr") != std::string::npos
        || lowered.find(".get()") != std::string::npos) {
        return "Ownership";
    }
    if (lowered.find("string") != std::string::npos
        || lowered.find("strcmp") != std::string::npos
        || lowered.find("strcpy") != std::string::npos
        || lowered.find("strlen") != std::string::npos) {
        return "String";
    }
    if (lowered.find("ofstream") != std::string::npos
        || lowered.find("ifstream") != std::string::npos
        || lowered.find("file") != std::string::npos
        || lowered.find("fclose") != std::string::npos
        || lowered.find("fprintf") != std::string::npos) {
        return "File I/O";
    }
    if (lowered.find("pair") != std::string::npos
        || lowered.find("structured binding") != std::string::npos
        || lowered.find("range") != std::string::npos
        || lowered.find("iterator") != std::string::npos) {
        return "Containers";
    }
    if (lowered.find("vector") != std::string::npos
        || lowered.find("resize") != std::string::npos
        || lowered.find("push_back") != std::string::npos
        || lowered.find("count") != std::string::npos) {
        return "Containers";
    }
    if (lowered.find("const") != std::string::npos
        || lowered.find("return type") != std::string::npos) {
        return "Semantic";
    }
    return "Semantic";
}

std::string affectedSymbolForChange(const ConversionChange& change)
{
    const std::string combined = change.before + "\n" + change.after + "\n" + change.ruleName;
    const std::vector<std::regex> patterns{
        std::regex(R"(\bstd::vector\s*<[^>]+>\s+([A-Za-z_]\w*))"),
        std::regex(R"(\bstd::(?:unique_ptr|shared_ptr)\s*<[^>]+>\s+([A-Za-z_]\w*))"),
        std::regex(R"(\bstd::(?:o|i)fstream\s+([A-Za-z_]\w*))"),
        std::regex(R"(\b(?:const\s+)?[A-Za-z_:][A-Za-z0-9_:<>]*\s*\*+\s*([A-Za-z_]\w*))"),
        std::regex(R"(\b([A-Za-z_]\w*)\s*\.)"),
    };
    for (const std::regex& pattern : patterns) {
        std::smatch match;
        if (std::regex_search(combined, match, pattern) && match.size() > 1U) {
            return diagnosticField(match[1].str());
        }
    }
    return "translation-unit";
}

std::set<std::string> collectStreamVariables(const std::string& code)
{
    std::set<std::string> streams;
    const std::regex declarationPattern(R"(\bstd::(?:o|i|f)stream\s+([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        streams.insert((*iterator)[1].str());
    }
    return streams;
}

std::vector<std::string> splitLines(const std::string& code)
{
    std::vector<std::string> lines;
    std::stringstream input(code);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines, const bool trailingNewline)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    if (trailingNewline) {
        output << '\n';
    }
    return output.str();
}

std::string repairStreamArtifacts(const std::string& code,
                                  std::vector<ConversionChange>& changes)
{
    const std::set<std::string> streams = collectStreamVariables(code);
    if (streams.empty()) {
        return code;
    }

    std::vector<std::string> lines = splitLines(code);
    bool changed = false;
    for (std::string& line : lines) {
        const std::string beforeLine = line;
        for (const std::string& stream : streams) {
            const std::string escapedStream = escapeRegex(stream);
            line = std::regex_replace(line,
                                      std::regex(escapedStream + R"(\s*!=\s*(?:nullptr|NULL))"),
                                      stream);
            line = std::regex_replace(line,
                                      std::regex(R"((?:nullptr|NULL)\s*!=\s*)" + escapedStream),
                                      stream);
            line = std::regex_replace(line,
                                      std::regex(escapedStream + R"(\s*==\s*(?:nullptr|NULL))"),
                                      "!" + stream);
            line = std::regex_replace(line,
                                      std::regex(R"((?:nullptr|NULL)\s*==\s*)" + escapedStream),
                                      "!" + stream);

            const std::regex fclosePattern(R"(^([ \t]*)fclose\s*\(\s*)" + escapedStream + R"(\s*\)\s*;\s*$)");
            if (std::regex_match(line, fclosePattern)) {
                line.clear();
            }

            const std::regex fprintfPattern(R"(^[ \t]*fprintf\s*\(\s*)" + escapedStream + R"(\s*,)");
            if (std::regex_search(line, fprintfPattern)) {
                addSuggestion(changes,
                              "Semantic validation and repair",
                              stream,
                              "Skipped FILE* formatting repair because fprintf on a converted stream may require format-string specific rewriting.");
            }
        }

        if (line != beforeLine) {
            changed = true;
            addAppliedChange(changes,
                             "Semantic validation and repair",
                             trim(beforeLine),
                             trim(line).empty() ? "removed" : trim(line),
                             "Repaired FILE*/fstream artifact after stream modernization.");
        }
    }

    if (!changed) {
        return code;
    }
    return joinLines(lines, !code.empty() && code.back() == '\n');
}

bool isStandardValueObjectType(std::string type)
{
    type = trim(std::move(type));
    if (type.find('*') != std::string::npos || type.find('&') != std::string::npos) {
        return false;
    }
    return type == "std::string"
        || type.starts_with("std::vector<")
        || type.starts_with("std::array<")
        || type.starts_with("std::optional<")
        || type.starts_with("std::deque<")
        || type.starts_with("std::list<")
        || type.starts_with("std::map<")
        || type.starts_with("std::set<")
        || type.starts_with("std::unordered_map<")
        || type.starts_with("std::unordered_set<");
}

bool contextContainsTypeChange(const TransformationContext& context,
                               const std::string& symbolName,
                               const std::string& scopeName,
                               const std::string& newType)
{
    return std::any_of(context.typeChanges().begin(), context.typeChanges().end(), [&](const TypeChangeRecord& record) {
        return record.symbolName == symbolName
            && record.scopeName == scopeName
            && record.newType == newType;
    });
}

TransformationContext valueTypeRepairContextForCode(const std::string& code,
                                                    const TransformationContext& context)
{
    TransformationContext repairContext;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        repairContext.registerTypeChange(record);
    }

    const LightweightCppParser parser;
    const ParsedDocument document = parser.parse(code);
    auto addVariable = [&](const ParsedVariable& variable) {
        if (!isStandardValueObjectType(variable.type)
            || contextContainsTypeChange(repairContext, variable.name, variable.parentName, variable.type)) {
            return;
        }
        repairContext.registerTypeChange(TypeChangeRecord{
            variable.name,
            "pointer-like storage",
            variable.type,
            variable.parentName,
            variable.isMember,
            "Semantic validation value-type fact",
            {"remove pointer-specific operations"},
            {},
            false,
        });
    };

    for (const ParsedVariable& variable : document.memberVariables) {
        addVariable(variable);
    }
    for (const ParsedVariable& variable : document.globalVariables) {
        addVariable(variable);
    }
    for (const ParsedVariable& variable : document.localVariables) {
        addVariable(variable);
    }

    return repairContext;
}

std::string normalizeDuplicateConstInLine(const std::string& line, bool& changed)
{
    std::string output;
    output.reserve(line.size());
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;

    auto isIdentifierCharacter = [&](const std::size_t index) {
        return index < line.size()
            && (std::isalnum(static_cast<unsigned char>(line[index])) != 0 || line[index] == '_');
    };
    auto hasWordAt = [&](const std::size_t index, const std::string_view word) {
        return index + word.size() <= line.size()
            && line.compare(index, word.size(), word) == 0
            && (index == 0U || !isIdentifierCharacter(index - 1U))
            && !isIdentifierCharacter(index + word.size());
    };

    for (std::size_t index = 0; index < line.size();) {
        const char current = line[index];
        if (escaped) {
            output.push_back(current);
            escaped = false;
            ++index;
            continue;
        }
        if ((inString || inCharacter) && current == '\\') {
            output.push_back(current);
            escaped = true;
            ++index;
            continue;
        }
        if (!inCharacter && current == '"') {
            inString = !inString;
            output.push_back(current);
            ++index;
            continue;
        }
        if (!inString && current == '\'') {
            inCharacter = !inCharacter;
            output.push_back(current);
            ++index;
            continue;
        }
        if (!inString && !inCharacter && hasWordAt(index, "const")) {
            std::size_t scan = index + 5U;
            std::size_t afterWhitespace = scan;
            while (afterWhitespace < line.size() && std::isspace(static_cast<unsigned char>(line[afterWhitespace])) != 0) {
                ++afterWhitespace;
            }

            bool duplicate = false;
            while (hasWordAt(afterWhitespace, "const")) {
                duplicate = true;
                scan = afterWhitespace + 5U;
                afterWhitespace = scan;
                while (afterWhitespace < line.size() && std::isspace(static_cast<unsigned char>(line[afterWhitespace])) != 0) {
                    ++afterWhitespace;
                }
            }

            if (duplicate) {
                output += "const";
                if (afterWhitespace < line.size() && !std::ispunct(static_cast<unsigned char>(line[afterWhitespace]))) {
                    output.push_back(' ');
                }
                index = afterWhitespace;
                changed = true;
                continue;
            }
        }

        output.push_back(current);
        ++index;
    }

    return output;
}

std::string normalizeDuplicateConstQualifiers(const std::string& code,
                                              std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        bool lineChanged = false;
        const std::string normalized = normalizeDuplicateConstInLine(codePart, lineChanged);
        if (lineChanged) {
            changed = true;
            addAppliedChange(changes,
                             "Duplicate const qualifier normalization",
                             trim(codePart),
                             trim(normalized),
                             "Collapsed duplicate const qualifiers introduced by combined type propagation.");
        }
        return normalized + trailingComment;
    });

    return changed ? updated : code;
}

void appendRepairDiagnostics(SemanticValidationAndRepairResult& result,
                             const ParsedDocument& document,
                             const std::string& frontendName,
                             const bool reusedSelectedRepresentation,
                             const std::vector<ConversionChange>& changes,
                             const std::size_t firstChange)
{
    result.diagnostics.push_back("SEMANTIC REPAIR section=started frontend=" + frontendName
                                 + " representation_reused=" + std::string(reusedSelectedRepresentation ? "true" : "false")
                                 + " parse="
                                 + std::string(document.parseSucceeded ? "ok" : "fallback")
                                 + " functions=" + std::to_string(document.functions.size())
                                 + " classes=" + std::to_string(document.aggregates.size())
                                 + " enums=" + std::to_string(document.enums.size())
                                 + " locals=" + std::to_string(document.localVariables.size())
                                 + " members=" + std::to_string(document.memberVariables.size()));

    for (std::size_t index = firstChange; index < changes.size(); ++index) {
        const ConversionChange& change = changes[index];
        const std::string status = change.applied ? "repaired" : "skipped";
        if (change.applied) {
            ++result.issuesRepaired;
        } else {
            ++result.issuesSkipped;
        }
        ++result.issuesDetected;
        result.diagnostics.push_back("SEMANTIC REPAIR issue category=" + categoryForChange(change)
                                     + " status=" + status
                                     + " pass=\"" + diagnosticField(change.ruleName) + "\""
                                     + " entity=\"" + affectedSymbolForChange(change) + "\""
                                     + " reason=\"" + diagnosticField(change.reason) + "\"");
    }

    result.diagnostics.push_back("SEMANTIC REPAIR section=summary detected="
                                 + std::to_string(result.issuesDetected)
                                 + " repaired=" + std::to_string(result.issuesRepaired)
                                 + " skipped=" + std::to_string(result.issuesSkipped));
}
} // namespace

SemanticValidationAndRepairResult SemanticValidationAndRepairPass::validateAndRepair(
    const std::string& code,
    const ModernizationOptions& options,
    const TransformationContext& context,
    const ParsedDocument& selectedDocument,
    const std::string& selectedFrontendName,
    const bool reusedSelectedRepresentation,
    std::vector<ConversionChange>& changes) const
{
    SemanticValidationAndRepairResult result;
    result.code = code;

    const std::size_t firstChange = changes.size();

    const ReturnTypePropagationPass returnTypePropagationPass;
    result.code = returnTypePropagationPass.rewrite(result.code, changes);

    const SmartPointerSinkPropagationPass smartPointerSinkPropagationPass;
    result.code = smartPointerSinkPropagationPass.rewrite(result.code, changes);

    const ScopedEnumUsagePropagationPass scopedEnumUsagePropagationPass;
    result.code = scopedEnumUsagePropagationPass.rewrite(result.code, changes);
    const ScopedEnumCastValidationPass scopedEnumCastValidationPass;
    result.code = scopedEnumCastValidationPass.validateAndNormalize(result.code, changes);
    const ScopedEnumOutputPropagationPass scopedEnumOutputPropagationPass;
    result.code = scopedEnumOutputPropagationPass.rewrite(result.code, options, changes);
    const ScopedEnumOutputValidator scopedEnumOutputValidator;
    result.code = scopedEnumOutputValidator.validateAndRepair(result.code, options, changes);

    const SemanticTypeValidationPass semanticTypeValidationPass;
    result.code = semanticTypeValidationPass.validateAndRepair(result.code, options, changes);

    const FileIoModernizationPass fileIoModernizationPass;
    result.code = fileIoModernizationPass.rewrite(result.code, changes);
    result.code = repairStreamArtifacts(result.code, changes);

    ModernizationOptions pairSafeOptions = options;
    pairSafeOptions.useRangeBasedFor = true;
    pairSafeOptions.useStructuredBindings = true;
    const RangeForModernizationPass rangeForModernizationPass;
    result.code = rangeForModernizationPass.rewrite(result.code, pairSafeOptions, changes);

    if (!context.empty()) {
        const ContainerModernizationCleanupPass containerModernizationCleanupPass;
        result.code = containerModernizationCleanupPass.rewrite(result.code, context, changes);
        const VectorParadigmRewritePass vectorParadigmRewritePass;
        result.code = vectorParadigmRewritePass.rewrite(result.code, context, changes);
    }

    const TransformationContext valueTypeRepairContext = valueTypeRepairContextForCode(result.code, context);
    const ValueTypePointerOperationScanner valueTypePointerOperationScanner;
    result.code = valueTypePointerOperationScanner.rewrite(result.code, valueTypeRepairContext, changes);

    const SemanticConsistencyValidator semanticConsistencyValidator;
    result.code = semanticConsistencyValidator.validateAndRepair(result.code, options, context, {}, changes);
    const SemanticModernizationValidator semanticModernizationValidator;
    result.code = semanticModernizationValidator.validateAndRepair(result.code, options, context, {}, changes);
    result.code = normalizeDuplicateConstQualifiers(result.code, changes);

    if (result.code != code) {
        addAppliedChange(changes,
                         "Semantic validation and repair",
                         "post-modernization semantic artifacts",
                         "repaired",
                         "Ran final semantic validation and repaired safe inconsistencies before formatting and compile verification.");
    }

    appendRepairDiagnostics(result, selectedDocument, selectedFrontendName, reusedSelectedRepresentation, changes, firstChange);
    return result;
}

SemanticValidationAndRepairResult SemanticValidationAndRepairPass::validateAndRepair(
    const std::string& code,
    const ModernizationOptions& options,
    const TransformationContext& context,
    std::vector<ConversionChange>& changes) const
{
    const LightweightCppParser parser;
    const ParsedDocument document = parser.parse(code);
    return validateAndRepair(code,
                             options,
                             context,
                             document,
                             "LightweightFrontend",
                             false,
                             changes);
}
