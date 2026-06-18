#include "converter/RuleBasedConverterEngine.h"

#include "converter/ModernCppExplanationGenerator.h"
#include "converter/OfflineModernizationPipeline.h"
#include "converter/RawTextRepresentation.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
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

std::string collapseWhitespace(std::string value)
{
    value = std::regex_replace(value, std::regex(R"(\s+)"), " ");
    return trim(value);
}

std::string singularName(const std::string& collectionName)
{
    if (collectionName.size() > 1 && collectionName.back() == 's') {
        return collectionName.substr(0, collectionName.size() - 1);
    }
    return "value";
}

bool atLeastBalanced(const ModernizationOptions& options)
{
    return options.offlineModernizationLevel == OfflineModernizationLevel::Balanced
        || options.offlineModernizationLevel == OfflineModernizationLevel::AggressiveSafe
        || options.offlineModernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
}

bool aggressiveSafe(const ModernizationOptions& options)
{
    return options.offlineModernizationLevel == OfflineModernizationLevel::AggressiveSafe
        || options.offlineModernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
}

bool aiStyleAggressive(const ModernizationOptions& options)
{
    return options.offlineRewriteStyle == OfflineRewriteStyle::AggressiveAiLikeRewrite
        || options.offlineModernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
}

std::string ensureStringInclude(std::string code)
{
    if (code.find("#include <string>") != std::string::npos) {
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
            output << "#include <string>\n";
            inserted = true;
        }

        output << line;

        if (std::regex_match(line, includePattern)) {
            sawInclude = true;
        }
    }

    if (!inserted) {
        if (sawInclude) {
            output << '\n' << "#include <string>";
        } else {
            output.str({});
            output.clear();
            output << "#include <string>\n" << code;
        }
    }

    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }

    return output.str();
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

    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }

    return output.str();
}

bool containsEscapingUse(const std::string& code, const std::string& variable)
{
    const std::regex returnPattern("\\breturn\\s+" + variable + "\\s*;");
    const std::regex assignmentPattern("\\b[A-Za-z_]\\w*\\s*=\\s*" + variable + "\\s*;");
    return std::regex_search(code, returnPattern)
        || std::regex_search(code, assignmentPattern);
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

void addSkippedChange(std::vector<ConversionChange>& changes,
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
        true,
    });
}

class NullToNullptrRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "NULL to nullptr";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex pattern(R"(\bNULL\b)");
        std::string code = representation.sourceText();

        std::stringstream input(code);
        std::ostringstream output;
        std::string line;
        bool firstLine = true;

        while (std::getline(input, line)) {
            const std::string rewrittenLine = std::regex_replace(line, pattern, "nullptr");

            if (rewrittenLine != line) {
                if (options.useNullptr) {
                    addAppliedChange(changes,
                                     name(),
                                     trim(line),
                                     trim(rewrittenLine),
                                     "nullptr is type-safe and avoids the overload ambiguities caused by NULL.");
                } else {
                    addSkippedChange(changes,
                                     name(),
                                     trim(line),
                                     "Skipped because the nullptr modernization option is disabled.");
                }
            }

            if (!firstLine) {
                output << '\n';
            }
            firstLine = false;
            output << (options.useNullptr ? rewrittenLine : line);
        }

        if (!code.empty() && code.back() == '\n') {
            output << '\n';
        }

        representation.replaceSourceText(output.str());
    }
};

class DropNullMacroWorkaroundsRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Drop NULL/nullptr macro workaround";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useNullptr) {
            return;
        }

        const std::string code = representation.sourceText();
        std::stringstream input(code);
        std::ostringstream output;
        std::string line;
        bool firstOutputLine = true;

        while (std::getline(input, line)) {
            const std::string stripped = trim(line);

            if (stripped == "#ifndef nullptr") {
                std::string defineLine;
                std::string endifLine;
                if (std::getline(input, defineLine) && std::getline(input, endifLine)
                    && std::regex_match(trim(defineLine), std::regex(R"(^#define\s+nullptr\s+NULL\s*$)"))
                    && trim(endifLine) == "#endif") {
                    addAppliedChange(changes,
                                     name(),
                                     stripped + "\n" + trim(defineLine) + "\n" + trim(endifLine),
                                     "",
                                     "Removed a custom nullptr macro workaround. Modern C++ has native nullptr.");
                    continue;
                }

                if (!firstOutputLine) {
                    output << '\n';
                }
                firstOutputLine = false;
                output << line;
                if (!defineLine.empty()) {
                    output << '\n' << defineLine;
                }
                if (!endifLine.empty()) {
                    output << '\n' << endifLine;
                }
                continue;
            }

            if (std::regex_match(stripped, std::regex(R"(^#define\s+NULL\s+(.+)$)"))
                || std::regex_match(stripped, std::regex(R"(^#define\s+nullptr\s+(.+)$)"))) {
                addAppliedChange(changes,
                                 name(),
                                 stripped,
                                 "",
                                 "Removed a legacy null pointer macro so the converted code relies on the compiler's native nullptr.");
                continue;
            }

            if (!firstOutputLine) {
                output << '\n';
            }
            firstOutputLine = false;
            output << line;
        }

        if (!code.empty() && code.back() == '\n') {
            output << '\n';
        }

        representation.replaceSourceText(output.str());
    }
};

class TypedefToUsingRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "typedef to using";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex simpleTypedef(
            R"(^([ \t]*)typedef\s+([^;\(\)\{\}=]+?)\s+([A-Za-z_]\w*)\s*;\s*$)");

        const std::string code = representation.sourceText();
        std::stringstream input(code);
        std::ostringstream output;
        std::string line;
        bool firstLine = true;

        while (std::getline(input, line)) {
            std::smatch match;
            std::string rewrittenLine = line;

            if (std::regex_match(line, match, simpleTypedef)) {
                const std::string indent = match[1].str();
                const std::string type = collapseWhitespace(match[2].str());
                const std::string alias = match[3].str();

                const std::string usingLine = indent + "using " + alias + " = " + type + ";";
                if (options.useUsingAliases) {
                    rewrittenLine = usingLine;
                    addAppliedChange(changes,
                        name(),
                        trim(line),
                        trim(rewrittenLine),
                        "Replace a simple typedef alias with the clearer modern using alias syntax.");
                } else {
                    addSkippedChange(changes,
                                     name(),
                                     trim(line),
                                     "Skipped because the using aliases modernization option is disabled.");
                }
            }

            if (!firstLine) {
                output << '\n';
            }
            firstLine = false;
            output << rewrittenLine;
        }

        if (!code.empty() && code.back() == '\n') {
            output << '\n';
        }

        representation.replaceSourceText(output.str());
    }
};

class OldStyleCastSuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Old-style cast suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions&,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex castPattern(
            R"(\(([A-Za-z_]\w*(?:::\w+)*(?:\s*[*&])?)\)\s*([A-Za-z_]\w*|[-+]?\d+(?:\.\d+)?))");

        const std::string code = representation.sourceText();
        for (std::sregex_iterator it(code.begin(), code.end(), castPattern), end; it != end; ++it) {
            addSuggestion(changes,
                          name(),
                          it->str(),
                          "Old-style casts can hide const, reinterpret, or narrowing conversions. Review and replace with static_cast, const_cast, or reinterpret_cast as appropriate.");
        }
    }
};

class OldStyleCastConversionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Old-style cast to named cast";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!atLeastBalanced(options)) {
            return;
        }

        static const std::regex numericCastPattern(
            R"(\((int|long|long long|float|double|std::size_t|size_t)\)\s*([A-Za-z_]\w*|[-+]?\d+(?:\.\d+)?))");

        std::string remaining = representation.sourceText();
        std::string updated;
        std::smatch match;
        bool changed = false;

        while (std::regex_search(remaining, match, numericCastPattern)) {
            const std::string before = match[0].str();
            const std::string after = "static_cast<" + match[1].str() + ">(" + match[2].str() + ")";
            addAppliedChange(changes,
                             name(),
                             before,
                             after,
                             "Converted a simple scalar old-style cast to static_cast so the conversion intent is explicit.");
            updated += match.prefix().str();
            updated += after;
            remaining = match.suffix().str();
            changed = true;
        }

        if (changed) {
            updated += remaining;
            representation.replaceSourceText(updated);
        }
    }
};

class RawPointerOwnershipRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Raw pointer to std::unique_ptr";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex allocationPattern(
            R"(^([ \t]*)([A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;\n{}]+>)?)\s*(?:\(([^;{}]*)\)|\{([^;{}]*)\})?\s*;\s*$)");
        static const std::regex deletePattern(R"(^[ \t]*delete\s+([A-Za-z_]\w*)\s*;\s*$)");

        const std::string originalCode = representation.sourceText();
        std::stringstream input(originalCode);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }

        std::set<std::size_t> deleteLinesToRemove;
        bool changed = false;

        for (std::size_t index = 0; index < lines.size(); ++index) {
            std::smatch allocationMatch;
            if (!std::regex_match(lines[index], allocationMatch, allocationPattern)) {
                continue;
            }

            const std::string indent = allocationMatch[1].str();
            const std::string typeName = trim(allocationMatch[2].str());
            const std::string variable = allocationMatch[3].str();
            const std::string allocatedType = trim(allocationMatch[4].str());
            const std::string arguments = allocationMatch[5].matched
                ? trim(allocationMatch[5].str())
                : (allocationMatch[6].matched ? trim(allocationMatch[6].str()) : "");

            auto compactType = [](std::string value) {
                value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char character) {
                                return std::isspace(character) != 0;
                            }),
                            value.end());
                return value;
            };
            const bool allocatedAsDeclaredType = compactType(typeName) == compactType(allocatedType);

            std::size_t deleteLine = lines.size();
            bool sawConditionalDelete = false;
            for (std::size_t candidate = index + 1; candidate < lines.size(); ++candidate) {
                std::smatch deleteMatch;
                if (std::regex_match(lines[candidate], deleteMatch, deletePattern) && deleteMatch[1].str() == variable) {
                    deleteLine = candidate;
                    break;
                }
                if (lines[candidate].find("delete " + variable) != std::string::npos) {
                    sawConditionalDelete = true;
                    break;
                }
            }

            const bool safe = deleteLine != lines.size()
                && !sawConditionalDelete
                && !containsEscapingUse(originalCode, variable);

            if (safe && options.useSmartPointers && options.useMakeUnique && options.applySafeOwnershipModernization) {
                const std::string replacement = allocatedAsDeclaredType
                    ? indent + "auto " + variable + " = std::make_unique<" + typeName + ">(" + arguments + ");"
                    : indent + "std::unique_ptr<" + typeName + "> " + variable + " = std::make_unique<" + allocatedType + ">(" + arguments + ");";
                addAppliedChange(changes,
                                 name(),
                                 trim(lines[index]),
                                 trim(replacement),
                                 "The pointer owns a dynamically allocated object and has a matching delete. std::unique_ptr provides automatic RAII cleanup.");
                addAppliedChange(changes,
                                 "Remove manual delete",
                                 trim(lines[deleteLine]),
                                 "removed",
                                 "Manual delete is no longer needed because std::unique_ptr automatically destroys the object.");
                lines[index] = replacement;
                deleteLinesToRemove.insert(deleteLine);
                changed = true;
            } else if (options.useSmartPointers || options.useMakeUnique) {
                addSuggestion(changes,
                              name(),
                              trim(lines[index]),
                              safe ? "Safe ownership modernization is disabled. Enable it to convert this matching new/delete pair to std::unique_ptr."
                                   : "Detected manual allocation, but ownership may escape or the matching delete is ambiguous. Consider std::unique_ptr when ownership is local and clear.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 trim(lines[index]),
                                 "Skipped because smart pointer and make_unique modernization options are disabled.");
            }
        }

        if (!changed) {
            return;
        }

        std::ostringstream output;
        bool firstOutputLine = true;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (deleteLinesToRemove.contains(index)) {
                continue;
            }
            if (!firstOutputLine) {
                output << '\n';
            }
            firstOutputLine = false;
            output << lines[index];
        }

        std::string updated = ensureInclude(output.str(), "#include <memory>");
        if (!originalCode.empty() && originalCode.back() == '\n' && (updated.empty() || updated.back() != '\n')) {
            updated.push_back('\n');
        }
        representation.replaceSourceText(updated);
    }
};

class CStyleArraySuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "C-style array suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex arrayPattern(
            R"(\b(?:const\s+)?[A-Za-z_]\w*(?:::\w+)*\s+[A-Za-z_]\w*\s*\[[^\]]+\])");

        const std::string code = representation.sourceText();
        for (std::sregex_iterator it(code.begin(), code.end(), arrayPattern), end; it != end; ++it) {
            if (options.useSpan || options.customInstruction.find("vector") != std::string::npos) {
                addSuggestion(changes,
                              name(),
                              it->str(),
                              "C-style arrays can often be replaced with std::array, std::vector, or std::span depending on ownership and size.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 it->str(),
                                 "Skipped because span is disabled and no custom array/container instruction was provided.");
            }
        }
    }
};

class StrncpyToStringRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "std::strncpy buffer to std::string";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions&,
               std::vector<ConversionChange>& changes) const override
    {
        std::string code = representation.sourceText();
        static const std::regex safePattern(
            R"((^[ \t]*)char\s+([A-Za-z_]\w*)\s*\[\s*\d+\s*\]\s*;\s*\n[ \t]*std::strncpy\s*\(\s*\2\s*,\s*([A-Za-z_]\w*)\s*,\s*sizeof\s*\(\s*\2\s*\)\s*\)\s*;)",
            std::regex::ECMAScript | std::regex::multiline);

        std::smatch match;
        bool changed = false;
        std::string updated;

        while (std::regex_search(code, match, safePattern)) {
            const std::string replacement = match[1].str() + "std::string " + match[2].str() + " = " + match[3].str() + ";";
            addAppliedChange(changes,
                             name(),
                             trim(match[0].str()),
                             trim(replacement),
                             "Replaced a fixed-size C-string buffer and bounded copy with std::string where the assignment is direct and safe.");
            updated += match.prefix().str();
            updated += replacement;
            code = match.suffix().str();
            changed = true;
        }

        if (changed) {
            updated += code;
            updated = ensureStringInclude(updated);
            if (updated.find("std::strncpy") == std::string::npos
                && updated.find("std::strcpy") == std::string::npos
                && updated.find("std::strcmp") == std::string::npos
                && updated.find("std::strlen") == std::string::npos
                && updated.find("#include <cstring>") != std::string::npos) {
                updated = std::regex_replace(updated, std::regex(R"(^#include\s+<cstring>\s*\n?)", std::regex::multiline), "");
            }
            representation.replaceSourceText(updated);
            return;
        }

        static const std::regex ambiguousPattern(R"(\bstd::strncpy\s*\()");
        const std::string source = representation.sourceText();
        if (std::regex_search(source, ambiguousPattern)) {
            addSuggestion(changes,
                          name(),
                          "std::strncpy(...)",
                          "Detected std::strncpy, but the surrounding buffer pattern was not simple enough to rewrite safely. Consider replacing C-string buffers with std::string.");
        }
    }
};

class ManualLoopSuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Range-based loop suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        std::string code = representation.sourceText();

        static const std::regex iteratorPrintLoop(
            R"((^[ \t]*)for\s*\(\s*[\w:<>]+\s*::iterator\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\.begin\(\)\s*;\s*\2\s*!=\s*\3\.end\(\)\s*;\s*\+\+\2\s*\)\s*\n\1\{\s*\n([ \t]*)std::cout\s*<<\s*\*\2\s*<<\s*std::endl\s*;\s*\n\1\})",
            std::regex::ECMAScript | std::regex::multiline);
        static const std::regex indexPrintLoop(
            R"((^[ \t]*)for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*([A-Za-z_]\w*)\.size\(\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*\n\1\{\s*\n([ \t]*)std::cout\s*<<\s*\3\s*\[\s*\2\s*\]\s*<<\s*std::endl\s*;\s*\n\1\})",
            std::regex::ECMAScript | std::regex::multiline);

        auto rewriteLoops = [&changes, &options, this](std::string source, const std::regex& pattern) {
            std::smatch match;
            std::string updated;
            bool changed = false;

            while (std::regex_search(source, match, pattern)) {
                const std::string collection = match[3].str();
                const std::string element = singularName(collection);
                const std::string replacement = match[1].str() + "for (const auto& " + element + " : " + collection + ")\n"
                    + match[1].str() + "{\n"
                    + match[4].str() + "std::cout << " + element + " << std::endl;\n"
                    + match[1].str() + "}";

                if (options.useRangeBasedFor) {
                    addAppliedChange(changes,
                                     name(),
                                     trim(match[0].str()),
                                     trim(replacement),
                                     "Converted a simple printing loop to a range-based for loop because the index/iterator was only used to access each element.");
                    updated += match.prefix().str();
                    updated += replacement;
                    changed = true;
                } else {
                    addSkippedChange(changes,
                                     name(),
                                     trim(match[0].str()),
                                     "Skipped because the range-based for loops modernization option is disabled.");
                    updated += match.prefix().str();
                    updated += match[0].str();
                }

                source = match.suffix().str();
            }

            updated += source;
            return std::pair<std::string, bool>{updated, changed};
        };

        auto [afterIteratorRewrite, changedIterator] = rewriteLoops(code, iteratorPrintLoop);
        auto [afterIndexRewrite, changedIndex] = rewriteLoops(afterIteratorRewrite, indexPrintLoop);
        if (changedIterator || changedIndex) {
            representation.replaceSourceText(afterIndexRewrite);
            return;
        }

        static const std::regex loopPattern(
            R"(\bfor\s*\(\s*(?:int|size_t|std::size_t|auto)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*([A-Za-z_]\w*)(?:\.size\(\)|\[[^\]]+\])?\s*;\s*(?:\+\+\1|\1\+\+)\s*\))");

        code = representation.sourceText();
        for (std::sregex_iterator it(code.begin(), code.end(), loopPattern), end; it != end; ++it) {
            if (options.useRangeBasedFor || options.useRanges) {
                addSuggestion(changes,
                              name(),
                              it->str(),
                              "Index-based loops over a collection may be clearer as range-based for loops when the index itself is not needed.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 it->str(),
                                 "Skipped because range-based for loops and ranges modernization options are disabled.");
            }
        }
    }
};

class AutoUsageRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Use auto for obvious construction";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useAuto || !atLeastBalanced(options)) {
            return;
        }

        static const std::regex directConstruction(
            R"(^([ \t]*)(std::[A-Za-z_]\w*(?:::\w+)*(?:<[^;\n=]+>)?|[A-Z][A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*=\s*\2\s*(\([^;\n]*\)|\{[^;\n]*\})\s*;\s*$)");

        std::stringstream input(representation.sourceText());
        std::ostringstream output;
        std::string line;
        bool firstLine = true;
        bool changed = false;

        while (std::getline(input, line)) {
            std::smatch match;
            std::string rewritten = line;
            if (std::regex_match(line, match, directConstruction)) {
                rewritten = match[1].str() + "auto " + match[3].str() + " = " + match[2].str() + match[4].str() + ";";
                addAppliedChange(changes,
                                 name(),
                                 trim(line),
                                 trim(rewritten),
                                 "The right-hand side already states the constructed type, so auto removes repetition without hiding meaning.");
                changed = true;
            }
            if (!firstLine) {
                output << '\n';
            }
            firstLine = false;
            output << rewritten;
        }

        if (changed) {
            representation.replaceSourceText(output.str());
        }
    }
};

class ConstexprRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "constexpr for compile-time value";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useConstexpr || !atLeastBalanced(options)) {
            return;
        }

        static const std::regex constIntegral(
            R"(^([ \t]*)const\s+(int|long|long long|std::size_t|size_t|double|float|bool)\s+([A-Z][A-Za-z_]\w*)\s*=\s*([-+]?\d+(?:\.\d+)?|true|false)\s*;\s*$)");
        static const std::regex simpleFunction(
            R"(^([ \t]*)(int|long|long long|double|float|bool)\s+([A-Za-z_]\w*)\s*\(([^;\{\}]*)\)\s*\{\s*return\s+([^;\{\}]+);\s*\}\s*$)");

        std::stringstream input(representation.sourceText());
        std::ostringstream output;
        std::string line;
        bool firstLine = true;
        bool changed = false;

        while (std::getline(input, line)) {
            std::smatch match;
            std::string rewritten = line;
            if (std::regex_match(line, match, constIntegral)) {
                rewritten = match[1].str() + "constexpr " + match[2].str() + " " + match[3].str() + " = " + match[4].str() + ";";
            } else if (aggressiveSafe(options) && std::regex_match(line, match, simpleFunction)) {
                rewritten = match[1].str() + "constexpr " + match[2].str() + " " + match[3].str() + "(" + match[4].str() + ") { return " + match[5].str() + "; }";
            }

            if (rewritten != line) {
                addAppliedChange(changes,
                                 name(),
                                 trim(line),
                                 trim(rewritten),
                                 "The value or simple function can be evaluated at compile time, so constexpr communicates intent and enables compile-time use.");
                changed = true;
            }

            if (!firstLine) {
                output << '\n';
            }
            firstLine = false;
            output << rewritten;
        }

        if (changed) {
            representation.replaceSourceText(output.str());
        }
    }
};

class OverrideAnnotationRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Add override annotation";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useOverrideFinal || !atLeastBalanced(options)) {
            return;
        }

        const std::string code = representation.sourceText();
        static const std::regex virtualMethodPattern(R"(\bvirtual\s+[\w:<>~*&\s]+\s+([A-Za-z_]\w*)\s*\([^;\{\}]*\)\s*(?:const\s*)?[;{])");
        std::set<std::string> virtualNames;
        for (std::sregex_iterator it(code.begin(), code.end(), virtualMethodPattern), end; it != end; ++it) {
            virtualNames.insert((*it)[1].str());
        }
        if (virtualNames.empty()) {
            return;
        }

        static const std::regex declarationPattern(R"(^([ \t]*)([\w:<>~*&\s]+)\s+([A-Za-z_]\w*)\s*(\([^;\{\}]*\)\s*(?:const\s*)?)(;|[ \t]*\{)\s*$)");
        std::stringstream input(code);
        std::ostringstream output;
        std::string line;
        bool firstLine = true;
        bool changed = false;

        while (std::getline(input, line)) {
            std::smatch match;
            std::string rewritten = line;
            if (std::regex_match(line, match, declarationPattern)
                && virtualNames.contains(match[3].str())
                && line.find("virtual") == std::string::npos
                && line.find("override") == std::string::npos) {
                rewritten = match[1].str() + trim(match[2].str()) + " " + match[3].str() + match[4].str() + " override" + match[5].str();
                addAppliedChange(changes,
                                 name(),
                                 trim(line),
                                 trim(rewritten),
                                 "The method name matches a virtual method in the snippet, so override makes the inheritance contract explicit.");
                changed = true;
            }

            if (!firstLine) {
                output << '\n';
            }
            firstLine = false;
            output << rewritten;
        }

        if (changed) {
            representation.replaceSourceText(output.str());
        }
    }
};

class LambdaExtractionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Extract repeated/simple computation to lambda";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useLambdas || !atLeastBalanced(options) || aiStyleAggressive(options)) {
            return;
        }

        std::string code = representation.sourceText();
        static const std::regex computationPattern(
            R"(((?:unsigned\s+)?(?:long\s+long|long|int|short|double|float)|[A-Za-z_:]\w*(?:::\w+)*(?:<[^;\n{}]+>)?)\s+([A-Za-z_]\w*)\s*\{\s*([A-Za-z_]\w*)\s*\}\s*;\s*\n\s*\1\s+([A-Za-z_]\w*)\s*\{\s*([^{};]+)\s*\}\s*;\s*\n\s*while\s*\(\s*\3\s*([!<>=]+)\s*([^)]*)\)\s*\n\s*\{\s*\n\s*(?:if\s*\([^\)]*\)\s*\n\s*\{\s*\n\s*return\s+false\s*;\s*\n\s*\}\s*\n\s*)?\4\s*=\s*([^;]+);\s*\n\s*\3\s*([/%*+\-]?=)\s*([^;]+);\s*\n\s*\}\s*\n\s*return\s+\2\s*==\s*\4\s*;)",
            std::regex::ECMAScript);

        std::smatch match;
        if (!std::regex_search(code, match, computationPattern)) {
            return;
        }

        auto replaceIdentifier = [](std::string text, const std::string& from, const std::string& to) {
            return std::regex_replace(text, std::regex("\\b" + from + "\\b"), to);
        };
        auto lambdaNameForAccumulator = [](std::string accumulator) {
            if (accumulator.empty()) {
                return std::string("computeValue");
            }
            accumulator[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(accumulator[0])));
            return std::string("compute") + accumulator;
        };

        const std::string valueType = match[1].str();
        const std::string sourceVariable = match[3].str();
        const std::string accumulator = match[4].str();
        const std::string initialValue = trim(match[5].str());
        const std::string conditionOperator = match[6].str();
        const std::string conditionRight = replaceIdentifier(trim(match[7].str()), sourceVariable, "value");
        const std::string accumulatorExpression = replaceIdentifier(trim(match[8].str()), sourceVariable, "value");
        const std::string mutationOperator = match[9].str();
        const std::string mutationExpression = replaceIdentifier(trim(match[10].str()), sourceVariable, "value");
        const std::string lambdaName = lambdaNameForAccumulator(accumulator);
        const std::string replacement =
            "const auto " + lambdaName + " = [](" + valueType + " value)\n"
            "{\n"
            "    " + valueType + " " + accumulator + "{" + initialValue + "};\n\n"
            "    while (value " + conditionOperator + " " + conditionRight + ")\n"
            "    {\n"
            "        " + accumulator + " = " + accumulatorExpression + ";\n"
            "        value " + mutationOperator + " " + mutationExpression + ";\n"
            "    }\n\n"
            "    return " + accumulator + ";\n"
            "};\n\n"
            "return " + sourceVariable + " == " + lambdaName + "(" + sourceVariable + ");";

        addAppliedChange(changes,
                         name(),
                         trim(match[0].str()),
                         trim(replacement),
                         "Encapsulates self-contained local computation and improves readability using modern C++ lambda expressions.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
        representation.replaceSourceText(code);
    }
};

class FunctorToLambdaRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Function object to lambda";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useLambdas || !atLeastBalanced(options) || aiStyleAggressive(options)) {
            return;
        }

        static const std::regex functorPattern(
            R"(struct\s+([A-Za-z_]\w*)\s*\{\s*([A-Za-z_]\w*(?:::\w+)*|int|long|long long|double|float|bool)\s+operator\(\)\s*\(([^;\{\}]*)\)\s*const\s*\{\s*return\s+([^;\{\}]+);\s*\}\s*\};)");

        std::string code = representation.sourceText();
        std::smatch match;
        if (!std::regex_search(code, match, functorPattern)) {
            return;
        }

        std::string lambdaName = match[1].str();
        if (!lambdaName.empty()) {
            lambdaName[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lambdaName[0])));
        }
        const std::string replacement = "const auto " + lambdaName + " = [](" + match[3].str() + ") { return " + match[4].str() + "; };";
        addAppliedChange(changes,
                         name(),
                         trim(match[0].str()),
                         replacement,
                         "A stateless function object with a simple call operator can be represented more locally and clearly as a lambda.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
        representation.replaceSourceText(code);
    }
};

class StructuredBindingRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "Structured binding for pair-like value";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        if (!options.useStructuredBindings || !aggressiveSafe(options)) {
            return;
        }

        static const std::regex pairLoop(
            R"((^[ \t]*)for\s*\(\s*const\s+auto&\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\n\1\{\s*\n([ \t]*)std::cout\s*<<\s*\2\.first\s*<<\s*\2\.second\s*<<\s*std::endl\s*;\s*\n\1\})",
            std::regex::ECMAScript | std::regex::multiline);

        std::string code = representation.sourceText();
        std::smatch match;
        if (!std::regex_search(code, match, pairLoop)) {
            return;
        }

        const std::string replacement = match[1].str() + "for (const auto& [key, value] : " + match[3].str() + ")\n"
            + match[1].str() + "{\n"
            + match[4].str() + "std::cout << key << value << std::endl;\n"
            + match[1].str() + "}";
        addAppliedChange(changes,
                         name(),
                         trim(match[0].str()),
                         trim(replacement),
                         "Structured bindings make pair-like loop values clearer by naming each element directly.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
        representation.replaceSourceText(code);
    }
};

class EnumClassSuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "enum class suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex enumPattern(R"(\benum\s+([A-Za-z_]\w*)\s*\{)");
        const std::string code = representation.sourceText();

        for (std::sregex_iterator it(code.begin(), code.end(), enumPattern), end; it != end; ++it) {
            if (options.useEnumClass) {
                addSuggestion(changes,
                              name(),
                              it->str(),
                              "Plain enums leak enumerator names into the surrounding scope. Consider enum class for scoped, type-safe enumerations.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 it->str(),
                                 "Skipped because the enum class modernization option is disabled.");
            }
        }
    }
};

class OptionalSuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "std::optional suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex nullableReturnPattern(R"(\breturn\s+nullptr\s*;)");
        const std::string code = representation.sourceText();

        for (std::sregex_iterator it(code.begin(), code.end(), nullableReturnPattern), end; it != end; ++it) {
            if (options.useOptional) {
                addSuggestion(changes,
                              name(),
                              it->str(),
                              "If nullptr represents an absent value rather than pointer ownership, consider std::optional for clearer intent.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 it->str(),
                                 "Skipped because the std::optional modernization option is disabled.");
            }
        }
    }
};

class StringViewSuggestionRule final : public IConversionRule
{
public:
    [[nodiscard]] std::string name() const override
    {
        return "std::string_view suggestion";
    }

    void apply(CodeRepresentation& representation,
               const ModernizationOptions& options,
               std::vector<ConversionChange>& changes) const override
    {
        static const std::regex stringParamPattern(R"(const\s+std::string\s*&\s+([A-Za-z_]\w*))");
        const std::string code = representation.sourceText();

        if (options.useStringView && options.applyStringViewWhenSafe) {
            const std::regex safeFunctionPattern(
                R"((^[ \t]*(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&]*\s+)+[A-Za-z_]\w*\s*\([^)]*?)const\s+std::string\s*&\s+([A-Za-z_]\w*)([^)]*\)\s*(?:const\s*)?\{([\s\S]*?)^\s*\}))",
                std::regex::ECMAScript | std::regex::multiline);
            auto escapes = [](const std::string& body, const std::string& parameter) {
                return std::regex_search(body, std::regex(R"(\breturn\s+)" + parameter + R"(\b)"))
                    || std::regex_search(body, std::regex(R"(\b[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*\s*=\s*)" + parameter + R"(\b)"))
                    || std::regex_search(body, std::regex(R"(\b(?:push_back|emplace_back|insert|assign)\s*\([^;\n]*\b)" + parameter + R"(\b)"));
            };

            std::string updated = code;
            std::smatch match;
            std::string search = updated;
            std::size_t consumed = 0;
            bool changed = false;
            while (std::regex_search(search, match, safeFunctionPattern)) {
                const std::string parameter = match[2].str();
                if (escapes(match[4].str(), parameter)) {
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }

                const std::string before = "const std::string& " + parameter;
                const std::string after = "std::string_view " + parameter;
                const std::size_t replacementPosition = consumed
                    + static_cast<std::size_t>(match.position())
                    + static_cast<std::size_t>(match[1].length());
                updated.replace(replacementPosition, before.size(), after);
                addAppliedChange(changes,
                                 name(),
                                 before,
                                 after,
                                 "The parameter is read-only at the function boundary. std::string_view can avoid unnecessary string construction for callers.");
                changed = true;
                consumed = replacementPosition + after.size();
                search = updated.substr(consumed);
            }

            const std::regex declarationParameterPattern(R"(const\s+std::string\s*&\s+([A-Za-z_]\w*))",
                                                        std::regex::ECMAScript);
            search = updated;
            consumed = 0;
            while (std::regex_search(search, match, declarationParameterPattern)) {
                const std::size_t position = consumed + static_cast<std::size_t>(match.position());
                const std::size_t semicolon = updated.find(';', position);
                const std::size_t openBrace = updated.find('{', position);
                if (semicolon == std::string::npos || (openBrace != std::string::npos && openBrace < semicolon)) {
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }

                const std::string parameter = match[1].str();
                const std::string before = match[0].str();
                const std::string after = "std::string_view " + parameter;
                updated.replace(position, before.size(), after);
                addAppliedChange(changes,
                                 name(),
                                 before,
                                 after,
                                 "The declaration uses a read-only string parameter. std::string_view can avoid unnecessary string construction for callers.");
                changed = true;
                consumed = position + after.size();
                search = updated.substr(consumed);
            }

            if (changed) {
                updated = ensureInclude(updated, "#include <string_view>");
                representation.replaceSourceText(updated);
                return;
            }
        }

        for (std::sregex_iterator it(code.begin(), code.end(), stringParamPattern), end; it != end; ++it) {
            if (options.useStringView) {
                addSuggestion(changes,
                              name(),
                              it->str(),
                              "Read-only string parameters can sometimes use std::string_view to avoid unnecessary string construction. Auto-apply is disabled or the context may need review.");
            } else {
                addSkippedChange(changes,
                                 name(),
                                 it->str(),
                                 "Skipped because the std::string_view modernization option is disabled.");
            }
        }
    }
};

