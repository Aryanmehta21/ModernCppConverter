#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class CompilerDiagnosticCleanupPass
{
public:
    [[nodiscard]] std::string run(const std::string& code,
                                  const TransformationContext& context,
                                  const std::string& compilerOutput,
                                  std::vector<ConversionChange>& changes) const;
};
