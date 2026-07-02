#pragma once

#include "models/ConversionChange.h"

#include <cstddef>
#include <string>
#include <vector>

struct IncludeCleanupResult
{
    std::string code;
    std::size_t syntaxNormalizedCount = 0;
    std::size_t duplicateIncludesRemovedCount = 0;
    std::size_t includesPreservedCount = 0;
    std::size_t requiredIncludesAddedCount = 0;
    std::size_t requiredIncludesAlreadyPresentCount = 0;
    std::vector<std::string> requiredIncludeNamesAdded;
    std::size_t obsoleteIncludesRemovedCount = 0;
    std::size_t obsoleteIncludesKeptCount = 0;
    std::size_t obsoleteIncludesSkippedCount = 0;
    std::vector<std::string> obsoleteIncludeNamesRemoved;
    std::vector<std::string> obsoleteIncludeNamesKept;
    std::vector<std::string> obsoleteIncludeNamesSkipped;
    bool modified = false;
};

class IncludeCleanupPass
{
public:
    [[nodiscard]] IncludeCleanupResult run(const std::string& code,
                                           std::vector<ConversionChange>& changes) const;
};
