#pragma once

#include <string>

struct ConversionChange
{
    std::string ruleName;
    std::string before;
    std::string after;
    std::string reason;
    bool applied = false;
    bool skipped = false;
};
