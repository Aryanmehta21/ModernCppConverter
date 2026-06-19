#include "converter/RangeForModernizationPass.h"

#include <algorithm>
#include <cctype>
#include <map>
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
    escaped.reserve(text.size() * 2);
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

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: RangeForModernizationPass",
        "pass started",
        {},
        "candidates found: " + std::to_string(candidates)
            + ", candidates converted: " + std::to_string(converted)
            + ", candidates skipped: " + std::to_string(skipped),
        false,
        true,
    });
}

void addSkippedDiagnostic(std::vector<ConversionChange>& changes, std::string reason)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: RangeForModernizationPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}

bool containsIdentifier(const std::string& text, const std::string& identifier)
{
    return std::regex_search(text, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::set<std::string> collectMapLikeContainers(const std::string& code)
{
    std::set<std::string> containers;
    const std::regex declarationPattern(
        R"(\b(?:const\s+)?std::(?:unordered_)?map\s*<[^;\n{}]+>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        containers.insert((*iterator)[1].str());
    }
    return containers;
}

std::set<std::string> collectSequenceLikeContainers(const std::string& code)
{
    std::set<std::string> containers;
    const std::regex declarationPattern(
        R"(\b(?:const\s+)?std::(?:vector|list|deque|set|unordered_set)\s*<[^;\n{}]+>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        containers.insert((*iterator)[1].str());
    }
    return containers;
}

std::set<std::string> collectPairLikeRangeContainers(const std::string& code)
{
    std::set<std::string> containers = collectMapLikeContainers(code);
    const std::regex pairContainerDeclaration(
        R"(\b(?:const\s+)?std::(?:vector|list|deque|set|unordered_set)\s*<\s*std::pair\s*<[^;\n{}]+>\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)\b)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), pairContainerDeclaration), end; iterator != end; ++iterator) {
        containers.insert((*iterator)[1].str());
    }
    return containers;
}

std::string ensureInclude(std::string code, const std::string& includeLine)
{
    if (code.find(includeLine) != std::string::npos) {
        return code;
    }

    std::size_t insertion = 0;
    const std::regex includePattern(R"(^\s*#include\s*[<"][^>"]+[>"]\s*$)", std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), includePattern), end; iterator != end; ++iterator) {
        insertion = static_cast<std::size_t>(iterator->position() + iterator->length());
    }
    code.insert(insertion, (insertion == 0 ? "" : "\n") + includeLine);
    return code;
}

std::string ensurePairLikeTraitSupport(std::string code)
{
    if (code.find("struct ModernCppConverterPairLike") != std::string::npos) {
        return code;
    }

    code = ensureInclude(std::move(code), "#include <type_traits>");
    code = ensureInclude(std::move(code), "#include <utility>");

    std::size_t insertion = 0;
    const std::regex includePattern(R"(^\s*#include\s*[<"][^>"]+[>"]\s*$)", std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), includePattern), end; iterator != end; ++iterator) {
        insertion = static_cast<std::size_t>(iterator->position() + iterator->length());
    }

    const std::string trait =
        "\n\n"
        "template <typename T, typename = void>\n"
        "struct ModernCppConverterPairLike : std::false_type {};\n\n"
        "template <typename T>\n"
        "struct ModernCppConverterPairLike<T, std::void_t<decltype(std::declval<T>().first), decltype(std::declval<T>().second)>> : std::true_type {};\n";
    code.insert(insertion, trait);
    return code;
}

std::string uniqueLocalName(const std::string& body, const std::string& preferred)
{
    const std::vector<std::string> candidates{preferred, "entry", "element", "current", "pairValue"};
    for (const std::string& candidate : candidates) {
        if (!containsIdentifier(body, candidate)) {
            return candidate;
        }
    }
    int suffix = 1;
    while (containsIdentifier(body, preferred + std::to_string(suffix))) {
        ++suffix;
    }
    return preferred + std::to_string(suffix);
}

std::string normalizedStreamSuffix(std::string suffix)
{
    suffix = trim(std::move(suffix));
    return suffix.empty() ? std::string{} : " " + suffix;
}

