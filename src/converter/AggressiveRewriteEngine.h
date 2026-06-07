#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <string>
#include <vector>

class AggressiveRewriteEngine
{
public:
    [[nodiscard]] std::string rewrite(const std::string& code,
                                      const ModernizationOptions& options,
                                      std::vector<ConversionChange>& changes) const;

    [[nodiscard]] std::string rewriteOwnershipModernizations(const std::string& code,
                                                             std::vector<ConversionChange>& changes) const;

    [[nodiscard]] std::string ensureModernIncludes(const std::string& code,
                                                   const ModernizationOptions& options,
                                                   std::vector<ConversionChange>* changes = nullptr) const;

private:
    [[nodiscard]] std::string rewriteClassMemberOwnership(const std::string& code,
                                                          std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteAliasedPointerOwnership(const std::string& code,
                                                             std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteStringViewStringOwnership(const std::string& code,
                                                               std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteLocalComputationBlocks(const std::string& code,
                                                            std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteHelperFunctionUsedOnce(const std::string& code,
                                                            std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteFunctorPredicate(const std::string& code,
                                                      const ModernizationOptions& options,
                                                      std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string rewriteOutputLoopsToAlgorithms(const std::string& code,
                                                             const ModernizationOptions& options,
                                                             std::vector<ConversionChange>& changes) const;
    [[nodiscard]] std::string lightlyFormat(const std::string& code) const;
};
