#pragma once

#include "parser/SourceRange.h"

#include <string>
#include <vector>

struct RewriteEdit
{
    SourceRange range;
    std::string replacementText;
    std::string passName;
    std::string reason;
    std::string affectedSymbol;
    int priority = 0;
    bool allowOverlap = false;
};

struct SkippedRewriteEdit
{
    RewriteEdit edit;
    std::string reason;
};

struct RewriteApplicationResult
{
    std::string code;
    std::vector<RewriteEdit> appliedEdits;
    std::vector<SkippedRewriteEdit> skippedEdits;
    std::size_t duplicateEdits = 0;
    std::size_t overlapConflicts = 0;
    std::size_t invalidRanges = 0;
};

class RewriteCoordinator
{
public:
    [[nodiscard]] RewriteApplicationResult apply(const std::string& source,
                                                 const std::vector<RewriteEdit>& edits) const;
};