std::vector<std::unique_ptr<IConversionRule>> createDefaultRules()
{
    std::vector<std::unique_ptr<IConversionRule>> rules;
    rules.push_back(std::make_unique<DropNullMacroWorkaroundsRule>());
    rules.push_back(std::make_unique<NullToNullptrRule>());
    rules.push_back(std::make_unique<TypedefToUsingRule>());
    rules.push_back(std::make_unique<StrncpyToStringRule>());
    rules.push_back(std::make_unique<LambdaExtractionRule>());
    rules.push_back(std::make_unique<FunctorToLambdaRule>());
    rules.push_back(std::make_unique<OldStyleCastConversionRule>());
    rules.push_back(std::make_unique<OldStyleCastSuggestionRule>());
    rules.push_back(std::make_unique<RawPointerOwnershipRule>());
    rules.push_back(std::make_unique<AutoUsageRule>());
    rules.push_back(std::make_unique<ConstexprRule>());
    rules.push_back(std::make_unique<OverrideAnnotationRule>());
    rules.push_back(std::make_unique<CStyleArraySuggestionRule>());
    rules.push_back(std::make_unique<ManualLoopSuggestionRule>());
    rules.push_back(std::make_unique<StructuredBindingRule>());
    rules.push_back(std::make_unique<EnumClassSuggestionRule>());
    rules.push_back(std::make_unique<OptionalSuggestionRule>());
    rules.push_back(std::make_unique<StringViewSuggestionRule>());
    return rules;
}
} // namespace

RuleBasedConverterEngine::RuleBasedConverterEngine()
    : RuleBasedConverterEngine(createDefaultRules(), std::make_unique<ModernCppExplanationGenerator>())
{
}

RuleBasedConverterEngine::RuleBasedConverterEngine(std::vector<std::unique_ptr<IConversionRule>> rules,
                                                   std::unique_ptr<IExplanationGenerator> explanationGenerator)
    : rules_(std::move(rules))
    , explanationGenerator_(std::move(explanationGenerator))
{
    if (!explanationGenerator_) {
        throw std::invalid_argument("RuleBasedConverterEngine requires an explanation generator.");
    }

    for (const auto& rule : rules_) {
        if (!rule) {
            throw std::invalid_argument("RuleBasedConverterEngine cannot contain a null conversion rule.");
        }
    }
}

ConversionResult RuleBasedConverterEngine::convert(const std::string& legacyCode) const
{
    return convert(legacyCode, ModernizationOptions{});
}

ConversionResult RuleBasedConverterEngine::convert(const std::string& legacyCode,
                                                   const ModernizationOptions& options) const
{
    ConversionResult result;
    RawTextRepresentation representation(legacyCode);

    if (!options.customInstruction.empty()) {
        addSuggestion(result.changes,
                      "Custom modernization instruction",
                      options.customInstruction,
                      "Stored for future converter engines. The current rule-based engine does not perform AI-based interpretation of free-form instructions.");
    }

    for (const auto& rule : rules_) {
        rule->apply(representation, options, result.changes);
    }

    result.modernCode = representation.sourceText();
    const OfflineModernizationPipeline pipeline;
    const OfflineModernizationPipelineResult pipelineResult = pipeline.runAfterSafeRules(result.modernCode, options, result.changes);
    result.modernCode = pipelineResult.modernCode;
    result.compileVerificationEnabled = pipelineResult.compileVerificationEnabled;
    result.compileVerificationPassed = pipelineResult.compileVerificationPassed;
    result.compileVerificationAutoFixAttempted = pipelineResult.compileVerificationAutoFixAttempted;
    result.compilerUsed = pipelineResult.compilerUsed;
    result.compilerOutput = pipelineResult.compilerOutput;
    result.rewriteLevel = pipelineResult.rewriteLevel;
    result.diagnosticMessages = pipelineResult.diagnosticMessages;
    result.explanation = explanationGenerator_->generate(result.modernCode, result.changes, options);
    return result;
}