bool rewriteDirectIteratorStreamStatementPairAware(std::string& body, const std::string& iteratorName)
{
    const std::regex directStreamStatement(
        R"((^[ \t]*)([^\n;]*?<<[^\n;]*?)<<\s*(?:\(\s*)?\*\s*)"
            + escapeRegex(iteratorName)
            + R"(\s*(?:\))?([^\n;]*);)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (!std::regex_search(body, match, directStreamStatement)) {
        return false;
    }

    const std::string indent = match[1].str();
    const std::string streamPrefix = trim(match[2].str());
    const std::string streamSuffix = normalizedStreamSuffix(match[3].str());
    const std::string valueName = uniqueLocalName(body, "value");
    const std::string replacement =
        indent + "const auto& " + valueName + " = *" + iteratorName + ";\n"
        + indent + "if constexpr (ModernCppConverterPairLike<std::decay_t<decltype(" + valueName + ")>>::value)\n"
        + indent + "{\n"
        + indent + "    " + streamPrefix + " << " + valueName + ".first << \": \" << " + valueName + ".second" + streamSuffix + ";\n"
        + indent + "}\n"
        + indent + "else\n"
        + indent + "{\n"
        + indent + "    " + streamPrefix + " << " + valueName + streamSuffix + ";\n"
        + indent + "}";

    body.replace(static_cast<std::size_t>(match.position()),
                 static_cast<std::size_t>(match.length()),
                 replacement);
    return true;
}

bool rewriteDirectRangeStreamStatementPairAware(std::string& body, const std::string& elementName)
{
    const std::regex directStreamStatement(
        R"((^[ \t]*)([^\n;]*?<<[^\n;]*?)<<\s*)"
            + escapeRegex(elementName)
            + R"(\b(?!\s*(?:\.|->))([^\n;]*);)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (!std::regex_search(body, match, directStreamStatement)) {
        return false;
    }

    const std::string indent = match[1].str();
    const std::string streamPrefix = trim(match[2].str());
    const std::string streamSuffix = normalizedStreamSuffix(match[3].str());
    const std::string replacement =
        indent + "if constexpr (ModernCppConverterPairLike<std::decay_t<decltype(" + elementName + ")>>::value)\n"
        + indent + "{\n"
        + indent + "    " + streamPrefix + " << " + elementName + ".first << \": \" << " + elementName + ".second" + streamSuffix + ";\n"
        + indent + "}\n"
        + indent + "else\n"
        + indent + "{\n"
        + indent + "    " + streamPrefix + " << " + elementName + streamSuffix + ";\n"
        + indent + "}";

    body.replace(static_cast<std::size_t>(match.position()),
                 static_cast<std::size_t>(match.length()),
                 replacement);
    return true;
}

std::size_t findMatchingBrace(const std::string& text, const std::size_t openBrace)
{
    if (openBrace >= text.size() || text[openBrace] != '{') {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool escaped = false;

    for (std::size_t index = openBrace; index < text.size(); ++index) {
        const char current = text[index];
        const char next = (index + 1 < text.size()) ? text[index + 1] : '\0';

        if (inLineComment) {
            if (current == '\n') {
                inLineComment = false;
            }
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                inBlockComment = false;
                ++index;
            }
            continue;
        }
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                inString = false;
            }
            continue;
        }
        if (inCharacter) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '\'') {
                inCharacter = false;
            }
            continue;
        }

        if (current == '/' && next == '/') {
            inLineComment = true;
            ++index;
            continue;
        }
        if (current == '/' && next == '*') {
            inBlockComment = true;
            ++index;
            continue;
        }
        if (current == '"') {
            inString = true;
            continue;
        }
        if (current == '\'') {
            inCharacter = true;
            continue;
        }

        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }

    return std::string::npos;
}

struct TemplateFunctionContext
{
    std::string typeParameter;
    std::string functionName;
    std::string parameterName;
    std::size_t start = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
};

