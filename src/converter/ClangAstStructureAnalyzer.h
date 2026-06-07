#pragma once

#include "converter/ICodeStructureAnalyzer.h"

class ClangAstStructureAnalyzer final : public ICodeStructureAnalyzer
{
public:
    [[nodiscard]] CodeStructure analyze(const std::string& code) const override;
};
