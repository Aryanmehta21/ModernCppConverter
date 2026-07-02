#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"
#include "parser/ParsedEntity.h"

#include <string>
#include <vector>

struct SemanticValidationAndRepairResult
{
    std::string code;
    int issuesDetected = 0;
    int issuesRepaired = 0;
    int issuesSkipped = 0;
    std::vector<std::string> diagnostics;
};

class SemanticValidationAndRepairPass
{
public:
    [[nodiscard]] SemanticValidationAndRepairResult validateAndRepair(const std::string& code,
                                                                      const ModernizationOptions& options,
                                                                      const TransformationContext& context,
                                                                      std::vector<ConversionChange>& changes) const;
    [[nodiscard]] SemanticValidationAndRepairResult validateAndRepair(const std::string& code,
                                                                      const ModernizationOptions& options,
                                                                      const TransformationContext& context,
                                                                      const ParsedDocument& selectedDocument,
                                                                      const std::string& selectedFrontendName,
                                                                      bool reusedSelectedRepresentation,
                                                                      std::vector<ConversionChange>& changes) const;
};
