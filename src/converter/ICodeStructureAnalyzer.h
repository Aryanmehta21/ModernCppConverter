#pragma once

#include "converter/CodeStructure.h"

#include <string>

class ICodeStructureAnalyzer
{
public:
    virtual ~ICodeStructureAnalyzer() = default;

    [[nodiscard]] virtual CodeStructure analyze(const std::string& code) const = 0;
};
