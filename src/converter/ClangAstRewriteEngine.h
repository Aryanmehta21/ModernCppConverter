#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"
#include "parser/SourceRange.h"

#include <cstddef>
#include <string>
#include <vector>

struct ASTRewriteEdit
{
    SourceRange range;
    std::string replacementText;
    std::string symbolId;
    std::string rewriteReason;
    std::string passName;
    std::string safetyStatus;
};

struct ClangAstRewriteResult
{
    std::string code;
    std::vector<std::string> diagnostics;
    std::size_t editsProposed = 0;
    std::size_t editsApplied = 0;
    std::size_t editsSkipped = 0;
    bool attempted = false;
    bool enabled = false;
};

class ClangAstRewriteEngine
{
public:
    [[nodiscard]] ClangAstRewriteResult rewriteTypedefAliases(const std::string& source,
                                                              const ModernizationOptions& options,
                                                              std::vector<ConversionChange>& changes) const;
};
