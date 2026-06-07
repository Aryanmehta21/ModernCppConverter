#pragma once

#include <filesystem>
#include <string>

struct RepositoryBackupResult
{
    bool success = false;
    std::filesystem::path backupPath;
    std::string message;
};

class RepositoryBackupService
{
public:
    [[nodiscard]] RepositoryBackupResult createBackup(const std::filesystem::path& filePath) const;
};
