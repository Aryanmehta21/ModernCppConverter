#include "converter/CrossScopeTypePropagationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct SmartVectorSymbol
{
    std::string elementType;
    std::string name;
};

struct SmartPointerSymbol
{
    std::string pointerKind;
    std::string elementType;
    std::string name;
};

struct RawVectorSink
{
    std::string elementType;
    std::string functionName;
};

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

std::vector<SmartVectorSymbol> collectSmartVectors(const std::string& code)
{
    std::vector<SmartVectorSymbol> symbols;
    const std::regex declarationPattern(R"(\bstd::vector\s*<\s*std::unique_ptr\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>\s*>\s+([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator it(code.begin(), code.end(), declarationPattern), end; it != end; ++it) {
        symbols.push_back(SmartVectorSymbol{(*it)[1].str(), (*it)[2].str()});
    }
    return symbols;
}

std::vector<SmartPointerSymbol> collectSmartPointers(const std::string& code)
{
    std::vector<SmartPointerSymbol> symbols;
    const std::regex explicitPattern(R"(\bstd::(unique_ptr|shared_ptr)\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>\s+([A-Za-z_]\w*)\b)");
    for (std::sregex_iterator it(code.begin(), code.end(), explicitPattern), end; it != end; ++it) {
        symbols.push_back(SmartPointerSymbol{(*it)[1].str(), (*it)[2].str(), (*it)[3].str()});
    }

    const std::regex autoPattern(R"(\bauto\s+([A-Za-z_]\w*)\s*=\s*std::make_(unique|shared)\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>\s*\()");
    for (std::sregex_iterator it(code.begin(), code.end(), autoPattern), end; it != end; ++it) {
        const std::string pointerKind = (*it)[2].str() == "unique" ? "unique_ptr" : "shared_ptr";
        symbols.push_back(SmartPointerSymbol{pointerKind, (*it)[3].str(), (*it)[1].str()});
    }
    return symbols;
}

std::vector<RawVectorSink> collectRawVectorSinks(const std::string& code)
{
    std::vector<RawVectorSink> sinks;
    const std::regex functionPattern(
        R"(\b[A-Za-z_][A-Za-z0-9_:<>,\s*&]*\s+([A-Za-z_]\w*)\s*\([^)]*std::vector\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\*\s*>\s*(const\s*)?([&*]\s*)?[A-Za-z_]\w*[^)]*\))");
    for (std::sregex_iterator it(code.begin(), code.end(), functionPattern), end; it != end; ++it) {
        sinks.push_back(RawVectorSink{(*it)[2].str(), (*it)[1].str()});
    }
    return sinks;
}

bool hasDirectCallWithSymbol(const std::string& code, const std::string& functionName, const std::string& symbol)
{
    const std::regex callPattern(R"((^|[^A-Za-z0-9_:])([A-Za-z_]\w*(\s*(\.|->)\s*)?)?)"
                                 + escapeRegex(functionName)
                                 + R"(\s*\(\s*)" + escapeRegex(symbol) + R"(\s*\))");
    return std::regex_search(code, callPattern);
}

bool hasDefinitionBodyAfterParameter(const std::string& code, std::size_t parameterPosition)
{
    const std::size_t closeParen = code.find(')', parameterPosition);
    if (closeParen == std::string::npos) {
        return false;
    }
    const std::size_t nextBrace = code.find('{', closeParen);
    const std::size_t nextSemicolon = code.find(';', closeParen);
    return nextBrace != std::string::npos && (nextSemicolon == std::string::npos || nextBrace < nextSemicolon);
}

