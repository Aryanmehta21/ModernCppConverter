#pragma once

#include "ConversionChange.h"

#include <string>
#include <vector>

struct ConversionResult
{
    std::string modernCode;
    std::vector<ConversionChange> changes;
    std::string explanation;
};
