#include "converter/CrossFunctionTypePropagationPass.h"

#include "converter/IncludeManager.h"
#include "converter/SafeReplacementEngine.h"

#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct UniquePtrVector
{
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

std::vector<UniquePtrVector> collectUniquePtrVectors(const std::string& code)
{
    std::vector<UniquePtrVector> vectors;
    const std::regex declarationPattern(R"(\bstd::vector\s*<\s*std::unique_ptr\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>\s*>\s+([A-Za-z_]\w*)\b)",
                                        std::regex::ECMAScript);
    for (std::sregex_iterator it(code.begin(), code.end(), declarationPattern), end; it != end; ++it) {
        vectors.push_back(UniquePtrVector{(*it)[1].str(), (*it)[2].str()});
    }
    return vectors;
}

std::vector<RawVectorSink> collectRawVectorSinks(const std::string& code)
{
    std::vector<RawVectorSink> sinks;
    const std::regex functionPattern(
        R"(\b[A-Za-z_][A-Za-z0-9_:<>,\s*&]*\s+([A-Za-z_]\w*)\s*\([^)]*std::vector\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\*\s*>\s*(const\s*)?([&*]\s*)?[A-Za-z_]\w*[^)]*\))",
        std::regex::ECMAScript);
    for (std::sregex_iterator it(code.begin(), code.end(), functionPattern), end; it != end; ++it) {
        sinks.push_back(RawVectorSink{(*it)[2].str(), (*it)[1].str()});
    }
    return sinks;
}

std::string rawViewNameFor(const std::string& vectorName)
{
    return vectorName + "RawView";
}

std::string makeRawViewBlock(const std::string& indent,
                             const UniquePtrVector& vector,
                             const RawVectorSink& sink)
{
    const std::string rawView = rawViewNameFor(vector.name);
    std::ostringstream output;
    output << indent << "std::vector<" << vector.elementType << "*> " << rawView << ";\n"
           << indent << rawView << ".reserve(" << vector.name << ".size());\n"
           << indent << "for (const auto& item : " << vector.name << ")\n"
           << indent << "{\n"
           << indent << "    " << rawView << ".push_back(item.get());\n"
           << indent << "}\n"
           << indent << sink.functionName << "(" << rawView << ");";
    return output.str();
}
} // namespace

std::string CrossFunctionTypePropagationPass::rewrite(const std::string& code,
                                                      std::vector<ConversionChange>& changes) const
{
    const std::vector<UniquePtrVector> vectors = collectUniquePtrVectors(code);
    const std::vector<RawVectorSink> sinks = collectRawVectorSinks(code);
    if (vectors.empty() || sinks.empty()) {
        return code;
    }

    bool changed = false;
    std::set<std::string> adaptedCalls;
    const SafeReplacementEngine safeReplacement;
    std::string updated = safeReplacement.rewriteCodeLines(code, [&](const std::string& line) {
        std::string trailingComment;
        const std::string codePart = SafeReplacementEngine::splitTrailingLineComment(line, trailingComment);
        for (const UniquePtrVector& vector : vectors) {
            for (const RawVectorSink& sink : sinks) {
                if (sink.elementType != vector.elementType) {
                    continue;
                }
                const std::regex callPattern(R"(^([ \t]*))" + escapeRegex(sink.functionName)
                                             + R"(\s*\(\s*)" + escapeRegex(vector.name) + R"(\s*\)\s*;\s*$)");
                std::smatch match;
                if (!std::regex_match(codePart, match, callPattern)) {
                    continue;
                }
                const std::string key = sink.functionName + ":" + vector.name;
                if (adaptedCalls.contains(key)) {
                    return line;
                }
                adaptedCalls.insert(key);
                const std::string replacement = makeRawViewBlock(match[1].str(), vector, sink) + trailingComment;
                addAppliedChange(changes,
                                 "Cross-function smart pointer container propagation",
                                 trim(codePart),
                                 trim(replacement),
                                 "Adapted a vector of owning smart pointers to a visible legacy raw-pointer vector API by creating a temporary non-owning raw pointer view at the call site.");
                changed = true;
                return replacement;
            }
        }
        return line;
    });

    if (changed) {
        const IncludeManager includeManager;
        updated = includeManager.ensureInclude(updated, "#include <vector>");
    }
    return changed ? updated : code;
}