std::string rewriteUniquePtrVectorRangeConsumers(std::string code,
                                                 const std::string& elementType,
                                                 const std::string& parameterName,
                                                 std::vector<ConversionChange>& changes)
{
    const std::regex rawPointerRangeLoop(R"(for\s*\(\s*)" + escapeRegex(elementType)
                                         + R"(\s*\*\s*([A-Za-z_]\w*)\s*:\s*)"
                                         + escapeRegex(parameterName)
                                         + R"(\s*\))");
    std::smatch match;
    while (std::regex_search(code, match, rawPointerRangeLoop)) {
        const std::string replacement = "for (const auto& " + match[1].str() + " : " + parameterName + ")";
        addAppliedChange(changes,
                         "CrossScopeTypePropagationPass",
                         trim(match[0].str()),
                         trim(replacement),
                         "Updated a raw-pointer range loop after the collection parameter became a vector of unique_ptr.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
    }
    return code;
}

std::string updateRawVectorSignatures(std::string code,
                                      const std::vector<SmartVectorSymbol>& smartVectors,
                                      std::vector<ConversionChange>& changes)
{
    for (const SmartVectorSymbol& vector : smartVectors) {
        const std::regex signaturePattern(
            R"(const\s+std::vector\s*<\s*)" + escapeRegex(vector.elementType)
            + R"(\s*\*\s*>\s*&\s*([A-Za-z_]\w+))");
        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, signaturePattern)) {
            const std::size_t position = consumed + static_cast<std::size_t>(match.position());
            const std::size_t paren = code.rfind('(', position);
            const std::size_t nameEnd = paren == std::string::npos ? std::string::npos : code.find_last_not_of(" \t\n", paren - 1);
            const std::size_t nameStart = nameEnd == std::string::npos ? std::string::npos : code.find_last_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", nameEnd);
            const std::string functionName = (nameStart == std::string::npos || nameEnd == std::string::npos)
                ? std::string{}
                : code.substr(nameStart + 1, nameEnd - nameStart);
            if (functionName.empty()
                || !hasDefinitionBodyAfterParameter(code, position)
                || !hasDirectCallWithSymbol(code, functionName, vector.name)) {
                consumed = position + static_cast<std::size_t>(match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string replacement = "const std::vector<std::unique_ptr<" + vector.elementType + ">>& " + match[1].str();
            const std::string before = match[0].str();
            code.replace(position, static_cast<std::size_t>(match.length()), replacement);
            code = rewriteUniquePtrVectorRangeConsumers(std::move(code), vector.elementType, match[1].str(), changes);
            addAppliedChange(changes,
                             "CrossScopeTypePropagationPass",
                             trim(before),
                             trim(replacement),
                             "Updated a visible raw-pointer vector consumer signature after the reachable argument became a vector of unique_ptr.");
            consumed = position + replacement.size();
            search = code.substr(consumed);
        }
    }
    return code;
}

std::string updateRawPointerSignatures(std::string code,
                                      const std::vector<SmartPointerSymbol>& smartPointers,
                                      std::vector<ConversionChange>& changes)
{
    for (const SmartPointerSymbol& pointer : smartPointers) {
        const std::regex signaturePattern(R"((const\s+)?)" + escapeRegex(pointer.elementType)
                                          + R"(\s*\*\s*([A-Za-z_]\w+))");
        std::smatch match;
        std::string search = code;
        std::size_t consumed = 0;
        while (std::regex_search(search, match, signaturePattern)) {
            const std::size_t position = consumed + static_cast<std::size_t>(match.position());
            const std::size_t lineStart = code.rfind('\n', position) == std::string::npos ? 0 : code.rfind('\n', position) + 1;
            const std::size_t lineEnd = code.find('\n', position);
            const std::string line = code.substr(lineStart, (lineEnd == std::string::npos ? code.size() : lineEnd) - lineStart);
            if (line.find('(') == std::string::npos || line.find(')') == std::string::npos) {
                consumed = position + static_cast<std::size_t>(match.length());
                search = code.substr(consumed);
                continue;
            }
            const std::size_t paren = code.rfind('(', position);
            const std::size_t nameEnd = paren == std::string::npos ? std::string::npos : code.find_last_not_of(" \t\n", paren - 1);
            const std::size_t nameStart = nameEnd == std::string::npos ? std::string::npos : code.find_last_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", nameEnd);
            const std::string functionName = (nameStart == std::string::npos || nameEnd == std::string::npos)
                ? std::string{}
                : code.substr(nameStart + 1, nameEnd - nameStart);
            if (functionName.empty()
                || !hasDefinitionBodyAfterParameter(code, position)
                || !hasDirectCallWithSymbol(code, functionName, pointer.name)) {
                consumed = position + static_cast<std::size_t>(match.length());
                search = code.substr(consumed);
                continue;
            }

            const std::string smartType = "const std::" + pointer.pointerKind + "<" + pointer.elementType + ">& " + match[2].str();
            const std::string before = match[0].str();
            code.replace(position, static_cast<std::size_t>(match.length()), smartType);
            addAppliedChange(changes,
                             "CrossScopeTypePropagationPass",
                             trim(before),
                             trim(smartType),
                             "Updated a visible raw-pointer consumer signature after the reachable argument became a smart pointer.");
            consumed = position + smartType.size();
            search = code.substr(consumed);
        }
    }
    return code;
}

std::string adaptExternalRawVectorSinks(std::string code,
                                        const std::vector<SmartVectorSymbol>& smartVectors,
                                        const std::vector<RawVectorSink>& externalSinks,
                                        std::vector<ConversionChange>& changes)
{
    if (smartVectors.empty() || externalSinks.empty()) {
        return code;
    }

    bool changed = false;
    std::set<std::string> adapted;
    for (const SmartVectorSymbol& vector : smartVectors) {
        for (const RawVectorSink& sink : externalSinks) {
            if (sink.elementType != vector.elementType) {
                continue;
            }
            const std::string key = sink.functionName + ":" + vector.name;
            if (adapted.contains(key)) {
                continue;
            }
            const std::string callNeedle = sink.functionName + "(" + vector.name + ");";
            const std::size_t callPosition = code.find(callNeedle);
            if (callPosition == std::string::npos) {
                continue;
            }
            const std::size_t lineStart = code.rfind('\n', callPosition) == std::string::npos ? 0 : code.rfind('\n', callPosition) + 1;
            const std::string prefix = code.substr(lineStart, callPosition - lineStart);
            if (prefix.find_first_not_of(" \t") != std::string::npos) {
                continue;
            }
            const std::string indent = prefix;
            adapted.insert(key);
            const std::string rawView = vector.name + "RawView";
            std::ostringstream replacement;
            replacement << indent << "std::vector<" << vector.elementType << "*> " << rawView << ";\n"
                        << indent << rawView << ".reserve(" << vector.name << ".size());\n"
                        << indent << "for (const auto& item : " << vector.name << ")\n"
                        << indent << "{\n"
                        << indent << "    " << rawView << ".push_back(item.get());\n"
                        << indent << "}\n"
                        << indent << sink.functionName << "(" << rawView << ");";
            const std::string replacementText = replacement.str();
            addAppliedChange(changes,
                             "CrossScopeTypePropagationPass",
                             trim(indent + callNeedle),
                             trim(replacementText),
                             "Adapted a vector of unique_ptr to an external raw-pointer vector API using a temporary non-owning view.");
            code.replace(lineStart, indent.size() + callNeedle.size(), replacementText);
            changed = true;
        }
    }
    return changed ? IncludeManager().ensureInclude(code, "#include <vector>") : code;
}

std::string repairClassVectorGetter(std::string code, std::vector<ConversionChange>& changes)
{
    const std::regex getterPattern(
        R"(const\s+std::vector\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\*\s*>\s*&\s*([A-Za-z_]\w*)\s*\(\s*\)\s*const\s*\{\s*return\s+([A-Za-z_]\w*)\s*;\s*\})");
    std::smatch match;
    while (std::regex_search(code, match, getterPattern)) {
        const std::string member = match[3].str();
        const std::regex memberPattern(R"(std::vector\s*<\s*std::unique_ptr\s*<\s*)"
                                       + escapeRegex(match[1].str()) + R"(\s*>\s*>\s+)"
                                       + escapeRegex(member) + R"(\s*;)");
        if (!std::regex_search(code, memberPattern)) {
            break;
        }
        const std::string replacement = "const std::vector<std::unique_ptr<" + match[1].str() + ">>& "
            + match[2].str() + "() const { return " + member + "; }";
        addAppliedChange(changes,
                         "CrossScopeTypePropagationPass",
                         trim(match[0].str()),
                         trim(replacement),
                         "Propagated a class getter return type after the member became a vector of unique_ptr.");
        code.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
    }
    return code;
}
} // namespace