std::vector<TemplateFunctionContext> collectSingleParameterTemplateFunctions(const std::string& code)
{
    std::vector<TemplateFunctionContext> functions;
    const std::regex headerPattern(
        R"(template\s*<\s*(?:class|typename)\s+([A-Za-z_]\w*)\s*>\s*(?:inline\s+|static\s+)?[A-Za-z_:~][A-Za-z0-9_:<>,\s*&]*?\s+([A-Za-z_]\w*)\s*\(\s*(?:const\s+)?\1\s*&\s+([A-Za-z_]\w*)\s*\)\s*\{)",
        std::regex::ECMAScript);

    for (std::sregex_iterator iterator(code.begin(), code.end(), headerPattern), end; iterator != end; ++iterator) {
        const std::smatch match = *iterator;
        const std::size_t start = static_cast<std::size_t>(match.position());
        const std::size_t openBrace = start + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            continue;
        }
        functions.push_back(TemplateFunctionContext{
            match[1].str(),
            match[2].str(),
            match[3].str(),
            start,
            openBrace,
            closeBrace,
        });
    }

    return functions;
}

struct TemplateCallsiteUse
{
    int mapLikeCalls = 0;
    int sequenceLikeCalls = 0;
    int unknownCalls = 0;
};

TemplateCallsiteUse collectTemplateCallsiteUse(const std::string& code,
                                               const TemplateFunctionContext& function,
                                               const std::set<std::string>& mapLikeContainers,
                                               const std::set<std::string>& sequenceLikeContainers)
{
    TemplateCallsiteUse usage;
    const std::regex callPattern(R"(\b)" + escapeRegex(function.functionName) + R"(\s*\(\s*([A-Za-z_]\w*)\s*\))",
                                 std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), callPattern), end; iterator != end; ++iterator) {
        const std::size_t position = static_cast<std::size_t>((*iterator).position());
        if (position >= function.start && position <= function.closeBrace) {
            continue;
        }

        const std::string argument = (*iterator)[1].str();
        if (mapLikeContainers.find(argument) != mapLikeContainers.end()) {
            ++usage.mapLikeCalls;
        } else if (sequenceLikeContainers.find(argument) != sequenceLikeContainers.end()) {
            ++usage.sequenceLikeCalls;
        } else {
            ++usage.unknownCalls;
        }
    }
    return usage;
}

std::string baseNameForCollection(std::string collection)
{
    const std::size_t separator = collection.find_last_of(".>");
    if (separator != std::string::npos) {
        collection = collection.substr(separator + 1);
    }
    collection.erase(std::remove_if(collection.begin(), collection.end(), [](unsigned char character) {
                         return !std::isalnum(character) && character != '_';
                     }),
                     collection.end());
    if (collection.size() > 1 && collection.back() == 's') {
        collection.pop_back();
    }
    return collection.empty() ? "item" : collection;
}

std::string finalIdentifierFromCollection(std::string collection)
{
    collection = trim(std::move(collection));
    std::smatch match;
    const std::regex trailingIdentifier(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]+\])?\s*$)");
    if (std::regex_search(collection, match, trailingIdentifier)) {
        return match[1].str();
    }
    return {};
}

std::string variableNameForCollection(const std::string& collection, const std::string& body)
{
    std::vector<std::string> candidates;
    candidates.push_back(baseNameForCollection(collection));
    candidates.push_back("item");
    candidates.push_back("element");
    candidates.push_back("value");
    candidates.push_back("entry");

    const std::string collectionIdentifier = finalIdentifierFromCollection(collection);
    for (const std::string& candidate : candidates) {
        if (candidate.empty() || candidate == collectionIdentifier || containsIdentifier(body, candidate)) {
            continue;
        }
        return candidate;
    }
    return collectionIdentifier == "item" ? "element" : "item";
}

bool isUnsafeIteratorBody(const std::string& body, const std::string& iteratorName)
{
    return body.find("erase(") != std::string::npos
        || body.find("insert(") != std::string::npos
        || body.find("std::advance(" + iteratorName) != std::string::npos
        || body.find("++" + iteratorName) != std::string::npos
        || body.find(iteratorName + "++") != std::string::npos
        || body.find("--" + iteratorName) != std::string::npos
        || body.find(iteratorName + "--") != std::string::npos;
}

bool isConstIteratorTraversal(const std::string& iteratorType, const std::string& loopText)
{
    return iteratorType.find("const_iterator") != std::string::npos
        || loopText.find(".cbegin") != std::string::npos
        || loopText.find(".cend") != std::string::npos;
}

