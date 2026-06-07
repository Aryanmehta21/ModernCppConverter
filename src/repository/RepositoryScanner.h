#pragma once

#include <filesystem>
#include <vector>

class RepositoryScanner
{
public:
    [[nodiscard]] std::vector<std::filesystem::path> scanCppFiles(const std::filesystem::path& root) const;
};
