#pragma once

#include "converter/TransformationContext.h"
#include "models/ConversionChange.h"

#include <string>
#include <vector>

class DependentUsageRewritePass
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const TransformationContext& context,
                                      std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string runConsistencyChecks(const std::string& code,
                                                   const TransformationContext& context,
                                                   std::vector<ConversionChange>& changes) const;

private:
    [[nodiscard]] std::string rewriteValueTypePointerUsages(const std::string& code,
                                                            const TransformationContext& context,
                                                            std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteVectorUsages(const std::string& code,
                                                  const TransformationContext& context,
                                                  std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteStringUsages(const std::string& code,
                                                  const TransformationContext& context,
                                                  std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteSmartPointerUsages(const std::string& code,
                                                        const TransformationContext& context,
                                                        std::vector<ConversionChange>& changes) const;
};
