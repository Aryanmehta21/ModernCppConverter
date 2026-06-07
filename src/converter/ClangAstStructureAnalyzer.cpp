#include "converter/ClangAstStructureAnalyzer.h"

#include "converter/TokenBasedStructureAnalyzer.h"

CodeStructure ClangAstStructureAnalyzer::analyze(const std::string& code) const
{
    // Placeholder for future Clang LibTooling integration.
    // Until Clang is linked, reuse the token/block analyzer so callers can depend on the interface.
    const TokenBasedStructureAnalyzer fallbackAnalyzer;
    return fallbackAnalyzer.analyze(code);
}
