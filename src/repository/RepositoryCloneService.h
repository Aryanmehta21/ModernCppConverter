#pragma once

#include "models/RepositoryModernizationModels.h"

#include <filesystem>
#include <string>

struct RepositoryCloneResult
{
    bool success = false;
    std::filesystem::path clonePath;
    std::string message;
};

class RepositoryCloneService
{
public:
    [[nodiscard]] static bool isValidGitHubUrl(const std::string& url);
    [[nodiscard]] static bool isSafeBranchName(const std::string& branch);
    [[nodiscard]] RepositoryCloneResult cloneRepository(const RepositoryModernizationOptions& options) const;
};
