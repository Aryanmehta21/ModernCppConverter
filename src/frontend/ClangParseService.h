#pragma once

#include "frontend/IModernizationFrontend.h"

#include <optional>
#include <string>

class ClangParseService
{
public:
    explicit ClangParseService(ClangParseConfig config = {});

    [[nodiscard]] ModernizationFrontendResult parse(const std::string& source) const;

private:
    [[nodiscard]] ModernizationFrontendResult parseInProcess(const std::string& source) const;
    [[nodiscard]] ModernizationFrontendResult parseOutOfProcess(const std::string& source) const;

    ClangParseConfig config_;
};

[[nodiscard]] std::string serializeClangFrontendResult(const ModernizationFrontendResult& result);
[[nodiscard]] std::optional<ModernizationFrontendResult> deserializeClangFrontendResult(const std::string& payload,
                                                                                       const std::string& originalSource);