std::string CrossScopeTypePropagationPass::rewrite(const std::string& code,
                                                   const ModernizationOptions& options,
                                                   const TransformationContext& context,
                                                   std::vector<ConversionChange>& changes) const
{
    return rewrite(code, options, context, {}, changes);
}

std::string CrossScopeTypePropagationPass::rewrite(const std::string& code,
                                                   const ModernizationOptions&,
                                                   const TransformationContext&,
                                                   const std::string& externalContext,
                                                   std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const std::vector<SmartVectorSymbol> smartVectors = collectSmartVectors(updated);
    const std::vector<SmartPointerSymbol> smartPointers = collectSmartPointers(updated);

    updated = updateRawVectorSignatures(std::move(updated), smartVectors, changes);
    updated = updateRawPointerSignatures(std::move(updated), smartPointers, changes);
    updated = repairClassVectorGetter(std::move(updated), changes);

    if (!externalContext.empty()) {
        const std::vector<SmartVectorSymbol> currentSmartVectors = collectSmartVectors(updated);
        const std::vector<RawVectorSink> externalSinks = collectRawVectorSinks(externalContext);
        updated = adaptExternalRawVectorSinks(std::move(updated),
                                              currentSmartVectors,
                                              externalSinks,
                                              changes);
    }

    return updated;
}