std::string modernizeGenericPairPrintLoopsFromMapCallsites(std::string code,
                                                           const ModernizationOptions& options,
                                                           std::vector<ConversionChange>& changes,
                                                           int& candidates,
                                                           int& converted,
                                                           int& skipped)
{
    if (!options.useRangeBasedFor && !options.useStructuredBindings) {
        return code;
    }

    const std::set<std::string> mapLikeContainers = collectMapLikeContainers(code);
    if (mapLikeContainers.empty()) {
        return code;
    }
    const std::set<std::string> sequenceLikeContainers = collectSequenceLikeContainers(code);

    std::vector<TemplateFunctionContext> functions = collectSingleParameterTemplateFunctions(code);
    for (std::size_t functionIndex = 0; functionIndex < functions.size(); ++functionIndex) {
        TemplateFunctionContext function = functions[functionIndex];
        const TemplateCallsiteUse usage = collectTemplateCallsiteUse(code, function, mapLikeContainers, sequenceLikeContainers);
        if (usage.mapLikeCalls == 0) {
            continue;
        }

        ++candidates;
        if (usage.unknownCalls > 0) {
            ++skipped;
            addSkippedDiagnostic(changes,
                                 "Generic print template was preserved because visible call sites include unknown containers; streaming behavior needs manual review.");
            continue;
        }

        std::string functionText = code.substr(function.start, function.closeBrace - function.start + 1);
        std::string rewrittenFunctionText = functionText;
        bool changedFunction = false;
        bool unsafeResidualDereference = false;

        const std::string iteratorLoopPattern =
            R"((^[ \t]*)for\s*\(\s*typename\s+)" + escapeRegex(function.typeParameter)
            + R"(::(?:const_)?iterator\s+([A-Za-z_]\w*)\s*=\s*)"
            + escapeRegex(function.parameterName)
            + R"(\s*\.\s*c?begin\s*\(\s*\)\s*;\s*\2\s*!=\s*)"
            + escapeRegex(function.parameterName)
            + R"(\s*\.\s*c?end\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})";
        const std::regex iteratorLoop(iteratorLoopPattern, std::regex::ECMAScript | std::regex::multiline);

        std::smatch match;
        std::string search = rewrittenFunctionText;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, iteratorLoop)) {
            const std::string iteratorName = match[2].str();
            std::string body = match[3].str();
            const std::regex directStreamPattern(R"(<<\s*(?:\(\s*)?\*\s*)"
                                                 + escapeRegex(iteratorName)
                                                 + R"(\s*(?:\))?)",
                                                 std::regex::ECMAScript);
            if (!std::regex_search(body, directStreamPattern)) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            std::string bodyWithoutDirectStream = std::regex_replace(body, directStreamPattern, "");
            const std::regex plainDeref(R"(\*\s*)" + escapeRegex(iteratorName) + R"(\b)");
            const std::regex parenDeref(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\))");
            if (std::regex_search(bodyWithoutDirectStream, plainDeref)
                || std::regex_search(bodyWithoutDirectStream, parenDeref)) {
                unsafeResidualDereference = true;
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            if (usage.sequenceLikeCalls > 0) {
                if (!rewriteDirectIteratorStreamStatementPairAware(body, iteratorName)) {
                    unsafeResidualDereference = true;
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }
            } else {
                body = std::regex_replace(body, directStreamPattern, "<< " + iteratorName + "->first << \": \" << " + iteratorName + "->second ");
            }
            const std::string replacement = match[1].str() + "for (typename " + function.typeParameter
                + "::const_iterator " + iteratorName + " = " + function.parameterName + ".begin(); " + iteratorName
                + " != " + function.parameterName + ".end(); ++" + iteratorName + ")\n"
                + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
            rewrittenFunctionText.replace(consumed + static_cast<std::size_t>(match.position()),
                                          static_cast<std::size_t>(match.length()),
                                          replacement);
            consumed += static_cast<std::size_t>(match.position()) + replacement.size();
            search = rewrittenFunctionText.substr(consumed);
            changedFunction = true;
        }

        const std::string rangeLoopPattern =
            R"((^[ \t]*)for\s*\(\s*(const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*)"
            + escapeRegex(function.parameterName)
            + R"(\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})";
        const std::regex rangeLoop(rangeLoopPattern, std::regex::ECMAScript | std::regex::multiline);
        search = rewrittenFunctionText;
        consumed = 0;
        while (std::regex_search(search, match, rangeLoop)) {
            const std::string referenceKind = match[2].str();
            const std::string element = match[3].str();
            std::string body = match[4].str();
            const std::regex directStreamPattern(R"(<<\s*)" + escapeRegex(element) + R"(\b(?!\s*(?:\.|->))\s*)",
                                                 std::regex::ECMAScript);
            if (!std::regex_search(body, directStreamPattern)) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }
            std::string bodyWithoutDirectStream = std::regex_replace(body, directStreamPattern, "");
            if (containsIdentifier(bodyWithoutDirectStream, element)) {
                unsafeResidualDereference = true;
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            std::string replacement;
            if (usage.sequenceLikeCalls > 0) {
                if (!rewriteDirectRangeStreamStatementPairAware(body, element)) {
                    unsafeResidualDereference = true;
                    consumed += static_cast<std::size_t>(match.position() + match.length());
                    search = match.suffix().str();
                    continue;
                }
                replacement = match[1].str() + "for (" + referenceKind + " " + element + " : " + function.parameterName + ")\n"
                    + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
            } else if (options.useStructuredBindings
                && (options.targetStandard == CppStandard::Cpp17 || options.targetStandard == CppStandard::Cpp20)) {
                body = std::regex_replace(body, directStreamPattern, "<< key << \": \" << mapped ");
                replacement = match[1].str() + "for (" + referenceKind + " [key, mapped] : " + function.parameterName + ")\n"
                    + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
            } else {
                body = std::regex_replace(body, directStreamPattern, "<< " + element + ".first << \": \" << " + element + ".second ");
                replacement = match[1].str() + "for (" + referenceKind + " " + element + " : " + function.parameterName + ")\n"
                    + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
            }
            rewrittenFunctionText.replace(consumed + static_cast<std::size_t>(match.position()),
                                          static_cast<std::size_t>(match.length()),
                                          replacement);
            consumed += static_cast<std::size_t>(match.position()) + replacement.size();
            search = rewrittenFunctionText.substr(consumed);
            changedFunction = true;
        }

        if (unsafeResidualDereference) {
            ++skipped;
            addSkippedDiagnostic(changes,
                                 "Generic map print template was preserved because the loop uses the dereferenced pair beyond direct stream output.");
            continue;
        }
        if (!changedFunction) {
            ++skipped;
            continue;
        }

        code.replace(function.start, functionText.size(), rewrittenFunctionText);
        if (usage.sequenceLikeCalls > 0) {
            code = ensurePairLikeTraitSupport(std::move(code));
        }
        ++converted;
        addAppliedChange(changes,
                         "Generic map print loop pair formatting",
                         trim(functionText),
                         trim(rewrittenFunctionText),
                         "Visible call sites instantiate this generic printer with map-like containers, so direct pair streaming was rewritten to key/value output.");

        functions = collectSingleParameterTemplateFunctions(code);
        if (functionIndex >= functions.size()) {
            break;
        }
    }

    return code;
}

