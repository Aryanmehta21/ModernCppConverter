#pragma once

#include "converter/ICodeStructureAnalyzer.h"

class TokenBasedStructureAnalyzer final : public ICodeStructureAnalyzer
{
public:
    [[nodiscard]] CodeStructure analyze(const std::string& code) const override;
};
