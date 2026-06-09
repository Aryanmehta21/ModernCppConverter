#include "converter/MemberApiCascadePass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
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

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (const char character : text) {
        if (std::string(R"(\.^$|()[]{}*+?)").find(character) != std::string::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string vectorElementType(const std::string& vectorType)
{
    const std::string prefix = "std::vector<";
    if (!vectorType.starts_with(prefix) || vectorType.back() != '>') {
        return {};
    }
    return trim(vectorType.substr(prefix.size(), vectorType.size() - prefix.size() - 1));
}

bool isVectorRecord(const TypeChangeRecord& record)
{
    return record.newType.starts_with("std::vector<");
}

bool isStringRecord(const TypeChangeRecord& record)
{
    return record.newType == "std::string";
}

bool isRawBufferGetterName(const std::string& functionName)
{
    const std::string lowered = lowercase(functionName);
    return lowered.find("raw") != std::string::npos
        || lowered.find("buffer") != std::string::npos
        || lowered.find("bytes") != std::string::npos
        || lowered.find("data") != std::string::npos
        || lowered.find("ptr") != std::string::npos
        || lowered.find("pointer") != std::string::npos;
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

std::string rewriteWholeVectorGetters(std::string code,
                                      const TypeChangeRecord& record,
                                      std::vector<ConversionChange>& changes,
                                      bool& changed)
{
    const std::string elementType = vectorElementType(record.newType);
    if (elementType.empty()) {
        return code;
    }

    const std::string escapedElement = escapeRegex(elementType);
    const std::string escapedSymbol = escapeRegex(record.symbolName);
    const std::regex getterPattern(
        R"((^[ \t]*)(const\s+)?)" + escapedElement
            + R"(\s*\*\s*([A-Za-z_]\w*)\s*\(\s*\)\s*(const\s*)?\{\s*return\s+(?:this->)?)"
            + escapedSymbol
            + R"(\s*;\s*\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, getterPattern)) {
        const std::string functionName = match[3].str();
        const bool constReturn = match[2].matched;
        const bool constMethod = match[4].matched;
        std::string replacement;

        if (isRawBufferGetterName(functionName)) {
            const std::string returnType = constReturn || constMethod ? "const " + elementType + "*" : elementType + "*";
            replacement = match[1].str() + returnType + " " + functionName + "()"
                + (constMethod ? " const" : "")
                + " { return " + record.symbolName + ".data(); }";
            addAppliedChange(changes,
                             "Vector raw buffer getter cascade",
                             trim(match[0].str()),
                             trim(replacement),
                             "The getter name indicates raw buffer access, so the vector-backed storage now exposes data() instead of the container object.");
        } else {
            const bool readonly = constReturn || constMethod;
            replacement = match[1].str()
                + (readonly ? "const " + record.newType + "& " : record.newType + "& ")
                + functionName + "()"
                + (readonly ? " const" : "")
                + " { return " + record.symbolName + "; }";
            addAppliedChange(changes,
                             "Vector getter return type cascade",
                             trim(match[0].str()),
                             trim(replacement),
                             "Updated a whole-collection getter after storage became std::vector so the API returns the container consistently.");
        }

        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string rewriteVectorIndexAccessors(std::string code,
                                        const TypeChangeRecord& record,
                                        std::vector<ConversionChange>& changes,
                                        bool& changed)
{
    const std::string elementType = vectorElementType(record.newType);
    if (elementType.empty()) {
        return code;
    }

    const std::string escapedElement = escapeRegex(elementType);
    const std::string escapedSymbol = escapeRegex(record.symbolName);
    const std::regex accessorPattern(
        R"((^[ \t]*)(const\s+)?)" + escapedElement
            + R"(\s*\*\s*([A-Za-z_]\w*)\s*\(\s*(?:int|long|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*\)\s*(const\s*)?\{\s*([\s\S]*?)\n?\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, accessorPattern)) {
        const std::string body = match[6].str();
        const std::string indexName = match[4].str();
        const std::regex returnElementPattern(R"(return\s+&\s*)" + escapedSymbol + R"(\s*\[\s*)"
                                                  + escapeRegex(indexName) + R"(\s*\]\s*;)");
        if (!std::regex_search(body, returnElementPattern)) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string rewrittenBody = body;
        rewrittenBody = std::regex_replace(rewrittenBody,
                                           std::regex(R"(if\s*\(\s*)"
                                                          + escapeRegex(indexName)
                                                          + R"(\s*>=\s*0\s*&&\s*)"
                                                          + escapeRegex(indexName)
                                                          + R"(\s*<\s*[A-Za-z_]\w*\s*\))"),
                                           "if (" + indexName + " >= 0 && static_cast<std::size_t>("
                                               + indexName + ") < " + record.symbolName + ".size())");
        rewrittenBody = std::regex_replace(rewrittenBody,
                                           std::regex(R"(if\s*\(\s*)"
                                                          + escapeRegex(indexName)
                                                          + R"(\s*<\s*[A-Za-z_]\w*\s*\))"),
                                           "if (static_cast<std::size_t>(" + indexName + ") < "
                                               + record.symbolName + ".size())");
        rewrittenBody = std::regex_replace(rewrittenBody,
                                           returnElementPattern,
                                           "return &" + record.symbolName + "[static_cast<std::size_t>("
                                               + indexName + ")];");

        if (rewrittenBody == body) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool constReturn = match[2].matched || match[5].matched;
        const std::string replacement = match[1].str() + (constReturn ? "const " : "") + elementType + "* " + match[3].str()
            + "(int " + indexName + ") "
            + (match[5].matched ? "const " : "")
            + "{\n" + rewrittenBody + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Member API cascade",
                         trim(match[0].str()),
                         trim(replacement),
                         "Updated an index accessor so it observes vector-backed storage with size() bounds and safe element addressing.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string rewriteVectorSizeGetters(std::string code,
                                     const TypeChangeRecord& record,
                                     std::vector<ConversionChange>& changes,
                                     bool& changed)
{
    const std::regex sizeGetterPattern(
        R"((^[ \t]*)(?:constexpr\s+)?(?:int|long|size_t|std::size_t|auto)\s+([A-Za-z_]\w*)\s*\(\s*\)\s*const\s*\{\s*return\s+([A-Za-z_]\w*(?:\.size\s*\(\s*\))?)\s*;\s*\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, sizeGetterPattern)) {
        const std::string functionName = lowercase(match[2].str());
        const std::string returned = match[3].str();
        const bool countLike = functionName.find("count") != std::string::npos
            || functionName.find("size") != std::string::npos;
        const bool alreadyVectorSize = std::regex_match(returned,
                                                        std::regex(escapeRegex(record.symbolName) + R"(\.size\s*\(\s*\))"));
        if (!countLike && !alreadyVectorSize) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string replacement = match[1].str() + "std::size_t " + match[2].str()
            + "() const { return " + record.symbolName + ".size(); }";
        if (trim(match[0].str()) == trim(replacement)) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Count getter to vector size",
                         trim(match[0].str()),
                         trim(replacement),
                         "Updated a size/count getter to return std::vector::size() after storage became a vector.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string rewriteStringGetters(std::string code,
                                 const TypeChangeRecord& record,
                                 std::vector<ConversionChange>& changes,
                                 bool& changed)
{
    const std::string escapedSymbol = escapeRegex(record.symbolName);
    const std::regex getterPattern(
        R"((^[ \t]*)(const\s+)?char\s*\*\s*([A-Za-z_]\w*)\s*\(\s*\)\s*(const\s*)?\{\s*return\s+(?:this->)?)"
            + escapedSymbol
            + R"(\s*;\s*\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, getterPattern)) {
        const std::string functionName = match[3].str();
        std::string replacement;
        if (isRawBufferGetterName(functionName)) {
            replacement = match[1].str() + "const char* " + functionName + "() const { return "
                + record.symbolName + ".c_str(); }";
        } else {
            replacement = match[1].str() + "const std::string& " + functionName + "() const { return "
                + record.symbolName + "; }";
        }

        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "String getter return type cascade",
                         trim(match[0].str()),
                         trim(replacement),
                         "Updated a getter after char-buffer storage became std::string so the API exposes string ownership consistently.");
        changed = true;
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

template <typename RewriteFunction>
std::string rewriteClassForRecord(std::string code,
                                  const TypeChangeRecord& record,
                                  const RewriteFunction& rewriteFunction)
{
    if (!record.isClassMember || record.scopeName.empty()) {
        return code;
    }

    const ClassResourceAnalyzer analyzer;
    const std::vector<ClassBlock> classes = analyzer.analyzeClasses(code);
    for (auto iterator = classes.rbegin(); iterator != classes.rend(); ++iterator) {
        if (iterator->name != record.scopeName) {
            continue;
        }

        std::string classText = code.substr(iterator->start, iterator->end - iterator->start);
        const std::string rewrittenClassText = rewriteFunction(std::move(classText));
        if (rewrittenClassText != code.substr(iterator->start, iterator->end - iterator->start)) {
            code.replace(iterator->start, iterator->end - iterator->start, rewrittenClassText);
        }
        break;
    }

    return code;
}
} // namespace

std::string MemberApiCascadePass::rewrite(const std::string& code,
                                          const TransformationContext& context,
                                          std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = false;

    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (isVectorRecord(record)) {
            updated = rewriteClassForRecord(std::move(updated), record, [&](std::string classText) {
                classText = rewriteWholeVectorGetters(std::move(classText), record, changes, changed);
                classText = rewriteVectorIndexAccessors(std::move(classText), record, changes, changed);
                classText = rewriteVectorSizeGetters(std::move(classText), record, changes, changed);
                return classText;
            });
        } else if (isStringRecord(record)) {
            updated = rewriteClassForRecord(std::move(updated), record, [&](std::string classText) {
                return rewriteStringGetters(std::move(classText), record, changes, changed);
            });
        }
    }

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(std::move(updated), "#include <cstddef>");
    }

    return updated;
}
