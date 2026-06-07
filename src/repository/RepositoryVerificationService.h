#pragma once

#include "models/RepositoryModernizationModels.h"

class RepositoryVerificationService
{
public:
    void verifyFile(FileModernizationResult& fileResult, const std::string& code) const;
    [[nodiscard]] std::string summarize(const RepositoryModernizationResult& result) const;
};
