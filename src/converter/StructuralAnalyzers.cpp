#include "converter/StructuralAnalyzers.h"

#include "converter/TokenBasedStructureAnalyzer.h"

#include <regex>

namespace
{
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
} // namespace

std::vector<PreprocessorBlock> PreprocessorAnalyzer::analyze(const std::string& code) const
{
    const TokenBasedStructureAnalyzer analyzer;
    return analyzer.analyze(code).preprocessorBlocks;
}

std::vector<StructTypedefDeclaration> TypeDeclarationAnalyzer::analyzeTypedefStructs(const std::string& code) const
{
    const TokenBasedStructureAnalyzer analyzer;
    return analyzer.analyze(code).typedefStructs;
}

std::vector<ClassBlock> ClassResourceAnalyzer::analyzeClasses(const std::string& code) const
{
    const TokenBasedStructureAnalyzer analyzer;
    return analyzer.analyze(code).classes;
}

std::vector<LoopBlock> LoopAnalyzer::analyzeLoops(const std::string& code) const
{
    const TokenBasedStructureAnalyzer analyzer;
    return analyzer.analyze(code).loops;
}

bool OwnershipAnalyzer::hasPotentialPointerEscape(const std::string& code, const std::string& variableName) const
{
    const std::string escaped = escapeRegex(variableName);
    return std::regex_search(code, std::regex("\\breturn\\s+" + escaped + R"(\s*;)"))
        || std::regex_search(code, std::regex(R"(\b[A-Za-z_]\w*\s*=\s*)" + escaped + R"(\s*;)"));
}

bool OwnershipAnalyzer::hasPointerArithmetic(const std::string& code, const std::string& variableName) const
{
    const std::string escaped = escapeRegex(variableName);
    return std::regex_search(code, std::regex("\\b" + escaped + R"(\s*(?:\+\+|--|\+|-))"))
        || std::regex_search(code, std::regex(R"((?:\+|-)\s*)" + escaped + R"(\b)"));
}