std::string modernizeMapLoops(std::string code,
                              const ModernizationOptions& options,
                              std::vector<ConversionChange>& changes,
                              int& candidates,
                              int& converted,
                              int& skipped)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::string iteratorType =
        R"((?:auto|typename\s+[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*)"
            + iteratorType
            + R"(\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.\s*c?begin\s*\(\s*\)\s*;\s*\2\s*!=\s*\3\s*\.\s*c?end\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        ++candidates;
        const std::string iteratorName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        if (isUnsafeIteratorBody(body, iteratorName)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map iterator loop was preserved because it mutates iteration or changes the container while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex arrowFirst("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*first\b)");
        const std::regex arrowSecond("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*second\b)");
        const std::regex derefFirst(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\)\s*\.\s*first\b)");
        const std::regex derefSecond(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\)\s*\.\s*second\b)");
        const bool usesFirst = std::regex_search(body, arrowFirst) || std::regex_search(body, derefFirst);
        const bool usesSecond = std::regex_search(body, arrowSecond) || std::regex_search(body, derefSecond);
        if (!usesFirst || !usesSecond) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map iterator loop was preserved because it did not use both key and value.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string bodyWithoutPairAccess = std::regex_replace(body, arrowFirst, "");
        bodyWithoutPairAccess = std::regex_replace(bodyWithoutPairAccess, arrowSecond, "");
        bodyWithoutPairAccess = std::regex_replace(bodyWithoutPairAccess, derefFirst, "");
        bodyWithoutPairAccess = std::regex_replace(bodyWithoutPairAccess, derefSecond, "");
        if (containsIdentifier(bodyWithoutPairAccess, iteratorName)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map iterator loop was preserved because the iterator is used for more than first/second traversal.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool mutableValue = std::regex_search(body, std::regex(R"((?:->\s*second|\)\s*\.\s*second)\s*(?:=|\+=|-=|\*=|/=|%=))"));
        body = std::regex_replace(body, arrowFirst, "key");
        body = std::regex_replace(body, arrowSecond, "value");
        body = std::regex_replace(body, derefFirst, "key");
        body = std::regex_replace(body, derefSecond, "value");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = match[1].str() + "for (" + (mutableValue ? "auto& " : "const auto& ")
            + "[key, value] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Map iterator loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a map-like iterator loop that only accessed first/second into a structured-binding range loop.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string modernizeGenericLoops(std::string code,
                                  const ModernizationOptions& options,
                                  std::vector<ConversionChange>& changes,
                                  int& candidates,
                                  int& converted,
                                  int& skipped)
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::string iteratorType =
        R"((auto|typename\s+[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*)"
            + iteratorType
            + R"(\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.\s*c?begin\s*\(\s*\)\s*;\s*\3\s*!=\s*\4\s*\.\s*c?end\s*\(\s*\)\s*;\s*(?:\+\+\3|\3\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        ++candidates;
        const std::string iteratorTypeText = match[2].str();
        const std::string iteratorName = match[3].str();
        const std::string collection = match[4].str();
        std::string body = match[5].str();
        if (isUnsafeIteratorBody(body, iteratorName)) {
            ++skipped;
            addSuggestion(changes,
                          "Explicit iterator loop to range-based for",
                          trim(match[0].str()),
                          "Iterator loop was preserved because it mutates iteration or changes the container while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex plainDeref(R"(\*\s*)" + escapeRegex(iteratorName) + R"(\b)");
        const std::regex parenDeref(R"(\(\s*\*\s*)" + escapeRegex(iteratorName) + R"(\s*\))");
        const std::regex arrowAccess("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*)");
        const std::regex directDerefStream(R"(<<\s*(?:\(\s*)?\*\s*)"
                                           + escapeRegex(iteratorName)
                                           + R"(\s*(?:\))?)",
                                           std::regex::ECMAScript);
        if (iteratorTypeText.find("typename ") != std::string::npos
            && std::regex_search(body, directDerefStream)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Dependent iterator loop was preserved because it streams the element directly and the element type may be a non-streamable pair.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string bodyWithoutDeref = std::regex_replace(body, parenDeref, "");
        bodyWithoutDeref = std::regex_replace(bodyWithoutDeref, plainDeref, "");
        bodyWithoutDeref = std::regex_replace(bodyWithoutDeref, arrowAccess, "");
        if (std::regex_search(bodyWithoutDeref, std::regex("\\b" + escapeRegex(iteratorName) + "\\b"))) {
            ++skipped;
            addSkippedDiagnostic(changes, "Iterator loop was preserved because the iterator is used for more than dereferencing the current element.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool constIterator = isConstIteratorTraversal(iteratorTypeText, match[0].str());
        const bool mutatesThroughArrow = std::regex_search(body, std::regex("\\b" + escapeRegex(iteratorName)
                                                                            + R"(\s*->\s*[A-Za-z_]\w*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--))"))
            || std::regex_search(body, std::regex("\\b" + escapeRegex(iteratorName)
                                                  + R"(\s*->\s*[A-Za-z_]\w*\s*\()"));
        const bool mutatesThroughDottedDeref = std::regex_search(body, std::regex(R"(\(\s*\*\s*)"
                                                                                  + escapeRegex(iteratorName)
                                                                                  + R"(\s*\)\s*\.\s*[A-Za-z_]\w*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--|\())"));
        const bool mutableElement = std::regex_search(body, std::regex(R"((?:\*\s*)" + escapeRegex(iteratorName) + R"(\b|\(\s*\*\s*)"
                                                                       + escapeRegex(iteratorName)
                                                                       + R"(\s*\)(?:\s*(?:\.|->)[^;\n]*)?)\s*(?:=|\+=|-=|\*=|/=|%=))"))
            || (!constIterator && (mutatesThroughArrow || mutatesThroughDottedDeref));
        const std::string itemName = variableNameForCollection(collection, body);
        body = std::regex_replace(body, parenDeref, itemName);
        body = std::regex_replace(body, plainDeref, itemName);
        body = std::regex_replace(body, arrowAccess, itemName + ".");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = match[1].str() + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + itemName + " : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Explicit iterator loop to range-based for",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a safe explicit iterator traversal into a range-based for loop.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string modernizeDirectPairStreamRangeLoops(std::string code,
                                                const ModernizationOptions& options,
                                                std::vector<ConversionChange>& changes,
                                                int& candidates,
                                                int& converted,
                                                int& skipped)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::set<std::string> pairLikeContainers = collectPairLikeRangeContainers(code);
    if (pairLikeContainers.empty()) {
        return code;
    }

    const std::regex rangeLoop(
        R"((^[ \t]*)for\s*\(\s*(const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, rangeLoop)) {
        ++candidates;
        const std::string referenceKind = match[2].str();
        const std::string element = match[3].str();
        const std::string collection = match[4].str();
        std::string body = match[5].str();
        if (pairLikeContainers.find(collection) == pairLikeContainers.end()) {
            ++skipped;
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex directStreamPattern(R"(<<\s*)" + escapeRegex(element) + R"(\b(?!\s*(?:\.|->))\s*)",
                                             std::regex::ECMAScript);
        if (!std::regex_search(body, directStreamPattern)) {
            ++skipped;
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        std::string bodyWithoutDirectStream = std::regex_replace(body, directStreamPattern, "");
        if (containsIdentifier(bodyWithoutDirectStream, element)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Map-like range loop was preserved because the pair element is used beyond direct stream output.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        body = std::regex_replace(body, directStreamPattern, "<< key << \": \" << mapped ");
        const std::string replacement = match[1].str() + "for (" + referenceKind + " [key, mapped] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Map pair stream to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted direct streaming of a pair-like range element into key/value structured-binding output.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}

std::string modernizePairRangeLoops(std::string code,
                                    const ModernizationOptions& options,
                                    std::vector<ConversionChange>& changes,
                                    int& candidates,
                                    int& converted,
                                    int& skipped)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::regex rangeLoop(
        R"((^[ \t]*)for\s*\(\s*(const\s+auto&|auto&)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, rangeLoop)) {
        ++candidates;
        const std::string referenceKind = match[2].str();
        const std::string element = match[3].str();
        const std::string collection = match[4].str();
        std::string body = match[5].str();
        const std::string escapedElement = escapeRegex(element);
        const std::regex firstPattern(R"((?:\b)" + escapedElement
                                      + R"(\s*\.\s*first\b|\(\s*)"
                                      + escapedElement
                                      + R"(\s*\)\s*\.\s*first\b))");
        const std::regex secondPattern(R"((?:\b)" + escapedElement
                                       + R"(\s*\.\s*second\b|\(\s*)"
                                       + escapedElement
                                       + R"(\s*\)\s*\.\s*second\b))");
        if (!std::regex_search(body, firstPattern) || !std::regex_search(body, secondPattern)) {
            ++skipped;
            addSkippedDiagnostic(changes, "Pair range loop was preserved because it did not use both first and second.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        body = std::regex_replace(body, firstPattern, "key");
        body = std::regex_replace(body, secondPattern, "value");
        const std::string replacement = match[1].str() + "for (" + referenceKind + " [key, value] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        ++converted;
        addAppliedChange(changes,
                         "Map range loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted pair member access in a range loop to structured bindings.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }

    return code;
}
} // namespace

std::string RangeForModernizationPass::rewrite(const std::string& code,
                                               const ModernizationOptions& options,
                                               std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    updated = modernizeMapLoops(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizeGenericPairPrintLoopsFromMapCallsites(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizeGenericLoops(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizeDirectPairStreamRangeLoops(std::move(updated), options, changes, candidates, converted, skipped);
    updated = modernizePairRangeLoops(std::move(updated), options, changes, candidates, converted, skipped);
    addDiagnostic(changes, candidates, converted, skipped);
    return updated;
}
