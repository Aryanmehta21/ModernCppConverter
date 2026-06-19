#include "converter/ModernizationPolishPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

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

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBracePosition)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;

    for (std::size_t index = openBracePosition; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
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

std::string stripCommentsBracesAndWhitespace(std::string text)
{
    text = std::regex_replace(text, std::regex(R"(//[^\n]*)"), "");
    text = std::regex_replace(text, std::regex(R"(/\*[\s\S]*?\*/)"), "");
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char character) {
                   return std::isspace(character) || character == '{' || character == '}';
               }),
               text.end());
    return text;
}

bool hasBusinessLogic(const std::string& body)
{
    const std::string lowered = lowercase(body);
    if (lowered.find("todo") != std::string::npos
        || lowered.find("required") != std::string::npos
        || lowered.find("intentional") != std::string::npos) {
        return true;
    }

    static const std::regex sideEffectPattern(R"(\b(?:std::cout|std::cerr|printf|fprintf|throw|open|close|lock|unlock|callback|telemetry|log)\b)",
                                              std::regex_constants::icase);
    return std::regex_search(body, sideEffectPattern);
}

std::string removeEmptyIfBlocks(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex emptyIfPattern(R"(\n?[ \t]*if\s*\([^)\n]+\)\s*\n?[ \t]*\{\s*(?://[^\n]*)?\s*\}\s*\n?)",
                                    std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    while (std::regex_search(code, match, emptyIfPattern)) {
        addAppliedChange(changes,
                         "Remove empty cleanup block",
                         trim(match[0].str()),
                         "removed",
                         "Removed an empty control block left by resource-cleanup modernization.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), "\n");
    }
    return code;
}

std::string cleanupEmptyDestructors(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex destructorHeader(R"((^[ \t]*)~([A-Za-z_]\w*)\s*\(\s*\)\s*\{)",
                                      std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, destructorHeader)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string functionText = code.substr(position, closeBrace - position + 1);
        const std::string body = code.substr(openBrace + 1, closeBrace - openBrace - 1);
        if (!stripCommentsBracesAndWhitespace(body).empty() || hasBusinessLogic(body)) {
            consumed = closeBrace + 1;
            search = code.substr(consumed);
            continue;
        }

        addAppliedChange(changes,
                         "Rule of Zero destructor cleanup",
                         trim(functionText),
                         "removed",
                         "Removed an empty cleanup-only destructor after RAII modernization.");
        addAppliedChange(changes,
                         "Rule of Zero special member removal",
                         trim(functionText),
                         "removed",
                         "Applied Rule of Zero because standard library members now own cleanup.");
        code.replace(position, closeBrace - position + 1, "");
        consumed = position;
        search = code.substr(consumed);
    }
    return code;
}

std::string removeIncorrectRuntimeConstexpr(const std::string& code, std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        const bool looksRuntimeState = codePart.find("constexpr") != std::string::npos
            && codePart.find('(') != std::string::npos
            && (codePart.find(".size()") != std::string::npos
                || codePart.find(".data()") != std::string::npos
                || codePart.find("std::vector") != std::string::npos
                || codePart.find("std::string") != std::string::npos);
        if (!looksRuntimeState) {
            return line;
        }

        const std::string replacement = std::regex_replace(codePart, std::regex(R"(\bconstexpr\s+)"), "");
        if (replacement != codePart) {
            changed = true;
            addAppliedChange(changes,
                             "Constexpr correctness cleanup",
                             trim(codePart),
                             trim(replacement),
                             "Removed constexpr from a function that reads runtime standard-library state.");
            return replacement + trailingComment;
        }
        return line;
    });

    return changed ? updated : code;
}

