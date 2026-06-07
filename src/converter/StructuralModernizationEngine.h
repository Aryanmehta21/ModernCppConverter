#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class TransformationContext;

class StructuralModernizationEngine
{
public:
    [[nodiscard]] std::string modernize(const std::string& code,
                                        const ModernizationOptions& options,
                                        std::vector<ConversionChange>& changes,
                                        TransformationContext& context) const;

private:
    [[nodiscard]] std::string modernizePreprocessor(const std::string& code,
                                                    const ModernizationOptions& options,
                                                    std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string modernizeTypedefStructs(const std::string& code,
                                                      std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string modernizeCharBuffers(const std::string& code,
                                                   std::vector<ConversionChange>& changes,
                                                   TransformationContext& context) const;
    [[nodiscard]] std::string modernizeDynamicArrays(const std::string& code,
                                                     std::vector<ConversionChange>& changes,
                                                     TransformationContext& context) const;
    [[nodiscard]] std::string modernizeEnums(const std::string& code,
                                             const ModernizationOptions& options,
                                             std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string modernizeLoops(const std::string& code,
                                             const ModernizationOptions& options,
                                             std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string modernizeStreamFormatting(const std::string& code,
                                                        const ModernizationOptions& options,
                                                        std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string validatePreprocessorBalance(const std::string& code,
                                                          std::vector<ConversionChange>& changes) const;
};
