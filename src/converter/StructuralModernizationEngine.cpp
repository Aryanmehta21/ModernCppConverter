#include "converter/StructuralModernizationEngine.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"
#include "converter/TransformationContext.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

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

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string accessExpressionRegex(const std::string& symbolName)
{
    return R"((?:(?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)(?:[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?)*(?:\.|->))?)"
        + escapeRegex(symbolName)
        + R"(\b)";
}

std::string singularName(const std::string& collectionName)
{
    const std::size_t lastSeparator = collectionName.find_last_of(".>");
    const std::string leafName = lastSeparator == std::string::npos ? collectionName : collectionName.substr(lastSeparator + 1);
    if (leafName.size() > 1 && leafName.back() == 's') {
        return leafName.substr(0, leafName.size() - 1);
    }
    return "item";
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

bool isCommentOrBlank(const std::string& line)
{
    const std::string stripped = trim(line);
    return stripped.empty() || stripped.starts_with("//") || stripped.starts_with("/*") || stripped.starts_with("*");
}

bool isPreprocessorOpen(const std::string& stripped)
{
    return std::regex_match(stripped, std::regex(R"(^#\s*(if|ifdef|ifndef)\b.*)"));
}

bool isPreprocessorEnd(const std::string& stripped)
{
    return std::regex_match(stripped, std::regex(R"(^#\s*endif\b.*)"));
}

std::size_t findMatchingEndif(const std::vector<std::string>& lines, std::size_t start)
{
    int depth = 0;
    for (std::size_t index = start; index < lines.size(); ++index) {
        const std::string stripped = trim(lines[index]);
        if (isPreprocessorOpen(stripped)) {
            ++depth;
        } else if (isPreprocessorEnd(stripped)) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return lines.size();
}

bool isNullWorkaroundDefine(const std::string& stripped)
{
    return std::regex_match(stripped, std::regex(R"(^#\s*define\s+NULL\s+.+$)"))
        || std::regex_match(stripped, std::regex(R"(^#\s*define\s+nullptr\s+.+$)"));
}

bool isNullWorkaroundBlock(const std::vector<std::string>& lines, std::size_t start, std::size_t end)
{
    const std::string opening = trim(lines[start]);
    if (opening.find("nullptr") == std::string::npos && opening.find("NULL") == std::string::npos) {
        return false;
    }

    for (std::size_t index = start + 1; index < end; ++index) {
        const std::string stripped = trim(lines[index]);
        if (isCommentOrBlank(stripped)) {
            continue;
        }
        if (isNullWorkaroundDefine(stripped)) {
            continue;
        }
        return false;
    }
    return true;
}

bool isSimpleConstantMacroValue(const std::string& value)
{
    static const std::regex numeric(R"([-+]?(?:0x[0-9A-Fa-f]+|\d+)(?:[uUlLfF]*)?)");
    static const std::regex floating(R"([-+]?\d+\.\d+(?:[fFlL])?)");
    static const std::regex quoted(R"("([^"\\]|\\.)*")");
    static const std::regex character(R"('([^'\\]|\\.)')");
    const std::string stripped = trim(value);
    return std::regex_match(stripped, numeric)
        || std::regex_match(stripped, floating)
        || std::regex_match(stripped, quoted)
        || std::regex_match(stripped, character)
        || stripped == "true"
        || stripped == "false";
}

std::string removeCstringIfUnused(std::string code)
{
    const IncludeManager includeManager;
    return includeManager.removeIncludeIfUnused(
        std::move(code),
        "#include <cstring>",
        {"std::strcpy", "std::strncpy", "std::strcmp", "std::strlen", "strcpy(", "strncpy(", "strcmp(", "strlen("});
}

} // namespace

std::string StructuralModernizationEngine::modernize(const std::string& code,
                                                     const ModernizationOptions& options,
                                                     std::vector<ConversionChange>& changes,
                                                     TransformationContext& context) const
{
    std::string updated = code;
    updated = modernizePreprocessor(updated, options, changes);
    updated = modernizeTypedefStructs(updated, changes);
    updated = modernizeCharBuffers(updated, changes, context);
    updated = modernizeDynamicArrays(updated, changes, context);
    updated = modernizeEnums(updated, options, changes);
    updated = modernizeLoops(updated, options, changes);
    updated = modernizeStreamFormatting(updated, options, changes);
    updated = validatePreprocessorBalance(updated, changes);
    return updated;
}

std::string StructuralModernizationEngine::modernizePreprocessor(const std::string& code,
                                                                 const ModernizationOptions& options,
                                                                 std::vector<ConversionChange>& changes) const
{
    std::vector<std::string> lines = splitLines(code);
    std::vector<std::string> output;
    bool changed = false;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string stripped = trim(lines[index]);

        if (isPreprocessorOpen(stripped)) {
            const std::size_t endifLine = findMatchingEndif(lines, index);
            if (endifLine != lines.size() && isNullWorkaroundBlock(lines, index, endifLine)) {
                std::ostringstream before;
                for (std::size_t removed = index; removed <= endifLine; ++removed) {
                    if (removed > index) {
                        before << '\n';
                    }
                    before << trim(lines[removed]);
                }
                addAppliedChange(changes,
                                 "Remove obsolete preprocessor workaround block",
                                 before.str(),
                                 "removed",
                                 "Removed an obsolete NULL/nullptr workaround block so modern C++ relies on native nullptr support.");
                index = endifLine;
                changed = true;
                continue;
            }
        }

        if (options.useNullptr && isNullWorkaroundDefine(stripped)) {
            addAppliedChange(changes,
                             "Remove obsolete preprocessor workaround block",
                             stripped,
                             "removed",
                             "Removed an obsolete NULL/nullptr macro workaround.");
            changed = true;
            continue;
        }

        std::smatch macroMatch;
        if (std::regex_match(stripped, macroMatch, std::regex(R"(^#\s*define\s+([A-Za-z_]\w*)\s+(.+)$)"))) {
            const std::string macroName = macroMatch[1].str();
            const std::string macroValue = trim(macroMatch[2].str());
            if (isSimpleConstantMacroValue(macroValue) && options.useConstexpr) {
                const std::string replacement = "inline constexpr auto " + macroName + " = " + macroValue + ";";
                addAppliedChange(changes,
                                 "Constant macro to constexpr",
                                 stripped,
                                 replacement,
                                 "Converted a simple object-like constant macro to an inline constexpr value with normal C++ scope and type checking.");
                output.push_back(replacement);
                changed = true;
                continue;
            }
        }

        if (std::regex_match(stripped, std::regex(R"(^#\s*define\s+[A-Za-z_]\w*\s*\(.*$)"))) {
            addSuggestion(changes,
                          "Constant macro to constexpr",
                          stripped,
                          "Function-like macros can change evaluation and ABI behavior. They were preserved for manual review.");
        }

        output.push_back(lines[index]);
    }

    if (!changed) {
        return code;
    }

    bool removedEmptyBlock = true;
    while (removedEmptyBlock) {
        removedEmptyBlock = false;
        std::vector<std::size_t> stack;
        for (std::size_t index = 0; index < output.size(); ++index) {
            const std::string stripped = trim(output[index]);
            if (isPreprocessorOpen(stripped)) {
                stack.push_back(index);
            } else if (isPreprocessorEnd(stripped) && !stack.empty()) {
                const std::size_t start = stack.back();
                stack.pop_back();
                const bool empty = std::all_of(output.begin() + static_cast<std::ptrdiff_t>(start + 1),
                                               output.begin() + static_cast<std::ptrdiff_t>(index),
                                               isCommentOrBlank);
                if (empty) {
                    addAppliedChange(changes,
                                     "Preprocessor block cleanup",
                                     trim(output[start]) + "\n" + trim(output[index]),
                                     "removed",
                                     "Removed an empty preprocessor block left after obsolete macro cleanup.");
                    output.erase(output.begin() + static_cast<std::ptrdiff_t>(start),
                                 output.begin() + static_cast<std::ptrdiff_t>(index + 1));
                    removedEmptyBlock = true;
                    break;
                }
            }
        }
    }

    return joinLines(output);
}

std::string StructuralModernizationEngine::modernizeTypedefStructs(const std::string& code,
                                                                   std::vector<ConversionChange>& changes) const
{
    const TypeDeclarationAnalyzer analyzer;
    const std::vector<StructTypedefDeclaration> typedefStructs = analyzer.analyzeTypedefStructs(code);
    if (typedefStructs.empty()) {
        return code;
    }

    std::string updated = code;
    for (auto iterator = typedefStructs.rbegin(); iterator != typedefStructs.rend(); ++iterator) {
        if (iterator->containsFunctionPointer) {
            addSuggestion(changes,
                          "C-style typedef struct to C++ struct",
                          trim(iterator->declarationText),
                          "This typedef struct contains function pointer or ABI-sensitive syntax, so it was preserved for manual review.");
            continue;
        }

        const std::string replacement = "struct " + iterator->aliasName + "\n{" + iterator->body + "\n};";
        addAppliedChange(changes,
                         "C-style typedef struct to C++ struct",
                         trim(iterator->declarationText),
                         trim(replacement),
                         "Removed a redundant C-style typedef wrapper and kept the public C++ struct type name.");
        updated.replace(iterator->start, iterator->end - iterator->start, replacement);
    }

    return updated;
}

std::string StructuralModernizationEngine::modernizeCharBuffers(const std::string& code,
                                                                std::vector<ConversionChange>& changes,
                                                                TransformationContext& context) const
{
    std::string updated = code;
    static const std::regex charArrayPattern(R"((^[ \t]*)char\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]\s*;)",
                                             std::regex::ECMAScript | std::regex::multiline);

    std::vector<std::string> convertedNames;
    std::string search = updated;
    std::smatch match;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, charArrayPattern)) {
        const std::string indent = match[1].str();
        const std::string name = match[2].str();
        const std::string size = match[3].str();
        const std::string targetExpression = accessExpressionRegex(name);
        const bool textUsage = std::regex_search(updated, std::regex(R"(\b(?:std::)?str(?:n?cpy|cmp|len)\s*\(\s*)" + targetExpression))
            || std::regex_search(updated, std::regex(R"(\b(?:std::)?str(?:n?cpy|cmp|len)\s*\([^;]*,\s*)" + targetExpression));

        if (!textUsage) {
            addSuggestion(changes,
                          "Char buffer member to std::string",
                          trim(match[0].str()),
                          "The char array was preserved because it was not clearly used as owning text storage.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string before = match[0].str();
        const std::string replacement = indent + "std::string " + name + ";";
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        addAppliedChange(changes,
                         "Char buffer member to std::string",
                         trim(before),
                         trim(replacement),
                         "Converted fixed-size character storage used with C-string APIs to std::string ownership.");
        context.registerTypeChange(TypeChangeRecord{
            name,
            "char[" + size + "]",
            "std::string",
            {},
            false,
            "Char buffer member to std::string",
            {"rewrite C-string copy APIs", "remove manual null termination", "rewrite C-string comparisons", "remove sizeof buffer usage"},
            {},
            false,
        });
        convertedNames.push_back(name);
        consumed += static_cast<std::size_t>(match.position() + replacement.size());
        search = updated.substr(consumed);
    }

    for (const std::string& name : convertedNames) {
        const std::string escapedName = escapeRegex(name);
        updated = std::regex_replace(updated,
                                     std::regex(R"(\bstd::strncpy\s*\(\s*)" + escapedName + R"(\s*,\s*([^,;]+?)\s*,\s*sizeof\s*\(\s*)" + escapedName + R"(\s*\)\s*\)\s*;)"),
                                     name + " = $1;");
        updated = std::regex_replace(updated,
                                     std::regex(R"(\bstd::strcpy\s*\(\s*)" + escapedName + R"(\s*,\s*([^;]+?)\s*\)\s*;)"),
                                     name + " = $1;");
        const std::regex nullTerminationPattern(escapedName + R"(\s*\[\s*sizeof\s*\(\s*)" + escapedName + R"(\s*\)\s*-\s*1\s*\]\s*=\s*'\\0'\s*;\s*\n?)");
        std::smatch nullTerminationMatch;
        if (std::regex_search(updated, nullTerminationMatch, nullTerminationPattern)) {
            addAppliedChange(changes,
                             "Remove manual null termination after string modernization",
                             trim(nullTerminationMatch[0].str()),
                             "removed",
                             "Removed manual null termination because std::string manages text storage.");
            updated = std::regex_replace(updated, nullTerminationPattern, "");
        }
        updated = std::regex_replace(updated,
                                     std::regex(R"(\bstd::strcmp\s*\(\s*)" + escapedName + R"(\s*,\s*([^)]+?)\s*\)\s*==\s*0)"),
                                     name + " == $1");
        updated = std::regex_replace(updated,
                                     std::regex(R"(\bstd::strcmp\s*\(\s*)" + escapedName + R"(\s*,\s*([^)]+?)\s*\)\s*!=\s*0)"),
                                     name + " != $1");
        updated = std::regex_replace(updated,
                                     std::regex(R"(\bstd::strlen\s*\(\s*)" + escapedName + R"(\s*\))"),
                                     name + ".size()");

        addAppliedChange(changes,
                         "C-string copy to std::string assignment",
                         name,
                         name + " = ...;",
                         "Replaced C-string copying into converted text storage with std::string assignment.");
        addAppliedChange(changes,
                         "C-string comparison to std::string comparison",
                         name,
                         name + " == ...",
                         "Replaced C-string comparison on converted text storage with normal std::string comparison where applicable.");
    }

    if (!convertedNames.empty()) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <string>");
        updated = removeCstringIfUnused(updated);
    }

    return updated;
}

std::string StructuralModernizationEngine::modernizeDynamicArrays(const std::string& code,
                                                                  std::vector<ConversionChange>& changes,
                                                                  TransformationContext& context) const
{
    std::string updated = code;
    bool changed = false;

    static const std::regex localArrayPattern(
        R"((^[ \t]*)(?!char\b)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*=\s*new\s+\2\s*\[\s*([^\]]+)\s*\]\s*;\s*(//.*)?)",
        std::regex::ECMAScript | std::regex::multiline);
    std::string search = updated;
    std::smatch match;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, localArrayPattern)) {
        const std::string indent = match[1].str();
        const std::string type = trim(match[2].str());
        const std::string name = match[3].str();
        const std::string sizeExpression = trim(match[4].str());
        const std::string trailingComment = match[5].matched ? trim(match[5].str()) : "";
        const OwnershipAnalyzer ownershipAnalyzer;
        if (ownershipAnalyzer.hasPointerArithmetic(updated, name) || updated.find("delete " + name) != std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex deleteArrayPattern(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapeRegex(name) + R"(\s*;\s*$)",
                                            std::regex::ECMAScript | std::regex::multiline);
        if (!std::regex_search(updated, deleteArrayPattern)) {
            addSuggestion(changes,
                          "Raw dynamic array to std::vector",
                          trim(match[0].str()),
                          "Dynamic array allocation was preserved because no clear matching delete[] cleanup was found.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string before = match[0].str();
        const std::string vectorDeclaration = indent + "std::vector<" + type + "> " + name + "(" + sizeExpression + ");";
        const std::string replacement = trailingComment.empty()
            ? vectorDeclaration
            : indent + trailingComment + "\n" + vectorDeclaration;
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        updated = std::regex_replace(updated, deleteArrayPattern, "");
        addAppliedChange(changes,
                         "Raw dynamic array to std::vector",
                         trim(before),
                         trim(replacement),
                         "Converted a raw dynamic array with matching delete[] cleanup to std::vector ownership.");
        context.registerTypeChange(TypeChangeRecord{
            name,
            type + "*",
            "std::vector<" + type + ">",
            {},
            false,
            "Raw dynamic array to std::vector",
            {"rewrite new[] allocation", "remove delete[]", "remove nullptr checks", "preserve indexed access after resize"},
            {},
            false,
        });
        addAppliedChange(changes,
                         "Remove delete array after vector modernization",
                         "delete[] " + name + ";",
                         "removed",
                         "std::vector releases its storage automatically.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position() + replacement.size());
        search = updated.substr(consumed);
    }

    const OwnershipAnalyzer ownershipAnalyzer;
    const ClassResourceAnalyzer classAnalyzer;
    const std::vector<ClassBlock> classes = classAnalyzer.analyzeClasses(updated);
    for (auto classIterator = classes.rbegin(); classIterator != classes.rend(); ++classIterator) {
        std::string classText = updated.substr(classIterator->start, classIterator->end - classIterator->start);
        const std::string originalClassText = classText;
        static const std::regex memberPattern(
            R"((^[ \t]*)(?!char\b)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*;)",
            std::regex::ECMAScript | std::regex::multiline);
        std::string memberSearch = classText;
        std::smatch memberMatch;
        while (std::regex_search(memberSearch, memberMatch, memberPattern)) {
            const std::string indent = memberMatch[1].str();
            const std::string type = trim(memberMatch[2].str());
            const std::string name = memberMatch[3].str();
            const std::string escapedName = escapeRegex(name);
            const std::regex allocationPattern("\\b" + escapedName + R"(\s*=\s*new\s+)" + escapeRegex(type) + R"(\s*\[\s*([^\]]+)\s*\]\s*;)");
            const std::regex deletePattern(R"(^[ \t]*delete\s*\[\s*\]\s*)" + escapedName + R"(\s*;\s*\n?)",
                                           std::regex::ECMAScript | std::regex::multiline);
            std::smatch allocationMatch;
            if (!std::regex_search(classText, allocationMatch, allocationPattern)
                || !std::regex_search(classText, deletePattern)
                || ownershipAnalyzer.hasPointerArithmetic(classText, name)) {
                memberSearch = memberMatch.suffix().str();
                continue;
            }

            const std::string memberDeclaration = memberMatch[0].str();
            const std::string sizeExpression = trim(allocationMatch[1].str());
            const std::string declarationReplacement = indent + "std::vector<" + type + "> " + name + ";";
            const std::string resizeReplacement = name + ".resize(" + sizeExpression + ");";
            classText.replace(static_cast<std::size_t>(allocationMatch.position()),
                              static_cast<std::size_t>(allocationMatch.length()),
                              resizeReplacement);
            const std::size_t declarationPosition = classText.find(memberDeclaration);
            if (declarationPosition != std::string::npos) {
                classText.replace(declarationPosition, memberDeclaration.size(), declarationReplacement);
            }
            classText = std::regex_replace(classText, deletePattern, "");
            addAppliedChange(changes,
                             "Raw dynamic array to std::vector",
                             trim(memberDeclaration),
                             trim(declarationReplacement),
                             "Converted an owning dynamic array member to std::vector storage.");
            context.registerTypeChange(TypeChangeRecord{
                name,
                type + "*",
                "std::vector<" + type + ">",
                classIterator->name,
                true,
                "Raw dynamic array to std::vector",
                {"rewrite member new[] allocation", "remove delete[] cleanup", "remove nullptr checks", "remove cleanup-only special members"},
                {},
                false,
            });
            addAppliedChange(changes,
                             "Remove delete array after vector modernization",
                             "delete[] " + name + ";",
                             "removed",
                             "std::vector handles array cleanup automatically.");
            changed = true;
            memberSearch = classText;
        }

        if (classText != originalClassText) {
            static const std::regex emptyDestructorPattern(R"(\n?[ \t]*~[A-Za-z_]\w*\s*\(\s*\)\s*\{\s*\}\s*)");
            if (std::regex_search(classText, emptyDestructorPattern)) {
                classText = std::regex_replace(classText, emptyDestructorPattern, "\n");
                addAppliedChange(changes,
                                 "Rule of Zero after container modernization",
                                 "empty cleanup destructor",
                                 "removed",
                                 "The destructor only existed for array cleanup, so std::vector enables Rule of Zero.");
                addAppliedChange(changes,
                                 "Rule of Zero destructor removal",
                                 "empty cleanup destructor",
                                 "removed",
                                 "Removed an obsolete destructor whose cleanup is now handled by std::vector.");
            }
            updated.replace(classIterator->start, classIterator->end - classIterator->start, classText);
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <vector>");
    }

    return updated;
}

std::string StructuralModernizationEngine::modernizeEnums(const std::string& code,
                                                          const ModernizationOptions& options,
                                                          std::vector<ConversionChange>& changes) const
{
    if (!options.useEnumClass) {
        return code;
    }

    std::string updated = code;
    static const std::regex enumPattern(R"(\benum\s+(?!class\b)([A-Za-z_]\w*)\s*(?::\s*([^\{\n]+?))?\s*\{([^{}]+)\}\s*;)",
                                        std::regex::ECMAScript);
    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;

    while (std::regex_search(search, match, enumPattern)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::string enumName = match[1].str();
        const std::string underlyingType = trim(match[2].str());
        const std::string body = match[3].str();
        std::vector<std::string> enumerators;
        std::stringstream bodyStream(body);
        std::string enumerator;
        while (std::getline(bodyStream, enumerator, ',')) {
            std::string name = trim(enumerator);
            const auto assignment = name.find('=');
            if (assignment != std::string::npos) {
                name = trim(name.substr(0, assignment));
            }
            if (std::regex_match(name, std::regex(R"([A-Za-z_]\w*)"))) {
                enumerators.push_back(name);
            }
        }

        std::string outside = updated;
        outside.replace(position, static_cast<std::size_t>(match.length()), "");
        bool safe = !enumerators.empty();
        for (const std::string& name : enumerators) {
            const std::string escaped = escapeRegex(name);
            const std::regex arithmeticUse(R"((?:\b)" + escaped + R"(\b\s*[\+\-\*/%]|\b[\+\-\*/%]\s*\b)" + escaped + R"(\b))");
            const std::regex integerAssignment(R"(\b(?:int|short|long|unsigned|size_t|std::size_t)\s+[A-Za-z_]\w*\s*=\s*)" + escaped + R"(\b)");
            if (std::regex_search(outside, arithmeticUse) || std::regex_search(outside, integerAssignment)) {
                safe = false;
                break;
            }
        }

        if (!safe) {
            addSuggestion(changes,
                          "Unscoped enum to enum class",
                          trim(match[0].str()),
                          "The enum was preserved because one or more enumerators appear to rely on unscoped or integer-conversion behavior.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string replacement = "enum class " + enumName
            + (underlyingType.empty() ? "" : " : " + underlyingType)
            + "\n{" + body + "};";
        std::string prefix = updated.substr(0, position);
        std::string suffix = updated.substr(position + static_cast<std::size_t>(match.length()));
        for (const std::string& name : enumerators) {
            const std::regex referencePattern("(^|[^:A-Za-z0-9_])" + escapeRegex(name) + R"(\b)");
            prefix = std::regex_replace(prefix, referencePattern, "$1" + enumName + "::" + name);
            suffix = std::regex_replace(suffix, referencePattern, "$1" + enumName + "::" + name);
        }

        updated = prefix + replacement + suffix;
        addAppliedChange(changes,
                         "Unscoped enum to enum class",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted an unscoped enum and updated local enumerator references to scoped enum class names.");
        consumed = position + replacement.size();
        search = updated.substr(consumed);
    }

    return updated;
}

std::string StructuralModernizationEngine::modernizeLoops(const std::string& code,
                                                          const ModernizationOptions& options,
                                                          std::vector<ConversionChange>& changes) const
{
    if (!options.useRangeBasedFor && !options.useRanges) {
        return code;
    }

    std::string updated = code;
    auto rewriteIteratorLoops = [&changes](std::string source, const std::regex& pattern) {
        std::string search = source;
        std::smatch match;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, pattern)) {
            const std::string indent = match[1].str();
            const std::string iteratorName = match[2].str();
            const std::string collection = match[3].str();
            std::string body = match[4].str();
            if (body.find("erase(") != std::string::npos
                || body.find("insert(") != std::string::npos
                || body.find("std::advance(" + iteratorName) != std::string::npos
                || body.find("++" + iteratorName) != std::string::npos
                || body.find(iteratorName + "++") != std::string::npos
                || body.find("--" + iteratorName) != std::string::npos
                || body.find(iteratorName + "--") != std::string::npos) {
                addSuggestion(changes,
                              "Explicit iterator loop to range-based for",
                              trim(match[0].str()),
                              "Iterator loop was preserved because it mutates iteration or erases while traversing.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            const std::string dereferencePattern = R"(\*)" + escapeRegex(iteratorName) + R"(\b)";
            const std::string bodyWithoutDereference = std::regex_replace(body, std::regex(dereferencePattern), "");
            if (std::regex_search(bodyWithoutDereference, std::regex("\\b" + escapeRegex(iteratorName) + "\\b"))) {
                addSuggestion(changes,
                              "Explicit iterator loop to range-based for",
                              trim(match[0].str()),
                              "Iterator loop was preserved because the iterator is used for more than dereferencing the current element.");
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            const std::string element = singularName(collection);
            const bool mutableElement = std::regex_search(body, std::regex(dereferencePattern + R"(\s*(?:=|\+=|-=|\*=|/=|%=))"))
                || std::regex_search(body, std::regex(R"((?:\+\+|--)\s*)" + dereferencePattern))
                || std::regex_search(body, std::regex(dereferencePattern + R"(\s*(?:\+\+|--))"));
            body = std::regex_replace(body, std::regex(dereferencePattern), element);
            body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");
            const std::string qualifier = mutableElement ? "auto& " : "const auto& ";
            const std::string replacement = indent + "for (" + qualifier + element + " : " + collection + ")\n"
                + indent + "{\n" + body + "\n" + indent + "}";
            source.replace(consumed + static_cast<std::size_t>(match.position()),
                           static_cast<std::size_t>(match.length()),
                           replacement);
            addAppliedChange(changes,
                             "Explicit iterator loop to range-based for",
                             trim(match[0].str()),
                             trim(replacement),
                             "Converted a structurally simple iterator loop to a range-based for loop while preserving element constness.");
            consumed += static_cast<std::size_t>(match.position() + replacement.size());
            search = source.substr(consumed);
        }
        return source;
    };

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::regex explicitIteratorLoop(
        R"((^[ \t]*)for\s*\(\s*(?:[A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;()]+>)?::(?:const_)?iterator)\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.c?begin\(\)\s*;\s*\2\s*!=\s*\3\s*\.c?end\(\)\s*;\s*\+\+\2\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    const std::regex autoIteratorLoop(
        R"((^[ \t]*)for\s*\(\s*auto\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.c?begin\(\)\s*;\s*\2\s*!=\s*\3\s*\.c?end\(\)\s*;\s*\+\+\2\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    updated = rewriteIteratorLoops(updated, explicitIteratorLoop);
    updated = rewriteIteratorLoops(updated, autoIteratorLoop);

    static const std::regex indexLoop(
        R"((^[ \t]*)for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*([A-Za-z_]\w*)\.size\(\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    std::string search = updated;
    std::smatch match;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, indexLoop)) {
        const std::string indent = match[1].str();
        const std::string indexName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        const std::string indexedExpression = collection + "\\s*\\[\\s*" + escapeRegex(indexName) + "\\s*\\]";
        const std::string bodyWithoutIndexed = std::regex_replace(body, std::regex(indexedExpression), "");
        if (std::regex_search(bodyWithoutIndexed, std::regex("\\b" + escapeRegex(indexName) + "\\b"))) {
            addSuggestion(changes,
                          "Index loop to range-based for",
                          trim(match[0].str()),
                          "Index loop was preserved because the index has meaning beyond selecting the current element.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string element = singularName(collection);
        const bool mutableElement = std::regex_search(body, std::regex(indexedExpression + R"(\s*=)"));
        body = std::regex_replace(body, std::regex(indexedExpression), element);
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");
        const std::string qualifier = mutableElement ? "auto& " : "const auto& ";
        const std::string replacement = indent + "for (" + qualifier + element + " : " + collection + ")\n"
            + indent + "{\n" + body + "\n" + indent + "}";
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        addAppliedChange(changes,
                         "Index loop to range-based for",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a simple size-based loop to range-based for because the index only selected the current element.");
        consumed += static_cast<std::size_t>(match.position() + replacement.size());
        search = updated.substr(consumed);
    }

    return updated;
}

std::string StructuralModernizationEngine::modernizeStreamFormatting(const std::string& code,
                                                                     const ModernizationOptions& options,
                                                                     std::vector<ConversionChange>& changes) const
{
    if (!options.useStdFormatForStreams || options.targetStandard != CppStandard::Cpp20) {
        return code;
    }

    std::string updated = code;
    bool changed = false;
    const SafeReplacementEngine safeReplacement;
    updated = safeReplacement.rewriteCodeLines(updated, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;
        const std::regex coutPattern("^([ \\t]*)std::cout\\s*<<\\s*\"([^\"{}]*)\"\\s*<<\\s*([A-Za-z_]\\w*(?:\\.[A-Za-z_]\\w*)?)\\s*<<\\s*std::endl\\s*;\\s*$");
        if (!std::regex_match(codePart, match, coutPattern)) {
            if (codePart.find("std::cout") != std::string::npos && codePart.find("<<") != std::string::npos) {
                addSuggestion(changes,
                              "Stream formatting to std::format",
                              trim(codePart),
                              "The stream expression was preserved because it contains formatting state or multiple expressions that need manual review.");
            }
            return line;
        }

        const std::string replacement = match[1].str() + "std::cout << std::format(\""
            + match[2].str() + "{}\\n\", " + trim(match[3].str()) + ");";
        changed = true;
        addAppliedChange(changes,
                         "Stream formatting to std::format",
                         trim(codePart),
                         trim(replacement),
                         "Converted a simple output stream chain to std::format while preserving the trailing newline.");
        return replacement + trailingComment;
    });

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <format>");
    }
    return updated;
}

std::string StructuralModernizationEngine::validatePreprocessorBalance(const std::string& code,
                                                                       std::vector<ConversionChange>& changes) const
{
    std::vector<std::string> lines = splitLines(code);
    std::vector<std::size_t> stack;
    bool changed = false;

    for (std::size_t index = 0; index < lines.size();) {
        const std::string stripped = trim(lines[index]);
        if (isPreprocessorOpen(stripped)) {
            stack.push_back(index);
            ++index;
            continue;
        }
        if (isPreprocessorEnd(stripped)) {
            if (stack.empty()) {
                addAppliedChange(changes,
                                 "Preprocessor block cleanup",
                                 stripped,
                                 "removed",
                                 "Removed a dangling #endif so converted code does not contain an unmatched preprocessor terminator.");
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                changed = true;
                continue;
            }
            stack.pop_back();
        }
        ++index;
    }

    for (const std::size_t unmatchedStart : stack) {
        addSuggestion(changes,
                      "Preprocessor block cleanup",
                      trim(lines[unmatchedStart]),
                      "A preprocessor block is still unmatched after modernization. The converter preserved it because removing source guarded by it could change behavior.");
    }

    return changed ? joinLines(lines) : code;
}
