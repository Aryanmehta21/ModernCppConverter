#pragma once

#include "parser/ParsedEntity.h"

#include <string>

class LightweightCppParser
{
public:
    [[nodiscard]] ParsedDocument parse(const std::string& source) const;
};
