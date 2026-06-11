#pragma once

#include "models/ConversionChange.h"

#include <string>
#include <vector>

class EnumToStringCandidatePass
{
public:
    void suggest(const std::string& code, std::vector<ConversionChange>& changes) const;
};