std::string replaceSimpleEndl(std::string code, std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    code = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        if ((codePart.find("std::cout") == std::string::npos && codePart.find("std::cerr") == std::string::npos)
            || codePart.find("std::endl") == std::string::npos
            || codePart.find("std::flush") != std::string::npos) {
            return line;
        }

        const std::string replacement = std::regex_replace(codePart, std::regex(R"(<<\s*std::endl\s*;)"), "<< '\\n';");
        if (replacement == codePart) {
            return line;
        }

        changed = true;
        addAppliedChange(changes,
                         "Stream newline cleanup",
                         trim(codePart),
                         trim(replacement),
                         "Replaced std::endl with a newline character where flushing is not required.");
        return replacement + trailingComment;
    });
    return changed ? code : code;
}

std::string variableNameForCollection(const std::string& collection)
{
    const std::size_t separator = collection.find_last_of(".>");
    std::string name = separator == std::string::npos ? collection : collection.substr(separator + 1);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char character) {
                   return !std::isalnum(character) && character != '_';
               }),
               name.end());
    if (name.size() > 1 && name.back() == 's') {
        name.pop_back();
    }
    return name.empty() ? "item" : name;
}

std::string modernizeMapIteratorLoops(std::string code,
                                      const ModernizationOptions& options,
                                      std::vector<ConversionChange>& changes)
{
    if (!options.useStructuredBindings
        || (options.targetStandard != CppStandard::Cpp17 && options.targetStandard != CppStandard::Cpp20)) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*((?:auto|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.c?begin\(\)\s*;\s*\3\s*!=\s*\4\s*\.c?end\(\)\s*;\s*(?:\+\+\3|\3\+\+)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        const std::string iteratorName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        if (body.find("erase(") != std::string::npos
            || body.find("insert(") != std::string::npos
            || body.find("++" + iteratorName) != std::string::npos
            || body.find(iteratorName + "++") != std::string::npos) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex firstPattern("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*first\b)");
        const std::regex secondPattern("\\b" + escapeRegex(iteratorName) + R"(\s*->\s*second\b)");
        if (!std::regex_search(body, firstPattern) && !std::regex_search(body, secondPattern)) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        body = std::regex_replace(body, firstPattern, "key");
        body = std::regex_replace(body, secondPattern, "value");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");
        const std::string replacement = match[1].str() + "for (const auto& [key, value] : " + collection + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Map iterator loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a simple map-like iterator loop to a range loop with structured bindings.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::string modernizeRangeLoopsToStructuredBindings(std::string code,
                                                    const ModernizationOptions& options,
                                                    std::vector<ConversionChange>& changes)
{
    if (!options.useStructuredBindings) {
        return code;
    }

    const std::regex rangeLoop(
        R"((^[ \t]*)for\s*\(\s*const\s+auto&\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, rangeLoop)) {
        const std::string element = match[2].str();
        std::string body = match[4].str();
        const std::regex firstPattern("\\b" + escapeRegex(element) + R"(\.first\b)");
        const std::regex secondPattern("\\b" + escapeRegex(element) + R"(\.second\b)");
        if (!std::regex_search(body, firstPattern) || !std::regex_search(body, secondPattern)) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        body = std::regex_replace(body, firstPattern, "key");
        body = std::regex_replace(body, secondPattern, "value");
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");
        const std::string replacement = match[1].str() + "for (const auto& [key, value] : " + match[3].str() + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Map range loop to structured binding",
                         trim(match[0].str()),
                         trim(replacement),
                         "Replaced pair member access in a map-like range loop with structured bindings.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::string modernizeGenericIteratorLoops(std::string code,
                                          const ModernizationOptions& options,
                                          std::vector<ConversionChange>& changes)
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::regex iteratorLoop(
        R"((^[ \t]*)for\s*\(\s*((?:auto|typename\s+[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator|[A-Za-z_:][A-Za-z0-9_:<>,\s]*::(?:const_)?iterator))\s+([A-Za-z_]\w*)\s*=\s*()"
            + collectionExpression
            + R"()\s*\.c?begin\(\)\s*;\s*\3\s*!=\s*\4\s*\.c?end\(\)\s*;\s*(?:\+\+\3|\3\+\+)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = code;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, iteratorLoop)) {
        const std::string iteratorTypeText = match[2].str();
        const std::string iteratorName = match[3].str();
        std::string body = match[5].str();
        if (body.find("erase(") != std::string::npos
            || body.find("insert(") != std::string::npos
            || body.find("++" + iteratorName) != std::string::npos
            || body.find(iteratorName + "++") != std::string::npos
            || body.find(iteratorName + "--") != std::string::npos
            || body.find("--" + iteratorName) != std::string::npos) {
            addSuggestion(changes,
                          "Explicit iterator loop to range-based for",
                          trim(match[0].str()),
                          "Iterator loop was preserved because it mutates iteration or changes the container while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::regex directDerefStream(R"(<<\s*(?:\(\s*)?\*\s*)"
                                           + escapeRegex(iteratorName)
                                           + R"(\s*(?:\))?)",
                                           std::regex::ECMAScript);
        if (iteratorTypeText.find("typename") != std::string::npos
            && std::regex_search(body, directDerefStream)) {
            addSuggestion(changes,
                          "Explicit iterator loop to range-based for",
                          trim(match[0].str()),
                          "Dependent iterator loop was preserved because it streams the element directly and the element type may be a non-streamable pair.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string dereferencePattern = R"(\*)" + escapeRegex(iteratorName) + R"(\b)";
        const std::string bodyWithoutDereference = std::regex_replace(body, std::regex(dereferencePattern), "");
        if (std::regex_search(bodyWithoutDereference, std::regex("\\b" + escapeRegex(iteratorName) + "\\b"))) {
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const bool mutableElement = std::regex_search(body, std::regex(dereferencePattern + R"(\s*(?:=|\+=|-=|\*=|/=|%=))"));
        const std::string elementName = variableNameForCollection(match[4].str());
        body = std::regex_replace(body, std::regex(dereferencePattern), elementName);
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");
        const std::string replacement = match[1].str() + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + elementName + " : " + match[4].str() + ")\n"
            + match[1].str() + "{\n" + body + "\n" + match[1].str() + "}";
        code.replace(consumed + static_cast<std::size_t>(match.position()),
                     static_cast<std::size_t>(match.length()),
                     replacement);
        addAppliedChange(changes,
                         "Explicit iterator loop to range-based for",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a safe explicit iterator loop to a range-based for loop.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = code.substr(consumed);
    }
    return code;
}

std::vector<std::string> uniquePtrVectorNames(const std::string& code)
{
    std::vector<std::string> names;
    const std::regex declarationPattern(R"((?:const\s+)?std::vector\s*<\s*std::unique_ptr\s*<[^;\n{}]+>\s*>\s*(?:const\s*)?(?:[&*]\s*)?([A-Za-z_]\w*))");
    for (std::sregex_iterator iterator(code.begin(), code.end(), declarationPattern), end; iterator != end; ++iterator) {
        const std::string name = (*iterator)[1].str();
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

std::string modernizeUniquePtrCollectionIndexLoops(std::string code,
                                                   const ModernizationOptions& options,
                                                   std::vector<ConversionChange>& changes)
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    for (const std::string& collection : uniquePtrVectorNames(code)) {
        const std::regex loopPattern(
            R"((^[ \t]*)for\s*\(\s*(?:int|size_t|std::size_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*)"
                + escapeRegex(collection)
                + R"(\.size\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*\n\1\{\s*\n([\s\S]*?)\n\1\})",
            std::regex::ECMAScript | std::regex::multiline);

        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, loopPattern)) {
            const std::string indent = match[1].str();
            const std::string indexName = match[2].str();
            std::string body = match[3].str();
            if (body.find("delete ") != std::string::npos
                || body.find(collection + ".erase") != std::string::npos
                || body.find(collection + ".insert") != std::string::npos) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            const std::string indexedExpression = escapeRegex(collection) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
            const std::string bodyWithoutIndexed = std::regex_replace(body, std::regex(indexedExpression), "");
            const bool needsIndex = std::regex_search(bodyWithoutIndexed, std::regex("\\b" + escapeRegex(indexName) + "\\b"));
            std::string rewrittenBody = body;
            rewrittenBody = std::regex_replace(rewrittenBody,
                                               std::regex(indexedExpression + R"(\s*!=\s*nullptr)"),
                                               "item");
            rewrittenBody = std::regex_replace(rewrittenBody,
                                               std::regex(indexedExpression + R"(\s*==\s*nullptr)"),
                                               "!item");
            rewrittenBody = std::regex_replace(rewrittenBody,
                                               std::regex(indexedExpression + R"(\s*->)"),
                                               "item->");
            rewrittenBody = std::regex_replace(rewrittenBody,
                                               std::regex(indexedExpression),
                                               "item");
            if (needsIndex) {
                rewrittenBody = std::regex_replace(rewrittenBody,
                                                   std::regex("\\b" + escapeRegex(indexName) + "\\b"),
                                                   "index");
            }

            if (std::regex_search(rewrittenBody, std::regex(indexedExpression))) {
                consumed += static_cast<std::size_t>(match.position() + match.length());
                search = match.suffix().str();
                continue;
            }

            std::ostringstream replacement;
            if (needsIndex) {
                replacement << indent << "std::size_t index = 0;\n";
            }
            replacement << indent << "for (const auto& item : " << collection << ")\n"
                        << indent << "{\n"
                        << rewrittenBody;
            if (needsIndex) {
                replacement << "\n" << indent << "    ++index;";
            }
            replacement << "\n" << indent << "}";

            const std::string replacementText = replacement.str();
            code.replace(consumed + static_cast<std::size_t>(match.position()),
                         static_cast<std::size_t>(match.length()),
                         replacementText);
            addAppliedChange(changes,
                             "Unique_ptr collection loop to range-based for",
                             trim(match[0].str()),
                             trim(replacementText),
                             "Converted index traversal over a vector of unique_ptr into range traversal while preserving index use when needed.");
            consumed += static_cast<std::size_t>(match.position()) + replacementText.size();
            search = code.substr(consumed);
        }
    }

    return code;
}

std::string modernizeVectorPushBackInitializers(std::string code, std::vector<ConversionChange>& changes)
{
    const std::vector<std::string> lines = splitLines(code);
    std::vector<std::string> rewritten;
    rewritten.reserve(lines.size());
    const std::regex declarationPattern(R"(^([ \t]*)std::vector\s*<\s*([^>]+)\s*>\s+([A-Za-z_]\w*)\s*;\s*$)");
    bool changed = false;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::smatch declarationMatch;
        if (!std::regex_match(lines[index], declarationMatch, declarationPattern)) {
            rewritten.push_back(lines[index]);
            continue;
        }

        const std::string indent = declarationMatch[1].str();
        const std::string type = trim(declarationMatch[2].str());
        const std::string name = declarationMatch[3].str();
        if (type.find("unique_ptr") != std::string::npos) {
            rewritten.push_back(lines[index]);
            continue;
        }
        std::vector<std::string> values;
        std::size_t scan = index + 1;
        const std::regex pushPattern("^" + escapeRegex(indent) + escapeRegex(name)
                                         + R"(\.push_back\s*\(\s*([^;]+?)\s*\)\s*;\s*$)");
        for (; scan < lines.size(); ++scan) {
            std::smatch pushMatch;
            if (!std::regex_match(lines[scan], pushMatch, pushPattern)) {
                break;
            }
            if (pushMatch[1].str().find("std::make_unique") != std::string::npos
                || pushMatch[1].str().find("std::unique_ptr") != std::string::npos) {
                values.clear();
                break;
            }
            values.push_back(trim(pushMatch[1].str()));
        }

        if (values.size() < 2) {
            rewritten.push_back(lines[index]);
            continue;
        }

        std::ostringstream initializer;
        initializer << indent << "std::vector<" << type << "> " << name << "{";
        for (std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
            if (valueIndex > 0) {
                initializer << ", ";
            }
            initializer << values[valueIndex];
        }
        initializer << "};";

        std::ostringstream before;
        before << lines[index];
        for (std::size_t beforeIndex = index + 1; beforeIndex < scan; ++beforeIndex) {
            before << '\n' << lines[beforeIndex];
        }
        const std::string replacement = initializer.str();
        rewritten.push_back(replacement);
        changed = true;
        addAppliedChange(changes,
                         "Repeated push_back to initializer list",
                         trim(before.str()),
                         trim(replacement),
                         "Converted consecutive push_back calls on a newly declared vector into initializer-list construction.");
        index = scan - 1;
    }

    return changed ? joinLines(rewritten) : code;
}

std::string modernizeMapPairInsertions(const std::string& code, std::vector<ConversionChange>& changes)
{
    const SafeReplacementEngine safeReplacement;
    bool changed = false;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        std::smatch match;
        const std::regex pairInsertPattern(R"(^([ \t]*)([A-Za-z_]\w*)\.insert\s*\(\s*std::pair\s*<[^;()]+>\s*\(\s*([^,;]+)\s*,\s*([^)]+)\s*\)\s*\)\s*;\s*$)");
        if (std::regex_match(codePart, match, pairInsertPattern)) {
            const std::string replacement = match[1].str() + match[2].str() + ".emplace("
                + trim(match[3].str()) + ", " + trim(match[4].str()) + ");";
            addAppliedChange(changes,
                             "Map pair insert to emplace",
                             trim(codePart),
                             trim(replacement),
                             "Replaced verbose std::pair construction during map insertion with emplace().");
            changed = true;
            return replacement + trailingComment;
        }

        const std::regex makePairInsertPattern(R"(^([ \t]*)([A-Za-z_]\w*)\.insert\s*\(\s*std::make_pair\s*\(\s*([^,;]+)\s*,\s*([^)]+)\s*\)\s*\)\s*;\s*$)");
        if (std::regex_match(codePart, match, makePairInsertPattern)) {
            const std::string replacement = match[1].str() + match[2].str() + ".emplace("
                + trim(match[3].str()) + ", " + trim(match[4].str()) + ");";
            addAppliedChange(changes,
                             "Map pair insert to emplace",
                             trim(codePart),
                             trim(replacement),
                             "Replaced make_pair insertion with emplace().");
            changed = true;
            return replacement + trailingComment;
        }

        return line;
    });
    return changed ? updated : code;
}
} // namespace

std::string ModernizationPolishPass::rewrite(const std::string& code,
                                             const ModernizationOptions& options,
                                             const TransformationContext&,
                                             std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    updated = cleanupEmptyDestructors(std::move(updated), changes);
    updated = removeEmptyIfBlocks(std::move(updated), changes);
    updated = removeIncorrectRuntimeConstexpr(updated, changes);

    if (options.useRangeBasedFor) {
        updated = modernizeUniquePtrCollectionIndexLoops(std::move(updated), options, changes);
        updated = modernizeMapIteratorLoops(std::move(updated), options, changes);
        updated = modernizeGenericIteratorLoops(std::move(updated), options, changes);
        updated = modernizeRangeLoopsToStructuredBindings(std::move(updated), options, changes);
    }

    updated = modernizeVectorPushBackInitializers(std::move(updated), changes);
    updated = modernizeMapPairInsertions(updated, changes);
    updated = replaceSimpleEndl(std::move(updated), changes);

    if (updated.find("std::size_t") != std::string::npos) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(std::move(updated), "#include <cstddef>");
    }

    return updated;
}
